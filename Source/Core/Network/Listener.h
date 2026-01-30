#pragma once

#include "NetworkFwd.h"
#include "../Common/Types.h"

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#define ASIO_STANDALONE
//#define ASIO_HAS_STD_ATOMIC
#endif

#include <asio.hpp>
#include <atomic>


//=========================================================================
// CListener - TCP 연결 수신
// CListener의 역할 요약
// 만약 포트를 여러 개(예 : 게임용 9000, 채팅용 9001) 열고 싶다면 CListener를 두 개 생성하면 됩니다.
// 역할 : 외부에서 들어오는 TCP 연결 요청(Knock)을 감지하고, 연결이 성립되면 이를** CSocket 객체(실제 통신 담당자)** 로 변환하여 ThreadManager에게 넘겨줍니다.
// CListener 하나로 수천 명 단위의 동시 접속 시도도 충분히 방어가능. (초당 3,000 ~ 5,000명수준)
//=========================================================================
class CListener
{
public:
	struct FConnectionHandler
	{
		void (*Callback)(void* user_data, CSocket* socket);
		void* UserData;
	};

public:
	CListener(asio::io_context& io_context, uint16_t port);
	~CListener();

	CListener(const CListener&) = delete;
	CListener& operator=(const CListener&) = delete;

	bool Start();
	void Stop();
	bool IsRunning() const { return bIsRunning.load(std::memory_order_acquire); }

	void SetConnectionHandler(void (*callback)(void*, CSocket*), void* user_data);

	uint64_t GetAcceptCount() const { return AcceptCount.load(std::memory_order_relaxed); }
	uint64_t GetRejectCount() const { return RejectCount.load(std::memory_order_relaxed); }

private:
	void DoAccept();         //DoAccept(): 손님 맞이 준비 (비동기)    나중에 손님 오면 OnAccept 함수 실행예약.
	void OnAccept(const asio::error_code& ec);  //손님 오면 호출

private:
	asio::io_context& IoContext;
	asio::ip::tcp::acceptor Acceptor;  //Acceptor가 현재포트를 감시하고 클라이언트가 connect를 요청하면 비동기 작업(async_accept)을 완료
	asio::ip::tcp::socket AcceptSocket;
	uint16_t Port;

	FConnectionHandler ConnectionHandler;

	std::atomic<bool> bIsRunning;
	std::atomic<uint64_t> AcceptCount;
	std::atomic<uint64_t> RejectCount;
};
