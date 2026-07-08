#include "Client.h"
#include "net.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr int kPort = 9000;
constexpr const char* kServerAddress = "127.0.0.1";

bool connect_to_server(int& sockfd) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return false;
    }

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(kPort);
    serv.sin_addr.s_addr = inet_addr(kServerAddress);

    if (connect(sockfd, (sockaddr*)&serv, sizeof(serv)) != 0) {
        close(sockfd);
        sockfd = -1;
        return false;
    }

    return true;
}

bool read_user_command(std::string& sql_query) {
    std::cout << "Enter SQL query(or EXIT to quit): ";
    return static_cast<bool>(std::getline(std::cin, sql_query));
}
}

int main() {
    int sockfd = -1;
    if (!connect_to_server(sockfd)) {
        std::cerr << "[Client] Failed to connect to server\n";
        return 1;
    }

    std::cout << "[Client] Connected to server\n";
    Client client(1);

    while (true) {
        std::string sql_query;
        if (!read_user_command(sql_query) || sql_query == "EXIT") {
            break;
        }

        Request req = client.build_request(sql_query);
        send_buffer(sockfd, req.payload);

        std::vector<uint8_t> recv_buf;
        if (!recv_buffer(sockfd, recv_buf)) {
            std::cerr << "[Client] Server disconnected\n";
            break;
        }

        Response resp;
        resp.payload = std::move(recv_buf);
        client.handle_response(resp);
    }

    close(sockfd);
    return 0;
}
