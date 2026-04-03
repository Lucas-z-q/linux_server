#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @file TcpConnection.h
 * @brief Wrapper around a connected TCP socket descriptor.
 */

/**
 * @brief Represents one established TCP client connection.
 *
 * This class owns the connection file descriptor and closes it on destruction.
 */
class TcpConnection {
public:
    /**
     * @brief Constructs a connection wrapper.
     * @param conn_fd Connected socket file descriptor.
     * @param peer_ip Remote peer IPv4 string.
     * @param peer_port Remote peer TCP port.
     */
    TcpConnection(int conn_fd, const std::string& peer_ip, uint16_t peer_port);

    /**
     * @brief Destroys the connection and closes the socket if still open.
     */
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    /**
     * @brief Receives bytes from the peer.
     * @param buffer Output buffer for received bytes.
     * @param size Maximum number of bytes to receive.
     * @return Number of bytes received, 0 on peer close, or -1 on error.
     */
    ssize_t recv(char* buffer, size_t size);

    /**
     * @brief Sends all bytes in the given buffer.
     * @param data Input byte buffer.
     * @param len Number of bytes to send.
     * @return true if all bytes are sent, otherwise false.
     */
    bool sendAll(const char* data, size_t len);

    /**
     * @brief Closes the underlying socket if valid.
     */
    void close();

    /**
     * @brief Checks whether the connection socket is valid.
     * @return true if socket descriptor is valid, otherwise false.
     */
    bool isValid() const;

    /**
     * @brief Gets the underlying socket descriptor.
     * @return Connection file descriptor.
     */
    int fd() const;

    /**
     * @brief Gets the remote peer IP address.
     * @return Remote IPv4 address string.
     */
    std::string peerIp() const;

    /**
     * @brief Gets the remote peer port.
     * @return Remote TCP port.
     */
    uint16_t peerPort() const;

private:
    int conn_fd_;
    std::string peer_ip_;
    uint16_t peer_prot_;
};
