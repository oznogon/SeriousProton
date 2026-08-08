#include "multiplayer_server_scanner.h"
#include "io/http/request.h"
#ifdef STEAMSDK
#include <steam/steam_api.h>
#include <unordered_set>
#endif
using namespace std::chrono_literals;

ServerScanner::ServerScanner(int version_number, int server_port)
: server_port(server_port), version_number(version_number)
{
}

ServerScanner::~ServerScanner()
{
    destroy();
    if (master_server_scan_thread.joinable()) master_server_scan_thread.join();
}

void ServerScanner::scanMasterServer(string url)
{
    abort_wait.notify_all();
    if (master_server_scan_thread.joinable()) return;
    LOG(Info, "[sp-serverscan] Starting registration server scanning.");

    master_server_url = url;
    master_server_scan_thread = std::thread(&ServerScanner::masterServerScanThread, this);
}

void ServerScanner::scanLocalNetwork()
{
    if (socket) return;

    LOG(Info, "[sp-serverscan] Starting local server scanning.");

    socket = std::make_unique<sp::io::network::UdpSocket>();
    int port_nr = server_port + 1;

    while (!socket->bind(static_cast<uint16_t>(port_nr))) port_nr++;

    if (!socket->joinMulticast(666))
        LOG(Error, "[sp-serverscan] Failed to join multicast port 666 for local network discovery.");

    socket->setBlocking(false);
    broadcast_timer.repeat(BROADCAST_TIMEOUT);
}

#ifdef STEAMSDK
void ServerScanner::scanSteamFriends()
{
    if (steam_scan_enabled) return;

    LOG(Info, "[sp-serverscan] Starting Steam friends server scanning.");
    steam_scan_enabled = true;
    steam_scan_timer.repeat(STEAM_SCAN_INTERVAL);
}

void ServerScanner::updateSteamFriendEntries()
{
    // Any Steam friend running EmptyEpsilon is offered as a  Steam P2P server.
    // This isn't ideal since friends running EmptyEpsilon as a client still
    // appear. The client's connect timeout handles failures.
    // TODO: Just don't show clients on the list.
    std::unordered_set<uint64_t> in_game_friends;

    int friend_count = SteamFriends()->GetFriendCount(k_EFriendFlagAll);
    for (int i = 0; i < friend_count; i++)
    {
        CSteamID friend_id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagAll);
        FriendGameInfo_t game_info;
        if (!SteamFriends()->GetFriendGamePlayed(friend_id, &game_info))
            continue;
        if (!game_info.m_gameID.IsValid()) continue;
        if (game_info.m_gameID.AppID() != STEAM_APP_ID) continue;

        uint64_t steam_id = friend_id.ConvertToUint64();
        in_game_friends.insert(steam_id);
        updateServerEntry({ServerType::SteamFriend, {}, 0, steam_id, SteamFriends()->GetFriendPersonaName(friend_id), {}});
    }

    // Remove SteamFriend entries for friends who are no longer in-game.
    for (unsigned int n = 0; n < server_list.size(); n++)
    {
        if (server_list[n].type != ServerType::SteamFriend) continue;
        if (in_game_friends.find(server_list[n].steam_id) != in_game_friends.end())
            continue;

        if (removedServerCallback) removedServerCallback(server_list[n]);
        server_list.erase(server_list.begin() + n);
        n--;
    }
}
#endif

void ServerScanner::update(float /* game_delta */)
{
    master_server_list_mutex.lock();
    for (const auto& info : master_server_update_list) updateServerEntry(info);

    master_server_update_list.clear();
    master_server_list_mutex.unlock();

#ifdef STEAMSDK
    // Repeat steam scans on interval.
    if (steam_scan_enabled && steam_scan_timer.isExpired())
    {
        steam_scan_timer.repeat(STEAM_SCAN_INTERVAL);
        updateSteamFriendEntries();
    }
#endif

    for (unsigned int n = 0; n < server_list.size(); n++)
    {
        if (server_list[n].timeout.isExpired())
        {
            if (removedServerCallback) removedServerCallback(server_list[n]);
            server_list.erase(server_list.begin() + n);
            n--;
        }
    }

    if (socket)
    {
        if (broadcast_timer.isExpired())
        {
            sp::io::DataBuffer sendPacket;
            sendPacket << MULTIPLAYER_VERIFICATION_NUMBER << "ServerQuery" << int32_t(version_number);
            socket->sendMulticast(sendPacket, 666, server_port);
        }

        sp::io::network::Address recv_address;
        int recv_port;
        sp::io::DataBuffer recv_packet;
        while (socket->receive(recv_packet, recv_address, recv_port))
        {
            int32_t verification, version_nr;
            string name;
            recv_packet >> verification >> version_nr >> name;

            if (verification == MULTIPLAYER_VERIFICATION_NUMBER
                && (version_nr == version_number || version_nr == 0 || version_number == 0)
            ) updateServerEntry({ServerType::LAN, recv_address, static_cast<uint64_t>(recv_port), 0, name, {}});
        }
    }
}

void ServerScanner::updateServerEntry(const ServerInfo& info)
{
    for (unsigned int n = 0; n < server_list.size(); n++)
    {
        if (server_list[n].type != info.type) continue;

        // SteamFriend entries are ID'd by the friend's SteamID. All other
        // server types are ID'd by their address.
        if (info.type == ServerType::SteamFriend)
            { if (server_list[n].steam_id != info.steam_id) continue; }
        else if (!(server_list[n].address == info.address)) continue;

        server_list[n].port = info.port;
        server_list[n].name = info.name;
        server_list[n].timeout.start(info.type == ServerType::SteamFriend ? STEAM_SERVER_TIMEOUT : SERVER_TIMEOUT);
        return;
    }

    // Add a new server to the list.
    LOG(Info, "[sp-serverscan] New server: ", info.address.getHumanReadable()[0], " ", info.port, " ", info.name);
    ServerInfo si = info;
    si.timeout.start(si.type == ServerType::SteamFriend ? STEAM_SERVER_TIMEOUT : SERVER_TIMEOUT);
    server_list.push_back(si);

    if (newServerCallback) newServerCallback(si);
}

void ServerScanner::addCallbacks(std::function<void(const ServerInfo&)> newServerCallbackIn, std::function<void(const ServerInfo&)> removedServerCallbackIn)
{
    this->newServerCallback = newServerCallbackIn;
    this->removedServerCallback = removedServerCallbackIn;
}

std::vector<ServerScanner::ServerInfo> ServerScanner::getServerList()
{
    return server_list;
}

void ServerScanner::masterServerScanThread()
{
    if (!master_server_url.startswith("http://"))
    {
        LOG(Error, "[sp-serverscan] Registration server URL ", master_server_url, " doesn't start with \"http://\".");
        return;
    }

    string hostname = master_server_url.substr(7);
    int path_start = hostname.find("/");
    if (path_start < 0)
    {
        LOG(Error, "[sp-serverscan] Registration server URL ", master_server_url, " doesn't have a URI after the hostname.");
        return;
    }

    int port = 80;
    int port_start = hostname.find(":");
    string uri = hostname.substr(path_start);

    if (port_start >= 0)
    {
        // If a port is attached to the hostname, parse it out.
        // No validation is performed.
        port = hostname.substr(port_start + 1, path_start).toInt();
        hostname = hostname.substr(0, port_start);
        LOG(Info, "[sp-serverscan] Port definition detected: ", hostname, ":", port);
    }
    else hostname = hostname.substr(0, path_start);

    LOG(Info, "[sp-serverscan] Reading servers from registration server ", master_server_url);

    sp::io::http::Request http(hostname, port);
    while (!isDestroyed() && master_server_url != "")
    {
        auto response = http.get(uri);
        if (response.status != 200)
            LOG(Warning, "[sp-serverscan] Failed to query registration server ", master_server_url, " (status ", response.status, ").");

        for (string line : response.body.split("\n"))
        {
            std::vector<string> parts = line.split(":", 3);
            if (parts.size() == 4)
            {
                sp::io::network::Address address(parts[0]);
                int part_port = parts[1].toInt();
                int version = parts[2].toInt();
                string name = parts[3];

                // Server/clients compiled with version 0 ignore version checks.
                if (version == version_number
                    || version == 0
                    || version_number == 0
                ) {
                    master_server_list_mutex.lock();
                    master_server_update_list.push_back({ServerType::MasterServer, address, static_cast<uint64_t>(part_port), 0, name, {}});
                    master_server_list_mutex.unlock();
                }
            }
        }

        if (!isDestroyed() && master_server_url != "")
        {
            std::mutex wait_mutex;
            std::unique_lock<std::mutex> lk(wait_mutex);
            abort_wait.wait_for(lk, 10s);
        }
    }
}

void ServerScanner::destroy()
{
    PObject::destroy();
    abort_wait.notify_all();
}
