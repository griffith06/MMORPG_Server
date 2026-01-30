#include "../Core/Common/Types.h"
#include "../Core/Common/Buffer.h"
#include "../Core/Network/NetworkFwd.h"
#include "../Core/Network/Socket.h"
#include "../Core/Network/Session.h"
#include "../Core/Network/Listener.h"
#include "../Core/Network/NetworkMonitor.h"
#include "../Core/Threading/ThreadManager.h"

#include <cstdio>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <conio.h>

#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")

LONG WINAPI UnhandledExceptionFilterFunc(struct _EXCEPTION_POINTERS* ExceptionInfo)
{
    // 1. 파일 생성 확인
    HANDLE hFile = CreateFile(L"ServerCrash.dmp", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printf("!!! Failed to create dump file. Error: %u\n", GetLastError());
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION M = { 0 };
    M.ThreadId = GetCurrentThreadId();
    M.ExceptionPointers = ExceptionInfo;
    M.ClientPointers = FALSE;

    printf("!!! CRASH DETECTED. Writing dump...\n");

    // 2. 덤프 쓰기 시도 및 결과 확인
    BOOL bSuccess = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpNormal,
        &M,
        NULL,
        NULL);

    if (bSuccess)
    {
        printf("!!! Dump saved successfully to ServerCrash.dmp\n");
    }
    else
    {
        // 여기서 실패 원인을 확인해야 합니다. (예: 2147483645 = E_FAIL 등)
        printf("!!! Failed to write dump. Error Code: %u\n", GetLastError());
    }

    CloseHandle(hFile);

    // 잠시 멈춰서 로그 볼 시간을 줍니다.
    printf("Press ENTER to exit...\n");
    getchar();

    return EXCEPTION_EXECUTE_HANDLER;
}

static std::atomic<bool> g_bRunning(true);

static uint64_t GetCurrentTimeMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

static void SignalHandler(int signum)
{
    (void)signum;
    g_bRunning.store(false, std::memory_order_release);
}

static void OnNewConnection(void* user_data, CSocket* socket)
{
    (void)user_data;

    if (!socket)
        return;

    // ThreadManager가 적절한 LocalThread로 라우팅
    if (!CThreadManager::Instance().RouteNewConnection(socket))
    {
        //넣을 곳이 없다.
        printf("[Server] Connection rejected - server full\n");
        CSocketPool::Instance().DestroySocket(socket);  //메모리 즉시 해제
        return;
    }
    //로그 너무 많아서 잠시끄자        
    //printf("[Server] New connection accepted\n");
}
void PrintPerformanceStats(CListener& listener)
{
    // ... (기존 변수들) ...
    static uint64_t LastAcceptCount = 0;
    static uint64_t LastTime = GetCurrentTimeMs();

    uint64_t curTime = GetCurrentTimeMs();
    double deltaSec = (curTime - LastTime) / 1000.0;
    if (deltaSec < 0.1) return; // 0 division 방지

    // Accept 처리량 계산
    uint64_t curAccept = listener.GetAcceptCount();
    uint64_t acceptDiff = curAccept - LastAcceptCount;
    double acceptPerSec = acceptDiff / deltaSec;

    // ... (기존 출력) ...

    printf(" [ Performance Monitor ]\n");
    printf("----------------------------------------------------------------\n");

    // 1. Accept 병목 확인
    // 위험 수치: Accept Queue가 밀리면 OS단에서 Connection Refused 발생.
    // 안정: 수백~수천/sec 처리 중이면서 RejectCount가 0일 때.
    // 위험: RejectCount 증가 OR AcceptPPS가 0에 가까운데 봇은 연결 실패 중일 때.
    printf(" Accept PPS : %8.2f / sec (Total Rejects: %llu)\n",
        acceptPerSec, (unsigned long long)listener.GetRejectCount());

    // 2. 스레드 로드율 (Deadlock / Overload 감지)
    for (size_t i = 0; i < MAX_LOCAL_THREADS; ++i)
    {
        // 각 스레드의 최근 통계 가져오기 (ThreadManager에 Getter 필요하다고 가정)
        // 여기서는 예시로 출력 포맷만 보여드립니다.
        // 실제로는 ThreadManager::Instance().GetThreadStats(i) 등을 호출

        // 예: 1초동안 틱이 아예 안 돌았다? -> 데드락(Deadlock) 의심
        // 예: Load가 100%에 육박한다? -> 처리 한계 도달
        // printf(" Thread[%zu] Load: %.1f%% (AvgTick: %.2fus)\n", ...);
    }
    //        printf("================================================================\n\n");

    LastAcceptCount = curAccept;
    LastTime = curTime;
}
static void PrintServerStats(CListener& listener)
{
    printf("\n=== Server Statistics ===\n");
    printf("Active Sessions: %zu\n", CSessionManager::Instance().GetActiveSessionCount());
    printf("Local Threads: %zu\n", CThreadManager::Instance().GetLocalThreadCount());
    printf("PacketBuffer Pool: %zu / %zu free (Used: %zu / Max: %zu)\n",
        CPacketBufferPool::Instance().GetFreeCount(),
        CPacketBufferPool::Instance().GetPoolSize(),
        CPacketBufferPool::Instance().GetPoolSize() - CPacketBufferPool::Instance().GetFreeCount(),
        CPacketBufferPool::Instance().GetMaxUsedCount()
    );

    for (size_t i = 0; i < CThreadManager::Instance().GetLocalThreadCount(); ++i)
    {
        CLocalThread* thread = CThreadManager::Instance().GetLocalThread(static_cast<ThreadId>(i));
        if (thread)
        {
            CLocalThread::FStats stats = thread->GetStats();
            double avg_tick_us = stats.TickCount > 0 ?
                static_cast<double>(stats.TotalTickTimeUs) / stats.TickCount : 0.0;

            printf("Thread[%zu]: Sessions=%zu/%zu, Ticks=%llu, AvgTick=%.2fus, MaxTick=%lluus, "
                    "Pkts=%llu, Sent=%llu\n",
                    i,
                    thread->GetSessionCount(),
                    thread->GetMaxSessions(),
                    static_cast<unsigned long long>(stats.TickCount),
                    avg_tick_us,
                    static_cast<unsigned long long>(stats.MaxTickTimeUs),
                    static_cast<unsigned long long>(stats.PacketsProcessed),
                    static_cast<unsigned long long>(stats.PacketsSent));
        }
    }
    // 상세 통계 가져오기
    auto connStats = CSessionManager::Instance().GetConnectionStats();
    printf(" [ Session Status ] Total: %zu (Active: %zu / Waiting: %zu)\n",
        connStats.TotalSessions, connStats.ConnectedCount, connStats.WaitingCount);
    // [추가] 리커넥트 현황판
    auto recStats = CSessionManager::Instance().GetReconnectStats();
    printf(" [ Reconnect Status ]\n");
    printf(" Success : %llu\n", recStats.SuccessCount);
    printf(" Failures: Token(%llu) / NotFound(%llu) / State(%llu) / Expired(%llu)\n",
        recStats.Fail_TokenMismatch, // 이게 0이어야 함
        recStats.Fail_NotFound,      // 이건 좀 있어도 됨 (시간 지나서)
        recStats.Fail_InvalidState,
        recStats.Fail_Expired);

    PrintPerformanceStats(listener);
    printf("================================================================\n\n");
}

// 네트웍 단절 테스트
bool CheckNetworkStallTest()
{
    bool ret = true;
    if (_kbhit())
    {
        int ch = _getch();
        if (ch == 'q' || ch == 'Q')
        {
            ret = false;
//            g_bRunning = false;
        }
        else if (ch == 'p' || ch == 'P')
        {
            bool expected = false;
            // false -> true로 전환 (테스트 시작)
            if (g_bNetworkStallTest.compare_exchange_strong(expected, true))
            {
                printf("\n====================================================\n");
                printf(" [TEST] !!! NETWORK STALL SIMULATION STARTED !!!\n");
                printf(" [TEST] IO Thread stops sending. SendQueue will explode.\n");
                printf("====================================================\n");
            }
            // true -> false로 전환 (테스트 종료 - 복구 시도)
            else
            {
                g_bNetworkStallTest.store(false);
                printf("\n====================================================\n");
                printf(" [TEST] !!! NETWORK STALL SIMULATION ENDED !!!\n");
                printf(" [TEST] IO Thread resumed.\n");
                printf("====================================================\n");
            }
        }
    }
    return ret; 
}

int main(int argc, char* argv[])
{
    SetUnhandledExceptionFilter(UnhandledExceptionFilterFunc); // 이 줄 추가

    printf("=== High-Performance MMORPG Server ===\n");
    printf("Built with C++20, Standalone Asio, Lock-free Queues\n");
    printf("Max Sessions: %zu (Threads: %zu x %zu)\n\n",
           MAX_TOTAL_SESSIONS, MAX_LOCAL_THREADS, MAX_SESSIONS_PER_THREAD);

    uint16_t port = 9000;
    size_t io_threads = MAX_IO_THREADS;
    size_t local_threads = MAX_LOCAL_THREADS;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
        {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-local") == 0 && i + 1 < argc)
        {
            local_threads = static_cast<size_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -p <port>       Listen port (default: 9000)\n");
            printf("  -io <count>     IO thread count (default: 2)\n");
            printf("  -local <count>  Local thread count (default: %zu)\n", MAX_LOCAL_THREADS);
            printf("  -h, --help      Show this help\n");
            return 0;
        }
    }

    printf("Configuration:\n");
    printf("  Port: %u\n", port);
    printf("  IO Threads: %zu\n", io_threads);
    printf("  Local Threads: %zu\n", local_threads);
    printf("\n");

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // PacketBuffer 풀 초기화
    printf("Initializing PacketBuffer pool (%zu buffers)...\n", BUFFER_POOL_INIT_SIZE);
    if (!CPacketBufferPool::Instance().Initialize(BUFFER_POOL_INIT_SIZE))
    {
        printf("ERROR: Failed to initialize PacketBuffer pool\n");
        return 1;
    }

    // ThreadManager 초기화
    printf("Initializing thread manager...\n");
    if (!CThreadManager::Instance().Initialize(io_threads, local_threads))
    {
        printf("ERROR: Failed to initialize thread manager\n");
        CPacketBufferPool::Instance().Shutdown();
        return 1;
    }

    // Listener 생성
    printf("Creating listener on port %u...\n", port);
    CListener listener(CThreadManager::Instance().GetIOContext(), port);
    listener.SetConnectionHandler(OnNewConnection, nullptr);

    if (!listener.Start())
    {
        printf("ERROR: Failed to start listener\n");
        CThreadManager::Instance().Shutdown();
        CPacketBufferPool::Instance().Shutdown();
        return 1;
    }

    printf("Server started successfully!\n");
    printf("Press 'Q' to quit, 'P' to simulate Network Stall.\n\n");
    printf("Press Ctrl+C to stop.\n\n");

    auto last_stats_time = std::chrono::steady_clock::now();
    auto last_tick = last_stats_time;
    while (g_bRunning.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick).count();
        last_tick = now;
        // [추가] 10초 단위 판단을 위해 Update 호출 (내부에서 시간 누적함)
        CNetworkMonitor::Instance().Update(delta);

        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_time).count() >= 10)
        {
            PrintServerStats(listener);
            last_stats_time = now;
        }
        if (CheckNetworkStallTest() == false)
            g_bRunning.store(false, std::memory_order_release);
    }

    printf("\nShutting down...\n");

    listener.Stop();
    CThreadManager::Instance().Shutdown();
    CPacketBufferPool::Instance().Shutdown();

    printf("Server stopped.\n");
    return 0;
}
