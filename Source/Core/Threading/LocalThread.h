#pragma once

#include "../Network/NetworkFwd.h"
#include "../Common/Types.h"
#include "../Common/LockFreeQueue.h"
#include "../Common/Buffer.h"
#include "../Network/Session.h"

#include <thread>
#include <atomic>


//=========================================================================
// CLocalThread - 게임 로직 스레드
// 세션 풀을 직접 소유하여 Lock-free 처리
//=========================================================================
class CLocalThread
{
public:
    static constexpr size_t COMMAND_QUEUE_CAPACITY = 1024;
    static constexpr uint32_t TICK_INTERVAL_MS = 16;

    enum class ECommandType : uint8_t
    {
        None,
        CreateSession,
        RemoveSession,
        Broadcast,
        Shutdown
    };

    struct FCommand
    {
        ECommandType Type;
        SessionId SessionId;
        MapId MapId;
        CSocket* pSocket;
        FPacketBuffer* pPacket;

        FCommand()
            : Type(ECommandType::None)
            , SessionId(0)
            , MapId(0)
            , pSocket(nullptr)
            , pPacket(nullptr)
        {}
    };

    struct FStats
    {
        uint64_t TickCount;
        uint64_t TotalTickTimeUs;
        uint64_t MaxTickTimeUs;
        uint64_t PacketsProcessed;
        uint64_t PacketsSent;
    };

public:
    CLocalThread();
    ~CLocalThread();

    CLocalThread(const CLocalThread&) = delete;
    CLocalThread& operator=(const CLocalThread&) = delete;

    bool Start(ThreadId thread_id);
    void Stop();
    bool IsRunning() const { return bIsRunning.load(std::memory_order_acquire); }

    ThreadId GetThreadId() const { return OwnerThreadId; }

    // 외부 스레드에서 호출 (Command Queue 통해 전달)
    bool PostCommand(const FCommand& command);
    bool PostCreateSession(CSocket* socket);
    bool PostRemoveSession(SessionId session_id);
    bool PostBroadcast(MapId map_id, FPacketBuffer* packet, SessionId exclude_session_id = 0);

    // 세션 정보
    size_t GetSessionCount() const { return SessionCount; }
    size_t GetMaxSessions() const { return MAX_SESSIONS_PER_THREAD; }

    FStats GetStats() const;
    void ResetStats();

private:
    static void ThreadFunc(CLocalThread* self);

    void ProcessTick();
    void ProcessCommands();
    void ProcessSessions();
    void ProcessSessionPackets(CGameSession* session);
    void HandlePacket(CGameSession* session, FPacketBuffer* packet);

    // Command handlers
    CGameSession* CreateSession(CSocket* socket);
    void RemoveSession(SessionId session_id);
    void HandleBroadcast(MapId map_id, FPacketBuffer* packet, SessionId exclude_session_id);

    // 세션 풀에서 빈 슬롯 찾기
    CGameSession* AllocateSession();
    void FreeSession(CGameSession* session);

private:
    ThreadId OwnerThreadId;
    std::thread Thread;
    std::atomic<bool> bIsRunning;

    // 세션 풀 - 이 스레드가 직접 소유 (약 12KB * 2500 = 30MB)
    CGameSession SessionPool[MAX_SESSIONS_PER_THREAD];
    bool SessionUsed[MAX_SESSIONS_PER_THREAD];
    size_t SessionCount;

    // 활성 세션 포인터 배열 (빠른 순회용)
    CGameSession* ActiveSessions[MAX_SESSIONS_PER_THREAD];
    size_t ActiveSessionCount;

    // Command Queue
    MPSCQueue<FCommand, COMMAND_QUEUE_CAPACITY> CommandQueue;

    // Statistics
    std::atomic<uint64_t> TickCount;
    std::atomic<uint64_t> TotalTickTimeUs;
    std::atomic<uint64_t> MaxTickTimeUs;
    std::atomic<uint64_t> PacketsProcessed;
    std::atomic<uint64_t> PacketsSent;
};
