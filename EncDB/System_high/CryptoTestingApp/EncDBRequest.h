#ifndef ENCDBREQUEST_H
#define ENCDBREQUEST_H

#include <cstdint>

// ---------------------------
// Shared protocol limits
// ---------------------------

#define MAX_TERMS 16
#define TRAPDOOR_LEN 32
#define DOCID_LEN 16
#define MAX_DOC_SIZE 4096
#define MAX_INDEX_SIZE MAX_DOC_SIZE
#define MAX_KEYWORD_LEN 32

// RPC wire byte after doc_id
#define UPDATE_WIRE_V1 1u
#define UPDATE_WIRE_LEGACY_SINGLE 2u
#define MAX_RESULT_DOCS 100
#define ENC_KEY_SIZE 16

// ---------------------------
// Top-level request/response tags
// ---------------------------

enum class RequestType {
    INIT,
    SELECT,
    UPDATE,
    SHUTDOWN,
    INVALID,
};

enum class ResponseStatus : uint32_t {
    OK = 0,
    ERROR = 1
};

enum class ResponseType : uint32_t {
    INIT_RESULT,
    SELECT_RESULT,
    UPDATE_RESULT,
    SHUTDOWN_RESULT,
    INVALID
};

// ---------------------------
// Shared payload building blocks
// ---------------------------

enum BoolOp : uint8_t {
    OP_TERM = 0x01,
    OP_AND  = 0x02,
    OP_OR   = 0x03,
    OP_NOT  = 0x04
};

struct EncTerm {
    uint8_t data[MAX_KEYWORD_LEN];
    uint8_t len;
};

// ---------------------------
// Request payloads
// ---------------------------

#define INIT_MODE_CREATE 0u
#define INIT_MODE_RESUME 1u

struct InitRequest {
    unsigned char key[ENC_KEY_SIZE];
    uint8_t mode;
    uint32_t target_edb_id;
};

struct ShutdownRequest {
    uint8_t reserved;
};

struct SelectRequest {
    EncTerm terms[MAX_TERMS];
    int term_count;
    uint8_t bool_expr[128];
    int bool_len;
    uint8_t type; // 0 for base, 1-4 for MAX/MIN/SUM/AVG
};

// UpdateRequest.op
#define OP_DELETE 0u
#define OP_INSERT 1u
#define OP_REPLACE 2u

#define DOC_BUNDLE_MAGIC_LEGACY 0u
#define DOC_BUNDLE_MAGIC_V1 1u
#define DOC_BUNDLE_MAGIC_V2 2u

#define UPDATE_LEGACY 0u
#define UPDATE_SPLIT 1u

struct UpdateRequest {
    uint8_t op; // OP_DELETE / OP_INSERT / OP_REPLACE
    uint8_t flags; // UPDATE_LEGACY / UPDATE_SPLIT
    int doc_id;
    uint8_t doc_content[MAX_DOC_SIZE];
    int doc_len;
    uint8_t index_content[MAX_DOC_SIZE];
    int index_len;
};

struct EncDBRequest {
    uint8_t client_id;
    RequestType type;
    union {
        InitRequest init_req;
        SelectRequest select_req;
        UpdateRequest update_req;
        ShutdownRequest shutdown_req;
    };
};

// ---------------------------
// Response payloads
// ---------------------------

struct InitResponse {
    uint8_t edb_id;
};

struct SelectResponse {
    int doc_count;
    int doc_ids[MAX_RESULT_DOCS];
    uint8_t doc_contents[MAX_RESULT_DOCS][MAX_DOC_SIZE];
};

struct UpdateResponse {
    int success;   // 1 success, 0 fail
    int doc_id;
};

struct ShutdownResponse {
    uint8_t success;
};

struct EncDBResponse {
    ResponseStatus status;
    ResponseType type;
    union {
        InitResponse init_resp;
        SelectResponse select_resp;
        UpdateResponse update_resp;
        ShutdownResponse shutdown_resp;
    };
};

#endif
