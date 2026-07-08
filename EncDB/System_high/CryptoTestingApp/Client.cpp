#include "Client.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/rand.h>
#include <cstdio>

namespace {

const char* select_type_name(uint8_t type) {
    switch (type) {
    case 1: return "MAX";
    case 2: return "MIN";
    case 3: return "SUM";
    case 4: return "AVG";
    default: return "SELECT";
    }
}

void parse_aggregate_meta(const char* content, int& match_count) {
    match_count = 0;
    if (!content) {
        return;
    }
    const char* key = "match_count=";
    const char* pos = std::strstr(content, key);
    if (pos) {
        match_count = std::atoi(pos + std::strlen(key));
    }
}
uint8_t parse_select_type(const std::vector<std::string>& tokens, int& expr_begin) {
    expr_begin = 1;
    if (tokens.size() > 1) {
        if (tokens[1] == "MAX") {
            expr_begin = 2;
            return 1;
        }
        if (tokens[1] == "MIN") {
            expr_begin = 2;
            return 2;
        }
        if (tokens[1] == "SUM") {
            expr_begin = 2;
            return 3;
        }
        if (tokens[1] == "AVG") {
            expr_begin = 2;
            return 4;
        }
    }
    return 0;
}
}

Client::Client(uint32_t id) : client_id(id) {
    RAND_bytes(KF, ENC_KEY_SIZE);
}

Request Client::build_request(const std::string& sql) {
    auto tokens = tokenize_query(sql);
    if (tokens.empty()) {
        EncDBRequest invalid_req{};
        invalid_req.type = RequestType::INVALID;
        return serialize_encdb_request(invalid_req, client_id);
    }

    const std::string command = to_upper(tokens[0]);
    if (command == "INIT") {
        return build_init_request(tokens);
    }
    if (command == "INSERT") {
        return build_update_request(tokens, true);
    }
    if (command == "DELETE") {
        return build_update_request(tokens, false);
    }
    if (command == "SELECT") {
        return build_select_request(tokens);
    }

    EncDBRequest invalid_req{};
    invalid_req.client_id = client_id;
    invalid_req.type = RequestType::INVALID;
    return serialize_encdb_request(invalid_req, client_id);
}

Request Client::build_init_request(const std::vector<std::string>& tokens) {
    client_id = tokens.size() > 1 ? std::stoi(tokens[1]) : client_id;

    EncDBRequest req{};
    req.client_id = client_id;
    req.type = RequestType::INIT;
    std::memcpy(req.init_req.key, KF, ENC_KEY_SIZE);
    return serialize_encdb_request(req, client_id);
}

Request Client::build_update_request(const std::vector<std::string>& tokens, bool is_insert) {
    EncDBRequest req{};
    req.client_id = client_id;
    req.type = RequestType::INVALID;

    if (tokens.size() < 2) {
        return serialize_encdb_request(req, client_id);
    }

    std::string document_content;
    if (!load_document_by_id(tokens[1], document_content)) {
        return serialize_encdb_request(req, client_id);
    }

    req.type = RequestType::UPDATE;
    req.update_req.flags = UPDATE_LEGACY;
    req.update_req.op = is_insert ? OP_INSERT : OP_DELETE;
    req.update_req.doc_id = std::stoi(tokens[1]);
    req.update_req.index_len = 0;
    req.update_req.doc_len = static_cast<int>(document_content.size()) + 1;
    std::memcpy(req.update_req.doc_content, document_content.c_str(), document_content.size() + 1);
    return serialize_encdb_request(req, client_id);
}

Request Client::build_select_request(const std::vector<std::string>& tokens) {
    EncDBRequest req{};
    req.client_id = client_id;
    req.type = RequestType::INVALID;

    if (tokens.size() < 2) {
        return serialize_encdb_request(req, client_id);
    }

    req.type = RequestType::SELECT;

    int expr_begin = 1;
    req.select_req.type = parse_select_type(tokens, expr_begin);
    last_select_type_ = req.select_req.type;

    std::vector<std::string> expr_tokens(tokens.begin() + expr_begin, tokens.end());
    BoolExprParser parser(expr_tokens);
    std::unique_ptr<ASTNode> root = parser.parse_expr();

    req.select_req.term_count = static_cast<int>(parser.keywords().size());
    for (size_t i = 0; i < parser.keywords().size(); ++i) {
        req.select_req.terms[i] = make_term(parser.keywords()[i]);
    }

    req.select_req.bool_len = 0;
    encode_bool_expr(root.get(), req.select_req.bool_expr, req.select_req.bool_len);
    return serialize_encdb_request(req, client_id);
}

bool Client::load_document_by_id(const std::string& doc_id, std::string& content) const {
    std::ifstream in_file(raw_doc_dir + doc_id);
    if (!in_file.is_open()) {
        return false;
    }

    std::stringstream str_stream;
    str_stream << in_file.rdbuf();
    content = str_stream.str();
    return true;
}

void Client::handle_response(const Response& resp) {
    EncDBResponse enc_resp = deserialize_encdb_response(resp);
    if (enc_resp.status != ResponseStatus::OK) {
        std::cout << "[Client] Error response\n";
        return;
    }

    if (enc_resp.type == ResponseType::INIT_RESULT) {
        print_init_response(enc_resp.init_resp);
        return;
    }
    if (enc_resp.type == ResponseType::SELECT_RESULT) {
        print_select_response(enc_resp.select_resp);
        return;
    }
    if (enc_resp.type == ResponseType::UPDATE_RESULT) {
        print_update_response(enc_resp.update_resp);
    }
}

void Client::print_init_response(const InitResponse& response) const {
    std::cout << "[Client] INIT result, edb_id = "
              << static_cast<uint32_t>(response.edb_id) << "\n";
}

void Client::print_select_response(const SelectResponse& response) const {
    if (last_select_type_ >= 1 && last_select_type_ <= 4) {
        int match_count = 0;
        std::string meta((char*)response.doc_contents[0]);
        parse_aggregate_meta(meta.c_str(), match_count);

        std::cout << "[Client] " << select_type_name(last_select_type_)
                  << " aggregate result\n";
        std::cout << "matched_docs = " << match_count << "\n";
        std::cout << "value = " << (response.doc_count > 0 ? response.doc_ids[0] : 0) << "\n";
        if (!meta.empty()) {
            std::cout << "meta = " << meta << "\n";
        }
        std::cout << "\n";
        return;
    }

    std::cout << "[Client] SELECT result, doc_count = "
              << response.doc_count << "\n";
    if (response.doc_count == 0) {
        std::cout << "(no documents)\n\n";
        return;
    }

    for (int i = 0; i < response.doc_count; ++i) {
        std::cout << "Document " << response.doc_ids[i] << ":\n";
        std::string doc_content((char*)response.doc_contents[i]);
        std::cout << doc_content << "\n";
    }
    std::cout << "\n";
}

void Client::print_update_response(const UpdateResponse& response) const {
    std::cout << "[Client] UPDATE result: success="
              << response.success << " doc_id=" << response.doc_id << "\n";
}
