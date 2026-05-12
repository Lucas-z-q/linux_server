// ConnectionContext 的实现。
// 为单个客户端连接提供状态管理、缓冲机制和统计信息跟踪。

#include "net/ConnectionContext.h"

#include <chrono>
#include <utility>

ConnectionContext::ConnectionContext(int fd, chat::ConnectionId conn_id, const std::string& peer_ip, uint16_t peer_port)
    : fd_(fd), conn_id_(conn_id), request_in_flight_(false) {
    // 使用默认值初始化连接元数据。
    meta_.conn_id = conn_id;
    meta_.fd = fd;
    meta_.peer_ip = peer_ip;
    meta_.peer_port = peer_port;
    meta_.connected_at = std::chrono::system_clock::now();
    meta_.last_active_at = std::chrono::steady_clock::now();
    meta_.recv_count = 0;
    meta_.send_count = 0;
    meta_.recv_bytes = 0;
    meta_.sent_bytes = 0;
    meta_.state = ConnectionMeta::State::CONNECTED;
}

int ConnectionContext::fd() const { return fd_; }

chat::ConnectionId ConnectionContext::conn_id() const { return conn_id_; }

const ConnectionMeta& ConnectionContext::meta() const { return meta_; }

bool ConnectionContext::feedPacketData(const std::string& chunk, std::vector<std::string>& packets) {
    return packet_codec_.feed(chunk, packets);
}

void ConnectionContext::appendPendingSend(const std::string& data) { pending_send_ += data; }

bool ConnectionContext::hasPendingSend() const { return !pending_send_.empty(); }

bool ConnectionContext::peekPendingSend(std::string& out) const {
    if (pending_send_.empty()) {
        out.clear();
        return false;
    }
    out = pending_send_;
    return true;
}

void ConnectionContext::consumePendingSend(size_t bytes) {
    if (bytes == 0)
        return;

    // 如果请求的字节数大于或等于待发送数据的总大小，则清空整个缓冲区。
    if (bytes >= pending_send_.size()) {
        pending_send_.clear();
        return;
    }
    pending_send_.erase(0, bytes);
}

void ConnectionContext::clearPendingSend() { pending_send_.clear(); }

bool ConnectionContext::startRequestOrQueue(const std::string& request) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (!request_in_flight_) {
        request_in_flight_ = true;
        return true;
    }

    pending_requests_.push(request);
    return false;
}

bool ConnectionContext::finishRequestAndPopNext(std::string& next_request) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (pending_requests_.empty()) {
        request_in_flight_ = false;
        next_request.clear();
        return false;
    }

    next_request = std::move(pending_requests_.front());
    pending_requests_.pop();
    request_in_flight_ = true;
    return true;
}

void ConnectionContext::clearPendingRequests() {
    std::lock_guard<std::mutex> lock(request_mutex_);
    request_in_flight_ = false;
    std::queue<std::string> empty;
    pending_requests_.swap(empty);
}

void ConnectionContext::touchOnRecv(size_t bytes) {
    meta_.last_active_at = std::chrono::steady_clock::now();
    meta_.recv_count += 1;
    meta_.recv_bytes += bytes;
}

void ConnectionContext::touchOnSend(size_t bytes) {
    meta_.last_active_at = std::chrono::steady_clock::now();
    meta_.send_count += 1;
    meta_.sent_bytes += bytes;
}

void ConnectionContext::markClosing() { meta_.state = ConnectionMeta::State::CLOSING; }

void ConnectionContext::markClosed() { meta_.state = ConnectionMeta::State::CLOSED; }
