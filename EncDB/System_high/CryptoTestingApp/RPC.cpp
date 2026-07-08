#include "RPC.h"

#include "EncDBDebug.h"

namespace {
constexpr size_t kRequestHeaderSize = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t kResponseHeaderSize = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    uint8_t* p = reinterpret_cast<uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

void append_i32(std::vector<uint8_t>& buf, int32_t v) {
    uint8_t* p = reinterpret_cast<uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

void append_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

uint32_t read_u32(const uint8_t*& p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    p += sizeof(v);
    return v;
}

int32_t read_i32(const uint8_t*& p) {
    int32_t v;
    std::memcpy(&v, p, sizeof(v));
    p += sizeof(v);
    return v;
}

uint8_t read_u8(const uint8_t*& p) {
    return *p++;
}

void write_request_body(std::vector<uint8_t>& buf, const EncDBRequest& enc_req) {
    if (enc_req.type == RequestType::INIT) {
        const InitRequest& init = enc_req.init_req;
        buf.insert(buf.end(), init.key, init.key + ENC_KEY_SIZE);
        append_u8(buf, init.mode);
        append_u32(buf, init.target_edb_id);
        return;
    }

    if (enc_req.type == RequestType::SHUTDOWN) {
        append_u8(buf, enc_req.shutdown_req.reserved);
        return;
    }

    if (enc_req.type == RequestType::SELECT) {
        const SelectRequest& select = enc_req.select_req;
        append_u32(buf, select.term_count);

        for (int i = 0; i < select.term_count; ++i) {
            append_u8(buf, select.terms[i].len);
            buf.insert(buf.end(), select.terms[i].data, select.terms[i].data + select.terms[i].len);
        }

        append_u32(buf, select.bool_len);
        buf.insert(buf.end(), select.bool_expr, select.bool_expr + select.bool_len);
        append_u8(buf, select.type);
        return;
    }

    if (enc_req.type == RequestType::UPDATE) {
        const UpdateRequest& update = enc_req.update_req;
        append_u8(buf, update.op);
        append_i32(buf, update.doc_id);
        if (update.flags == UPDATE_SPLIT) {
            append_u8(buf, UPDATE_WIRE_V1);
            append_u8(buf, update.flags);
            append_u32(buf, static_cast<uint32_t>(update.index_len));
            buf.insert(buf.end(), update.index_content,
                update.index_content + update.index_len);
            append_u32(buf, static_cast<uint32_t>(update.doc_len));
            buf.insert(buf.end(), update.doc_content, update.doc_content + update.doc_len);
        } else {
            append_u8(buf, UPDATE_WIRE_LEGACY_SINGLE);
            append_u32(buf, static_cast<uint32_t>(update.doc_len));
            buf.insert(buf.end(), update.doc_content, update.doc_content + update.doc_len);
        }
    }
}

void read_request_body(const uint8_t*& p, const uint8_t* body_end, EncDBRequest& enc_req) {
    if (enc_req.type == RequestType::INIT) {
        InitRequest& init = enc_req.init_req;
        std::memcpy(init.key, p, ENC_KEY_SIZE);
        p += ENC_KEY_SIZE;
        init.mode = INIT_MODE_CREATE;
        init.target_edb_id = 0;
        if (p < body_end) {
            init.mode = read_u8(p);
            if (p + sizeof(uint32_t) <= body_end) {
                init.target_edb_id = read_u32(p);
            }
        }
        return;
    }

    if (enc_req.type == RequestType::SHUTDOWN) {
        enc_req.shutdown_req.reserved = 0;
        if (p < body_end) {
            enc_req.shutdown_req.reserved = read_u8(p);
        }
        return;
    }

    if (enc_req.type == RequestType::SELECT) {
        SelectRequest& select = enc_req.select_req;
        select.term_count = read_u32(p);
        if (select.term_count > MAX_TERMS) {
            throw std::runtime_error("term_count overflow");
        }

        for (int i = 0; i < select.term_count; ++i) {
            select.terms[i].len = read_u8(p);
            std::memcpy(select.terms[i].data, p, select.terms[i].len);
            p += select.terms[i].len;
        }

        select.bool_len = read_u32(p);
        std::memcpy(select.bool_expr, p, select.bool_len);
        p += select.bool_len;
        select.type = read_u8(p);
#if ENCDB_SERVER_DEBUG
        fprintf(stderr,
            "[EncDB][RPC][deserialize] SELECT body select.type=%u bool_len=%d\n",
            static_cast<unsigned>(select.type),
            select.bool_len);
        encdb_debug_hex("RPC.select.bool_expr", select.bool_expr,
            static_cast<size_t>(select.bool_len));
#endif
        return;
    }

    if (enc_req.type == RequestType::UPDATE) {
        UpdateRequest& update = enc_req.update_req;
        update.op = read_u8(p);
        update.doc_id = read_i32(p);
        const uint8_t* tag_ptr = p;
        const uint8_t wire_tag = read_u8(p);
        if (wire_tag == UPDATE_WIRE_V1) {
            update.flags = read_u8(p);
            update.index_len = static_cast<int>(read_u32(p));
            if (update.index_len > MAX_INDEX_SIZE) {
                throw std::runtime_error("index_len overflow");
            }
            std::memcpy(update.index_content, p, update.index_len);
            p += update.index_len;
            update.doc_len = static_cast<int>(read_u32(p));
            if (update.doc_len > MAX_DOC_SIZE) {
                throw std::runtime_error("doc_len overflow");
            }
            std::memcpy(update.doc_content, p, update.doc_len);
            p += update.doc_len;
        } else if (wire_tag == UPDATE_WIRE_LEGACY_SINGLE) {
            update.flags = UPDATE_LEGACY;
            update.index_len = 0;
            update.doc_len = static_cast<int>(read_u32(p));
            if (update.doc_len > MAX_DOC_SIZE) {
                throw std::runtime_error("doc_len overflow");
            }
            std::memcpy(update.doc_content, p, update.doc_len);
            p += update.doc_len;
        } else {
            p = tag_ptr;
            update.flags = UPDATE_LEGACY;
            update.index_len = 0;
            update.doc_len = static_cast<int>(read_u32(p));
            if (update.doc_len > MAX_DOC_SIZE) {
                throw std::runtime_error("doc_len overflow");
            }
            std::memcpy(update.doc_content, p, update.doc_len);
            p += update.doc_len;
        }
    }
}

void write_response_body(std::vector<uint8_t>& buf, const EncDBResponse& enc_resp) {
    if (enc_resp.type == ResponseType::INIT_RESULT) {
        append_u8(buf, enc_resp.init_resp.edb_id);
        return;
    }

    if (enc_resp.type == ResponseType::SELECT_RESULT) {
        const SelectResponse& select = enc_resp.select_resp;
        append_u32(buf, select.doc_count);
        for (int i = 0; i < select.doc_count; ++i) {
            append_i32(buf, select.doc_ids[i]);
        }
        for (int i = 0; i < select.doc_count; ++i) {
            buf.insert(buf.end(), select.doc_contents[i], select.doc_contents[i] + MAX_DOC_SIZE);
        }
#if ENCDB_SERVER_DEBUG
        fprintf(stderr,
            "[EncDB][RPC][serialize] SELECT doc_count=%d doc_ids[0]=%d wire_body+=%zu\n",
            select.doc_count,
            select.doc_count > 0 ? select.doc_ids[0] : -1,
            static_cast<size_t>(select.doc_count) * MAX_DOC_SIZE);
        if (select.doc_count > 0) {
            encdb_debug_cstr("wire.doc_contents[0]", select.doc_contents[0]);
            encdb_debug_hex("wire.doc_contents[0].head", select.doc_contents[0], 48);
        }
#endif
        return;
    }

    if (enc_resp.type == ResponseType::UPDATE_RESULT) {
        const UpdateResponse& update = enc_resp.update_resp;
        append_u32(buf, update.success);
        append_i32(buf, update.doc_id);
        return;
    }

    if (enc_resp.type == ResponseType::SHUTDOWN_RESULT) {
        append_u8(buf, enc_resp.shutdown_resp.success);
    }
}

void read_response_body(const uint8_t*& p, EncDBResponse& enc_resp) {
    if (enc_resp.type == ResponseType::INIT_RESULT) {
        enc_resp.init_resp.edb_id = read_u8(p);
        return;
    }

    if (enc_resp.type == ResponseType::SELECT_RESULT) {
        SelectResponse& select = enc_resp.select_resp;
        select.doc_count = read_u32(p);
        if (select.doc_count > MAX_RESULT_DOCS) {
            throw std::runtime_error("doc_count overflow");
        }

        for (int i = 0; i < select.doc_count; ++i) {
            select.doc_ids[i] = read_i32(p);
        }

        for (int i = 0; i < select.doc_count; ++i) {
            std::memcpy(select.doc_contents[i], p, MAX_DOC_SIZE);
            p += MAX_DOC_SIZE;
        }
        return;
    }

    if (enc_resp.type == ResponseType::UPDATE_RESULT) {
        UpdateResponse& update = enc_resp.update_resp;
        update.success = read_u32(p);
        update.doc_id = read_i32(p);
        return;
    }

    if (enc_resp.type == ResponseType::SHUTDOWN_RESULT) {
        enc_resp.shutdown_resp.success = read_u8(p);
    }
}
}

Request serialize_encdb_request(const EncDBRequest& enc_req, uint32_t client_id) {
    Request req;
    req.client_id = client_id;
    req.type = static_cast<wireRequestType>(static_cast<uint32_t>(enc_req.type));

    auto& buf = req.payload;
    buf.reserve(kRequestHeaderSize);

    append_u8(buf, enc_req.client_id);
    append_u32(buf, static_cast<uint32_t>(enc_req.type));

    const size_t body_len_pos = buf.size();
    append_u32(buf, 0);
    const size_t body_start = buf.size();

    write_request_body(buf, enc_req);

    const uint32_t body_len = static_cast<uint32_t>(buf.size() - body_start);
    std::memcpy(buf.data() + body_len_pos, &body_len, sizeof(uint32_t));
    return req;
}

EncDBRequest deserialize_encdb_request(const Request& req) {
    EncDBRequest enc_req{};
    const uint8_t* p = req.payload.data();

    enc_req.client_id = read_u8(p);
    enc_req.type = static_cast<RequestType>(read_u32(p));
    const uint32_t body_len = read_u32(p);
    const uint8_t* body_end = p + body_len;

    read_request_body(p, body_end, enc_req);
    return enc_req;
}

Response serialize_encdb_response(const EncDBResponse& enc_resp, uint32_t client_id) {
    Response resp;
    resp.client_id = client_id;

    auto& buf = resp.payload;
    buf.reserve(kResponseHeaderSize);

    append_u32(buf, static_cast<uint32_t>(enc_resp.status));
    append_u32(buf, static_cast<uint32_t>(enc_resp.type));

    const size_t body_len_pos = buf.size();
    append_u32(buf, 0);
    const size_t body_start = buf.size();

    write_response_body(buf, enc_resp);

    const uint32_t body_len = static_cast<uint32_t>(buf.size() - body_start);
    std::memcpy(buf.data() + body_len_pos, &body_len, sizeof(uint32_t));
    return resp;
}

EncDBResponse deserialize_encdb_response(const Response& resp) {
    EncDBResponse enc_resp{};
    const uint8_t* p = resp.payload.data();

    enc_resp.status = static_cast<ResponseStatus>(read_u32(p));
    enc_resp.type = static_cast<ResponseType>(read_u32(p));
    const uint32_t body_len = read_u32(p);
    (void)body_len;

    read_response_body(p, enc_resp);
    return enc_resp;
}

std::vector<uint8_t> SecureChannel::encrypt(std::vector<uint8_t>& data) {
    std::vector<uint8_t> ciphertext;
    enc_aes_gcm(data.data(), data.size(), session_key.data(), ciphertext.data());
    return ciphertext;
}

std::vector<uint8_t> SecureChannel::decrypt(std::vector<uint8_t>& data) {
    std::vector<uint8_t> plaintext;
    dec_aes_gcm(data.data(), data.size(), session_key.data(), plaintext.data());
    return plaintext;
}
