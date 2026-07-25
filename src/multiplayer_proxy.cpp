#include "multiplayer_proxy.h"
#include "multiplayer_internal.h"
#include "engine.h"


GameServerProxy::GameServerProxy(sp::io::network::Address hostname, int hostPort, string password, int listenPort, string proxy_name)
: password(password), proxy_name(proxy_name)
{
    LOG(INFO) << "Starting proxy server";
    main_socket = std::make_unique<sp::io::network::TcpSocket>();
    if (!main_socket->connect(hostname, static_cast<uint16_t>(hostPort)))
        LOG(INFO) << "Failed to connect to server";
    else
        LOG(INFO) << "Connected to server";
    main_socket->setBlocking(false);
    listen_socket.listen(static_cast<uint16_t>(listenPort));
    listen_socket.setBlocking(false);

    new_socket = std::make_unique<sp::io::network::TcpSocket>();
    new_socket->setBlocking(false);

    broadcast_server_delay = 0.0f;
    if (proxy_name != "")
    {
        if (!broadcast_listen_socket.bind(static_cast<uint16_t>(listenPort)))
        {
            LOG(ERROR) << "Failed to listen on UDP port: " << listenPort;
        }
        if (!broadcast_listen_socket.joinMulticast(666))
        {
            LOG(ERROR) << "Failed to join multicast group for local network discovery.";
        }
        broadcast_listen_socket.setBlocking(false);
    }

    no_data_timeout.start(NO_DATA_DISCONNECT_TIME);
    heartbeat_timer.start(HEARTBEAT_TIME);
}

GameServerProxy::GameServerProxy(string password, int listenPort, string proxy_name)
: password(password), proxy_name(proxy_name)
{
    LOG(INFO) << "Starting listening proxy server";
    listen_socket.listen(static_cast<uint16_t>(listenPort));
    listen_socket.setBlocking(false);

    new_socket = std::make_unique<sp::io::network::TcpSocket>();
    new_socket->setBlocking(false);

    broadcast_server_delay = 0.0f;
    if (proxy_name != "")
    {
        if (!broadcast_listen_socket.bind(static_cast<uint16_t>(listenPort)))
        {
            LOG(ERROR) << "Failed to listen on UDP port: " << listenPort;
        }
        broadcast_listen_socket.setBlocking(false);
    }

    no_data_timeout.start(NO_DATA_DISCONNECT_TIME);
    heartbeat_timer.start(HEARTBEAT_TIME);
}

GameServerProxy::~GameServerProxy()
{
}

void GameServerProxy::destroy()
{
    client_list.clear();

    broadcast_listen_socket.close();
}

void GameServerProxy::update(float delta)
{
    if (main_socket)
    {
        sp::io::DataBuffer packet;
        while(main_socket->receive(packet))
        {
            no_data_timeout.start(NO_DATA_DISCONNECT_TIME);
            heartbeat_timer.start(HEARTBEAT_TIME);
            command_t command;
            packet >> command;
            switch(command)
            {
            case CMD_REQUEST_AUTH:
                {
                    bool requirePassword;
                    packet >> server_version >> requirePassword;

                    sp::io::DataBuffer reply;
                    reply << CMD_CLIENT_SEND_AUTH << int32_t(server_version) << string(password);
                    main_socket->send(reply);
                }
                break;
            case CMD_SET_CLIENT_ID:
                packet >> client_id;
                break;
            case CMD_ALIVE:
                {
                    sp::io::DataBuffer reply;
                    reply << CMD_ALIVE_RESP;
                    main_socket->send(reply);
                }
                sendAll(packet);
                break;
            case CMD_ALIVE_RESP:
                break;
            case CMD_CREATE:
            case CMD_DELETE:
            case CMD_UPDATE_VALUE:
            case CMD_SET_GAME_SPEED:
            case CMD_SERVER_COMMAND:
            case CMD_AUDIO_COMM_START:
            case CMD_AUDIO_COMM_DATA:
            case CMD_AUDIO_COMM_STOP:
            case CMD_ECS_UPDATE:
                sendAll(packet);
                break;
            case CMD_PROXY_TO_CLIENTS:
                {
                    while(packet.available())
                    {
                        int32_t id;
                        packet >> id;
                        targetClients.insert(id);
                    }
                }
                break;
            case CMD_SET_PROXY_CLIENT_ID:
                {
                    int32_t tempId, proxied_clientId;
                    packet >> tempId >> proxied_clientId;
                    for(auto& info : client_list)
                    {
                        if (!info.validClient && info.client_id == tempId)
                        {
                            info.validClient = true;
                            info.client_id = proxied_clientId;
                            info.receiveState = CRS_Main;
                            {
                                sp::io::DataBuffer proxied_packet;
                                proxied_packet << CMD_SET_CLIENT_ID << info.client_id;
                                info.socket->send(proxied_packet);
                            }
                        }
                    }
                }
                break;
            default:
                LOG(ERROR) << "Unknown command from server: " << command;
                break;
            }
        }

        if (heartbeat_timer.isExpired())
        {
            heartbeat_timer.start(HEARTBEAT_TIME);
            sp::io::DataBuffer ping;
            ping << CMD_ALIVE;
            main_socket->send(ping);
        }

        if (main_socket->getState() == sp::io::network::StreamSocket::State::Closed || no_data_timeout.isExpired())
        {
            LOG(INFO) << "Disconnected proxy";
            main_socket->close();
            engine->shutdown();
        }
    }

    if (proxy_name != "")
    {
        handleBroadcastUDPSocket(delta);
    }

    if (listen_socket.accept(*new_socket))
    {
        ClientInfo info;
        info.socket = std::move(new_socket);
        new_socket = std::make_unique<sp::io::network::TcpSocket>();
        new_socket->setBlocking(false);
        {
            sp::io::DataBuffer packet;
            packet << CMD_REQUEST_AUTH << int32_t(server_version) << bool(password != "");
            info.socket->send(packet);
        }
        client_list.emplace_back(std::move(info));
    }

    for(unsigned int n=0; n<client_list.size(); n++)
    {
        sp::io::DataBuffer packet;
        auto& info = client_list[n];
        while(info.socket && info.socket->receive(packet))
        {
            command_t command;
            packet >> command;
            switch(info.receiveState)
            {
            case CRS_Auth:
                switch(command)
                {
                case CMD_SERVER_CONNECT_TO_PROXY:
                    if (main_socket)
                    {
                        info.socket->close();
                        info.socket = NULL;
                    }
                    else
                    {
                        main_socket = std::move(info.socket);
                        no_data_timeout.start(NO_DATA_DISCONNECT_TIME);
                        heartbeat_timer.start(HEARTBEAT_TIME);
                    }
                    break;
                case CMD_CLIENT_SEND_AUTH:
                    {
                        int32_t clientVersion;
                        string clientPassword;
                        packet >> clientVersion >> clientPassword;
                        if (main_socket && clientVersion == server_version && clientPassword == password)
                        {
                            sp::io::DataBuffer serverUpdate;
                            serverUpdate << CMD_NEW_PROXY_CLIENT << info.client_id;
                            main_socket->send(serverUpdate);
                        }
                        else
                        {
                           info.socket->close();
                           info.socket = NULL;
                        }
                    }
                    break;
                case CMD_ALIVE_RESP:
                    break;
                default:
                    LOG(ERROR) << "Unknown command from client: " << command;
                    break;
                }
                break;
            case CRS_Main:
                switch(command)
                {
                case CMD_CLIENT_COMMAND:
                    packet >> info.commandObjectId;
                    info.receiveState = CRS_Command;
                    break;
                case CMD_AUDIO_COMM_START:
                case CMD_AUDIO_COMM_DATA:
                case CMD_AUDIO_COMM_STOP:
                    {
                        int32_t client_id = 0;
                        packet >> client_id;
                        if (client_id == info.client_id)
                            main_socket->send(packet);
                    }
                    break;
                case CMD_ALIVE_RESP:
                    break;
                case CMD_ALIVE:
                    {
                        sp::io::DataBuffer reply;
                        reply << CMD_ALIVE_RESP;
                        info.socket->send(reply);
                    }
                    break;
                default:
                    LOG(ERROR) << "Unknown command from client: " << command;
                    break;
                }
                break;
            case CRS_Command:
                {
                    sp::io::DataBuffer mainPacket;
                    mainPacket << CMD_PROXY_CLIENT_COMMAND << info.commandObjectId << info.client_id;
                    main_socket->send(mainPacket);
                    main_socket->send(packet);
                }
                info.receiveState = CRS_Main;
                break;
            }
        }
        if (info.socket == NULL || info.socket->getState() == sp::io::network::StreamSocket::State::Closed)
        {
            if (info.validClient)
            {
                sp::io::DataBuffer serverUpdate;
                serverUpdate << CMD_DEL_PROXY_CLIENT << info.client_id;
                main_socket->send(serverUpdate);
            }
            client_list.erase(client_list.begin() + n);
            n--;
        }
    }
}

void GameServerProxy::sendAll(sp::io::DataBuffer& packet)
{
    if (targetClients.empty())
    {
        for(auto& info : client_list)
        {
            if (info.validClient && info.socket)
                info.socket->send(packet);
        }
    }
    else
    {
        for(auto& info : client_list)
        {
            if (info.validClient && info.socket && targetClients.find(info.client_id) != targetClients.end())
                info.socket->send(packet);
        }
        targetClients.clear();
    }
}

void GameServerProxy::handleBroadcastUDPSocket(float delta)
{
    sp::io::network::Address recvAddress;
    int recvPort;
    sp::io::DataBuffer recvPacket;
    if (broadcast_listen_socket.receive(recvPacket, recvAddress, recvPort))
    {
        //We do not care about what we received. Reply that we live!
        sp::io::DataBuffer sendPacket;
        sendPacket << int32_t(MULTIPLAYER_VERIFICATION_NUMBER) << int32_t(server_version) << proxy_name;
        broadcast_listen_socket.send(sendPacket, recvAddress, recvPort);
    }
    if (broadcast_server_delay > 0.0f)
    {
        broadcast_server_delay -= delta;
    }else{
        broadcast_server_delay = 5.0f;

        sp::io::DataBuffer sendPacket;
        sendPacket << int32_t(MULTIPLAYER_VERIFICATION_NUMBER) << int32_t(server_version) << proxy_name;
        broadcast_listen_socket.sendMulticast(sendPacket, 666, 35667);
    }
}
