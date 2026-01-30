#pragma once

#include "NetworkFwd.h"
#include "../Common/Types.h"
#include "../Common/Buffer.h"
#include "../Common/LockFreeQueue.h"

#include <atomic>


//=========================================================================
// CGameSession - 논리적 세션 (LocalThread가 소유)
// Socket은 Bind/Unbind로 재접속 지원
//=========================================================================
class CGameSession
{
public:
    // 포인터 큐 - 8바이트 * 512 = 4KB (기존 4MB에서 대폭 감소)
    static constexpr size_t INCOMING_QUEUE_CAPACITY = 512;

public:
    CGameSession();
    ~CGameSession();

    CGameSession(const CGameSession&) = delete;
    CGameSession& operator=(const CGameSession&) = delete;

    // 초기화/리셋
    void Initialize(SessionId id, ThreadId owner_thread_id);
    void Reset();

    // Socket 바인딩
    bool BindSocket(CSocket* socket);
    void UnbindSocket();
    CSocket* GetSocket() const { return pSocket; }
    bool HasSocket() const { return pSocket != nullptr; }

    // 재접속
    void SetReconnectToken(const FReconnectToken& token) { ReconnectToken = token; }
    const FReconnectToken& GetReconnectToken() const { return ReconnectToken; }
    bool ValidateReconnectToken(uint64_t token) const;
    void StartDisconnectTimer();
    bool IsDisconnectTimerExpired() const;

    // 패킷 처리 (IO Thread에서 호출)
    void OnPacketReceived(FPacketBuffer* packet);
    void OnSocketDisconnected();

    // 패킷 처리 (LocalThread에서 호출)
    bool PopIncomingPacket(FPacketBuffer*& out_packet);
    size_t GetIncomingQueueSize() const { return IncomingQueue.Size(); }

    // Send (LocalThread에서 호출)
    bool Send(FPacketBuffer* packet);
    bool Send(ProtocolId protocol_id, const void* data, size_t size);

    // State
    ESessionState GetState() const { return State.load(std::memory_order_acquire); }
    void SetState(ESessionState new_state);
    bool TransitionState(ESessionState expected, ESessionState new_state);

    // Accessors
    SessionId GetSessionId() const { return OwnerSessionId; }
    AccountId GetAccountId() const { return OwnerAccountId; }
    void SetAccountId(AccountId id) { OwnerAccountId = id; }
    ThreadId GetOwnerThreadId() const { return OwnerThreadId; }
    MapId GetMapId() const { return OwnerMapId; }
    void SetMapId(MapId id) { OwnerMapId = id; }

    uint64_t GetLastActiveTime() const { return LastActiveTime; }
    void UpdateLastActiveTime();
    uint64_t GetUsn() const { return Usn; }
    void SetUsn(uint64_t usn) { Usn = usn; }

private:
    SessionId OwnerSessionId;
    AccountId OwnerAccountId;
    ThreadId OwnerThreadId;
    MapId OwnerMapId;
    uint64_t Usn = 0;
    std::atomic<ESessionState> State;

    CSocket* pSocket;
    FReconnectToken ReconnectToken;
    uint64_t DisconnectTime;

    // 포인터 큐 (FPacketBuffer* 저장)
    SPSCQueue<FPacketBuffer*, INCOMING_QUEUE_CAPACITY> IncomingQueue;

    uint64_t LastActiveTime;
};

//=========================================================================
// CSessionManager - 전역 세션 레지스트리 (포인터만 관리)
//=========================================================================
class CSessionManager
{
public:
    // [추가] 연결 상태 통계 구조체
    struct FConnectionStats
    {
        size_t TotalSessions;       // 전체 등록된 세션 수
        size_t ConnectedCount;      // 실제 연결된 세션 수 (Active)
        size_t WaitingCount;        // 재접속 대기 중인 세션 수 (TempDisconnect)
    };
    struct FReconnectStats
    {
        uint64_t SuccessCount;          // 성공 횟수
        uint64_t Fail_NotFound;         // 실패: 세션 없음 (만료됨)
        uint64_t Fail_InvalidState;     // 실패: 상태 오류 (TempDisconnect 아님)
        uint64_t Fail_TokenMismatch;    // 실패: 토큰 불일치 (해킹/버그)
        uint64_t Fail_Expired;          // 실패: 타이머 만료
    };
    static CSessionManager& Instance();

    // 세션 등록/해제 (실제 객체는 LocalThread가 소유)
    bool RegisterSession(CGameSession* session);
    void UnregisterSession(SessionId id);

    // 조회
    CGameSession* FindSession(SessionId id);
    CGameSession* FindSessionByAccountId(AccountId account_id);
    // [수정] USN으로 찾고 + 토큰 검증까지 수행하는 통합 함수
    CGameSession* FindSessionByUsn(uint64_t usn, uint64_t token, bool b_reconnect_attempt);
    // 통계
    size_t GetActiveSessionCount() const { return ActiveCount.load(std::memory_order_relaxed); }
    // [추가] 상세 연결 상태 통계 조회
    FConnectionStats GetConnectionStats() const;
    // [추가] 통계 가져오기
    FReconnectStats GetReconnectStats() const;
private:
    CSessionManager();
    ~CSessionManager() = default;

    CSessionManager(const CSessionManager&) = delete;
    CSessionManager& operator=(const CSessionManager&) = delete;

    size_t HashSessionId(SessionId id) const { return id % MAX_TOTAL_SESSIONS; }

private:
    // 포인터 테이블만 유지 (실제 객체 X)
    CGameSession* SessionTable_[MAX_TOTAL_SESSIONS];
    std::atomic<bool> SlotUsed_[MAX_TOTAL_SESSIONS];
    std::atomic<size_t> ActiveCount;
    std::atomic<uint64_t> RecSuccess;
    std::atomic<uint64_t> RecFail_NotFound;
    std::atomic<uint64_t> RecFail_InvalidState;
    std::atomic<uint64_t> RecFail_TokenMismatch;
    std::atomic<uint64_t> RecFail_Expired;
};
