#include "LoadTestManager.h"

#include <cstdio>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>


static std::atomic<bool> g_bRunning(true);
static void SignalHandler(int signum)
{
    (void)signum;
    g_bRunning.store(false, std::memory_order_release);
}


int main(int argc, char* argv[])
{
    printf("=== Load Test Client ===\n");
    printf("Simulates multiple game clients for server stress testing\n\n");

    CLoadTestManager::FConfig config;
    config.TargetHost = "127.0.0.1";
    config.TargetPort = 9000;
    config.ClientCount = MAX_SIM_PLAYERS;
    config.IoThreadCount = MAX_IO_THREADS;
    config.SendIntervalMs = 800;// 1200;   //1.2초에 한번씩 이동
    config.MinLatencyMs = 10;
    config.MaxLatencyMs = 30;
	config.bEnableArtificialLatency = true; //로컬 테스트시 0ms인부분 인위적 지연 활성화
    config.RampUpIntervalMs = 15;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-host") == 0 && i + 1 < argc)
        {
            config.TargetHost = argv[++i];
        }
        else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc)
        {
            config.TargetPort = static_cast<uint16_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-clients") == 0 && i + 1 < argc)
        {
            config.ClientCount = static_cast<size_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-io") == 0 && i + 1 < argc)
        {
            config.IoThreadCount = static_cast<size_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-interval") == 0 && i + 1 < argc)
        {
            config.SendIntervalMs = static_cast<uint32_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-latency") == 0)
        {
            config.bEnableArtificialLatency = true;
        }
        else if (strcmp(argv[i], "-minlat") == 0 && i + 1 < argc)
        {
            config.MinLatencyMs = static_cast<uint32_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-maxlat") == 0 && i + 1 < argc)
        {
            config.MaxLatencyMs = static_cast<uint32_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-rampup") == 0 && i + 1 < argc)
        {
            config.RampUpIntervalMs = static_cast<uint32_t>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -host <addr>      Target server host (default: 127.0.0.1)\n");
            printf("  -port <port>      Target server port (default: 9000)\n");
            printf("  -clients <count>  Number of clients (default: 5000)\n");
            printf("  -io <count>       IO thread count (default: 4)\n");
            printf("  -interval <ms>    Send interval in ms (default: 300)\n");
            printf("  -latency          Enable artificial latency\n");
            printf("  -minlat <ms>      Min latency in ms (default: 10)\n");
            printf("  -maxlat <ms>      Max latency in ms (default: 30)\n");
            printf("  -rampup <ms>      Delay between client connects (default: 1)\n");
            printf("  -h, --help        Show this help\n");
            return 0;
        }
    }

    printf("Configuration:\n");
    printf("  Target: %s:%u\n", config.TargetHost.c_str(), config.TargetPort);
    printf("  Clients: %zu\n", config.ClientCount);
    printf("  IO Threads: %zu\n", config.IoThreadCount);
    printf("  Send Interval: %u ms\n", config.SendIntervalMs);
    printf("  Artificial Latency: %s\n", config.bEnableArtificialLatency ? "enabled" : "disabled");
    if (config.bEnableArtificialLatency)
    {
        printf("    Range: %u-%u ms\n", config.MinLatencyMs, config.MaxLatencyMs);
    }
    printf("  Ramp-up Interval: %u ms\n", config.RampUpIntervalMs);
    printf("\n");

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    CLoadTestManager manager;

    printf("Initializing load test manager...\n");
    if (!manager.Initialize(config))
    {
        printf("ERROR: Failed to initialize load test manager\n");
        return 1;
    }

    printf("Starting load test...\n");
    printf("Press Ctrl+C to stop.\n\n");

    manager.Start();
    bool bBroadcastStarted = false;

    while (g_bRunning.load(std::memory_order_acquire) && manager.IsRunning())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!bBroadcastStarted)
        {
            size_t current_active = CDummyClient::s_ActiveClientCount.load(std::memory_order_relaxed);

            // 목표의 95% 이상이면 시작 (네트워크 상황 따라 100%가 안 될 수도 있으므로)
            if (current_active >= config.ClientCount * 0.95)
            {
                printf("\n\n====================================================\n");
                printf(" [SYSTEM] Target reached (%zu/%zu). Starting Broadcast!\n",
                    current_active, config.ClientCount);
                printf("====================================================\n\n");

                CDummyClient::s_bAllowBroadcast.store(true, std::memory_order_release);
                bBroadcastStarted = true;
            }
        }
    }

    printf("\nStopping load test...\n");
    manager.Stop();

    printf("\n=== Final Statistics ===\n");
    manager.PrintStats();

    printf("Load test completed.\n");
    return 0;
}
