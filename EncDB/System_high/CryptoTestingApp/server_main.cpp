#include "Server.h"
#include "net.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#include <sys/select.h>

namespace {
constexpr int kPort = 9000;

uint64_t time_since_epoch_microsec() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}
}

int main() {
    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);

    Server server;
    server.init();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[Server] Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Server] Failed to bind on port " << kPort << "\n";
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 1) < 0) {
        std::cerr << "[Server] Failed to listen on port " << kPort << "\n";
        close(listen_fd);
        return 1;
    }

    std::cout << "[Server] Listening on port " << kPort << "\n";

    while (!g_shutdown_requested) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        const int ready = select(listen_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (g_shutdown_requested) {
                break;
            }
            std::cerr << "[Server] select failed\n";
            continue;
        }
        if (ready == 0) {
            continue;
        }

        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (g_shutdown_requested) {
                break;
            }
            std::cerr << "[Server] Failed to accept client\n";
            continue;
        }

        std::cout << "[Server] Client connected\n";

        while (!g_shutdown_requested) {
            std::vector<uint8_t> recv_buf;
            if (!recv_buffer(client_fd, recv_buf)) {
                std::cout << "[Server] Client disconnected\n";
                break;
            }

            Request req;
            req.payload = std::move(recv_buf);

            uint64_t t_start = time_since_epoch_microsec();
            Response resp = server.process(req);
            uint64_t t_end = time_since_epoch_microsec();

            std::cout << "[Server] Request processed in "
                      << (t_end - t_start) << " us\n";

            send_buffer(client_fd, resp.payload);
        }

        close(client_fd);
    }

    std::cout << "[Server] Shutting down, flushing databases...\n";
    server.shutdown_flush();
    close(listen_fd);
    return 0;
}
