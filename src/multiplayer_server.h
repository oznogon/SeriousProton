#ifndef MULTIPLAYER_SERVER_H
#define MULTIPLAYER_SERVER_H

#include "io/network/udpSocket.h"
#include "io/network/tcpSocket.h"
#include "io/network/streamSocket.h"
#include "io/network/tcpListener.h"
#ifdef STEAMSDK
#include "io/network/steamP2PListener.h"
#endif
#include "Updatable.h"
#include "stringImproved.h"
#include "networkAudioStream.h"
#include "timer.h"

#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <functional>


static const int defaultServerPort = 35666;
static const int multiplayerVerficationNumber = 0x2fab3f0f; //Used to verify that the server is actually a serious proton server

class GameServer;
class MultiplayerObject;

extern P<GameServer> game_server;

class GameServer : public Updatable
{
public:
    enum class MasterServerState
    {
        Disabled,
        Registering,
        Success,
        FailedToReachMasterServer,
        FailedPortForwarding,
    };

private:
    sp::SystemStopwatch last_update_time;
    sp::SystemTimer keep_alive_send_timer;
    sp::io::network::UdpSocket broadcast_listen_socket;
    
    sp::io::network::TcpListener listen_socket;
    std::unique_ptr<sp::io::network::TcpSocket> new_socket;
#ifdef STEAMSDK
    sp::io::network::SteamP2PListener listen_steam;
#endif
    string server_name;
    int listen_port;
    int version_number;
    string server_password;

    int sendDataCounter;
    int sendDataCounterPerClient;
    float sendDataRate;
    float sendDataRatePerClient;
    float update_run_time;
    
    float lastGameSpeed;
    float boardcastServerDelay;

    bool collect_network_stats = false;
    sp::SystemTimer multiplayer_stats_dump_timer;
    std::unordered_map<string, int> multiplayer_stats;
    std::unordered_map<string, int> last_network_stats_snapshot;

    enum EClientReceiveState
    {
        CRS_Auth,
        CRS_Main,
        CRS_Command
    };
    struct ClientInfo
    {
        std::unique_ptr<sp::io::network::StreamSocket> socket;
        int32_t client_id;
        int32_t command_client_id = 0;
        EClientReceiveState receive_state;
        int32_t command_object_id = 0;
        sp::SystemStopwatch round_trip_start_time;
        int32_t ping = 0;
        std::vector<int32_t> proxy_ids;
        std::vector<std::pair<std::vector<uint8_t>, float>> delayed_packets;
    };
    int32_t nextclient_id;
    std::vector<ClientInfo> clientList;
    std::unordered_map<int32_t, std::unordered_set<int32_t>> voice_targets;
    NetworkAudioStreamManager audio_stream_manager;

    int32_t nextObjectId;
    std::unordered_map<int32_t, P<MultiplayerObject> > objectMap;

    std::vector<uint32_t> ecs_entity_version;

    string master_server_url;
    std::thread master_server_update_thread;
    MasterServerState master_server_state = MasterServerState::Disabled;

    string proxy_registry_url;
    string proxy_registry_password;
    int32_t proxy_registry_assigned_port = 0;
    sp::SystemTimer proxy_registry_heartbeat_timer;
public:
    bool simulate_high_latency = false;
    bool simulate_random_latency = false;
    GameServer(string server_name, int versionNumber, int listenPort = defaultServerPort);
    virtual ~GameServer();

    void connectToProxy(sp::io::network::Address address, int port = defaultServerPort);

    virtual void destroy() override;

    P<MultiplayerObject> getObjectById(int32_t id);
    virtual void update(float delta) override;
    inline float getSendDataRate() { return sendDataRate; }
    inline float getSendDataRatePerClient() { return sendDataRatePerClient; }
    inline float getUpdateTime() { return update_run_time; }

    void setCollectNetworkStats(bool enable) { collect_network_stats = enable; }

    int getClientCount();
    std::vector<std::pair<int32_t, int32_t>> getClientPings();
    const std::unordered_map<string, int>& getNetworkStatsSnapshot() { return last_network_stats_snapshot; }

    string getServerName() { return server_name; }
    void setServerName(string name) { server_name = name; }
    
    void registerOnMasterServer(string master_server_url);
    void registerOnProxyRegistry(string registry_url, string password);
    MasterServerState getMasterServerState() { return master_server_state; }
    void stopMasterServerRegistry();
    void setPassword(string password);

    void startAudio(int32_t client_id, int32_t target_identifier);
    void gotAudioPacket(int32_t client_id, const unsigned char* packet, int packet_size);
    void stopAudio(int32_t client_id);
    void sendAudioPacketFrom(int32_t client_id, sp::io::DataBuffer& packet);
private:
    void newClientConnection(std::unique_ptr<sp::io::network::StreamSocket> socket);
    void registerObject(P<MultiplayerObject> obj);
    void broadcastServerCommandFromObject(int32_t id, sp::io::DataBuffer& packet);
    void keepAliveAll();
    void sendAll(sp::io::DataBuffer& packet);

    void generateCreatePacketFor(P<MultiplayerObject> obj, sp::io::DataBuffer& packet);
    void generateDeletePacketFor(int32_t id, sp::io::DataBuffer& packet);
    
    void replicateInitialData(std::function<void(sp::io::DataBuffer&)> send_packet);
    void handleNewClient(ClientInfo& info);
    void handleNewProxy(ClientInfo& info, int32_t temp_id);
    
    void runMasterServerUpdateThread();
    void sendProxyRegistryHeartbeat();
    void sendProxyRegistryDeregister();

    void handleBroadcastUDPSocket(float delta);

    friend class MultiplayerObject;
public:
    virtual void onNewClient(int32_t client_id) {}
    virtual void onDisconnectClient(int32_t client_id) {}
    virtual std::unordered_set<int32_t> onVoiceChat(int32_t client_id, int32_t target_identifier);
};

#endif//MULTIPLAYER_SERVER_H
