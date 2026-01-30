#include "Socket.h"
#include "Session.h"
#include "NetworkMonitor.h"
#include <cstring>

std::atomic<bool> g_bNetworkStallTest(false);


//=========================================================================
// CSocket
//=========================================================================
CSocket::CSocket(asio::io_context& io_context, asio::ip::tcp::socket socket)
    : Socket(std::move(socket))
    , Strand(asio::make_strand(io_context))
    , RecvBuffer(RECV_BUFFER_SIZE)
    , CurrentBatchSize(0)
    , CurrentSendIndex(0)
    , pSession(nullptr)
    , SessionId(0)
    , State(EState::None)
    , bIsSending(false)
{
    asio::error_code ec;
    RemoteEndpoint = Socket.remote_endpoint(ec);

    for (size_t i = 0; i < SEND_BATCH_SIZE; ++i)
    {
        SendBatch[i] = nullptr;
    }
}

CSocket::~CSocket()
{
    Close();

    // 남은 SendBatch 정리
    for (size_t i = CurrentSendIndex; i < CurrentBatchSize; ++i)
    {
        if (SendBatch[i])
        {
            CPacketBufferPool::Instance().Release(SendBatch[i]);
            SendBatch[i] = nullptr;
        }
    }

    // SendQueue_ 정리
    FPacketBuffer* packet = nullptr;
    while (SendQueue.Pop(packet))
    {
        CPacketBufferPool::Instance().Release(packet);
    }
}

void CSocket::Start()
{
    State.store(EState::Connected, std::memory_order_release);
    DoRead();
}

void CSocket::Close()
{
    EState expected = EState::Connected;
    bool bTransitionSuccess = State.compare_exchange_strong(expected, EState::Closing,
        std::memory_order_acq_rel, std::memory_order_relaxed);

    // 2. 실패했고(Connected 아님), 현재 상태가 Sending이라면 -> Closing 전환 재시도
    if (!bTransitionSuccess && expected == EState::Sending)
    {
        // [중요 변경] Sending 상태일 때도 CAS를 사용하여 중복 진입 방지
        bTransitionSuccess = State.compare_exchange_strong(expected, EState::Closing,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    // 3. 여전히 실패했다면? (이미 Closing이거나 Closed 상태임) -> 그냥 종료
    if (!bTransitionSuccess)
    {
        return;
    }

    // ============================================================
    // [메모리 누수 수정] Close 시점에 큐에 쌓인 패킷 즉시 반환
    // ============================================================
    // 1. SendBatch(보내려던 뭉치) 비우기
    for (size_t i = 0; i < SEND_BATCH_SIZE; ++i)
    {
        if (SendBatch[i])
        {
            CPacketBufferPool::Instance().Release(SendBatch[i]);
            SendBatch[i] = nullptr;
        }
    }
    // 2. SendQueue(대기열) 비우기
    FPacketBuffer* packet = nullptr;
    while (SendQueue.Pop(packet))
    {
        if (packet)
        {
            CPacketBufferPool::Instance().Release(packet);
        }
    }
    // ============================================================
    // ============================================================
    // [진입 장벽 통과] 
    // 이 지점은 소켓 생명주기 동안 오직 하나의 스레드만, 딱 한 번 도달합니다.
    // ============================================================

    // [모니터링] 연결 해제 감지 (여기서 호출해야 정확함)
    CNetworkMonitor::Instance().OnDisconnect();

    // 실제 소켓 닫기
    asio::error_code ec;
    Socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    Socket.close(ec);

    // 최종 상태 Closed로 변경
    State.store(EState::Closed, std::memory_order_release);
}

bool CSocket::IsConnected() const
{
    EState s = State.load(std::memory_order_acquire);
    return s == EState::Connected || s == EState::Sending;
}

void CSocket::BindSession(CGameSession* session)
{
    pSession = session;
    if (pSession)
    {
        SessionId = pSession->GetSessionId();
    }
}

void CSocket::UnbindSession()
{
    pSession = nullptr;
    SessionId = 0;
}

bool CSocket::Send(FPacketBuffer* packet)
{
    if (!IsConnected() || !packet)
        return false;

    // [수정됨] 동적 제한값 가져오기
    size_t current_limit = CNetworkMonitor::Instance().GetCurrentSendQueueLimit();
    // 1. 큐 크기 제한 (Backpressure)
    // 큐가 너무 많이 쌓였다는 건, 클라가 못 받고 있거나(Disconnect 전조),
    // 서버가 너무 많이 보내고 있다는 뜻입니다. 과감히 끊습니다.
    if (SendQueue.ApproximateSize() >= current_limit)
    {
        // 로그를 남겨두면 좋습니다.
        printf("Error : Session[%llu] SendQueue Overflow (%zu/%zu). Disconnecting.\n", SessionId, SendQueue.ApproximateSize(), current_limit);
        Close(); // 소켓 연결 종료
        return false; // 전송 실패 반환 -> 호출자가 메모리 해제해야 함
    }

    // 큐 삽입 시도 및 실패 처리
    // 2. LockFreeQueue의 구현에 따라 Push가 실패(false)를 반환한다면 이를 반드시 체크해야 함
    if (!SendQueue.Push(packet))
    {
        // 물리적으로 큐가 꽉 참 -> 논리적 한계보다 큐 용량이 작거나, 큐가 가득 참
        // 이 경우도 전송 실패로 처리해야 메모리가 안 샘
        printf("Error : Session[%llu] SendQueue Full (Capacity Reached). Disconnecting.\n", SessionId);
        Close();
        return false; // <--- 여기서 false를 리턴해야 Session.cpp의 Release가 작동함!
    }

    // 3. IO 스레드 깨우기 (Push 성공 시에만 수행)
    // 큐가 비어있었는지 확인하는 것보다, 보내는 중이 아니면 깨우는 방식이 더 안전함
    bool expected = false;
    if (bIsSending.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        asio::post(Strand, [this]() { ProcessSendQueue(); });
    }
    return true;
}

void CSocket::ProcessSendQueue()
{
    if (!IsConnected())
    {
        // 큐에 남은 거 싹 비우기 (누수 방지)
        FPacketBuffer* packet = nullptr;
        while (SendQueue.Pop(packet))
        {
            if (packet) CPacketBufferPool::Instance().Release(packet);
        }
        bIsSending.store(false, std::memory_order_release);
        return;
    }
    // ============================================================
    // 네트워크 단절 시뮬레이션 코드
    // ============================================================
    if (g_bNetworkStallTest.load(std::memory_order_relaxed))
    {
        // 보내는 중(bIsSending) 상태는 true로 유지한 채 그냥 리턴합니다.
        // 결과:
        // 1. Send() 함수는 bIsSending이 true니 post를 안 함 (큐에 쌓기만 함).
        // 2. IO 스레드는 async_write를 호출하지 않음 (전송 중단).
        // 3. 결국 SendQueue가 MAX 제한에 도달할 때까지 계속 쌓임.
        return;
    }
    // Batch pop
    CurrentBatchSize = SendQueue.PopBatch(SendBatch, SEND_BATCH_SIZE);
    CurrentSendIndex = 0;

    if (CurrentBatchSize == 0)
    {
        bIsSending.store(false, std::memory_order_release);

        if (!SendQueue.IsEmpty())
        {
            bool expected = false;
            if (bIsSending.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                asio::post(Strand, [this]() { ProcessSendQueue(); });
            }
        }
        return;
    }

    DoWrite();
}

void CSocket::DoRead()
{
    if (!IsConnected())
        return;

    if (RecvBuffer.FreeSize() < MAX_PACKET_SIZE)
    {
        RecvBuffer.Clean();
    }

    Socket.async_read_some(
        asio::buffer(RecvBuffer.GetWritePos(), RecvBuffer.FreeSize()),
        asio::bind_executor(Strand,
            [this](const asio::error_code& ec, size_t bytes_transferred)
            {
                OnReadComplete(ec, bytes_transferred);
            }
        )
    );
}

void CSocket::DoWrite()
{
    if (CurrentSendIndex >= CurrentBatchSize)
    {
        ProcessSendQueue();
        return;
    }

    // 2. [Gathering] 남은 패킷들을 한 번에 묶습니다.
    GatherList.clear();
    for (size_t i = CurrentSendIndex; i < CurrentBatchSize; ++i)
    {
        FPacketBuffer* pkt = SendBatch[i];
        if (pkt)
        {
            // 패킷의 데이터 포인터와 사이즈를 등록
            GatherList.emplace_back(pkt->Data, pkt->Size);
        }
    }

    if (GatherList.empty())
    {
        // 혹시라도 널 포인터만 있었다면 스킵
        CurrentSendIndex = CurrentBatchSize;
        ProcessSendQueue();
        return;
    }

    // 3. [Syscall] 한 번의 호출로 N개의 버퍼를 동시에 전송 요청!
    // async_write는 모든 버퍼가 전송될 때까지 보장합니다 (Partial Write 걱정 없음)
    asio::async_write(
        Socket,
        GatherList,
        asio::bind_executor(Strand,
            [this](const asio::error_code& ec, size_t bytes_transferred)
            {
                OnWriteComplete(ec, bytes_transferred);
            }
        )
    );
}

void CSocket::OnReadComplete(const asio::error_code& ec, size_t bytes_transferred)
{
    if (ec)
    {
        printf("[Socket] Read Error: %s (SessionID: %llu)\n", ec.message().c_str(), SessionId);
        HandleDisconnect(ec);
        return;
    }

    if (!RecvBuffer.OnWrite(bytes_transferred))
    {
        Close();
        return;
    }

    HandlePacket();
    DoRead();
}

void CSocket::OnWriteComplete(const asio::error_code& ec, size_t bytes_transferred)
{
    (void)bytes_transferred;

    // [수정] Gather I/O를 썼으므로, 이번에 보낸 묶음(Batch)들에 해당하는 버퍼를 '모두' 반환해야 함.
    // DoWrite에서 GatherList를 만들 때 CurrentSendIndex_ 부터 끝까지 다 넣었으므로,
    // (성공했다면) 이 구간의 패킷은 모두 전송 완료된 것임.
    // (실패했다면) 어차피 다 반환하고 연결 끊어야 함.

    for (size_t i = CurrentSendIndex; i < CurrentBatchSize; ++i)
    {
        if (SendBatch[i])
        {
            CPacketBufferPool::Instance().Release(SendBatch[i]);
            SendBatch[i] = nullptr;
        }
    }

    // 에러 처리
    if (ec)
    {
        // 이미 위에서 버퍼는 다 반환했으므로, 전송 플래그 끄고 연결 종료 처리만 하면 됨
        bIsSending.store(false, std::memory_order_release);
        HandleDisconnect(ec);
        return;
    }

    // [중요] 인덱스를 끝으로 이동 (Batch 전체를 다 보냈으므로)
    CurrentSendIndex = CurrentBatchSize;

    // 다음 배치 가지러 가기 (ProcessSendQueue 내부에서 큐 검사 후 종료 or 계속)
    DoWrite();
}
void CSocket::HandlePacket()
{
    while (RecvBuffer.DataSize() >= PACKET_HEADER_SIZE)
    {
        FPacketHeader header;
        std::memcpy(&header, RecvBuffer.GetReadPos(), PACKET_HEADER_SIZE);

        if (header.Size > MAX_PACKET_SIZE || header.Size < PACKET_HEADER_SIZE)
        {
            Close();
            return;
        }

        if (RecvBuffer.DataSize() < header.Size)
        {
            break;
        }

        if (pSession)
        {
            FPacketBuffer* packet = CPacketBufferPool::Instance().Acquire();
            if (packet)
            {
                packet->CopyFrom(RecvBuffer.GetReadPos(), header.Size, header.ProtocolId, SessionId);
                pSession->OnPacketReceived(packet);
            }
        }

        RecvBuffer.OnRead(header.Size);
    }
}

void CSocket::HandleDisconnect(const asio::error_code& ec)
{
    (void)ec;

    if (pSession)
    {
        pSession->OnSocketDisconnected();
    }

    Close();
}

//=========================================================================
// CSocketPool
//=========================================================================
CSocketPool& CSocketPool::Instance()
{
    static CSocketPool instance;
    return instance;
}

CSocket* CSocketPool::CreateSocket(asio::io_context& io_context, asio::ip::tcp::socket socket)
{
    return new CSocket(io_context, std::move(socket));
}

void CSocketPool::DestroySocket(CSocket* socket)
{
    delete socket;
}
