#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "EncDBRequest.h"
#include "Utils.h"

enum class wireRequestType : uint32_t {
    INIT = 0,
    UPDATE = 1,
    SELECT = 2,
    INVALID = 3
};

struct Request {
    uint32_t client_id;
    wireRequestType type;
    std::vector<uint8_t> payload;
};

struct Response {
    uint32_t client_id;
    std::vector<uint8_t> payload;
};

Request serialize_encdb_request(const EncDBRequest& enc_req, uint32_t client_id);
EncDBRequest deserialize_encdb_request(const Request& req);

Response serialize_encdb_response(const EncDBResponse& enc_resp, uint32_t client_id);
EncDBResponse deserialize_encdb_response(const Response& resp);

class SecureChannel {
public:
    void set_key(const std::vector<uint8_t>& k) {
        session_key = k;
    }

    std::vector<uint8_t> encrypt(std::vector<uint8_t>& data);
    std::vector<uint8_t> decrypt(std::vector<uint8_t>& data);

private:
    std::vector<uint8_t> session_key;
};
