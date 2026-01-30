#pragma once

#include "../../Core/Common/Types.h"
#include "../../Core/Threading/IOThreadPool.h"
#include "DummyClient.h"

#include <atomic>
#include <thread>
#include <cstdio>


//=========================================================================
// CLoadTestManager - 부하 테스트 관리
//=========================================================================
class CLoadTestManager
{
public:
    static constexpr size_t MAX_CLIENTS = 10000;
    static constexpr size_t CLIENTS_PER_IO_CONTEXT = 2500;

    struct FAggregateStats
    {
        uint64_t TotalPacketsSent;
        uint64_t TotalPacketsReceived;
        uint64_t PacketsPerSecondSent;
        uint64_t PacketsPerSecondRecv;

        uint64_t TotalBytesSent;
        uint64_t TotalBytesReceived;
        uint64_t BytesPerSecondSent;
        uint64_t BytesPerSecondRecv;

        uint64_t ConnectedClients;
        uint64_t TotalConnectAttempts;
        uint64_t TotalDisconnects;

        uint64_t ElapsedTimeMs;
        uint64_t TryReconnect;
        uint64_t SuccessReconnect;
        uint64_t NewLogin;
        double UpdateLoopFPS; //봇 업데이트 루프 FPS
    };

    struct FConfig
    {
        std::string TargetHost;
        uint16_t TargetPort;
        size_t ClientCount;
        size_t IoThreadCount;
        uint32_t SendIntervalMs;
        uint32_t MinLatencyMs;
        uint32_t MaxLatencyMs;
        bool bEnableArtificialLatency;
        uint32_t RampUpIntervalMs;

        FConfig()
            : TargetHost("127.0.0.1")
            , TargetPort(9000)
            , ClientCount(5000)
            , IoThreadCount(4)
            , SendIntervalMs(300)
            , MinLatencyMs(10)
            , MaxLatencyMs(30)
            , bEnableArtificialLatency(false)
            , RampUpIntervalMs(1)
        {}
    };

public:
    CLoadTestManager();
    ~CLoadTestManager();

    CLoadTestManager(const CLoadTestManager&) = delete;
    CLoadTestManager& operator=(const CLoadTestManager&) = delete;

    bool Initialize(const FConfig& config);
    void Start();
    void Stop();
    bool IsRunning() const { return bIsRunning.load(std::memory_order_acquire); }

    FAggregateStats GetAggregateStats();
    void PrintStats();
    void ResetStats();

    const FConfig& GetConfig() const { return Config_; }

private:
    static void UpdateThreadFunc(CLoadTestManager* self);
    static void StatsThreadFunc(CLoadTestManager* self);

    void CreateClients();
    void DestroyClients();
    void ConnectClients();
    void UpdateClients();

private:
    FConfig Config_;
    std::atomic<bool> bIsRunning;
    std::atomic<bool> bIsInitialized;

    static constexpr size_t MAX_IO_CONTEXTS = 4;
    CIOThreadPool IoThreadPools_[MAX_IO_CONTEXTS];
    size_t IoContextCount_;

    CDummyClient* Clients_[MAX_CLIENTS];
    size_t ClientCount_;

    std::thread UpdateThread_;
    std::thread StatsThread_;

    uint64_t StartTime_;
    uint64_t LastStatsTime_;

    uint64_t LastPacketsSent_;
    uint64_t LastPacketsReceived_;
    uint64_t LastBytesSent_;
    uint64_t LastBytesReceived_;

    std::atomic<uint64_t> LoopCounter_{ 0 };
    uint64_t LastLoopCount_ = 0;
};
