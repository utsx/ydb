#pragma once

#include <ydb/core/base/appdata.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/protos/kqp.pb.h>

#include <library/cpp/actors/core/actorid.h>

#include <util/datetime/base.h>

namespace NKikimr::NKqp {

struct TKqpProxyRequest {
    TActorId Sender;
    ui64 SenderCookie = 0;
    TString TraceId;
    ui32 EventType;
    TString SessionId;
    TKqpDbCountersPtr DbCounters;

    TKqpProxyRequest(const TActorId& sender, ui64 senderCookie, const TString& traceId,
        ui32 eventType)
        : Sender(sender)
        , SenderCookie(senderCookie)
        , TraceId(traceId)
        , EventType(eventType)
        , SessionId()
    {}

    void SetSessionId(const TString& sessionId, TKqpDbCountersPtr dbCounters) {
        SessionId = sessionId;
        DbCounters = dbCounters;
    }
};


class TKqpProxyRequestTracker {
    ui64 RequestId;
    THashMap<ui64, TKqpProxyRequest> PendingRequests;

public:
    TKqpProxyRequestTracker()
        : RequestId(1)
    {}

    ui64 RegisterRequest(const TActorId& sender, ui64 senderCookie, const TString& traceId, ui32 eventType) {
        ui64 NewRequestId = ++RequestId;
        PendingRequests.emplace(NewRequestId, TKqpProxyRequest(sender, senderCookie, traceId, eventType));
        return NewRequestId;
    }

    const TKqpProxyRequest* FindPtr(ui64 requestId) const {
        return PendingRequests.FindPtr(requestId);
    }

    void SetSessionId(ui64 requestId, const TString& sessionId, TKqpDbCountersPtr dbCounters) {
        TKqpProxyRequest* ptr = PendingRequests.FindPtr(requestId);
        ptr->SetSessionId(sessionId, dbCounters);
    }

    void Erase(ui64 requestId) {
        PendingRequests.erase(requestId);
    }
};


template<typename TValue>
struct TProcessResult {
    Ydb::StatusIds::StatusCode YdbStatus;
    TString Error;
    TValue Value;
    bool ResourceExhausted = false;
};


struct TKqpSessionInfo {
    TString SessionId;
    TActorId WorkerId;
    TString Database;
    TKqpDbCountersPtr DbCounters;
    TInstant LastRequestAt;
    TInstant CreatedAt;
    TInstant ShutdownStartedAt;
    std::vector<i32> ReadyPos;

    TKqpSessionInfo(const TString& sessionId, const TActorId& workerId,
        const TString& database, TKqpDbCountersPtr dbCounters, std::vector<i32>&& pos)
        : SessionId(sessionId)
        , WorkerId(workerId)
        , Database(database)
        , DbCounters(dbCounters)
        , ShutdownStartedAt()
        , ReadyPos(std::move(pos))
    {
        auto now = TAppData::TimeProvider->Now();
        LastRequestAt = now;
        CreatedAt = now;
    }
};

struct TSimpleResourceStats {
    double Mean;
    double Deviation;
    ui64 CV;

    TSimpleResourceStats(double mean, double deviation, ui64 cv)
        : Mean(mean)
        , Deviation(deviation)
        , CV(cv)
    {}
};

struct TPeerStats {
    TSimpleResourceStats LocalSessionCount;
    TSimpleResourceStats CrossAZSessionCount;

    TSimpleResourceStats LocalCpu;
    TSimpleResourceStats CrossAZCpu;


    TPeerStats(TSimpleResourceStats localSessionsCount, TSimpleResourceStats crossAZSessionCount,
               TSimpleResourceStats localCpu, TSimpleResourceStats crossAZCpu)

        : LocalSessionCount(localSessionsCount)
        , CrossAZSessionCount(crossAZSessionCount)
        , LocalCpu(localCpu)
        , CrossAZCpu(crossAZCpu)
    {}
};


class TLocalSessionsRegistry {
    THashMap<TString, TKqpSessionInfo> LocalSessions;
    THashMap<TActorId, TString> TargetIdIndex;
    THashSet<TString> ShutdownInFlightSessions;
    THashMap<TString, ui32> SessionsCountPerDatabase;
    std::vector<std::vector<TString>> ReadySessions;
    TIntrusivePtr<IRandomProvider> RandomProvider;

public:
    TLocalSessionsRegistry(TIntrusivePtr<IRandomProvider> randomProvider)
        : ReadySessions(2)
        , RandomProvider(randomProvider)
    {}

    TKqpSessionInfo* Create(const TString& sessionId, const TActorId& workerId,
        const TString& database, TKqpDbCountersPtr dbCounters, bool supportsBalancing)
    {
        std::vector<i32> pos(2, -1);
        pos[0] = ReadySessions[0].size();
        ReadySessions[0].push_back(sessionId);

        if (supportsBalancing) {
            pos[1] = ReadySessions[1].size();
            ReadySessions[1].push_back(sessionId);
        }

        auto result = LocalSessions.emplace(sessionId,
            TKqpSessionInfo(sessionId, workerId, database, dbCounters, std::move(pos)));
        SessionsCountPerDatabase[database]++;
        Y_VERIFY(result.second, "Duplicate session id!");
        TargetIdIndex.emplace(workerId, sessionId);
        return &result.first->second;
    }

    const THashSet<TString>& GetShutdownInFlight() const {
        return ShutdownInFlightSessions;
    }

    TKqpSessionInfo* StartShutdownSession(const TString& sessionId) {
        ShutdownInFlightSessions.emplace(sessionId);
        auto ptr = LocalSessions.FindPtr(sessionId);
        ptr->ShutdownStartedAt = TAppData::TimeProvider->Now();
        RemoveSessionFromLists(ptr);
        return ptr;
    }

    TKqpSessionInfo* PickSessionToShutdown(bool force, ui32 minReasonableToKick) {
        auto& sessions = force ? ReadySessions.at(0) : ReadySessions.at(1);
        if (sessions.size() >= minReasonableToKick) {
            ui64 idx = RandomProvider->GenRand() % sessions.size();
            return StartShutdownSession(sessions[idx]);
        }

        return nullptr;
    }

    THashMap<TString, TKqpSessionInfo>::const_iterator begin() const {
        return LocalSessions.begin();
    }

    THashMap<TString, TKqpSessionInfo>::const_iterator end() const {
        return LocalSessions.end();
    }

    size_t GetShutdownInFlightSize() const {
        return ShutdownInFlightSessions.size();
    }

    void Erase(const TString& sessionId) {
        auto it = LocalSessions.find(sessionId);
        if (it != LocalSessions.end()) {
            auto counter = SessionsCountPerDatabase.find(it->second.Database);
            if (counter != SessionsCountPerDatabase.end()) {
                counter->second--;
                if (counter->second == 0) {
                    SessionsCountPerDatabase.erase(counter);
                }
            }

            RemoveSessionFromLists(&(it->second));
            ShutdownInFlightSessions.erase(sessionId);
            TargetIdIndex.erase(it->second.WorkerId);
            LocalSessions.erase(it);
        }
    }

    bool IsPendingShutdown(const TString& sessionId) const {
        return ShutdownInFlightSessions.find(sessionId) != ShutdownInFlightSessions.end();
   }

    bool CheckDatabaseLimits(const TString& database, ui32 databaseLimit) {
        auto it = SessionsCountPerDatabase.find(database);
        if (it == SessionsCountPerDatabase.end()){
            return true;
        }

        if (it->second + 1 <= databaseLimit) {
            return true;
        }

        return false;
    }

    size_t size() const {
        return LocalSessions.size();
    }

    const TKqpSessionInfo* FindPtr(const TString& sessionId) const {
        return LocalSessions.FindPtr(sessionId);
    }

    void Erase(const TActorId& targetId) {
        auto it = TargetIdIndex.find(targetId);
        if (it != TargetIdIndex.end()){
            Erase(it->second);
        }
    }

private:
    void RemoveSessionFromLists(TKqpSessionInfo* ptr) {
        for(ui32 i = 0; i < ptr->ReadyPos.size(); ++i) {
            i32& pos = ptr->ReadyPos.at(i);
            auto& sessions = ReadySessions.at(i);
            if (pos != -1 && pos + 1 != static_cast<i32>(sessions.size())) {
                auto& lastPos = LocalSessions.at(sessions.back()).ReadyPos.at(i);
                Y_VERIFY(lastPos + 1 == static_cast<i32>(sessions.size()));
                std::swap(sessions[pos], sessions[lastPos]);
                lastPos = pos;
            }

            if (pos != -1) {
                sessions.pop_back();
                pos = -1;
            }
        }
    }
};


TSimpleResourceStats CalcPeerStats(
    const TVector<NKikimrKqp::TKqpProxyNodeResources>& data, const TString& selfDataCenterId, bool localDatacenterPolicy,
    std::function<double(const NKikimrKqp::TKqpProxyNodeResources& entry)> ExtractValue);

TPeerStats CalcPeerStats(const TVector<NKikimrKqp::TKqpProxyNodeResources>& data, const TString& selfDataCenterId);

IActor* CreateKqpProxyService(const NKikimrConfig::TLogConfig& logConfig,
    const NKikimrConfig::TTableServiceConfig& tableServiceConfig,
    TVector<NKikimrKqp::TKqpSetting>&& settings,
    std::shared_ptr<IQueryReplayBackendFactory> queryReplayFactory);

}  // namespace NKikimr::NKqp