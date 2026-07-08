#ifndef DATA_TYPE_H
#define DATA_TYPE_H

#include "config.h"
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <array>
#include <list>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <bitset> 


/* for all sources except OCALL/ECALL */
#define AESGCM_IV_SIZE 12
#define AESGCM_MAC_SIZE 16
static unsigned char gcm_iv[] = {
    0x99, 0xaa, 0x3e, 0x68, 0xed, 0x81, 0x73, 0xa0, 0xee, 0xd0, 0x66, 0x84
};
#define ENTRY_HASH_KEY_LEN_128 16 // for HMAC-SHA128- bit key
#define HASH_VALUE_LEN_128 16 // for HMAC-SHA128- bit key
#define ENC_KEY_SIZE 16 // for AES128
const int ADD = 1;
const int DEL = 0;

#define MAX_FILE_SIZE 4096
#define MAX_RESULT_DOCS 100


// const int total_file_no = 10000;
const int total_file_no = 10000;
const int del_no = (int)total_file_no*0.75;
const std::string raw_doc_dir=  "../data/enron/";
// const std::string raw_doc_dir=  "../data/small/";
const std::string s_keyword[10]={"pleas","thank","forward","pm","pl","cc","the","am","enron","know"};	
// const std::string s_keyword[10]={"the","of","and","to","a","in","for","is","on","that"};
const int cipher_size = AESGCM_MAC_SIZE + AESGCM_IV_SIZE + int(total_file_no/8);

const int encdoc_size = AESGCM_MAC_SIZE + AESGCM_IV_SIZE + MAX_FILE_SIZE;


#define ENTRY_VALUE_LEN 128
#define MAX_FILE_LENGTH 10 

#define BUFLEN 10240 //buffer for enc + dec
#define RAND_LEN 64// 256 // 2048-bit

typedef struct
{
    size_t content_length;
    unsigned char content[HASH_VALUE_LEN_128];
} label_struct; //used to export between ecall and ocall
typedef struct
{
    size_t content_length;
    unsigned char content[cipher_size];
} cipher_struct; //used to export between ecall and ocall
typedef struct
{
    size_t content_length;
    unsigned char content[encdoc_size];
} encdoc_struct; //used to export between ecall and ocall

typedef struct
{
    uint32_t doc_count;
    int doc_id[MAX_RESULT_DOCS];
    char content[MAX_RESULT_DOCS][MAX_FILE_SIZE];
} result_doc; //used to export between ecall and ocall


typedef struct
{
    size_t content_length;
    unsigned char content[RAND_LEN];
} rand_t; //used to export between ecall and ocall


typedef struct
{
    size_t content_length;
    unsigned char content[ENTRY_VALUE_LEN];
} v; //used to export between ecall and ocall


/* packet related */
typedef struct docIds {
    char *doc_id; 
    size_t id_length;  // length of the doc_id
} docId; 

typedef struct entryKeys {
    char *content; 
    size_t content_length;  // length of the entry_value
} entryKey;

typedef struct entryValues {
    char *message; 
    size_t message_length;  // length of the entry_value
} entryValue;

typedef struct docContents{
    docId id;
    char* content;
    int content_length;
    //std::vector<std::string> wordList;
} docContent;

// Decrypted document slot held in Enclave cache (encrypt only on spill / flush).
struct CachedDocSlot {
    std::string index_csv;
    std::string body;
};

struct ClientContext {
    int client_id = 0;

    // 密钥
    unsigned char K[ENTRY_HASH_KEY_LEN_128] = {0};
    std::vector<uint8_t> enc_key;

    // 查询缓存（可选）
    std::unordered_map<std::string, std::bitset<total_file_no>> CK;
    std::unordered_map<std::string, CachedDocSlot> doc_CK;
    std::unordered_map<std::string, int> KVS;
    std::vector<std::string> words_KVS; 

    // 元数据
    int CK_max = 10;
    int doc_CK_max = 10;

    int dummy_count = 0;
    int dummy_size = 1000;
    int dummy_replenish_counter = 0;
    int retrieval_limit = 100;
    int retrieval_threshold = 5;
    int retrieval_batch_size = 5;

    std::bitset<total_file_no> all_docs;
    int endof_doc_id = 1;

    // Per-slot ciphertext version (monotonic on each store; hides insert/delete shape)
    uint32_t doc_slot_version[total_file_no] = {0};
};

typedef std::pair<entryKey, entryValue> entry;

#endif
