#include "net.h"
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>

static bool send_all(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool recv_all(int fd, uint8_t* data, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, data + recvd, len - recvd, 0);
        if (n <= 0) return false;
        recvd += n;
    }
    return true;
}

bool send_buffer(int sockfd, const std::vector<uint8_t>& buf) {
    uint32_t len = htonl(buf.size());
    if (!send_all(sockfd, reinterpret_cast<uint8_t*>(&len), 4))
        return false;
    return send_all(sockfd, buf.data(), buf.size());
}

bool recv_buffer(int sockfd, std::vector<uint8_t>& buf) {
    uint32_t len_net;
    if (!recv_all(sockfd, reinterpret_cast<uint8_t*>(&len_net), 4))
        return false;

    uint32_t len = ntohl(len_net);
    buf.resize(len);
    return recv_all(sockfd, buf.data(), len);
}
