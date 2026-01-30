#include "LoadTestManager.h"
#include <cstring>
#include <chrono>
#include <iostream>

// 키 입력 처리를 위한 헤더 (Windows)
#ifdef _WIN32
#include <conio.h> 
#else
// Linux 등의 환경이라면 ncurses 등을 써야 하지만, 여기선 Windows 가정
int _kbhit() { return 0; }
int _getch() { return 0; }
#endif


// 전역 플래그 (다른 파일에서 extern으로 참조 가능)
bool g_bRandomRollingMode = false;

static uint64_t GetCurrentTimeMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

//=========================================================================
// CLoadTestManager
//=========================================================================
CLoadTestManager::CLoadTestManager()
    : bIsRunning(false)
    , bIsInitialized(false)
    , IoContextCount_(0)
    , ClientCount_(0)
    , StartTime_(0)
    , LastStatsTime_(0)
    , LastPacketsSent_(0)
    , LastPacketsReceived_(0)
    , LastBytesSent_(0)
    , LastBytesReceived_(0)
{
    std::memset(Clients_, 0, sizeof(Clients_));
}

CLoadTestManager::~CLoadTestManager()
{
    Stop();
    DestroyClients();
}

bool CLoadTestManager::Initialize(const FConfig& config)
{
    if (bIsInitialized.load(std::memory_order_acquire))
        return true;

    Config_ = config;

    if (Config_.ClientCount == 0 || Config_.ClientCount > MAX_CLIENTS)
    {
        Config_.ClientCount = MAX_CLIENTS;
    }

    IoContextCount_ = (Config_.ClientCount + CLIENTS_PER_IO_CONTEXT - 1) / CLIENTS_PER_IO_CONTEXT;
    if (IoContextCount_ > MAX_IO_CONTEXTS)
    {
        IoContextCount_ = MAX_IO_CONTEXTS;
    }

    size_t threads_per_context = Config_.IoThreadCount / IoContextCount_;
    if (threads_per_context == 0)
        threads_per_context = 1;

    for (size_t i = 0; i < IoContextCount_; ++i)
    {
        if (!IoThreadPools_[i].Start(threads_per_context))
        {
            for (size_t j = 0; j < i; ++j)
            {
                IoThreadPools_[j].Stop();
            }
            return false;
        }
    }

    CreateClients();

    bIsInitialized.store(true, std::memory_order_release);
    return true;
}

void CLoadTestManager::Start()
{
    if (!bIsInitialized.load(std::memory_order_acquire))
        return;

    if (bIsRunning.load(std::memory_order_acquire))
        return;

    bIsRunning.store(true, std::memory_order_release);
    StartTime_ = GetCurrentTimeMs();
    LastStatsTime_ = StartTime_;

    UpdateThread_ = std::thread(UpdateThreadFunc, this);
    StatsThread_ = std::thread(StatsThreadFunc, this);

    ConnectClients();
}

void CLoadTestManager::Stop()
{
    if (!bIsRunning.exchange(false, std::memory_order_acq_rel))
        return;

    if (UpdateThread_.joinable())
    {
        UpdateThread_.join();
    }

    if (StatsThread_.joinable())
    {
        StatsThread_.join();
    }

    for (size_t i = 0; i < ClientCount_; ++i)
    {
        if (Clients_[i])
        {
            Clients_[i]->Disconnect();
        }
    }

    for (size_t i = 0; i < IoContextCount_; ++i)
    {
        IoThreadPools_[i].Stop();
    }

    bIsInitialized.store(false, std::memory_order_release);
}

CLoadTestManager::FAggregateStats CLoadTestManager::GetAggregateStats()
{
    FAggregateStats stats;
    std::memset(&stats, 0, sizeof(stats));

    uint64_t now = GetCurrentTimeMs();
    stats.ElapsedTimeMs = now - StartTime_;

    for (size_t i = 0; i < ClientCount_; ++i)
    {
        if (!Clients_[i])
            continue;

        CDummyClient::FStats client_stats = Clients_[i]->GetStats();
        stats.TotalPacketsSent += client_stats.PacketsSent;
        stats.TotalPacketsReceived += client_stats.PacketsReceived;
        stats.TotalBytesSent += client_stats.BytesSent;
        stats.TotalBytesReceived += client_stats.BytesReceived;
        stats.TotalConnectAttempts += client_stats.ConnectAttempts;
        stats.TotalDisconnects += client_stats.DisconnectCount;
        stats.TryReconnect += client_stats.TryReconnectCount;
        stats.SuccessReconnect += client_stats.SuccessReconnectCount;
        stats.NewLogin += client_stats.NewLoginCount;

        if (Clients_[i]->IsConnected())
        {
            ++stats.ConnectedClients;
        }
    }

    uint64_t time_delta = now - LastStatsTime_;
    if (time_delta > 0)
    {
        stats.PacketsPerSecondSent = ((stats.TotalPacketsSent - LastPacketsSent_) * 1000) / time_delta;
        stats.PacketsPerSecondRecv = ((stats.TotalPacketsReceived - LastPacketsReceived_) * 1000) / time_delta;
        stats.BytesPerSecondSent = ((stats.TotalBytesSent - LastBytesSent_) * 1000) / time_delta;
        stats.BytesPerSecondRecv = ((stats.TotalBytesReceived - LastBytesReceived_) * 1000) / time_delta;
        // Loop FPS 계산
        uint64_t currentLoops = LoopCounter_.load(std::memory_order_relaxed);
        stats.UpdateLoopFPS = ((double)(currentLoops - LastLoopCount_) * 1000.0) / time_delta;
    }

    return stats;
}

void CLoadTestManager::PrintStats()
{
    FAggregateStats stats = GetAggregateStats();

    LastPacketsSent_ = stats.TotalPacketsSent;
    LastPacketsReceived_ = stats.TotalPacketsReceived;
    LastBytesSent_ = stats.TotalBytesSent;
    LastBytesReceived_ = stats.TotalBytesReceived;
    LastStatsTime_ = GetCurrentTimeMs();
    LastLoopCount_ = LoopCounter_.load(std::memory_order_relaxed);

    // 평균 패킷 크기 계산 (0으로 나누기 방지)
    double avgSendSize = stats.TotalPacketsSent > 0 ?
        (double)stats.TotalBytesSent / stats.TotalPacketsSent : 0.0;
    double avgRecvSize = stats.TotalPacketsReceived > 0 ?
        (double)stats.TotalBytesReceived / stats.TotalPacketsReceived : 0.0;

    printf("\n================================================================\n");
    printf(" [ Load Test Statistics ]   Elapsed: %.2f sec\n", stats.ElapsedTimeMs / 1000.0);
    printf("================================================================\n");
    printf(" Connected Clients : %llu / %zu\n",
        static_cast<unsigned long long>(stats.ConnectedClients), ClientCount_);
    printf("----------------------------------------------------------------\n");
    printf(" [Send] Count : %12llu pkts (%8llu / sec)\n",
        static_cast<unsigned long long>(stats.TotalPacketsSent),
        static_cast<unsigned long long>(stats.PacketsPerSecondSent));
    printf("        Volume: %12.2f MB   (%8.2f KB/sec)\n",
        stats.TotalBytesSent / (1024.0 * 1024.0),
        stats.BytesPerSecondSent / 1024.0);
    printf("        AvgSz : %12.2f Bytes\n", avgSendSize);
    printf("----------------------------------------------------------------\n");
    printf(" [Recv] Count : %12llu pkts (%8llu / sec)\n",
        static_cast<unsigned long long>(stats.TotalPacketsReceived),
        static_cast<unsigned long long>(stats.PacketsPerSecondRecv));
    printf("        Volume: %12.2f MB   (%8.2f KB/sec)\n",
        stats.TotalBytesReceived / (1024.0 * 1024.0),
        stats.BytesPerSecondRecv / 1024.0);
    printf("        AvgSz : %12.2f Bytes\n", avgRecvSize);
    printf("----------------------------------------------------------------\n");
//        printf(" Failures      : Connect %llu / Disconnect %llu\n",
//            static_cast<unsigned long long>(stats.TotalConnectAttempts - stats.ConnectedClients),
//            static_cast<unsigned long long>(stats.TotalDisconnects));
    long long offlineClients = ClientCount_ - stats.ConnectedClients;
    printf(" Status        : Active %llu / Offline(Waiting) %lld\n",
        static_cast<unsigned long long>(stats.ConnectedClients),
        offlineClients);
    printf(" Events        : Total Disconnects %llu\n",
        static_cast<unsigned long long>(stats.TotalDisconnects));
    printf(" [ Client Reconnect Logic ]\n");
    printf(" Attempts : %llu\n", stats.TryReconnect);
    printf(" Restored : %llu (Success)\n", stats.SuccessReconnect);
    printf(" Expired  : %llu (New Login)\n", stats.NewLogin);
    printf(" Client Loop FPS : %8.2f / sec\n", stats.UpdateLoopFPS); // FPS 출력
    printf("================================================================\n\n");
}

void CLoadTestManager::ResetStats()
{
    for (size_t i = 0; i < ClientCount_; ++i)
    {
        if (Clients_[i])
        {
            Clients_[i]->ResetStats();
        }
    }

    StartTime_ = GetCurrentTimeMs();
    LastStatsTime_ = StartTime_;
    LastPacketsSent_ = 0;
    LastPacketsReceived_ = 0;
    LastBytesSent_ = 0;
    LastBytesReceived_ = 0;
}

void CLoadTestManager::UpdateThreadFunc(CLoadTestManager* self)
{
    using namespace std::chrono;

    while (self->bIsRunning.load(std::memory_order_acquire))
    {
        // 키 입력 처리 (테스트 트리거)
        if (_kbhit())
        {
            int key = _getch();

            // [1] 'r' : Random Rolling Toggle
            if (key == 'r' || key == 'R')
            {
                g_bRandomRollingMode = !g_bRandomRollingMode;
                // 모든 봇에게 전파 (static 변수나 멤버 변수로 제어)
                CDummyClient::bEnableRandomRolling = g_bRandomRollingMode;
                printf("\n[TEST] Random Rolling Mode: %s\n", g_bRandomRollingMode ? "ON" : "OFF");
            }

            // [2] 'm' : Mass Drop (50% 인원)
            else if (key == 'm' || key == 'M')
            {
                printf("\n[TEST] Triggering Mass Drop on 50%% clients...\n");
                size_t count = 0;

                for (size_t i = 0; i < self->ClientCount_; ++i)
                {
                    auto* client = self->Clients_[i];
                    if (client && client->IsConnected() && (rand() % 2 == 0)) // 50% 확률
                    {
                        client->SetTestMode(EBotState::MassDropping);
                        count++;
                    }
                }
                printf("[TEST] %zu clients disconnected. They will reconnect in 5s.\n", count);
            }

            // [3] 't' : Timeout Cleanup (5% 인원)
            else if (key == 't' || key == 'T')
            {
                printf("\n[TEST] creating Zombies (5%%)...\n");
                size_t count = 0;
                size_t target = (size_t)(MAX_SIM_PLAYERS * 0.05); // 전체 5% 일정수 만큼

                for (size_t i = 0; i < self->ClientCount_; ++i)
                {
                    auto* client = self->Clients_[i];
                    if (client && client->IsConnected())
                    {
                        client->SetTestMode(EBotState::ZombieDead);
                        count++;
                        if (count >= target) break;
                    }
                }
                printf("[TEST] %zu clients became zombies. Check Server Memory Release!\n", count);
            }
        }

        // 클라이언트 업데이트
        self->UpdateClients();
            
        self->LoopCounter_.fetch_add(1, std::memory_order_relaxed);

        std::this_thread::sleep_for(milliseconds(1));
    }
}

void CLoadTestManager::StatsThreadFunc(CLoadTestManager* self)
{
    using namespace std::chrono;

    while (self->bIsRunning.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(seconds(5));
        if (self->bIsRunning.load(std::memory_order_acquire))
        {
            self->PrintStats();
        }
    }
}

void CLoadTestManager::CreateClients()
{
    CDummyClient::FConfig client_config;
    client_config.SendIntervalMs = Config_.SendIntervalMs;
    client_config.MinLatencyMs = Config_.MinLatencyMs;
    client_config.MaxLatencyMs = Config_.MaxLatencyMs;
    client_config.bEnableArtificialLatency = Config_.bEnableArtificialLatency;
    client_config.Host = Config_.TargetHost; // Config에서 호스트/포트 전달
    client_config.Port = Config_.TargetPort;

    ClientCount_ = Config_.ClientCount;

    for (size_t i = 0; i < ClientCount_; ++i)
    {
        size_t context_index = i % IoContextCount_;

        Clients_[i] = new CDummyClient(
            IoThreadPools_[context_index].GetIOContext(),
            static_cast<uint32_t>(i)
        );
        Clients_[i]->SetConfig(client_config);
    }
}

void CLoadTestManager::DestroyClients()
{
    for (size_t i = 0; i < ClientCount_; ++i)
    {
        if (Clients_[i])
        {
            delete Clients_[i];
            Clients_[i] = nullptr;
        }
    }
    ClientCount_ = 0;
}

void CLoadTestManager::ConnectClients()
{
    #define PER_INTERVAL      10

    for (size_t i = 0; i < ClientCount_; ++i)
    {
        if (!bIsRunning.load(std::memory_order_acquire))
            break;

        if (Clients_[i])
        {
            Clients_[i]->Connect(Config_.TargetHost, Config_.TargetPort);
        }
        if (i % PER_INTERVAL == 0)
        {
            if (Config_.RampUpIntervalMs > 0 && i < ClientCount_ - 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(Config_.RampUpIntervalMs));
            }
        }
    }
}

void CLoadTestManager::UpdateClients()
{
    uint64_t now = GetCurrentTimeMs();

    for (size_t i = 0; i < ClientCount_; ++i)
    {
        if (Clients_[i])
        {
            Clients_[i]->Update(now);
        }
    }
}
