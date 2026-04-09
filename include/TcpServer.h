#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include "ConnectionMeta.h"
#include "IMessageHandler.h"

/**
 * @file TcpServer.h
 * @brief Declares a simple blocking TCP server.
 */

/**
 * @brief Blocking TCP server that dispatches client messages to a handler.
 */
class TcpServer
{
public:
    /**
     * @brief Constructs a TCP server instance.
     * @param ip IPv4 address string (currently not used by bind, kept for API).
     * @param port Local TCP port to listen on.
     * @param handler Message handler used to process requests.
     */
    TcpServer(const std::string &ip, uint16_t port, IMessageHandler &handler);

    /**
     * @brief Releases server resources.
     */
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    /**
     * @brief Starts listening and enters the accept loop.
     * @return true if startup succeeds and accept loop runs, otherwise false.
     */
    bool start();

    /**
     * @brief Stops listening and closes the listen socket.
     */
    void stop();

private:
    /**
     * @brief Creates the listening socket.
     * @return true on success, otherwise false.
     */
    bool createListenSocket();

    /**
     * @brief Binds the listening socket to the configured local port.
     * @return true on success, otherwise false.
     */
    bool bindAddress();

    /**
     * @brief Marks the listening socket as passive.
     * @return true on success, otherwise false.
     */
    bool startListen();

    /**
     * @brief Runs the epoll event loop.
     * @param epoll_fd epoll instance descriptor.
     */
    void acceptLoop(int epoll_fd);

private:
    int listen_fd_;
    int epoll_fd_;
    std::string ip_;
    uint16_t port_;
    IMessageHandler &handler_;

    std::atomic<uint64_t> next_conn_id_{1}; // 连接ID生成器
    std::mutex connections_mutex_;
    std::unordered_map<uint64_t, ConnectionMeta> connections_; // 活跃连接
    // Maps runtime socket fd to server-side conn_id for lifecycle bookkeeping.
    std::mutex fd_to_conn_id_mutex;
    std::unordered_map<int, uint64_t> fd_to_conn_id;

    /**
     * @brief Registers a newly accepted connection and returns its unique ID.
     * @param conn_fd Connected socket descriptor.
     * @param peer_ip Remote peer IPv4 string.
     * @param peer_port Remote peer TCP port.
     * @return Server-side unique connection id.
     */
    uint64_t registerConnection(int conn_fd, const std::string &peer_ip, uint16_t peer_port);

    /**
     * @brief Updates connection activity and receive statistics.
     * @param conn_id Server-side unique connection id.
     * @param bytes Number of received bytes for this message.
     */
    void touchOnRecv(uint64_t conn_id, size_t bytes);

    /**
     * @brief Updates connection activity and send statistics.
     * @param conn_id Server-side unique connection id.
     * @param bytes Number of sent bytes for this message.
     */
    void touchOnSend(uint64_t conn_id, size_t bytes);

    /**
     * @brief Removes a connection from active table and logs close summary.
     * @param conn_id Server-side unique connection id.
     * @param reason Human-readable close reason.
     */
    void unregisterConnection(uint64_t conn_id, const std::string &reason);

    /**
     * @brief Prints one connection metadata log entry.
     * @param meta Connection metadata snapshot.
     */
    void logConnectionMeta(const ConnectionMeta &meta);

    /**
     * @brief Sets a socket descriptor to non-blocking mode.
     * @param fd Socket descriptor.
     * @return true on success, otherwise false.
     */
    bool set_nonblocking(int fd);

    /**
     * @brief Closes one client socket and removes its metadata from all tables.
     * @param fd Client socket descriptor.
     * @param reason Human-readable close reason used in connection logs.
     */
    void closeClientFd(int fd, const std::string &reason);

    void onReadable(int fd); // 负责读事件主流程

    bool getConnIdByFd(int fd, uint64_t &conn_id); // 统一封装映射查询和加锁
};
