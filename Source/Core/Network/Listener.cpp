#include "Listener.h"
#include "Socket.h"


CListener::CListener(asio::io_context& io_context, uint16_t port)
    : IoContext(io_context)
    , Acceptor(io_context)
    , AcceptSocket(io_context)
    , Port(port)
    , bIsRunning(false)
    , AcceptCount(0)
    , RejectCount(0)
{
    ConnectionHandler.Callback = nullptr;
    ConnectionHandler.UserData = nullptr;
}

CListener::~CListener()
{
    Stop();
}

bool CListener::Start()
{
    if (bIsRunning.load(std::memory_order_acquire))
        return true;

    asio::error_code ec;
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), Port);

    Acceptor.open(endpoint.protocol(), ec);
    if (ec)
        return false;

    Acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec)
        return false;

    Acceptor.bind(endpoint, ec);
    if (ec)
        return false;

    Acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
        return false;

    bIsRunning.store(true, std::memory_order_release);
    DoAccept();

    return true;
}

void CListener::Stop()
{
    if (!bIsRunning.exchange(false, std::memory_order_acq_rel))
        return;

    asio::error_code ec;
    Acceptor.close(ec);

    //도입부에 소켓을 새로 교체하는 코드를 넣었기 때문에, Stop()에서의 close 여부는 크게 중요하지 않음
    AcceptSocket.close(ec); // 대기 중이던 소켓도 닫아줍니다.
}

void CListener::SetConnectionHandler(void (*callback)(void*, CSocket*), void* user_data)
{
    ConnectionHandler.Callback = callback;
    ConnectionHandler.UserData = user_data;
}

void CListener::DoAccept()
{
    if (!bIsRunning.load(std::memory_order_acquire))
        return;
    // [버그 수정] 소켓 초기화 (Refresh)
    // OnAccept에서 std::move로 소유권을 넘겨버리면 AcceptSocket_은 '빈 껍데기(Closed/Moved-from state)'가 됩니다.
    // 따라서 다음 async_accept를 걸기 전에, 새 소켓으로 교체해줘야 합니다.
    // 기존 소켓이 열려있다면 닫고, 새로운 소켓 인스턴스를 대입합니다.
    if (AcceptSocket.is_open())
    {
        AcceptSocket.close();
    }
    AcceptSocket = asio::ip::tcp::socket(IoContext);

    Acceptor.async_accept(AcceptSocket,
        [this](const asio::error_code& ec)
        {
            OnAccept(ec);
        }
    );
}

void CListener::OnAccept(const asio::error_code& ec)
{
    if (!bIsRunning.load(std::memory_order_acquire))
        return;

    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            RejectCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else
    {
        AcceptCount.fetch_add(1, std::memory_order_relaxed);

        asio::error_code opt_ec;
        AcceptSocket.set_option(asio::ip::tcp::no_delay(true), opt_ec);

        CSocket* socket = CSocketPool::Instance().CreateSocket(
            IoContext,
            std::move(AcceptSocket)
        );

        if (ConnectionHandler.Callback && socket)
        {
            ConnectionHandler.Callback(ConnectionHandler.UserData, socket);
        }
        else if (socket)
        {
            CSocketPool::Instance().DestroySocket(socket);
        }
    }

    DoAccept();
}
