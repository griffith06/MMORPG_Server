#pragma once

#include "NetworkFwd.h"
#include "../Common/Types.h"
#include "../Common/Buffer.h"
#include "../Common/LockFreeQueue.h"

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#define ASIO_STANDALONE
//#define ASIO_HAS_STD_ATOMIC
#endif

#include <asio.hpp>
#include <atomic>


//=========================================================================
// CSocket - 물리적 네트워크 연결
// IO Thread의 Strand에서 관리됨
//=========================================================================
class CSocket
{
public:
    enum class EState : uint8_t
    {
        None,
        Connected,
        Sending,
        Closing,
        Closed
    };

public:
    CSocket(asio::io_context& io_context, asio::ip::tcp::socket socket);
    ~CSocket();

    CSocket(const CSocket&) = delete;
    CSocket& operator=(const CSocket&) = delete;

    // Lifecycle
    void Start();
    void Close();
    bool IsConnected() const;

    // Session binding
    void BindSession(CGameSession* session);
    void UnbindSession();
    CGameSession* GetSession() const { return pSession; }
    SessionId GetSessionId() const { return SessionId; }

    // Send interface
    bool Send(FPacketBuffer* packet);

    // Internal
    void ProcessSendQueue();

    // Accessors
    asio::ip::tcp::socket& GetRawSocket() { return Socket; }
    const asio::ip::tcp::endpoint& GetRemoteEndpoint() const { return RemoteEndpoint; }
    EState GetState() const { return State.load(std::memory_order_acquire); }

private:
    void DoRead();
    void DoWrite();
    void HandlePacket();
    void HandleDisconnect(const asio::error_code& ec);

    void OnReadComplete(const asio::error_code& ec, size_t bytes_transferred);
    void OnWriteComplete(const asio::error_code& ec, size_t bytes_transferred);

private:
    asio::ip::tcp::socket Socket;
    asio::strand<asio::io_context::executor_type> Strand;
    asio::ip::tcp::endpoint RemoteEndpoint;

    CRecvBuffer RecvBuffer;
    MPSCQueue<FPacketBuffer*, SEND_QUEUE_CAPACITY> SendQueue;
    FPacketBuffer* SendBatch[SEND_BATCH_SIZE];
    size_t CurrentBatchSize;
    size_t CurrentSendIndex;

    CGameSession* pSession;
    SessionId SessionId;

    std::atomic<EState> State;
    std::atomic<bool> bIsSending;
    // Gathering I / O
    // 32개의 패킷을 1번의 시스템 콜로 CPU 병목을 획기적으로 줄임.
    // 이게 없었다면 10만~15만 PPS에서 CPU가 못버팀.
    std::vector<asio::const_buffer> GatherList;
};

//=========================================================================
// CSocketPool - 소켓 생성/소멸 관리
//=========================================================================
//class alignas(64) CSocketPool
class CSocketPool
{
public:
    static CSocketPool& Instance();

    CSocket* CreateSocket(asio::io_context& io_context, asio::ip::tcp::socket socket);
    void DestroySocket(CSocket* socket);

private:
    CSocketPool() = default;
    ~CSocketPool() = default;
};

// [전역 변수] 네트워크 단절 시뮬레이션 플래그
extern std::atomic<bool> g_bNetworkStallTest;
