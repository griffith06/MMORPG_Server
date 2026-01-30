#include "LocalThread.h"
#include "../Network/Socket.h"
#include "../Common/Protocol.h" // <--- 여기 추가!
#include <chrono>
#include <cstring> // memcpy

#ifdef _WIN32
#include <Windows.h>
#endif


static uint64_t GetMicroseconds()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()
    ).count();
}

//=========================================================================
// CLocalThread
//=========================================================================
CLocalThread::CLocalThread()
    : OwnerThreadId(0)
    , bIsRunning(false)
    , SessionCount(0)
    , ActiveSessionCount(0)
    , TickCount(0)
    , TotalTickTimeUs(0)
    , MaxTickTimeUs(0)
    , PacketsProcessed(0)
    , PacketsSent(0)
{
    for (size_t i = 0; i < MAX_SESSIONS_PER_THREAD; ++i)
    {
        SessionUsed[i] = false;
        ActiveSessions[i] = nullptr;
    }
}

CLocalThread::~CLocalThread()
{
    Stop();
}

bool CLocalThread::Start(ThreadId thread_id)
{
    if (bIsRunning.load(std::memory_order_acquire))
        return true;

    OwnerThreadId = thread_id;
    bIsRunning.store(true, std::memory_order_release);

    Thread = std::thread(ThreadFunc, this);

    return true;
}

void CLocalThread::Stop()
{
    if (!bIsRunning.exchange(false, std::memory_order_acq_rel))
        return;

    FCommand cmd;
    cmd.Type = ECommandType::Shutdown;
    CommandQueue.Push(cmd);

    if (Thread.joinable())
    {
        Thread.join();
    }

    // 모든 세션 정리
    for (size_t i = 0; i < MAX_SESSIONS_PER_THREAD; ++i)
    {
        if (SessionUsed[i])
        {
            CSessionManager::Instance().UnregisterSession(SessionPool[i].GetSessionId());
            SessionPool[i].Reset();
            SessionUsed[i] = false;
        }
    }
    SessionCount = 0;
    ActiveSessionCount = 0;
}

bool CLocalThread::PostCommand(const FCommand& command)
{
    return CommandQueue.Push(command);
}

bool CLocalThread::PostCreateSession(CSocket* socket)
{
    FCommand cmd;
    cmd.Type = ECommandType::CreateSession;
    cmd.pSocket = socket;
    return CommandQueue.Push(cmd);
}

bool CLocalThread::PostRemoveSession(SessionId session_id)
{
    FCommand cmd;
    cmd.Type = ECommandType::RemoveSession;
    cmd.SessionId = session_id;
    return CommandQueue.Push(cmd);
}

bool CLocalThread::PostBroadcast(MapId map_id, FPacketBuffer* packet, SessionId exclude_session_id)
{
    FCommand cmd;
    cmd.Type = ECommandType::Broadcast;
    cmd.MapId = map_id;
    cmd.SessionId = exclude_session_id;
    cmd.pPacket = packet;
    return CommandQueue.Push(cmd);
}

CLocalThread::FStats CLocalThread::GetStats() const
{
    FStats stats;
    stats.TickCount = TickCount.load(std::memory_order_relaxed);
    stats.TotalTickTimeUs = TotalTickTimeUs.load(std::memory_order_relaxed);
    stats.MaxTickTimeUs = MaxTickTimeUs.load(std::memory_order_relaxed);
    stats.PacketsProcessed = PacketsProcessed.load(std::memory_order_relaxed);
    stats.PacketsSent = PacketsSent.load(std::memory_order_relaxed);
    return stats;
}

void CLocalThread::ResetStats()
{
    TickCount.store(0, std::memory_order_relaxed);
    TotalTickTimeUs.store(0, std::memory_order_relaxed);
    MaxTickTimeUs.store(0, std::memory_order_relaxed);
    PacketsProcessed.store(0, std::memory_order_relaxed);
    PacketsSent.store(0, std::memory_order_relaxed);
}

void CLocalThread::ThreadFunc(CLocalThread* self)
{
    using namespace std::chrono;

#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif

    auto next_tick_time = high_resolution_clock::now();

    while (self->bIsRunning.load(std::memory_order_acquire))
    {
        auto tick_start = high_resolution_clock::now();

        self->ProcessTick();

        auto tick_end = high_resolution_clock::now();
        uint64_t tick_time_us = duration_cast<microseconds>(tick_end - tick_start).count();

        self->TickCount.fetch_add(1, std::memory_order_relaxed);
        self->TotalTickTimeUs.fetch_add(tick_time_us, std::memory_order_relaxed);

        uint64_t current_max = self->MaxTickTimeUs.load(std::memory_order_relaxed);
        while (tick_time_us > current_max)
        {
            if (self->MaxTickTimeUs.compare_exchange_weak(current_max, tick_time_us,
                std::memory_order_relaxed, std::memory_order_relaxed))
            {
                break;
            }
        }

        next_tick_time += milliseconds(TICK_INTERVAL_MS);
        auto now = high_resolution_clock::now();
        if (next_tick_time > now)
        {
            std::this_thread::sleep_until(next_tick_time);
        }
        else
        {
            next_tick_time = now;
        }
    }
}

void CLocalThread::ProcessTick()
{
    ProcessCommands();
    ProcessSessions();
}

void CLocalThread::ProcessCommands()
{
    FCommand cmd;
    while (CommandQueue.Pop(cmd))
    {
        switch (cmd.Type)
        {
        case ECommandType::CreateSession:
            CreateSession(cmd.pSocket);
            break;
        case ECommandType::RemoveSession:
            RemoveSession(cmd.SessionId);
            break;
        case ECommandType::Broadcast:
            HandleBroadcast(cmd.MapId, cmd.pPacket, cmd.SessionId);
            break;
        case ECommandType::Shutdown:
            bIsRunning.store(false, std::memory_order_release);
            return;
        default:
            break;
        }
    }
}

void CLocalThread::ProcessSessions()
{
    // 역순으로 순회 (삭제 시 안전)
    for (size_t i = ActiveSessionCount; i > 0; --i)
    {
        CGameSession* session = ActiveSessions[i - 1];
        if (!session)
            continue;

        ESessionState state = session->GetState();

        switch (state)
        {
        case ESessionState::Active:
            ProcessSessionPackets(session);
            break;

        case ESessionState::TempDisconnect:
            if (session->IsDisconnectTimerExpired())
            {
                RemoveSession(session->GetSessionId());
            }
            break;

            // [확인] Session.cpp에서 Closed로 바뀐 세션은 여기서 즉시 수거됨
        case ESessionState::Closed:
        case ESessionState::Disconnecting:
            RemoveSession(session->GetSessionId());
            break;

        default:
            break;
        }
    }
}

void CLocalThread::ProcessSessionPackets(CGameSession* session)
{
    FPacketBuffer* packet = nullptr;
    while (session->PopIncomingPacket(packet))
    {
        if (packet)
        {
            HandlePacket(session, packet);
            PacketsProcessed.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void CLocalThread::HandlePacket(CGameSession* session, FPacketBuffer* packet)
{
    ProtocolId protocol_id = packet->ProtocolID;

    switch (protocol_id)
    {
    case PKT_MOVE: // Move packet - echo/broadcast
        HandleBroadcast(session->GetMapId(), packet, session->GetSessionId());
        return; // packet은 Broadcast에서 처리됨

        // [추가된 부분] 로그인 요청 처리 (ID: 100)
    case PKT_LOGIN_REQ:
    {
        if (packet->Size < sizeof(FLoginPacket))
        {
            printf("[Server] Size Error from Session %llu. \n", session->GetSessionId());
            CPacketBufferPool::Instance().Release(packet);
            return;
        }

        // 1. 패킷 파싱
        FLoginPacket loginReq;
        std::memcpy(&loginReq, packet->Data, sizeof(FLoginPacket));
        //다량로그
        //printf("[Server] LoginReq: USN=%llu (IsReconnect=%d)\n", loginReq.Usn, loginReq.IsReconnect);

        CGameSession* oldSession = nullptr;
        //토큰이 있을 때만 "복구 시도"를 한다.
        if (loginReq.Token > 0)
        {
            // 2. [USN 검색] 서버 메모리에 이 USN을 가진 "죽은 세션"이 있는지 확인
            oldSession = CSessionManager::Instance().FindSessionByUsn(loginReq.Usn, loginReq.Token, true);

            // 리커넥트 가능한 상태인지 확인 (TempDisconnect 상태여야 함)
            if (oldSession && oldSession->GetState() != ESessionState::TempDisconnect)
            {
				// 기존 소켓과 신규소켓이 다르면 기존 소켓 닫기 (세션 탈취 시도) 새 소켓으로 바꿔야 함.
                if (oldSession->GetSocket() && oldSession->GetSocket() != session->GetSocket())
                {
                    CSocket* oldSock = oldSession->GetSocket();

                    // 1. 먼저 세션에서 분리 (그래야 세션이 자유로워짐)
                    oldSession->UnbindSocket();

                    // 2. [누수 수정] 분리된 소켓은 안전하게 삭제 예약
                    auto& executor = oldSock->GetRawSocket().get_executor();
                    asio::post(executor, [oldSock]() {
                        CSocketPool::Instance().DestroySocket(oldSock);
                        });

                    // 3. 소켓 닫기 (이러면 IO 스레드에서 에러가 뜨면서 종료 절차 밟음)
                    oldSock->Close();
                    // printf("[Server] Session Hijack! USN: %llu\n", loginReq.Usn);
                }
            }
        }
        FLoginResponsePacket response;
        std::memset(&response, 0, sizeof(FLoginResponsePacket));
        // =================================================================
        // CASE A: 리커넥트 성공 (기존 세션 복구)
        // =================================================================
        if (oldSession)
        {
            printf("[Server] !!! RECONNECT SUCCESS !!! Restoring USN %llu\n", loginReq.Usn);

            // 1. 소켓 가로채기 (현재 임시 세션의 소켓을 뺏어옴)
            CSocket* socket = session->GetSocket();
            if (socket == nullptr)// IO 스레드에서 연결이 끊겨 소켓이 nullptr 가능성 있음.
            {
                // 소켓이 죽었으므로 복구 불가능. 
                // 패킷 반환하고 조용히 리턴 (어차피 끊긴 유저임)
                printf("[Server] Reconnect Aborted - Socket already closed. USN %llu\n", loginReq.Usn);
                CPacketBufferPool::Instance().Release(packet);
                return;
            }
            // 2. 임시 세션에서 소켓 분리
            session->UnbindSocket();

            // 3. 되살릴 세션(oldSession)에 소켓 장착 & 상태 활성화
            oldSession->BindSocket(socket);
            oldSession->SetState(ESessionState::Active);

            // 4. 응답 구성 (되살아난 세션 정보)
            response.SessionId = oldSession->GetSessionId();
            response.Token = oldSession->GetReconnectToken().Token;
            response.Success = true;

            // 5. 응답 전송 (되살아난 세션으로 보냄)
            oldSession->Send(PKT_LOGIN_RES, &response, sizeof(response));

            // 6. 껍데기만 남은 임시 세션(session)은 삭제 요청
            // (현재 처리 중인 session 객체이므로 즉시 delete하지 말고 큐에 넣어 삭제)
            PostRemoveSession(session->GetSessionId());
        }
        // =================================================================
        // CASE B: 신규 접속 (또는 복구 실패)
        // =================================================================
        else
        {

            /*   리커넥트 처리.
            // [중요] oldSession이 없다고 무조건 New Login으로 가면 안 됩니다.
            // 만약 FindSessionByUsn 내부에서 "Active 세션이라서 Kick 했습니다"라는 신호를 줘야 한다면
            // 리턴값을 enum으로 바꾸거나, out 파라미터를 써야 합니다.
            // 하지만 간단하게 처리하려면:
            // CSessionManager::Instance().IsUserActive(usn) 같은 함수를 하나 더 만들어서
            // "이미 접속 중인가?"를 체크하고, 접속 중이면 에러 패킷을 보냅니다.
            CGameSession* activeSession = CSessionManager::Instance().FindActiveSessionByUsn(loginReq.Usn);
            if (activeSession)
            {
                // 이미 접속 중인데 oldSession(리커넥트 대상)으로는 못 받았다?
                // -> 상태가 꼬였거나 중복 접속임.
                // -> 위에서 Kick을 날렸을 테니, 이 요청자에게는 "잠시 후 다시 시도"를 보냄.

                FLoginFailPacket failPkt;
                failPkt.Header.ProtocolId = PKT_LOGIN_FAIL;
                failPkt.Header.Size = sizeof(failPkt);
                failPkt.Reason = FAIL_ALREADY_CONNECTED;

                session->Send(PKT_LOGIN_FAIL, &failPkt, sizeof(failPkt));
                return;
            }
            */
            //다량로그
            //printf("[Server] New Login: USN %llu\n", loginReq.Usn);

            // 현재 임시 세션을 정식 세션으로 승격
            session->SetUsn(loginReq.Usn); // USN 등록
            session->SetAccountId(loginReq.Usn); // (호환성용)

            response.SessionId = session->GetSessionId();
            response.Token = session->GetReconnectToken().Token;
            response.Success = true;

            session->Send(PKT_LOGIN_RES, &response, sizeof(response));
        }
 
        // 패킷 사용 완료 반환
        CPacketBufferPool::Instance().Release(packet);
        return;
    }

    default:
        break;
    }

    // 사용 완료된 패킷 반환
    CPacketBufferPool::Instance().Release(packet);
}

CGameSession* CLocalThread::CreateSession(CSocket* socket)
{
    if (!socket)
        return nullptr;

    CGameSession* session = AllocateSession();
    if (!session)
    {
        CSocketPool::Instance().DestroySocket(socket);
        return nullptr;
    }

    SessionId id = GenerateSessionId();
    session->Initialize(id, OwnerThreadId);
    session->BindSocket(socket);
    session->SetMapId(1); // Default map
    session->SetState(ESessionState::Active);

    // 전역 레지스트리에 등록
    CSessionManager::Instance().RegisterSession(session);

    // 소켓 시작
    socket->Start();

    return session;
}

void CLocalThread::RemoveSession(SessionId session_id)
{
    for (size_t i = 0; i < ActiveSessionCount; ++i)
    {
        CGameSession* session = ActiveSessions[i];
        if (session && session->GetSessionId() == session_id)
        {
            // 전역 레지스트리에서 제거
            CSessionManager::Instance().UnregisterSession(session_id);

            // 소켓 정리
            CSocket* socket = session->GetSocket();
            if (socket)
            {
                session->UnbindSocket();
                CSocketPool::Instance().DestroySocket(socket);
            }

            // 세션 반환
            FreeSession(session);

            // ActiveSessions 배열에서 제거 (마지막 요소로 대체)
            ActiveSessions[i] = ActiveSessions[ActiveSessionCount - 1];
            ActiveSessions[ActiveSessionCount - 1] = nullptr;
            --ActiveSessionCount;

            return;
        }
    }
}

void CLocalThread::HandleBroadcast(MapId map_id, FPacketBuffer* packet, SessionId exclude_session_id)
{
    if (!packet || ActiveSessionCount == 0)
        return;

    // 랜덤 위치에서 시작하여 부하 분산
    size_t start_index = rand() % ActiveSessionCount;
    size_t send_count = 0;

    for (size_t k = 0; k < ActiveSessionCount; ++k)
    {
        //MAX_BROADCAST_TARGETS(30) 명에게 보냈다면 루프 중단
        if (send_count >= MAX_BROADCAST_TARGETS)
            break;

        // 원형(Circular)으로 순회
        size_t i = (start_index + k) % ActiveSessionCount;
        CGameSession* session = ActiveSessions[i];
        if (!session)
            continue;

        if (session->GetMapId() != map_id)
            continue;

        if (session->GetSessionId() == exclude_session_id)
            continue;

        if (session->GetState() != ESessionState::Active)
            continue;

        // 각 세션에 새 패킷 복사본 전송
        FPacketBuffer* send_packet = CPacketBufferPool::Instance().Acquire();
        if (send_packet)
        {
            send_packet->CopyFrom(packet->Data, packet->Size, packet->ProtocolID, session->GetSessionId());
            if (session->Send(send_packet))
            {
                ++send_count;
            }
        }
    }

    PacketsSent.fetch_add(send_count, std::memory_order_relaxed);

    // 원본 패킷 반환
    CPacketBufferPool::Instance().Release(packet);
}

CGameSession* CLocalThread::AllocateSession()
{
    if (SessionCount >= MAX_SESSIONS_PER_THREAD)
        return nullptr;

    for (size_t i = 0; i < MAX_SESSIONS_PER_THREAD; ++i)
    {
        if (!SessionUsed[i])
        {
            SessionUsed[i] = true;
            ++SessionCount;

            // ActiveSessions에 추가
            ActiveSessions[ActiveSessionCount++] = &SessionPool[i];

            return &SessionPool[i];
        }
    }

    return nullptr;
}

void CLocalThread::FreeSession(CGameSession* session)
{
    if (!session)
        return;

    // 풀 내 인덱스 계산
    ptrdiff_t index = session - SessionPool;
    if (index >= 0 && static_cast<size_t>(index) < MAX_SESSIONS_PER_THREAD)
    {
        session->Reset();
        SessionUsed[index] = false;
        --SessionCount;
    }
}
