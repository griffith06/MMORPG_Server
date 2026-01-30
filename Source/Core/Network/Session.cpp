#include "Session.h"
#include "Socket.h"
#include <cstring>
#include <chrono>


//=========================================================================
// Helper
//=========================================================================
static uint64_t GetCurrentTimeMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

//=========================================================================
// CGameSession
//=========================================================================
CGameSession::CGameSession()
    : OwnerSessionId(0)
    , OwnerAccountId(0)
    , OwnerThreadId(0)
    , OwnerMapId(0)
    , State(ESessionState::None)
    , pSocket(nullptr)
    , DisconnectTime(0)
    , LastActiveTime(0)
{
    ReconnectToken.Token = 0;
    ReconnectToken.Timestamp = 0;
}

CGameSession::~CGameSession()
{
    Reset();
}

void CGameSession::Initialize(SessionId id, ThreadId owner_thread_id)
{
    OwnerSessionId = id;
    OwnerThreadId = owner_thread_id;
    OwnerAccountId = 0;
    OwnerMapId = 0;
    pSocket = nullptr;
    DisconnectTime = 0;
    LastActiveTime = GetCurrentTimeMs();

    ReconnectToken.Token = GenerateReconnectToken();
    ReconnectToken.Timestamp = LastActiveTime;

    State.store(ESessionState::Connecting, std::memory_order_release);
}

void CGameSession::Reset()
{
    if (pSocket)
    {
        pSocket->UnbindSession();
        pSocket = nullptr;
    }

    // IncomingQueue 비우기 (버퍼 반환)
    FPacketBuffer* packet = nullptr;
    while (IncomingQueue.Pop(packet))
    {
        if (packet)
        {
            CPacketBufferPool::Instance().Release(packet);
        }
    }

    OwnerSessionId = 0;
    OwnerAccountId = 0;
    OwnerThreadId = 0;
    OwnerMapId = 0;
    DisconnectTime = 0;
    LastActiveTime = 0;
    ReconnectToken.Token = 0;
    ReconnectToken.Timestamp = 0;

    State.store(ESessionState::None, std::memory_order_release);
}

bool CGameSession::BindSocket(CSocket* socket)
{
    if (pSocket)
        return false;

    pSocket = socket;
    pSocket->BindSession(this);
    DisconnectTime = 0;
    UpdateLastActiveTime();

    return true;
}

void CGameSession::UnbindSocket()
{
    if (pSocket)
    {
        pSocket->UnbindSession();
        pSocket = nullptr;
    }
}

bool CGameSession::ValidateReconnectToken(uint64_t token) const
{
    return ReconnectToken.Token == token;
}

void CGameSession::StartDisconnectTimer()
{
    DisconnectTime = GetCurrentTimeMs();
    SetState(ESessionState::TempDisconnect);
}

bool CGameSession::IsDisconnectTimerExpired() const
{
    if (DisconnectTime == 0)
        return false;

    uint64_t now = GetCurrentTimeMs();
    uint64_t elapsed = now - DisconnectTime;
    return elapsed >= (RECONNECT_TIMEOUT_SEC * 1000);
}

void CGameSession::OnPacketReceived(FPacketBuffer* packet)
{
    if (State.load(std::memory_order_acquire) != ESessionState::Active)
    {
        CPacketBufferPool::Instance().Release(packet);
        return;
    }

    if (!IncomingQueue.Push(packet))
    {
        // 큐가 가득 참 - 버퍼 반환
        CPacketBufferPool::Instance().Release(packet);
    }

    UpdateLastActiveTime();
}

void CGameSession::OnSocketDisconnected()
{
    CSocket* deadSocket = pSocket;
    UnbindSocket();
    // [중요] 연결이 끊긴 소켓 객체(CSocket)를 메모리에서 해제해야 합니다.
    // 현재 우리는 IO 스레드(CSocket 내부 함수)에서 이 코드를 실행 중일 수 있으므로, 
    // 여기서 즉시 'delete deadSocket'을 하면 오류가 날 위험이 있습니다.
    // 따라서 IO 컨텍스트(작업큐)에 "삭제 작업"을 예약(Post)하여, 
    // 현재 실행 중인 핸들러가 끝난 직후에 안전하게 소멸되도록 합니다.
    if (deadSocket)
    {
        auto& executor = deadSocket->GetRawSocket().get_executor();
        asio::post(executor, [deadSocket]() {
            CSocketPool::Instance().DestroySocket(deadSocket);
            });
    }
    // [수정] 상태에 따른 분기 처리 (세션 누수 방지 핵심 로직)
    ESessionState currentState = State.load(std::memory_order_acquire);
    // 이미 로그인 성공해서 게임 중이거나, 잠시 끊겼던 유저라면 -> 재접속 대기
    if (currentState == ESessionState::Active || currentState == ESessionState::TempDisconnect)
    {
        StartDisconnectTimer();
    }
    else
    {
        // [중요] 로그인도 안 한 놈(Connecting, None)이 끊겼다? -> 가차 없이 삭제!
        // 기존에는 무조건 StartDisconnectTimer()를 호출해서 좀비가 쌓였음.
        // Closed로 바꾸면 LocalThread::ProcessSessions()가 수거해감.
        SetState(ESessionState::Closed);
    }
}

bool CGameSession::PopIncomingPacket(FPacketBuffer*& out_packet)
{
    return IncomingQueue.Pop(out_packet);
}

bool CGameSession::Send(FPacketBuffer* packet)
{
    // [수정] 소켓이 없거나 연결 상태가 아니면 즉시 폐기 (메모리 누수 방지)
    if (!pSocket || !pSocket->IsConnected())
    {
        CPacketBufferPool::Instance().Release(packet);
        return false;
    }
    if (!pSocket->Send(packet))
    {
        CPacketBufferPool::Instance().Release(packet);
		return false;
    }
    return true;
}
/*
bool CGameSession::Send(ProtocolId protocol_id, const void* data, size_t size)
{
    if (!pSocket || size + PACKET_HEADER_SIZE > MAX_PACKET_SIZE)
        return false;

    FPacketBuffer* packet = CPacketBufferPool::Instance().Acquire();
    if (!packet)
        return false;

    FPacketHeader header;
    header.Size = static_cast<uint16_t>(size + PACKET_HEADER_SIZE);
    header.ProtocolId = protocol_id;

    std::memcpy(packet->Data, &header, PACKET_HEADER_SIZE);
    std::memcpy(packet->Data + PACKET_HEADER_SIZE, data, size);
    packet->Size = static_cast<uint16_t>(size + PACKET_HEADER_SIZE);
    packet->ProtocolID = protocol_id;
    packet->SessionID = SessionId;

    if (!pSocket->Send(packet))
    {
        CPacketBufferPool::Instance().Release(packet);
        return false;
    }
    return true;
}
*/

bool CGameSession::Send(ProtocolId protocol_id, const void* data, size_t size)
{
    // [수정] Method B: 입력된 data가 이미 [Header + Body]를 포함하고 있다고 가정합니다.
    // 따라서 size는 전체 패킷 크기입니다.

    // 1. 크기 체크 (헤더를 더하지 않고 size 자체로 비교)
    if (!pSocket || size > MAX_PACKET_SIZE)
        return false;

    FPacketBuffer* packet = CPacketBufferPool::Instance().Acquire();
    if (!packet)
        return false;

    // 2. [핵심 변경] 헤더를 따로 만들지 않고, data를 통째로 복사합니다.
    // 기존: memcpy(Header) + memcpy(Body)
    // 변경: memcpy(Total)
    std::memcpy(packet->Data, data, size);
    FPacketHeader* header = reinterpret_cast<FPacketHeader*>(packet->Data);
    header->ProtocolId = protocol_id;
    header->Size = static_cast<uint16_t>(size);

    // 3. 패킷 메타데이터 설정
    packet->Size = static_cast<uint16_t>(size);
    packet->ProtocolID = protocol_id;
    packet->SessionID = OwnerSessionId;

    // 4. 전송
    if (!pSocket->Send(packet))
    {
        CPacketBufferPool::Instance().Release(packet);
        return false;
    }
    return true;
}
void CGameSession::SetState(ESessionState new_state)
{
    State.store(new_state, std::memory_order_release);
}

bool CGameSession::TransitionState(ESessionState expected, ESessionState new_state)
{
    return State.compare_exchange_strong(expected, new_state,
        std::memory_order_acq_rel, std::memory_order_relaxed);
}

void CGameSession::UpdateLastActiveTime()
{
    LastActiveTime = GetCurrentTimeMs();
}

//=========================================================================
// CSessionManager
//=========================================================================
CSessionManager::CSessionManager()
    : ActiveCount(0)
    , RecSuccess(0)
    , RecFail_NotFound(0)
    , RecFail_InvalidState(0)
    , RecFail_TokenMismatch(0)
    , RecFail_Expired(0)
{
    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        SessionTable_[i] = nullptr;
        SlotUsed_[i].store(false, std::memory_order_relaxed);
    }
}
// [추가] 통계 조회 구현
CSessionManager::FReconnectStats CSessionManager::GetReconnectStats() const
{
    FReconnectStats stats;
    stats.SuccessCount = RecSuccess.load(std::memory_order_relaxed);
    stats.Fail_NotFound = RecFail_NotFound.load(std::memory_order_relaxed);
    stats.Fail_InvalidState = RecFail_InvalidState.load(std::memory_order_relaxed);
    stats.Fail_TokenMismatch = RecFail_TokenMismatch.load(std::memory_order_relaxed);
    stats.Fail_Expired = RecFail_Expired.load(std::memory_order_relaxed);
    return stats;
}
CSessionManager& CSessionManager::Instance()
{
    static CSessionManager instance;
    return instance;
}

bool CSessionManager::RegisterSession(CGameSession* session)
{
    if (!session)
        return false;

    SessionId id = session->GetSessionId();
    size_t start_index = HashSessionId(id);

    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        size_t index = (start_index + i) % MAX_TOTAL_SESSIONS;

        bool expected = false;
        if (SlotUsed_[index].compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            SessionTable_[index] = session;
            ActiveCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    return false;
}

void CSessionManager::UnregisterSession(SessionId id)
{
    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        if (SlotUsed_[i].load(std::memory_order_acquire) &&
            SessionTable_[i] && SessionTable_[i]->GetSessionId() == id)
        {
            SessionTable_[i] = nullptr;
            SlotUsed_[i].store(false, std::memory_order_release);
            ActiveCount.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
    }
}

CGameSession* CSessionManager::FindSession(SessionId id)
{
    size_t hash_index = HashSessionId(id);
    if (SlotUsed_[hash_index].load(std::memory_order_acquire) &&
        SessionTable_[hash_index] && SessionTable_[hash_index]->GetSessionId() == id)
    {
        return SessionTable_[hash_index];
    }

    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        if (SlotUsed_[i].load(std::memory_order_acquire) &&
            SessionTable_[i] && SessionTable_[i]->GetSessionId() == id)
        {
            return SessionTable_[i];
        }
    }

    return nullptr;
}

CGameSession* CSessionManager::FindSessionByAccountId(AccountId account_id)
{
    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        if (SlotUsed_[i].load(std::memory_order_acquire) &&
            SessionTable_[i] && SessionTable_[i]->GetAccountId() == account_id)
        {
            return SessionTable_[i];
        }
    }

    return nullptr;
}

CSessionManager::FConnectionStats CSessionManager::GetConnectionStats() const
{
    FConnectionStats stats = { 0, 0, 0 };

    // 전체 테이블 순회하며 상태 카운팅
    // 주의: 락을 걸지 않고 순회하므로 완벽하게 정확한 스냅샷은 아닐 수 있음 (근사치)
    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        // 슬롯이 사용 중인지 확인 (atomic load)
        if (SlotUsed_[i].load(std::memory_order_acquire))
        {
            CGameSession* session = SessionTable_[i];
            if (session)
            {
                stats.TotalSessions++;
                    
                // 세션 상태 확인
                ESessionState state = session->GetState();
                if (state == ESessionState::Active || state == ESessionState::Connecting)
                {
                    stats.ConnectedCount++;
                }
                else if (state == ESessionState::TempDisconnect)
                {
                    stats.WaitingCount++;
                }
            }
        }
    }
    return stats;
}
// [수정] USN 검색 + 리커넥트 검증 통합 함수
CGameSession* CSessionManager::FindSessionByUsn(uint64_t usn, uint64_t token, bool b_reconnect_attempt)
{
    if (usn == 0) return nullptr;

    CGameSession* targetSession = nullptr;

    // 1. USN으로 세션 검색 (Linear Search)
    // (최적화를 위해선 별도의 USN Map이 필요하지만, 현재 구조 유지)
    for (size_t i = 0; i < MAX_TOTAL_SESSIONS; ++i)
    {
        // 슬롯이 사용 중인지 확인
        if (SlotUsed_[i].load(std::memory_order_acquire))
        {
            CGameSession* session = SessionTable_[i];
            // 세션이 유효하고 USN이 일치하는지 확인
            if (session && session->GetUsn() == usn)
            {
                targetSession = session;
                break; // 찾았으니 루프 중단
            }
        }
    }

    // 2. 검증 로직 (기존 FindSessionForReconnect의 로직 이식)
    // [Case 1] 세션을 못 찾음
    if (!targetSession)
    {
        // 토큰을 들고 왔는데(isReconnectAttempt=true) 서버에 없다면 -> 진짜 실패(만료됨/서버재부팅)
        if (b_reconnect_attempt)
        {
            RecFail_NotFound.fetch_add(1, std::memory_order_relaxed);
            printf("[ReconnectFail] USN[%llu] Not Found (Session Expired or Server Restarted)\n", usn);
        }
        // 토큰 없이(0) 왔다면 신규 접속이므로, 못 찾는 게 당연함 -> 조용히 리턴
        return nullptr;
    }

    // [Case 2] 상태가 TempDisconnect(대기중)가 아님(즉, Active 상태)
    if (targetSession->GetState() != ESessionState::TempDisconnect)
    {
        // 봇 테스트 중 강제 뺏기를 위해 여기서는 return nullptr 하지 않고
            // 통계만 남기고 넘어가거나, LocalThread에서 처리하도록 할 수 있음.
            // 여기선 원칙대로 로그만 남기고 일단 리턴 (LocalThread가 Active 처리 로직이 있다면 수정 필요)

            // 만약 LocalThread에서 "Active라도 뺏겠다"는 로직이 있다면 여기서 nullptr 리턴하면 안 됨.
            // 하지만 일반적으로 FindSessionByUsn은 "유효한 복구 대상"을 찾는 것이므로
            // Active 상태는 복구 대상이 아님 -> 실패 처리하는 게 맞음.
            // (단, 봇 테스트의 Hijack을 위해선 이 체크를 LocalThread로 넘기거나 완화해야 함)
        // ★★★ [가장 중요한 부분] 소켓 연결을 끊어야 IO가 취소되고 메모리가 해제됨 ★★★
        CSocket* oldSocket = targetSession->GetSocket();
        if (oldSocket)
            oldSocket->Close();
        RecFail_InvalidState.fetch_add(1, std::memory_order_relaxed);
        // [비정상 로그] 이미 접속 중이거나, 로직 오류
        printf("[ReconnectFail] USN[%llu] Invalid State: %d (Not TempDisconnect)\n",
            usn, (int)targetSession->GetState());
        // LocalThread가 수거해가도록 Closed로 변경
        targetSession->SetState(ESessionState::Closed);
        UnregisterSession(targetSession->GetSessionId());
        return nullptr;
        // -----------------------------------------------------------
        // [중복 접속 처리 로직]  봇테스트가 아닐때는 아래처럼 처리.
        // -----------------------------------------------------------
/*
        // 1. 기존 유저에게 "너 쫓겨남" 패킷 전송
        FKickPacket kickPkt;
        kickPkt.Header.Size = sizeof(FKickPacket);
        kickPkt.Header.ProtocolId = PKT_KICK;
        kickPkt.Reason = KICK_DUPLICATE_LOGIN;
        targetSession->Send(PKT_KICK, &kickPkt, sizeof(kickPkt));

        // 2. 기존 유저 연결 끊기 (세션 종료 프로세스 시작)
        // 즉시 Unbind 하기보다는, OnSocketDisconnected를 호출하거나
        // Disconnect 플래그를 세워서 자연스럽게 정리되도록 유도
        CSocket* oldSocket = targetSession->GetSocket();
        if (oldSocket)
        {
            oldSocket->Disconnect(); // 소켓 레벨에서 연결 종료
        }

        // 3. 로그 남기기
        printf("[Server] KICK DUPLICATE SESSION: USN[%llu] Session[%llu]\n",
            usn, targetSession->GetSessionId());

        // 4. 새 유저에게는 일단 "실패"를 리턴해서 대기하게 함
        return nullptr;
*/
    }

    // [Case 3] 토큰 불일치 (보안 체크)
    // targetSession->GetReconnectToken().Token : 서버가 저장해둔 토큰
    // token : 클라가 보내준 토큰
    // 봇 테스트 등에서 토큰 체크를 끄고 싶다면 이 부분을 주석 처리하거나 token == 0 일 때 패스하도록 수정 가능
    if (!targetSession->ValidateReconnectToken(token))
    {
        CSocket* oldSocket = targetSession->GetSocket();
        if (oldSocket) 
            oldSocket->Close();
        RecFail_TokenMismatch.fetch_add(1, std::memory_order_relaxed);
        printf("[ReconnectFail] USN[%llu] Token Mismatch! Server:%llu vs Client:%llu\n",
            usn, targetSession->GetReconnectToken().Token, token);
        targetSession->SetState(ESessionState::Closed);
        UnregisterSession(targetSession->GetSessionId());
        return nullptr;
    }

    // [Case 4] 유효기간 만료 (시간 초과)
    if (targetSession->IsDisconnectTimerExpired())
    {
        CSocket* oldSocket = targetSession->GetSocket();
        if (oldSocket) 
            oldSocket->Close();
        RecFail_Expired.fetch_add(1, std::memory_order_relaxed);
        printf("[ReconnectFail] USN[%llu] Token Expired.\n", usn);
        targetSession->SetState(ESessionState::Closed);
        UnregisterSession(targetSession->GetSessionId());
        return nullptr;
    }

    // [성공] 모든 검증 통과
    RecSuccess.fetch_add(1, std::memory_order_relaxed);

    // 로그가 너무 많으면 주석 처리
    printf("[ReconnectSuccess] USN[%llu] Found & Restoring...\n", usn);

    return targetSession;
}
