#pragma once

#include <memory>
#include "multiplayer_server.h"

class GameServerProxy : public Updatable
{
    sp::SystemTimer heartbeat_timer;
    sp::SystemTimer no_data_timeout;

    // if the server doesn't send us any data for this long, send a packet to see if it's still there
    constexpr static float HEARTBEAT_TIME = 0.5;
    // if the server doesn't send us any data for this long, disconnect
    constexpr static float NO_DATA_DISCONNECT_TIME = 20.0f;

    sp::io::network::UdpSocket broadcast_listen_socket;
    sp::io::network::TcpListener listen_socket;
    std::unique_ptr<sp::io::network::TcpSocket> new_socket;

    enum EClientReceiveState
    {
        CRS_Auth,
        CRS_Main,
        CRS_Command
    };

    struct ClientInfo
    {
        std::unique_ptr<sp::io::network::TcpSocket> socket;
        int32_t client_id = 0;
        int32_t commandObjectId = 0;
        bool validClient = false;
        EClientReceiveState receiveState = CRS_Auth;
    };

    std::vector<ClientInfo> client_list;
    std::unordered_set<int32_t> targetClients;

    int32_t client_id = 0;
    string password;
    int32_t server_version = 0;
    string proxy_name;
    float broadcast_server_delay;
    std::unique_ptr<sp::io::network::TcpSocket> main_socket;
public:
    GameServerProxy(sp::io::network::Address hostname, int hostPort = DEFAULT_SERVER_PORT, string password = "", int listenPort = DEFAULT_SERVER_PORT, string proxy_name="");
    GameServerProxy(string password = "", int listenPort = DEFAULT_SERVER_PORT, string proxy_name="");
    virtual ~GameServerProxy();

    virtual void destroy() override;

    virtual void update(float delta) override;
private:
    void sendAll(sp::io::DataBuffer& packet);

    void handleBroadcastUDPSocket(float delta);
};
