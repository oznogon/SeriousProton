#pragma once

#include "multiplayer_server.h"
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

// Class to find all servers that have the correct version number. Creates a big nice list.
class ServerScanner : public Updatable
{
public:
    enum class ServerType
    {
        Manual,
        LAN,
        MasterServer,
        SteamFriend,
    };

    struct ServerInfo
    {
        ServerType type;
        sp::io::network::Address address;
        uint64_t port;
        uint64_t steam_id = 0;
        string name;

        sp::SystemTimer timeout;
    };

    ServerScanner(int version_number, int server_port = DEFAULT_SERVER_PORT);
    virtual ~ServerScanner();

    virtual void destroy() override;

    virtual void update(float delta) override;
    void addCallbacks(std::function<void(const ServerInfo&)> newServerCallback, std::function<void(const ServerInfo&)> removedServerCallback);

    void scanLocalNetwork();
    void scanMasterServer(string url);
#ifdef STEAMSDK
    // Scan Steam for friends also playing EmptyEpsilon.
    void scanSteamFriends();
#endif

    std::vector<ServerInfo> getServerList();

private:
    void masterServerScanThread();

    void updateServerEntry(const ServerInfo& info);

    int server_port;
    std::unique_ptr<sp::io::network::UdpSocket> socket;
    sp::SystemTimer broadcast_timer;

    std::vector<ServerInfo> server_list;
    int version_number;
    constexpr static float BROADCAST_TIMEOUT = 2.0f;
    constexpr static float SERVER_TIMEOUT = 30.0f;
    constexpr static float STEAM_SERVER_TIMEOUT = 60.0f;
    constexpr static float STEAM_SCAN_INTERVAL = 3.0f;

    string master_server_url;
    std::mutex master_server_list_mutex;
    std::vector<ServerInfo> master_server_update_list;
    std::thread master_server_scan_thread;
    std::condition_variable abort_wait;

#ifdef STEAMSDK
    // EmptyEpsilon's Steam AppID, used to detect friends running EmptyEpsilon
    // so they can be offered as Steam P2P servers.
    // https://store.steampowered.com/app/1907040/EmptyEpsilon/
    constexpr static uint32_t STEAM_APP_ID = 1907040;
    bool steam_scan_enabled = false;
    sp::SystemTimer steam_scan_timer;
    void updateSteamFriendEntries();
#endif

    std::function<void(const ServerInfo&)> newServerCallback;
    std::function<void(const ServerInfo&)> removedServerCallback;
};
