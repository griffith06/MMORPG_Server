// [NetworkMonitor.h]
#pragma once
#include <atomic>
#include <algorithm>


class CNetworkMonitor
{
public:
	static CNetworkMonitor& Instance()
	{
		static CNetworkMonitor instance;
		return instance;
	}

	// 소켓이 끊길 때마다 호출
	void OnDisconnect()
	{
		DisconnectCount_.fetch_add(1, std::memory_order_relaxed);
	}

	// 현재 적용해야 할 큐 제한 크기 반환 (Socket::Send에서 호출)
	size_t GetCurrentSendQueueLimit() const
	{
		return CurrentLimit_.load(std::memory_order_relaxed);
	}

	// 주기적으로 호출되어 상황 판단 (예: 1초마다 호출)
	void Update(uint64_t delta_ms)
	{
		ElapsedMs_ += delta_ms;
		if (ElapsedMs_ < 10000) // 10초 주기
			return;

		size_t count = DisconnectCount_.exchange(0, std::memory_order_relaxed);
		size_t new_limit = 2000; // 기본값 (Normal)

		// [동적 스케일링 로직]
		if (count >= 5000)
		{
			new_limit = MAX_SEND_QUEUE_SIZE_LOD2; // 심각한 장애 (Critical)
			printf("[Monitor] CRITICAL: %zu disconnects detected! Limit set to %zd.\n", count, new_limit);
		}
		else if (count >= 10000)
		{
			new_limit = MAX_SEND_QUEUE_SIZE_LOD1; // 주의 단계 (Warning)
			printf("[Monitor] WARNING: %zu disconnects detected! Limit set to %zd.\n", count, new_limit);
		}
		else
		{
			new_limit = MAX_SEND_QUEUE_SIZE_LOD0; // 정상화 (Recovery)
			if (CurrentLimit_.load() != 2000)
				printf("[Monitor] RECOVERY: Network stable. Limit reset to %zd.\n", new_limit);
		}

		CurrentLimit_.store(new_limit, std::memory_order_relaxed);
		ElapsedMs_ = 0;
	}

private:
	CNetworkMonitor() : DisconnectCount_(0), CurrentLimit_(2000), ElapsedMs_(0) {}

	std::atomic<size_t> DisconnectCount_;
	std::atomic<size_t> CurrentLimit_;
	uint64_t ElapsedMs_;
};
