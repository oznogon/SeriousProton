#include <io/network/tcpSocket.h>
#include <logging.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
static constexpr int flags = 0;

static inline int send(SOCKET s, const void* msg, size_t len, int flags)
{
    return send(s, static_cast<const char*>(msg), static_cast<int>(len), flags);
}

static inline int recv(SOCKET s, void* buf, size_t len, int flags)
{
    return recv(s, static_cast<char*>(buf), static_cast<int>(len), flags);
}

#else
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#if defined(__APPLE__)
static constexpr int flags = 0;
#else
static constexpr int flags = MSG_NOSIGNAL;
#endif
static constexpr intptr_t INVALID_SOCKET = -1;
#endif

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#if defined(_WIN32)
#include <wincrypt.h>
#endif
#endif

#ifdef HAVE_OPENSSL
static SSL_CTX* getSSLContext()
{
    static SSL_CTX* ctx = nullptr;
    static bool initialized = false;
    if (!initialized)
    {
        initialized = true;
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx)
        {
            SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
#ifdef _WIN32
            HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
            if (hStore)
            {
                X509_STORE* store = X509_STORE_new();
                PCCERT_CONTEXT pContext = NULL;
                while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != nullptr)
                {
                    const unsigned char* c = pContext->pbCertEncoded;
                    X509* x509 = d2i_X509(nullptr, &c, pContext->cbCertEncoded);
                    if (x509)
                    {
                        X509_STORE_add_cert(store, x509);
                        X509_free(x509);
                    }
                }
                CertFreeCertificateContext(pContext);
                CertCloseStore(hStore, 0);
                SSL_CTX_set_cert_store(ctx, store);
            }
#else
            SSL_CTX_set_default_verify_paths(ctx);
#endif
        }
    }
    return ctx;
}
#endif


namespace sp {
namespace io {
namespace network {


TcpSocket::TcpSocket()
: ssl_handle(nullptr)
{
}

TcpSocket::~TcpSocket()
{
    close();
}

#ifdef HAVE_OPENSSL
void TcpSocket::setSSLVerify(bool enabled)
{
    ssl_verify = enabled;
}
#endif

bool TcpSocket::connect(const Address& host, int port)
{
    if (handle != INVALID_SOCKET)
        close();

    for(const auto& addr_info : host.addr_info)
    {
        handle = ::socket(addr_info.family, SOCK_STREAM, 0);
        if (handle == INVALID_SOCKET)
        {
            LOG(Warning, "Failed to create socket for TCP connection");
            return false;
        }
        setBlocking(blocking);
        if (addr_info.family == AF_INET && sizeof(struct sockaddr_in) == addr_info.addr.size())
        {
            struct sockaddr_in server_addr;
            memset(&server_addr, 0, sizeof(server_addr));
            memcpy(&server_addr, addr_info.addr.data(), addr_info.addr.size());
#if defined(__APPLE__)
            server_addr.sin_len = sizeof(struct sockaddr_in);
            server_addr.sin_family = AF_INET;
#endif
            server_addr.sin_port = htons(port);
            if (::connect(handle, reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)) == 0)
                return true;
            if (isLastErrorNonBlocking())
            {
                connecting = true;
                return true;
            }
            LOG(Warning, "TCP connect to ", addr_info.human_readable, ":", port, " failed. errno=", errno);
        }
        else if (addr_info.family == AF_INET6 && sizeof(struct sockaddr_in6) == addr_info.addr.size())
        {
            struct sockaddr_in6 server_addr;
            memset(&server_addr, 0, sizeof(server_addr));
            memcpy(&server_addr, addr_info.addr.data(), addr_info.addr.size());
#if defined(__APPLE__)
            server_addr.sin6_len = sizeof(struct sockaddr_in6);
            server_addr.sin6_family = AF_INET6;
#endif
            server_addr.sin6_port = htons(port);
            if (::connect(handle, reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)) == 0)
                return true;
            if (isLastErrorNonBlocking())
            {
                connecting = true;
                return true;
            }
            LOG(Warning, "TCP connect to [", addr_info.human_readable, "]:", port, " failed. errno=", errno);
        }
        else
        {
            LOG(Warning, "Unsupported address family: ", addr_info.family, " addr_size=", addr_info.addr.size());
        }
        close();
    }
    LOG(Warning, "Failed to connect TCP socket to port ", port, ". Address had ", host.addr_info.size(), " resolved entries.");
    return false;
}

bool TcpSocket::connectSSL(const Address& host, int port)
{
#ifdef HAVE_OPENSSL
    if (!connect(host, port))
        return false;

    SSL_CTX* ctx = getSSLContext();
    if (!ctx)
    {
        LOG(Warning, "Failed to create SSL context");
        close();
        return false;
    }

    SSL* ssl = SSL_new(ctx);
    if (!ssl)
    {
        LOG(Warning, "Failed to create SSL session");
        close();
        return false;
    }
    SSL_set_fd(ssl, static_cast<int>(handle));
    int ssl_ret = SSL_connect(ssl);
    if (ssl_ret <= 0)
    {
        int ssl_error_code = SSL_get_error(ssl, ssl_ret);
        LOG(Warning, "Failed to connect SSL socket due to SSL negotiation failure. SSL error: ", ssl_error_code);
        SSL_free(ssl);
        close();
        return false;
    }
    if (ssl_verify && SSL_get_verify_result(ssl) != 0)
    {
        LOG(Warning, "Failed to connect SSL socket due to certificate verification failure.");
        SSL_free(ssl);
        close();
        return false;
    }
    ssl_handle = ssl;
    return true;
#else
    LOG(Warning, "SSL support not compiled in, connectSSL() called");
    close();
    return false;
#endif
}

void TcpSocket::setDelay(bool delay)
{
    if (handle == INVALID_SOCKET)
    {
        LOG(Warning, "Failed to setDelay due to being called on an incomplete socket");
        return;
    }
    int mode = delay ? 0 : 1;
    if (setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (char*)&mode, sizeof(mode)) == -1)
    {
        LOG(Warning, "Failed to setDelay on a socket");
    }
}

void TcpSocket::close()
{
    if (handle != INVALID_SOCKET)
    {
#ifdef _WIN32
        closesocket(handle);
#else
        ::close(handle);
#endif
        handle = INVALID_SOCKET;
        connecting = false;
        clearQueue();
#ifdef HAVE_OPENSSL
        if (ssl_handle)
            SSL_free(static_cast<SSL*>(ssl_handle));
#endif
        ssl_handle = nullptr;
    }
}

StreamSocket::State TcpSocket::getState()
{
    if (handle == INVALID_SOCKET)
        return StreamSocket::State::Closed;
    if (connecting) {
        struct pollfd fds;
        fds.fd = handle;
        fds.events = POLLOUT;
        fds.revents = 0;
#ifdef WIN32
        if (WSAPoll(&fds, 1, 0))
#else
        if (poll(&fds, 1, 0))
#endif
        {
            struct sockaddr_in6 server_addr;
            socklen_t server_addr_len = sizeof(server_addr);
            if (getpeername(handle, reinterpret_cast<sockaddr*>(&server_addr), &server_addr_len))
            {
                close();
                return StreamSocket::State::Closed;
            }
            connecting = false;
            return StreamSocket::State::Connected;
        }
        return StreamSocket::State::Connecting;
    }
    return StreamSocket::State::Connected;
}

size_t TcpSocket::_send(const void* data, size_t size)
{
    int result;
#ifdef HAVE_OPENSSL
    if (ssl_handle)
        result = SSL_write(static_cast<SSL*>(ssl_handle), static_cast<const char*>(data), static_cast<int>(size));
    else
#endif
        result = ::send(handle, reinterpret_cast<const void *>(static_cast<const char*>(data)), size, flags);
    if (result < 0)
    {
        if (!isLastErrorNonBlocking())
            close();
        return 0;
    }
    return result;
}

size_t TcpSocket::_receive(void* data, size_t size)
{
    int result;
#ifdef HAVE_OPENSSL
    if (ssl_handle)
        result = SSL_read(static_cast<SSL*>(ssl_handle), static_cast<char*>(data), static_cast<int>(size));
    else
#endif
        result = ::recv(handle, data, size, flags);
    if (result < 0)
    {
        result = 0;
        if (!isLastErrorNonBlocking())
            close();
    }
    return result;
}

}//namespace network
}//namespace io
}//namespace sp
