#include "CryptoEnclave_t.h"

#include "EnclaveUtils.h"

#include "sgx_tseal.h"
#include "sgx_trts.h"
#include "sgx_tcrypto.h"
#include "stdlib.h"
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <list>
#include <random>
#include <stack>
#include <stdarg.h>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <functional>
#include <vector>

#include "../common/data_type.h"
#include "../CryptoTestingApp/EncDBRequest.h"

using std::string;

#define PER_ROUND_SIZE 100

const int id_max = total_file_no;

// =========================================================
// 1. Context state
// =========================================================

class ContextManager {
public:
    struct CachedContext {
        ClientContext ctx;
        uint32_t edb_id = 0;
    };

    ClientContext ctx;
    uint32_t current_edb_id = 0;
    std::unordered_map<int, CachedContext> inactive_cache;
};

ContextManager context_manager; // Global context manager instance.
const size_t kInactiveContextCacheMax = 4;

void provision_all_doc_slot_tombstones(uint64_t edb_id);
void persist_active_context_to_storage(uint32_t edb_id);
static void persist_cached_slot_to_server(uint64_t edb_id, const string& doc_id, bool remove_from_cache);

// =========================================================
// 2. Lifecycle and initialization helpers
// =========================================================

void fill_bitmap_label(label_struct& label_out, const string& word, int version) {
    string msg = word + std::to_string(version) + std::to_string(0);
    unsigned char label[HASH_VALUE_LEN_128];
    hash_SHA128(context_manager.ctx.K, msg.c_str(), msg.length(), label);
    memcpy(label_out.content, label, HASH_VALUE_LEN_128);
    label_out.content_length = HASH_VALUE_LEN_128;
}

void derive_bitmap_key(unsigned char* key_out, const string& word, int version) {
    string enc_key_msg = word + std::to_string(version) + std::to_string(1);
    hash_SHA128(context_manager.ctx.K, enc_key_msg.c_str(), enc_key_msg.length(), key_out);
}

std::bitset<id_max> decrypt_bitmap_cipher(const cipher_struct& cipher_in, const unsigned char* enc_key) {
    size_t plain_len = cipher_in.content_length - AESGCM_MAC_SIZE - AESGCM_IV_SIZE;
    uint8_t* plain = (uint8_t*) malloc(plain_len);
    dec_aes_gcm(enc_key, cipher_in.content, cipher_in.content_length, plain, plain_len);
    std::bitset<id_max> bm = uint8ArrayToBitset<id_max>(plain);
    free(plain);
    return bm;
}

void encrypt_bitmap_cipher(const std::bitset<id_max>& bm_in, const unsigned char* enc_key, cipher_struct& cipher_out) {
    uint8_t* bm_p = (uint8_t*) malloc(int(id_max / 8));
    bitsetToUint8Array(bm_in, bm_p);
    cipher_out.content_length = AESGCM_MAC_SIZE + AESGCM_IV_SIZE + int(id_max / 8);
    enc_aes_gcm(enc_key, bm_p, int(id_max / 8), cipher_out.content, cipher_out.content_length);
    free(bm_p);
}

std::bitset<id_max> load_bitmap_from_storage(uint64_t edb_id, const string& word, int version) {
    label_struct label = {};
    fill_bitmap_label(label, word, version);

    unsigned char enc_key[HASH_VALUE_LEN_128];
    derive_bitmap_key(enc_key, word, version);

    cipher_struct cipher = {};
    ocall_load_bitmap(edb_id, &label, &cipher, sizeof(label_struct), sizeof(cipher_struct));
    return decrypt_bitmap_cipher(cipher, enc_key);
}

void store_bitmap_batch(uint64_t edb_id, const std::vector<string>& words) {
    int out_amt = static_cast<int>(words.size());
    if (out_amt <= 0) {
        return;
    }

    label_struct* outs = (label_struct*) malloc(out_amt * sizeof(label_struct));
    cipher_struct* outs_res = (cipher_struct*) malloc(out_amt * sizeof(cipher_struct));

    for (int i = 0; i < out_amt; ++i) {
        const string& word = words[i];
        int next_version = context_manager.ctx.KVS[word] + 1;

        fill_bitmap_label(outs[i], word, next_version);

        unsigned char enc_key[HASH_VALUE_LEN_128];
        derive_bitmap_key(enc_key, word, next_version);
        encrypt_bitmap_cipher(context_manager.ctx.CK[word], enc_key, outs_res[i]);

        context_manager.ctx.KVS[word] = next_version;
        context_manager.ctx.CK.erase(word);
    }

    ocall_store_bitmaps(edb_id, outs, outs_res, out_amt, sizeof(label_struct), sizeof(cipher_struct));
    free(outs);
    free(outs_res);
}

bool pick_random_uncached_word(string& word_out) {
    if (context_manager.ctx.words_KVS.empty()) {
        return false;
    }

    const size_t n = context_manager.ctx.words_KVS.size();
    if (context_manager.ctx.CK.size() >= n) {
        return false;
    }

    std::mt19937 rng{std::random_device{}()};
    constexpr size_t kMaxAttempts = 256;
    for (size_t attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const size_t r = rng() % n;
        const string& candidate = context_manager.ctx.words_KVS[r];
        if (context_manager.ctx.CK.find(candidate) == context_manager.ctx.CK.end()) {
            word_out = candidate;
            return true;
        }
    }
    return false;
}

ClientContext make_fresh_context(int client_id) {
    ClientContext fresh;
    fresh.client_id = client_id;
    fresh.endof_doc_id = 1;
    return fresh;
}

bool has_active_context() {
    return context_manager.ctx.client_id != 0 || context_manager.current_edb_id != 0;
}

void append_dummy_keywords(uint32_t edb_id, int count) {
    while (count > 0) {
        const int batch = std::min(count, PER_ROUND_SIZE);
        label_struct* outs = (label_struct*) malloc(batch * sizeof(label_struct));
        cipher_struct* outs_res = (cipher_struct*) malloc(batch * sizeof(cipher_struct));
        for (int j = 0; j < batch; j++) {
            string dummy = "dummy_" + std::to_string(context_manager.ctx.dummy_size);
            std::bitset<id_max> dummy_bm;
            context_manager.ctx.KVS.insert(std::pair<string, int>(dummy, 0));
            context_manager.ctx.words_KVS.push_back(dummy);
            context_manager.ctx.dummy_count++;
            context_manager.ctx.dummy_size++;
            fill_bitmap_label(outs[j], dummy, 0);
            unsigned char enc_key[HASH_VALUE_LEN_128];
            derive_bitmap_key(enc_key, dummy, 0);
            encrypt_bitmap_cipher(dummy_bm, enc_key, outs_res[j]);
        }
        ocall_store_bitmaps(edb_id, outs, outs_res, batch, sizeof(label_struct), sizeof(cipher_struct));
        free(outs);
        free(outs_res);
        count -= batch;
    }
}

/*** Initialize the update path by preloading dummy keywords. */
void ecall_update_init(uint32_t edb_id){
    const int initial_dummy_size = context_manager.ctx.dummy_size;
    context_manager.ctx.dummy_size = 0;
    context_manager.ctx.dummy_count = 0;
    append_dummy_keywords(edb_id, initial_dummy_size);
    provision_all_doc_slot_tombstones(edb_id);
}

int ecall_generate_init_key() {
    sgx_status_t status = sgx_read_rand(
        context_manager.ctx.K,
        ENTRY_HASH_KEY_LEN_128
    );
    if (status != SGX_SUCCESS) {
        memset(context_manager.ctx.K, 0, ENTRY_HASH_KEY_LEN_128);
        return -1;
    }
    return 0;
}

void ecall_persist_context(uint32_t edb_id) {
    if (has_active_context() && context_manager.current_edb_id == edb_id) {
        persist_active_context_to_storage(edb_id);
        return;
    }

    for (auto it = context_manager.inactive_cache.begin();
         it != context_manager.inactive_cache.end();
         ++it) {
        if (it->second.edb_id != edb_id) {
            continue;
        }

        ClientContext backup_ctx = std::move(context_manager.ctx);
        const uint32_t backup_edb = context_manager.current_edb_id;

        context_manager.ctx = std::move(it->second.ctx);
        context_manager.current_edb_id = edb_id;
        persist_active_context_to_storage(edb_id);

        it->second.ctx = std::move(context_manager.ctx);
        context_manager.ctx = std::move(backup_ctx);
        context_manager.current_edb_id = backup_edb;
        return;
    }
}

// =========================================================
// 3. Snapshot serialization helpers
// =========================================================

template<typename T>
inline void append_scalar(std::vector<uint8_t>& buf, T value) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be POD");
    uint8_t* p = reinterpret_cast<uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

inline void append_bytes(
    std::vector<uint8_t>& buf,
    const uint8_t* data,
    size_t len
) {
    buf.insert(buf.end(), data, data + len);
}


inline bool read_scalar(
    const uint8_t*& p,
    const uint8_t* end,
    int& out
) {
    if (p + sizeof(int) > end) return false;
    memcpy(&out, p, sizeof(int));
    p += sizeof(int);
    return true;
}

inline bool read_bytes(
    const uint8_t*& p,
    const uint8_t* end,
    uint8_t* out,
    size_t len
) {
    if (p + len > end) return false;
    memcpy(out, p, len);
    p += len;
    return true;
}

std::vector<uint8_t> serialize_context_metadata(const ClientContext& ctx) {
    std::vector<uint8_t> buf;
    buf.reserve(ctx.KVS.size() * 40 + int(id_max / 8) + 128);

    append_scalar(buf, ctx.client_id);
    append_bytes(buf, ctx.K, ENTRY_HASH_KEY_LEN_128);

    uint32_t enc_key_len = static_cast<uint32_t>(ctx.enc_key.size());
    append_scalar(buf, enc_key_len);
    if (enc_key_len > 0) {
        append_bytes(buf, ctx.enc_key.data(), enc_key_len);
    }

    append_scalar(buf, ctx.CK_max);
    append_scalar(buf, ctx.doc_CK_max);
    append_scalar(buf, ctx.dummy_count);
    append_scalar(buf, ctx.dummy_size);
    append_scalar(buf, ctx.endof_doc_id);

    uint32_t count = static_cast<uint32_t>(ctx.KVS.size());
    append_scalar(buf, count);
    for (const auto& [key, version] : ctx.KVS) {
        uint32_t key_len = static_cast<uint32_t>(key.size());
        if (key_len == 0 || key_len > MAX_KEYWORD_LEN) {
            continue;
        }
        append_scalar(buf, key_len);
        append_bytes(buf, reinterpret_cast<const uint8_t*>(key.data()), key_len);
        append_scalar(buf, version);
    }

    uint8_t* all_docs_vec = (uint8_t*) malloc(int(id_max / 8));
    bitsetToUint8Array(ctx.all_docs, all_docs_vec);
    append_bytes(buf, all_docs_vec, int(id_max / 8));
    free(all_docs_vec);
    append_scalar(buf, ctx.dummy_replenish_counter);
    append_scalar(buf, ctx.retrieval_limit);
    append_scalar(buf, ctx.retrieval_threshold);
    append_scalar(buf, ctx.retrieval_batch_size);
    append_bytes(
        buf,
        reinterpret_cast<const uint8_t*>(ctx.doc_slot_version),
        total_file_no * sizeof(uint32_t)
    );
    return buf;
}

bool deserialize_context_metadata(const uint8_t* buf, size_t len, ClientContext& restored) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;

    restored = ClientContext{};
    if (!read_scalar(p, end, restored.client_id)) {
        return false;
    }
    if (!read_bytes(p, end, restored.K, ENTRY_HASH_KEY_LEN_128)) {
        return false;
    }

    int enc_key_len = 0;
    if (!read_scalar(p, end, enc_key_len)) {
        return false;
    }
    if (enc_key_len < 0) {
        return false;
    }
    restored.enc_key.resize(static_cast<size_t>(enc_key_len));
    if (enc_key_len > 0 && !read_bytes(p, end, restored.enc_key.data(), static_cast<size_t>(enc_key_len))) {
        return false;
    }

    if (!read_scalar(p, end, restored.CK_max)) return false;
    if (!read_scalar(p, end, restored.doc_CK_max)) return false;
    if (!read_scalar(p, end, restored.dummy_count)) return false;
    if (!read_scalar(p, end, restored.dummy_size)) return false;
    if (!read_scalar(p, end, restored.endof_doc_id)) return false;

    int count = 0;
    if (!read_scalar(p, end, count)) {
        return false;
    }
    if (count < 0) {
        return false;
    }

    for (int i = 0; i < count; ++i) {
        int key_len = 0;
        int version = 0;
        if (!read_scalar(p, end, key_len)) {
            return false;
        }
        if (key_len <= 0 || key_len > MAX_KEYWORD_LEN) {
            return false;
        }
        if (p + key_len > end) {
            return false;
        }

        std::string key(reinterpret_cast<const char*>(p), key_len);
        p += key_len;
        if (!read_scalar(p, end, version)) {
            return false;
        }
        restored.KVS.emplace(std::move(key), version);
    }

    uint8_t* all_docs_vec = (uint8_t*) malloc(int(id_max / 8));
    if (!read_bytes(p, end, all_docs_vec, int(id_max / 8))) {
        free(all_docs_vec);
        return false;
    }
    restored.all_docs = uint8ArrayToBitset<id_max>(all_docs_vec);
    free(all_docs_vec);
    if (p + sizeof(int) <= end) {
        if (!read_scalar(p, end, restored.dummy_replenish_counter)) return false;
    }
    if (p + sizeof(int) <= end) {
        if (!read_scalar(p, end, restored.retrieval_limit)) return false;
    }
    if (p + sizeof(int) <= end) {
        if (!read_scalar(p, end, restored.retrieval_threshold)) return false;
    }
    if (p + sizeof(int) <= end) {
        if (!read_scalar(p, end, restored.retrieval_batch_size)) return false;
    }
    if (p + total_file_no * sizeof(uint32_t) <= end) {
        if (!read_bytes(
                p,
                end,
                reinterpret_cast<uint8_t*>(restored.doc_slot_version),
                total_file_no * sizeof(uint32_t))) {
            return false;
        }
    }

    restored.words_KVS.reserve(restored.KVS.size());
    for (const auto& [key, version] : restored.KVS) {
        (void)version;
        restored.words_KVS.push_back(key);
    }
    return true;
}

std::vector<uint8_t> seal_context_metadata(const ClientContext& ctx) {
    std::vector<uint8_t> plain = serialize_context_metadata(ctx);
    uint32_t sealed_size = sgx_calc_sealed_data_size(0, static_cast<uint32_t>(plain.size()));
    if (sealed_size == UINT32_MAX) {
        throw std::runtime_error("Failed to calculate sealed context size");
    }

    std::vector<uint8_t> sealed(sealed_size);
    sgx_status_t status = sgx_seal_data(
        0,
        nullptr,
        static_cast<uint32_t>(plain.size()),
        plain.data(),
        sealed_size,
        reinterpret_cast<sgx_sealed_data_t*>(sealed.data())
    );
    if (status != SGX_SUCCESS) {
        throw std::runtime_error("Failed to seal context metadata");
    }
    return sealed;
}

bool unseal_context_metadata(const uint8_t* sealed_buf, size_t sealed_len, ClientContext& restored) {
    if (sealed_len < sizeof(sgx_sealed_data_t)) {
        return false;
    }

    uint32_t plain_len = sgx_get_encrypt_txt_len(reinterpret_cast<const sgx_sealed_data_t*>(sealed_buf));
    if (plain_len == UINT32_MAX) {
        return false;
    }

    std::vector<uint8_t> plain(plain_len);
    sgx_status_t status = sgx_unseal_data(
        reinterpret_cast<const sgx_sealed_data_t*>(sealed_buf),
        nullptr,
        0,
        plain.data(),
        &plain_len
    );
    if (status != SGX_SUCCESS) {
        return false;
    }
    return deserialize_context_metadata(plain.data(), plain_len, restored);
}

// =========================================================
// 4. Context persistence helpers
// =========================================================

bool load_active_context_from_storage(uint32_t edb_id){
    static uint8_t buf[2048000];
    uint32_t actual_len = 0;

    ocall_load_context_snapshot(edb_id, buf, sizeof(buf), &actual_len);
    if (actual_len == 0) {
        return false;
    }

    ClientContext restored;
    if (!unseal_context_metadata(buf, actual_len, restored)) {
        printf("Context snapshot load failed\n");
        return false;
    }

    context_manager.ctx = std::move(restored);
    context_manager.current_edb_id = edb_id;
    context_manager.ctx.CK.clear();
    context_manager.ctx.doc_CK.clear();

    for (auto it = context_manager.ctx.KVS.begin(); it != context_manager.ctx.KVS.end(); ++it) {
        string word_in = it->first;
        std::bitset<id_max> bm_in = load_bitmap_from_storage(edb_id, word_in, context_manager.ctx.KVS[word_in]);
        context_manager.ctx.CK[word_in] = bm_in;
        if (context_manager.ctx.CK.size() == context_manager.ctx.CK_max) {
            break;
        }
    }
    return true;
}

void persist_active_context_to_storage(uint32_t edb_id){
    // Flush CK back to storage first.
    std::vector<string> words_out;                
    for (auto it = context_manager.ctx.CK.begin(); it != context_manager.ctx.CK.end(); ++it) {
        words_out.push_back(it->first);
    }
    store_bitmap_batch(edb_id, words_out);

    // Persist the sealed context metadata after runtime caches are flushed.
    std::vector<uint8_t> buf = seal_context_metadata(context_manager.ctx);

    ocall_store_context_snapshot(
        edb_id,
        buf.data(),
        static_cast<uint32_t>(buf.size())
    );

    std::vector<string> doc_CK_ids;
    doc_CK_ids.reserve(context_manager.ctx.doc_CK.size());
    for (auto it = context_manager.ctx.doc_CK.begin(); it != context_manager.ctx.doc_CK.end(); ++it) {
        doc_CK_ids.push_back(it->first);
    }
    for (const auto& doc_id : doc_CK_ids) {
        persist_cached_slot_to_server(edb_id, doc_id, true);
    }
}

void cache_active_context() {
    if (!has_active_context()) {
        return;
    }

    ContextManager::CachedContext cached;
    cached.ctx = std::move(context_manager.ctx);
    cached.edb_id = context_manager.current_edb_id;
    context_manager.inactive_cache[cached.ctx.client_id] = std::move(cached);

    context_manager.ctx = ClientContext{};
    context_manager.current_edb_id = 0;
}

void evict_one_inactive_context_if_needed() {
    if (context_manager.inactive_cache.size() <= kInactiveContextCacheMax) {
        return;
    }

    std::vector<int> client_ids;
    client_ids.reserve(context_manager.inactive_cache.size());
    for (const auto& [client_id, cached] : context_manager.inactive_cache) {
        (void)cached;
        client_ids.push_back(client_id);
    }

    int victim_index = std::mt19937{std::random_device{}()}() % client_ids.size();
    int victim_client_id = client_ids[victim_index];
    auto victim_it = context_manager.inactive_cache.find(victim_client_id);
    if (victim_it == context_manager.inactive_cache.end()) {
        return;
    }

    ClientContext active_backup = std::move(context_manager.ctx);
    uint32_t active_edb_backup = context_manager.current_edb_id;

    context_manager.ctx = std::move(victim_it->second.ctx);
    context_manager.current_edb_id = victim_it->second.edb_id;
    persist_active_context_to_storage(context_manager.current_edb_id);
    context_manager.inactive_cache.erase(victim_it);

    context_manager.ctx = std::move(active_backup);
    context_manager.current_edb_id = active_edb_backup;
}

int ecall_schedule_context(uint32_t client_id, uint32_t edb_id, uint8_t create_if_missing){
    if (has_active_context() &&
        context_manager.ctx.client_id == static_cast<int>(client_id) &&
        context_manager.current_edb_id == edb_id) {
        return 0;
    }

    cache_active_context();
    evict_one_inactive_context_if_needed();

    auto cached_it = context_manager.inactive_cache.find(client_id);
    if (cached_it != context_manager.inactive_cache.end()) {
        context_manager.ctx = std::move(cached_it->second.ctx);
        context_manager.current_edb_id = cached_it->second.edb_id;
        context_manager.inactive_cache.erase(cached_it);
        return 0;
    }

    if (create_if_missing) {
        context_manager.ctx = make_fresh_context(client_id);
        context_manager.current_edb_id = edb_id;
        return 0;
    }

    if (load_active_context_from_storage(edb_id)) {
        return 0;
    }

    context_manager.ctx = ClientContext{};
    context_manager.current_edb_id = 0;
    return -1;
}

// =========================================================
// 5. Document cache helpers (obfuscated access + bundle store)
// =========================================================

static void trim_trailing_null(string& value) {
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
}

static void append_unique_doc_id(std::vector<std::string>& ids, const string& doc_id) {
    if (doc_id.empty()) {
        return;
    }
    for (const auto& existing : ids) {
        if (existing == doc_id) {
            return;
        }
    }
    ids.push_back(doc_id);
}

static string pick_random_existing_doc_id() {
    std::vector<int> occupied;
    occupied.reserve(64);
    for (int i = 0; i < total_file_no; ++i) {
        if (context_manager.ctx.all_docs[i] != 0) {
            occupied.push_back(i + 1);
        }
    }
    if (occupied.empty()) {
        return "";
    }
    std::mt19937 rng{std::random_device{}()};
    const int pick = occupied[rng() % occupied.size()];
    return std::to_string(pick);
}

ObfuscatedDocAccessPlan build_doc_access_plan(
    DocAccessMode mode,
    const std::vector<std::string>& primary_ids
) {
    ObfuscatedDocAccessPlan plan;
    plan.mode = mode;

    std::vector<std::string> seeds;
    if (mode == DocAccessMode::DOC_ACCESS_INSERT_DECOY) {
        const string decoy = pick_random_existing_doc_id();
        if (!decoy.empty()) {
            seeds.push_back(decoy);
        }
    } else {
        seeds = primary_ids;
    }

    for (const auto& id : seeds) {
        append_unique_doc_id(plan.access_ids, id);
    }

    const int padded_target = context_manager.ctx.retrieval_threshold;
    constexpr int kMaxPaddingRounds = 256;
    for (int round = 0;
         static_cast<int>(plan.access_ids.size()) < padded_target && round < kMaxPaddingRounds;
         ++round) {
        const size_t before = plan.access_ids.size();
        const string padding_doc_id = pick_padding_doc_id(plan.access_ids);
        if (padding_doc_id.empty()) {
            break;
        }
        append_unique_doc_id(plan.access_ids, padding_doc_id);
        if (plan.access_ids.size() == before) {
            // Only one (or few) occupied doc_ids: padding cannot add new unique ids.
            break;
        }
    }

    if (mode == DocAccessMode::DOC_ACCESS_INSERT_DECOY) {
        plan.parse_ids.clear();
    } else {
        plan.parse_ids = primary_ids;
    }
    return plan;
}

static void fill_doc_label(const string& doc_id, label_struct& label_out) {
    const string msg = doc_id + std::to_string(0);
    unsigned char label[HASH_VALUE_LEN_128];
    hash_SHA128(context_manager.ctx.K, msg.c_str(), msg.length(), label);
    memcpy(label_out.content, label, HASH_VALUE_LEN_128);
    label_out.content_length = HASH_VALUE_LEN_128;
}

static encdoc_struct encrypt_doc_plaintext(const string& packed_plain) {
    encdoc_struct cipher;
    const size_t content_length = packed_plain.size();
    cipher.content_length = content_length + AESGCM_MAC_SIZE + AESGCM_IV_SIZE;
    enc_aes_gcm(
        context_manager.ctx.K,
        packed_plain.data(),
        content_length,
        cipher.content,
        cipher.content_length
    );
    return cipher;
}

static bool doc_slot_in_cache(const string& doc_id) {
    return context_manager.ctx.doc_CK.find(doc_id) != context_manager.ctx.doc_CK.end();
}

static bool decrypt_cipher_into_slot(
    const encdoc_struct& stored,
    CachedDocSlot& slot,
    uint32_t* slot_version_out = nullptr
) {
    slot.index_csv.clear();
    slot.body.clear();

    const size_t cipher_len = stored.content_length;
    if (cipher_len <= AESGCM_MAC_SIZE + AESGCM_IV_SIZE) {
        return false;
    }

    size_t plain_len = cipher_len - AESGCM_MAC_SIZE - AESGCM_IV_SIZE;
    if (plain_len > MAX_FILE_SIZE) {
        plain_len = MAX_FILE_SIZE;
    }

    unsigned char* plain = (unsigned char*) malloc(plain_len);
    dec_aes_gcm(
        context_manager.ctx.K,
        stored.content,
        cipher_len,
        plain,
        plain_len
    );

    string plain_str(reinterpret_cast<char*>(plain), plain_len);
    free(plain);

    if (!parse_doc_bundle_ex(plain_str, slot.index_csv, slot.body, slot_version_out)) {
        slot.body = plain_str;
    }
    trim_trailing_null(slot.index_csv);
    trim_trailing_null(slot.body);
    return true;
}

static bool fetch_doc_slot_from_server(uint64_t edb_id, const string& doc_id) {
    if (doc_slot_in_cache(doc_id)) {
        return true;
    }

    label_struct label;
    encdoc_struct cipher;
    fill_doc_label(doc_id, label);
    ocall_load_docs(
        edb_id,
        &label,
        &cipher,
        1,
        sizeof(label_struct),
        sizeof(encdoc_struct)
    );

    CachedDocSlot slot;
    uint32_t bundle_version = 0;
    if (!decrypt_cipher_into_slot(cipher, slot, &bundle_version)) {
        printf("Warning: failed to decrypt doc slot %s from server\n", doc_id.c_str());
        return false;
    }
    const int id_int = atoi(doc_id.c_str());
    if (id_int > 0 && id_int <= total_file_no && bundle_version > 0) {
        context_manager.ctx.doc_slot_version[id_int - 1] = std::max(
            context_manager.ctx.doc_slot_version[id_int - 1],
            bundle_version
        );
    }
    context_manager.ctx.doc_CK[doc_id] = std::move(slot);
    return true;
}

static void persist_cached_slot_to_server(uint64_t edb_id, const string& doc_id, bool remove_from_cache) {
    auto it = context_manager.ctx.doc_CK.find(doc_id);
    if (it == context_manager.ctx.doc_CK.end()) {
        return;
    }

    const int id_int = atoi(doc_id.c_str());
    if (id_int <= 0 || id_int > total_file_no) {
        return;
    }

    const uint32_t slot_version = ++context_manager.ctx.doc_slot_version[id_int - 1];
    const CachedDocSlot& slot = it->second;
    string packed;
    if (slot.index_csv.empty() && slot.body.empty()) {
        packed = pack_doc_tombstone_v2(
            slot_version,
            context_manager.ctx.K,
            doc_id
        );
    } else {
        packed = pack_doc_bundle_v2(
            slot.index_csv,
            slot.body,
            slot_version,
            context_manager.ctx.K,
            doc_id
        );
    }

    encdoc_struct cipher = encrypt_doc_plaintext(packed);
    label_struct label;
    fill_doc_label(doc_id, label);
    ocall_store_docs(
        edb_id,
        &label,
        &cipher,
        1,
        sizeof(label_struct),
        sizeof(encdoc_struct)
    );

    if (remove_from_cache) {
        context_manager.ctx.doc_CK.erase(it);
    }
}

static void persist_random_cached_slot(uint64_t edb_id) {
    if (context_manager.ctx.doc_CK.empty()) {
        return;
    }

    std::vector<string> cached_ids;
    cached_ids.reserve(context_manager.ctx.doc_CK.size());
    for (auto it = context_manager.ctx.doc_CK.begin(); it != context_manager.ctx.doc_CK.end(); ++it) {
        cached_ids.push_back(it->first);
    }

    std::mt19937 rng{std::random_device{}()};
    const string& pick = cached_ids[rng() % cached_ids.size()];
    persist_cached_slot_to_server(edb_id, pick, true);
}

static string pick_decoy_doc_id(const string& target_doc_id) {
    const int target_int = atoi(target_doc_id.c_str());
    std::mt19937 rng{std::random_device{}()};
    constexpr int kMaxAttempts = 256;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const int candidate = (rng() % total_file_no) + 1;
        if (candidate != target_int) {
            return std::to_string(candidate);
        }
    }
    if (target_int != 1) {
        return "1";
    }
    return "2";
}

static void run_slot_update(
    uint64_t edb_id,
    const string& target_doc_id,
    const std::function<void(CachedDocSlot&)>& mutate
) {
    const bool target_cached = doc_slot_in_cache(target_doc_id);
    const string read_id = target_cached ? pick_decoy_doc_id(target_doc_id) : target_doc_id;

    if (!doc_slot_in_cache(read_id)) {
        fetch_doc_slot_from_server(edb_id, read_id);
    }
    if (!doc_slot_in_cache(target_doc_id)) {
        fetch_doc_slot_from_server(edb_id, target_doc_id);
    }

    auto target_it = context_manager.ctx.doc_CK.find(target_doc_id);
    if (target_it == context_manager.ctx.doc_CK.end()) {
        printf("Warning: target doc slot %s not available in cache after fetch\n", target_doc_id.c_str());
        return;
    }

    mutate(target_it->second);
    persist_random_cached_slot(edb_id);
}

void provision_all_doc_slot_tombstones(uint64_t edb_id) {
    const int batch_size = std::max(1, context_manager.ctx.retrieval_batch_size);
    context_manager.ctx.all_docs.reset();
    context_manager.ctx.endof_doc_id = total_file_no + 1;
    for (int slot = 0; slot < total_file_no; ++slot) {
        context_manager.ctx.doc_slot_version[slot] = 0;
    }

    for (int start_id = 1; start_id <= total_file_no; start_id += batch_size) {
        const int batch = std::min(batch_size, total_file_no - start_id + 1);
        label_struct* labels = (label_struct*) malloc(batch * sizeof(label_struct));
        encdoc_struct* ciphers = (encdoc_struct*) malloc(batch * sizeof(encdoc_struct));

        for (int i = 0; i < batch; ++i) {
            const int doc_id = start_id + i;
            const string doc_id_str = std::to_string(doc_id);
            const string packed = pack_doc_tombstone_v2(
                0,
                context_manager.ctx.K,
                doc_id_str
            );
            fill_doc_label(doc_id_str, labels[i]);
            ciphers[i] = encrypt_doc_plaintext(packed);
        }

        ocall_store_docs(
            edb_id,
            labels,
            ciphers,
            batch,
            sizeof(label_struct),
            sizeof(encdoc_struct)
        );
        free(labels);
        free(ciphers);
    }
}

static void spill_doc_ck_if_needed(uint64_t edb_id) {
    while (static_cast<int>(context_manager.ctx.doc_CK.size()) > context_manager.ctx.doc_CK_max) {
        persist_random_cached_slot(edb_id);
    }
}

static void materialize_access_ids(uint64_t edb_id, const std::vector<std::string>& access_ids) {
    if (access_ids.empty()) {
        return;
    }

    std::vector<std::string> get_doc_ids;
    for (const auto& doc_id : access_ids) {
        if (context_manager.ctx.doc_CK.find(doc_id) == context_manager.ctx.doc_CK.end()) {
            get_doc_ids.push_back(doc_id);
        }
    }
    if (get_doc_ids.empty()) {
        return;
    }

    const int batch_size = std::max(1, context_manager.ctx.retrieval_batch_size);
    for (size_t offset = 0; offset < get_doc_ids.size(); offset += static_cast<size_t>(batch_size)) {
        const size_t batch = std::min(static_cast<size_t>(batch_size), get_doc_ids.size() - offset);

        label_struct *labels = (label_struct*) malloc(batch * sizeof(label_struct));
        encdoc_struct *ciphers = (encdoc_struct*) malloc(batch * sizeof(encdoc_struct));

        for (size_t i = 0; i < batch; ++i) {
            const string& doc_id = get_doc_ids[offset + i];
            string msg = doc_id + std::to_string(0);
            unsigned char *label = (unsigned char *) malloc(HASH_VALUE_LEN_128);
            hash_SHA128(context_manager.ctx.K, msg.c_str(), msg.length(), label);
            memcpy(labels[i].content, label, HASH_VALUE_LEN_128);
            labels[i].content_length = HASH_VALUE_LEN_128;
            free(label);
        }

        ocall_load_docs(edb_id, labels, ciphers, batch, sizeof(label_struct), sizeof(encdoc_struct));

        for (size_t i = 0; i < batch; ++i) {
            const string& loaded_id = get_doc_ids[offset + i];
            CachedDocSlot slot;
            if (decrypt_cipher_into_slot(ciphers[i], slot)) {
                context_manager.ctx.doc_CK[loaded_id] = std::move(slot);
            }
        }

        free(labels);
        free(ciphers);
    }
}

static bool decrypt_and_parse_doc_record(const string& doc_id, ParsedDocRecord& out) {
    out.found = false;
    out.index_csv.clear();
    out.body.clear();

    const auto it = context_manager.ctx.doc_CK.find(doc_id);
    if (it == context_manager.ctx.doc_CK.end()) {
        return false;
    }

    out.found = true;
    out.index_csv = it->second.index_csv;
    out.body = it->second.body;
    return true;
}

std::unordered_map<string, ParsedDocRecord> execute_obfuscated_doc_access(
    uint64_t edb_id,
    const ObfuscatedDocAccessPlan& plan
) {
    std::unordered_map<string, ParsedDocRecord> parsed;

    materialize_access_ids(edb_id, plan.access_ids);

    for (const auto& doc_id : plan.parse_ids) {
        ParsedDocRecord record;
        if (decrypt_and_parse_doc_record(doc_id, record)) {
            parsed[doc_id] = record;
        }
    }

    spill_doc_ck_if_needed(edb_id);
    return parsed;
}

std::string pick_padding_doc_id(const std::vector<std::string>& real_doc_ids) {
    constexpr int kMaxPaddingAttempts = 256;
    std::mt19937 rng{std::random_device{}()};

    if (context_manager.ctx.all_docs.none()) {
        for (int attempt = 0; attempt < kMaxPaddingAttempts; ++attempt) {
            const int d = (rng() % total_file_no) + 1;
            const string candidate = std::to_string(d);
            bool already = false;
            for (const auto& id : real_doc_ids) {
                if (id == candidate) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                return candidate;
            }
        }
        if (!real_doc_ids.empty()) {
            return real_doc_ids[rng() % real_doc_ids.size()];
        }
        return "";
    }

    if (real_doc_ids.empty()) {
        return "";
    }

    const int upper = std::max(1, context_manager.ctx.endof_doc_id);
    for (int attempt = 0; attempt < kMaxPaddingAttempts; ++attempt) {
        const int d = (rng() % upper) + 1;
        if (d > 0 && d <= total_file_no && context_manager.ctx.all_docs[d - 1] != 0) {
            return std::to_string(d);
        }
    }

    return real_doc_ids[rng() % real_doc_ids.size()];
}

void RetrieveDocs(uint64_t edb_id, std::vector<std::string> doc_ids, size_t in_amt, void* out) {
    (void)in_amt;
    const std::vector<std::string> real_ids = doc_ids;
    const int real_doc_count = static_cast<int>(real_ids.size());

    const ObfuscatedDocAccessPlan plan = build_doc_access_plan(
        DocAccessMode::DOC_ACCESS_QUERY,
        real_ids
    );
    const std::unordered_map<string, ParsedDocRecord> parsed =
        execute_obfuscated_doc_access(edb_id, plan);

    int out_idx = 0;
    for (int i = 0; i < real_doc_count; i++) {
        const string& id = real_ids[i];
        const auto it = parsed.find(id);
        if (it == parsed.end() || !it->second.found) {
            printf("Warning: requested document %s not found after obfuscated access\n", id.c_str());
            continue;
        }

        const string& body_part = it->second.body;
        ((result_doc*)out)->doc_id[out_idx] = atoi(id.c_str());
        memset(((result_doc*)out)->content[out_idx], 0, MAX_FILE_SIZE);
        const size_t copy_len = body_part.size() < MAX_FILE_SIZE ? body_part.size() : MAX_FILE_SIZE;
        if (copy_len > 0) {
            memcpy(((result_doc*)out)->content[out_idx], body_part.data(), copy_len);
        }
        out_idx++;
    }
    ((result_doc*)out)->doc_count = out_idx;
}


// =========================================================
// 6. Keyword index helpers
// =========================================================

/*** update with op */
void UpdateKeywords(uint64_t edb_id, const char *doc_id, size_t id_length, const char *content, size_t content_length, const int* op, size_t op_len){
    std::vector<string> wordList = wordTokenize(content, content_length);
    int id_int = atoi(std::string(doc_id, id_length).c_str());
    if (id_int <= 0 || id_int > id_max) {
        printf("Warning: invalid doc_id in UpdateKeywords: %d\n", id_int);
        return;
    }

    for (std::vector<string>::iterator it = wordList.begin(); it != wordList.end(); ++it) {
        string word = (*it);
        const bool word_cached = (context_manager.ctx.CK.find(word) != context_manager.ctx.CK.end());
        const bool word_exists = (context_manager.ctx.KVS.find(word) != context_manager.ctx.KVS.end());

        // When CK still has room, this round only touches the in-memory cache.
        if (context_manager.ctx.CK.empty()) {
            std::bitset<id_max> bm;
            bm[id_int - 1] = (*op == ADD);
            context_manager.ctx.CK[word] = bm;
            context_manager.ctx.KVS[word] = 1;
            context_manager.ctx.words_KVS.push_back(word);
            store_bitmap_batch(edb_id, {word});
            continue;
        }
        if (context_manager.ctx.CK.size() < context_manager.ctx.CK_max) {
            if (word_cached) {
                std::bitset<id_max> bm = context_manager.ctx.CK[word];
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
            } else if (word_exists) {
                std::bitset<id_max> bm = load_bitmap_from_storage(
                    edb_id,
                    word,
                    context_manager.ctx.KVS[word]
                );
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
            } else {
                std::bitset<id_max> bm;
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
                context_manager.ctx.KVS[word] = 1;
                context_manager.ctx.words_KVS.push_back(word);
            }
            store_bitmap_batch(edb_id, {word});
            continue;
        }

        if (context_manager.ctx.KVS.size() == context_manager.ctx.CK_max) {
            if (word_cached) {
                std::bitset<id_max> bm = context_manager.ctx.CK[word];
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
            } else if (word_exists) {
                std::bitset<id_max> bm = load_bitmap_from_storage(
                    edb_id,
                    word,
                    context_manager.ctx.KVS[word]
                );
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
            } else if (!word_exists) {
                std::bitset<id_max> bm;
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
                context_manager.ctx.KVS[word] = 1;
                context_manager.ctx.words_KVS.push_back(word);
            }
            store_bitmap_batch(edb_id, {word});
        } else {
            string word_in;
            if (word_cached) {
                if (!pick_random_uncached_word(word_in)) {
                    printf("Warning: no uncached keyword available during UpdateKeywords\n");
                    continue;
                }
            } else if (word_exists) {
                word_in = word;
            } else {
                if (context_manager.ctx.dummy_count <= 0) {
                    throw std::runtime_error("Lack of capacity to accommodate more keywords");
                }
                while (context_manager.ctx.KVS.find(word_in) == context_manager.ctx.KVS.end()) {
                    int d = std::mt19937{std::random_device{}()}() % context_manager.ctx.dummy_size;
                    word_in = "dummy_" + std::to_string(d);
                }
            }

            if (context_manager.ctx.KVS.find(word_in) == context_manager.ctx.KVS.end()) {
                printf("Warning: bitmap source word not found: %s\n", word_in.c_str());
                continue;
            }
            std::bitset<id_max> bm_in = load_bitmap_from_storage(edb_id, word_in, context_manager.ctx.KVS[word_in]);

            // Cached keywords keep their own bitmap in CK; the loaded bitmap only refills
            // the companion cache entry selected for this round.
            if (word_cached) {
                context_manager.ctx.CK[word_in] = bm_in;
                std::bitset<id_max> bm = context_manager.ctx.CK[word];
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
                store_bitmap_batch(edb_id, {word});
            } else if (word_exists) {
                // Existing but uncached keywords reuse the persisted bitmap and flip one bit.
                bm_in[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word_in] = bm_in;
                store_bitmap_batch(edb_id, {word});
            } else {
                // New keywords consume a dummy placeholder so the logical keyword set can grow.
                context_manager.ctx.KVS.erase(word_in);
                context_manager.ctx.words_KVS.erase(std::remove(context_manager.ctx.words_KVS.begin(), context_manager.ctx.words_KVS.end(), word_in), context_manager.ctx.words_KVS.end());
                context_manager.ctx.dummy_count--;

                std::bitset<id_max> bm;
                bm[id_int - 1] = (*op == ADD);
                context_manager.ctx.CK[word] = bm;
                context_manager.ctx.KVS[word] = 1;
                context_manager.ctx.words_KVS.push_back(word);
                store_bitmap_batch(edb_id, {word});
            }
        }

        int out_amt = context_manager.ctx.CK.size() - context_manager.ctx.CK_max;
        if (out_amt <= 0) {
            continue;
        }

        std::vector<string> words_without_w;
        for (auto ck_it = context_manager.ctx.CK.begin(); ck_it != context_manager.ctx.CK.end(); ++ck_it) {
            if (ck_it->first != word) {
                words_without_w.push_back(ck_it->first);
            }
        }
        std::vector<string> words_out;
        std::sample(words_without_w.begin(), words_without_w.end(), std::back_inserter(words_out), out_amt, std::mt19937{std::random_device{}()});
        store_bitmap_batch(edb_id, words_out);
    }
}


/*** search for a keyword */
std::bitset<id_max> Search(uint64_t edb_id, string word){
    string word_in;
    const bool word_exists = (context_manager.ctx.KVS.find(word) != context_manager.ctx.KVS.end());
    std::bitset<id_max> res;
    if (!word_exists) {
        if (!pick_random_uncached_word(word_in)) {
            throw std::runtime_error("No keywords available for random selection");
        }
    }
    else {
        const bool word_cached = (context_manager.ctx.CK.find(word) != context_manager.ctx.CK.end());
        if (!word_cached) {
            word_in = word;
        } else {
            if (!pick_random_uncached_word(word_in)) {
                throw std::runtime_error("No keywords available for random selection");
            }
            res = context_manager.ctx.CK[word];
        }
        if (context_manager.ctx.KVS.find(word_in) == context_manager.ctx.KVS.end()) {
            throw std::runtime_error("Bitmap source keyword not found");
        }
        std::bitset<id_max> bm_in = load_bitmap_from_storage(edb_id, word_in, context_manager.ctx.KVS[word_in]);
        if (!word_cached) {
            res = bm_in;
        }
        context_manager.ctx.CK[word_in] = bm_in;
    }

    int out_amt = context_manager.ctx.CK.size() - context_manager.ctx.CK_max;
    if (out_amt > 0) {
        std::vector<string> words_without_w;
        for (auto ck_it = context_manager.ctx.CK.begin(); ck_it != context_manager.ctx.CK.end(); ++ck_it) {
            if (ck_it->first != word) {
                words_without_w.push_back(ck_it->first);
            }
        }
        std::vector<string> words_out;
        std::sample(words_without_w.begin(), words_without_w.end(), std::back_inserter(words_out), out_amt, std::mt19937{std::random_device{}()});
        store_bitmap_batch(edb_id, words_out);
    }
    return res;
}


void ecall_printMem(){
    printf("%d", (sizeof(string)+sizeof(int))*context_manager.ctx.KVS.size()+(sizeof(string)+total_file_no/8)*context_manager.ctx.CK.size());
}


/*
 * Enclave state:
 * - keyword -> bitmap
 * - doc_id -> metadata
 */

// =========================================================
// 7. Request handlers
// =========================================================

void handle_select(uint64_t edb_id, const SelectRequest& req, void* out) {
    // Evaluate the boolean expression in stack form.
    std::stack<std::bitset<total_file_no>> stk;
    for (size_t i = 0; i < req.bool_len; ++i) {
        uint8_t token = req.bool_expr[i];
        if (token == 0x01) { // term
            int term_index = req.bool_expr[++i];
            const EncTerm& term = req.terms[term_index];
            string keyword((char*)term.data, term.len);
            std::bitset<total_file_no> bm = Search(edb_id, keyword);
            stk.push(bm);
        } else if (token == 0x02) { // AND
            auto right = stk.top(); stk.pop();
            auto left = stk.top(); stk.pop();
            stk.push(left & right);
        } else if (token == 0x03) { // OR
            auto right = stk.top(); stk.pop();
            auto left = stk.top(); stk.pop();
            stk.push(left | right);
        } else if (token == 0x04) { // NOT
            auto word = stk.top(); stk.pop();
            stk.push(context_manager.ctx.all_docs & (~word));
        }
    }
    std::bitset<total_file_no> final_bm = stk.top();
    // Collect matching document ids from the final bitmap.
    std::vector<std::string> doc_ids;
    for(size_t i=0;i < total_file_no;i++){
        if(final_bm[i]){
            std::string doc_id = std::to_string(i+1);
            doc_ids.push_back(doc_id);
        }
    }
    if (req.type == 0 && static_cast<int>(doc_ids.size()) > context_manager.ctx.retrieval_limit) {
        doc_ids.resize(context_manager.ctx.retrieval_limit);
    }
    if(req.type == 0) {
        // Base SELECT returns the final decrypted document payload.
        RetrieveDocs(edb_id, doc_ids, doc_ids.size(), out);
        return;
    }
    if (req.type >= 1 && req.type <= 4) {
        const int match_count = static_cast<int>(doc_ids.size());
        int agg_value = 0;
        const char* op_name = "AGG";

        if (match_count > 0) {
            if (req.type == 1) { // MAX
                op_name = "MAX";
                agg_value = atoi(doc_ids.back().c_str());
            } else if (req.type == 2) { // MIN
                op_name = "MIN";
                agg_value = atoi(doc_ids.front().c_str());
            } else if (req.type == 3) { // SUM
                op_name = "SUM";
                for (const auto& id : doc_ids) {
                    agg_value += atoi(id.c_str());
                }
            } else if (req.type == 4) { // AVG
                op_name = "AVG";
                for (const auto& id : doc_ids) {
                    agg_value += atoi(id.c_str());
                }
                agg_value /= match_count;
            }
        }

        // doc_id[1] 暂存 match_count，供 Server 组装 RPC 响应（仅 doc_count=1 会序列化到 wire）
        ((result_doc*)out)->doc_id[0] = agg_value;
        ((result_doc*)out)->doc_id[1] = match_count;
        ((result_doc*)out)->doc_count = 1;
        memset(((result_doc*)out)->content[0], 0, MAX_FILE_SIZE);
        snprintf(
            (char*)((result_doc*)out)->content[0],
            MAX_FILE_SIZE,
            "ENCDB_AGG|op=%s|match_count=%d|value=%d",
            op_name,
            match_count,
            agg_value
        );
        return;
    }


    RetrieveDocs(edb_id, doc_ids, doc_ids.size(), out);
}

void handle_update(uint64_t edb_id, const UpdateRequest& req) {
    const std::string fileName = std::to_string(req.doc_id);
    const size_t id_length = fileName.length();

    char* doc_id = (char*) malloc(id_length);
    memcpy(doc_id, fileName.c_str(), id_length);

    const string doc_id_str(doc_id, id_length);

    const char* index_ptr = nullptr;
    size_t index_length = 0;
    const char* body_ptr = nullptr;
    size_t body_length = 0;

    if (req.flags == UPDATE_SPLIT) {
        index_ptr = reinterpret_cast<const char*>(req.index_content);
        index_length = static_cast<size_t>(req.index_len);
        body_ptr = reinterpret_cast<const char*>(req.doc_content);
        body_length = static_cast<size_t>(req.doc_len);
    } else {
        index_ptr = reinterpret_cast<const char*>(req.doc_content);
        index_length = static_cast<size_t>(req.doc_len);
        body_ptr = reinterpret_cast<const char*>(req.doc_content);
        body_length = static_cast<size_t>(req.doc_len);
    }

    if (req.op == OP_DELETE) {
        string old_index;
        run_slot_update(edb_id, doc_id_str, [&](CachedDocSlot& slot) {
            old_index = slot.index_csv;
            slot.index_csv.clear();
            slot.body.clear();
        });

        if (!old_index.empty()) {
            const int del_op = DEL;
            UpdateKeywords(
                edb_id,
                doc_id,
                id_length,
                old_index.c_str(),
                old_index.size(),
                &del_op,
                sizeof(del_op)
            );
        } else {
            printf("Warning: DELETE doc %d bundle index not found\n", req.doc_id);
        }

        const int id_int = req.doc_id;
        if (id_int > 0 && id_int <= total_file_no) {
            context_manager.ctx.all_docs[id_int - 1] = 0;
        }

        context_manager.ctx.dummy_replenish_counter++;
        if (context_manager.ctx.dummy_replenish_counter % 20 == 0) {
            append_dummy_keywords(edb_id, 1);
        }
        free(doc_id);
        return;
    }

    context_manager.ctx.dummy_replenish_counter++;
    if (context_manager.ctx.dummy_replenish_counter % 20 == 0) {
        append_dummy_keywords(edb_id, 1);
    }
    if (context_manager.ctx.dummy_count * 10 < context_manager.ctx.dummy_size * 8) {
        printf("Warning: available dummy keywords below 80%% threshold (%d/%d)\n",
            context_manager.ctx.dummy_count,
            context_manager.ctx.dummy_size);
    }

    string index_str(index_ptr, index_length);
    string body_str(body_ptr, body_length);
    trim_trailing_null(index_str);
    trim_trailing_null(body_str);

    if (req.flags != UPDATE_SPLIT) {
        index_str = body_str;
    }

    string replace_old_index;
    run_slot_update(edb_id, doc_id_str, [&](CachedDocSlot& slot) {
        if (req.op == OP_REPLACE) {
            replace_old_index = slot.index_csv;
        }
        slot.index_csv = index_str;
        slot.body = body_str;
    });

    if (req.op == OP_REPLACE) {
        if (replace_old_index.empty()) {
            printf("Warning: REPLACE doc %d not found\n", req.doc_id);
            free(doc_id);
            return;
        }
        const int del_op = DEL;
        UpdateKeywords(
            edb_id,
            doc_id,
            id_length,
            replace_old_index.c_str(),
            replace_old_index.size(),
            &del_op,
            sizeof(del_op)
        );
    }

    if (index_length > 0) {
        const int add_op = ADD;
        UpdateKeywords(edb_id, doc_id, id_length, index_ptr, index_length, &add_op, sizeof(add_op));
    }

    const int id_int = req.doc_id;
    if (id_int > 0 && id_int <= total_file_no) {
        context_manager.ctx.all_docs[id_int - 1] = 1;
        if (id_int >= context_manager.ctx.endof_doc_id) {
            context_manager.ctx.endof_doc_id = id_int + 1;
        }
    }

    free(doc_id);
}


void ecall_handle_request(
    uint64_t edb_id,
    uint8_t* req_buf,
    size_t req_len,
    void* out,
    size_t out_len
) {
    if (req_len != sizeof(EncDBRequest)) {
        // Ignore malformed request buffers.
        return;
    }

    const EncDBRequest* req =
        reinterpret_cast<const EncDBRequest*>(req_buf);

    switch (req->type) {
    case RequestType::SELECT:
        handle_select(edb_id, req->select_req, out);
        break;
    case RequestType::UPDATE:
        handle_update(edb_id, req->update_req);
        break;
    }
}
