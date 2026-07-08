/***
 * Client-side request builder and response printer.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include <string>
#include <vector>

#include "EncDBRequest.h"
#include "QueryCodec.h"
#include "RPC.h"

#define ENC_KEY_SIZE 16

class Client {
public:
    explicit Client(uint32_t id);

    Request build_request(const std::string& sql);
    void handle_response(const Response& resp);

private:
    Request build_init_request(const std::vector<std::string>& tokens);
    Request build_update_request(const std::vector<std::string>& tokens, bool is_insert);
    Request build_select_request(const std::vector<std::string>& tokens);

    void print_init_response(const InitResponse& response) const;
    void print_select_response(const SelectResponse& response) const;
    void print_update_response(const UpdateResponse& response) const;

    bool load_document_by_id(const std::string& doc_id, std::string& content) const;

    uint32_t client_id;
    unsigned char KF[ENC_KEY_SIZE] = "abc";
    uint8_t last_select_type_ = 0; // 0 rows, 1-4 aggregate
};

#endif
