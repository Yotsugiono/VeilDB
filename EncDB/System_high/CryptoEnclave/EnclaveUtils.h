#ifndef ENCLAVE_UTILS_H
#define ENCLAVE_UTILS_H

#include "stdlib.h"
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <iterator>
#include <vector>
#include <cstring>
#include <bitset> 
#include "../common/data_type.h"


void printf( const char *fmt, ...);
void print_bytes(uint8_t *ptr, uint32_t len);
int  cmp(const uint8_t *value1, const uint8_t *value2, uint32_t len);
void clear(uint8_t *dest, uint32_t len);
std::vector<std::string> wordTokenize(const char *content, int content_length);

// doc_CK: decrypted index/body per slot; version++ and AES-GCM only on spill/flush to server
std::string pack_doc_bundle_v1(const std::string& index, const std::string& body);
std::string pack_doc_bundle_legacy(const std::string& content);
bool parse_doc_bundle(const std::string& plain, std::string& index_out, std::string& body_out);
bool parse_doc_bundle_ex(
    const std::string& plain,
    std::string& index_out,
    std::string& body_out,
    uint32_t* slot_version_out
);

std::string pack_doc_bundle_v2(
    const std::string& index,
    const std::string& body,
    uint32_t slot_version,
    const void* enc_key,
    const std::string& doc_id
);
std::string pack_doc_tombstone_v2(
    uint32_t slot_version,
    const void* enc_key,
    const std::string& doc_id
);

struct ParsedDocRecord {
    bool found;
    std::string index_csv;
    std::string body;
};

enum class DocAccessMode {
    DOC_ACCESS_QUERY = 0,
    DOC_ACCESS_MUTATE_USE = 1,
    DOC_ACCESS_INSERT_DECOY = 2,
};

struct ObfuscatedDocAccessPlan {
    DocAccessMode mode;
    std::vector<std::string> access_ids;
    std::vector<std::string> parse_ids;
};

std::string pick_padding_doc_id(const std::vector<std::string>& real_doc_ids);
ObfuscatedDocAccessPlan build_doc_access_plan(
    DocAccessMode mode,
    const std::vector<std::string>& primary_ids
);
std::unordered_map<std::string, ParsedDocRecord> execute_obfuscated_doc_access(
    uint64_t edb_id,
    const ObfuscatedDocAccessPlan& plan
);
void provision_all_doc_slot_tombstones(uint64_t edb_id);
void RetrieveDocs(uint64_t edb_id, std::vector<std::string> doc_ids, size_t in_amt, void* out);
void UpdateKeywords(
    uint64_t edb_id,
    const char* doc_id,
    size_t id_length,
    const char* content,
    size_t content_length,
    const int* op,
    size_t op_len
);

void enc_aes_gcm(const void *key, const void *plaintext, size_t plaintext_len, void *ciphertext, size_t ciphertext_len);
void dec_aes_gcm(const void *key, const void *ciphertext, size_t ciphertext_len, void *plaintext, size_t plaintext_len);
int hash_SHA128(const void *key, const void *msg, int msg_len, void *value);
int hash_SHA128_key(const void *key, int key_len, const void *msg, int msg_len, void *value);

template <size_t N>
std::bitset<N> uint8ArrayToBitset(const unsigned char* array) {
    std::bitset<N> bits;
    for (size_t i = 0; i < N / 8; ++i) {
        for (size_t j = 0; j < 8; ++j) {
            bits[i * 8 + j] = (array[i] >> (7 - j)) & 1;
        }
    }
    return bits;
}

template <size_t N>
void bitsetToUint8Array(const std::bitset<N>& bits, unsigned char* array) {
    for (size_t i = 0; i < N / 8; ++i) {
        array[i] = 0;
        for (size_t j = 0; j < 8; ++j) {
            array[i] |= (bits[i * 8 + j] << (7 - j));
        }
    }
}

//improved
//void prf_F_improve(const void *key,const void *plaintext,size_t plaintext_len, entryKey *k );
//void prf_Enc_improve(const void *key,const void *plaintext,size_t plaintext_len, entryValue *v);
//void prf_Dec_Improve(const void *key,const void *ciphertext,size_t ciphertext_len, entryValue *value );

#endif
