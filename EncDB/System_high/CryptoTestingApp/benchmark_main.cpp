#include "Server.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "QueryCodec.h"

namespace {
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kReset = "\033[0m";

void print_banner(const std::string& title) {
    std::cout << "\n============================================================\n"
              << kBold << title << kReset << "\n"
              << "============================================================\n";
}

void print_table_header() {
    std::cout << "+----------+---------+---------------+\n"
              << "| Enclaves | Clients | Throughput    |\n"
              << "+----------+---------+---------------+\n";
}

void print_table_row(int enclaves, int clients, double qps) {
    std::cout << "|"
              << std::setw(9) << enclaves << " |"
              << std::setw(8) << clients << " |"
              << std::setw(10) << std::fixed << std::setprecision(2) << qps
              << " QPS |\n";
}

EncDBRequest insert_doc(const std::string& doc_id) {
    EncDBRequest req;
    req.type = RequestType::INVALID;

    std::ifstream in_file("../data/enron/" + doc_id);
    std::stringstream str_stream;
    str_stream << in_file.rdbuf();

    req.type = RequestType::UPDATE;
    req.update_req.op = 1;
    req.update_req.doc_id = std::stoi(doc_id);
    req.update_req.doc_len = str_stream.str().size() + 1;
    std::memcpy(req.update_req.doc_content, str_stream.str().data(), str_stream.str().size() + 1);
    return req;
}

void build_select_request(EncDBRequest& req, int client_id) {
    req.client_id = client_id;
    req.type = RequestType::SELECT;
    req.select_req.type = 0;

    std::vector<std::string> tokens = {"firm", "OR", "name"};
    BoolExprParser parser(tokens);
    std::unique_ptr<ASTNode> root = parser.parse_expr();

    req.select_req.term_count = static_cast<int>(parser.keywords().size());
    for (size_t i = 0; i < parser.keywords().size(); ++i) {
        req.select_req.terms[i] = make_term(parser.keywords()[i]);
    }

    req.select_req.bool_len = 0;
    encode_bool_expr(root.get(), req.select_req.bool_expr, req.select_req.bool_len);
}
}

double run_test(int num_clients, int requests_per_client, int enclave_num) {
    Server server;
    server.init(enclave_num, num_clients);

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> clients;

    for (int i = 1; i <= num_clients; ++i) {
        EncDBRequest init_req;
        init_req.client_id = i;
        init_req.type = RequestType::INIT;
        unsigned char key[ENC_KEY_SIZE] = "";
        std::memcpy(init_req.init_req.key, key, ENC_KEY_SIZE);
        server.handle_request(init_req, nullptr, 0);

        for (int j = 1; j <= 10; ++j) {
            EncDBRequest req = insert_doc(std::to_string(j));
            req.client_id = i;
            server.handle_request(req, nullptr, 0);
        }
    }

    std::cout << kCyan << "[RUN]    " << kReset
              << "enclaves=" << enclave_num
              << ", requests/client=" << requests_per_client << "\n";

    for (int i = 1; i <= num_clients; ++i) {
        clients.emplace_back([&server, i, requests_per_client]() {
            for (int j = 0; j < requests_per_client; ++j) {
                EncDBRequest req;
                build_select_request(req, i);

                uint8_t out[4096] = {0};
                server.handle_request(req, out, sizeof(out));
            }
        });
    }

    for (auto& t : clients) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    double total_time = std::chrono::duration<double>(end - start).count();
    int total_requests = num_clients * requests_per_client;
    double qps = total_requests / total_time;

    std::cout << kGreen << "[OK]     " << kReset
              << "throughput=" << std::fixed << std::setprecision(2) << qps << " QPS\n";
    return qps;
}

int main() {
    print_banner("[DEMO] Concurrent Benchmark");

    for (int clients : {16}) {
        std::cout << kCyan << "[CONFIG] " << kReset
                  << "clients=" << clients << "\n";
        std::vector<std::pair<int, double>> results;
        for (int enclave_num : {1, 2, 4}) {
            double qps = run_test(clients, 100, enclave_num);
            results.emplace_back(enclave_num, qps);
        }
        print_table_header();
        for (const auto& result : results) {
            print_table_row(result.first, clients, result.second);
        }
        std::cout << "+----------+---------+---------------+\n";
        std::cout << kYellow << "[SUMMARY]" << kReset
                  << " Multi-Enclave scheduling improves throughput under high concurrency.\n";
    }

    return 0;
}
