#include "Server.h"

#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <random>
#include <set>

#include "CryptoEnclave_u.h"
#include "EdbDiskStore.h"
#include "EncDBDebug.h"

namespace {
constexpr const char* kEnclaveFile = "CryptoEnclave.signed.so";
}

static DatabaseManager* g_db_mgr = nullptr;
static Server* g_server_instance = nullptr;
volatile sig_atomic_t g_shutdown_requested = 0;

void handle_shutdown_signal(int) {
    g_shutdown_requested = 1;
}

// ---------------------------
// Enclave pool
// ---------------------------

void EnclavePool::init(size_t pool_size) {
    std::lock_guard<std::mutex> lock(pool_mtx);

    for (size_t i = 0; i < pool_size; ++i) {
        sgx_enclave_id_t eid;
        sgx_status_t ret;
        sgx_launch_token_t token = {0};
        int token_updated = 0;

        ret = sgx_create_enclave(kEnclaveFile, SGX_DEBUG_FLAG, &token, &token_updated, &eid, NULL);
        if (ret != SGX_SUCCESS) {
            printf("sgx_create_enclave failed: %#x\n", ret);
            continue;
        }

        auto instance = std::make_unique<EnclaveInstance>();
        instance->eid = eid;
        enclaves.push_back(std::move(instance));
        available_enclaves.push(enclaves.back().get());
    }
}

EnclavePool::~EnclavePool() {
    for (auto& instance : enclaves) {
        sgx_destroy_enclave(instance->eid);
    }
}

EnclaveInstance* EnclavePool::acquire(int client_id) {
    std::unique_lock<std::mutex> lock(pool_mtx);

    auto it = client_enclave_map.find(client_id);
    if (it != client_enclave_map.end()) {
        if (it->second->try_acquire()) {
            return it->second;
        }

        auto start_time = std::chrono::steady_clock::now();
        while (it->second->busy) {
            pool_mtx.unlock();
            std::this_thread::yield();
            pool_mtx.lock();
            if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(5)) {
                printf("Timeout while waiting for enclave instance for client_id: %d\n", client_id);
                return nullptr;
            }
        }

        if (it->second->try_acquire()) {
            return it->second;
        }

        printf("Failed to acquire busy enclave instance for client_id: %d\n", client_id);
        return nullptr;
    }

    if (!available_enclaves.empty()) {
        EnclaveInstance* instance = available_enclaves.front();
        available_enclaves.pop();

        if (instance->try_acquire()) {
            client_enclave_map[client_id] = instance;
            return instance;
        }
    }

    for (const auto& instance : enclaves) {
        if (instance->try_acquire()) {
            client_enclave_map[client_id] = instance.get();
            return instance.get();
        }
    }

    return nullptr;
}

void EnclavePool::release(EnclaveInstance* instance, int client_id) {
    std::lock_guard<std::mutex> lock(pool_mtx);
    instance->release();
    instance->last_client_id = client_id;
}

// ---------------------------
// Server lifecycle
// ---------------------------

Server::Server() {
    g_db_mgr = &edb_manager;
    g_server_instance = this;
}

Server::~Server() {
    shutdown_flush();
    g_server_instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        stop_flag = true;
    }

    queue_cv.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void Server::init(int enclave_num, int worker_num) {
    enclave_pool.init(enclave_num);

    for (int i = 0; i < worker_num; i++) {
        workers.emplace_back(&Server::worker_loop, this);
    }
}

void Server::release() {
}

bool Server::flush_client_edb(uint8_t client_id, sgx_enclave_id_t eid) {
    auto it = edb_id_map.find(client_id);
    if (it == edb_id_map.end()) {
        return false;
    }

    const uint32_t edb_id = it->second;
    int schedule_ret = 0;
    sgx_status_t schedule_status = ecall_schedule_context(
        eid,
        &schedule_ret,
        client_id,
        edb_id,
        0
    );
    if (schedule_status != SGX_SUCCESS || schedule_ret != 0) {
        printf("flush: schedule failed client_id=%u edb_id=%u\n", client_id, edb_id);
        return false;
    }

    sgx_status_t persist_status = ecall_persist_context(eid, edb_id);
    if (persist_status != SGX_SUCCESS) {
        printf("flush: ecall_persist_context failed edb_id=%u status=%#x\n", edb_id, persist_status);
        return false;
    }

    edb_manager.persist_edb(edb_id);
    return true;
}

void Server::shutdown_flush() {
    std::set<uint32_t> flushed_edbs;
    for (const auto& entry : edb_id_map) {
        EnclaveInstance* enclave = enclave_pool.acquire(entry.first);
        if (!enclave) {
            continue;
        }
        if (flush_client_edb(entry.first, enclave->eid)) {
            flushed_edbs.insert(entry.second);
        }
        enclave_pool.release(enclave, entry.first);
    }

    for (uint32_t edb_id : edb_manager.list_edb_ids_on_disk()) {
        if (flushed_edbs.count(edb_id) == 0) {
            edb_manager.open_edb(edb_id, false);
            edb_manager.persist_edb(edb_id);
        }
    }
}

uint32_t Server::EDB_Register(uint8_t client_id) {
    uint32_t edb_id = g_db_mgr->register_edb();
    return edb_id;
}

// ---------------------------
// Host-side request workers
// ---------------------------

void Server::worker_loop() {
    while (true) {
        Task* task = nullptr;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            queue_cv.wait(lock, [&]() {
                return !task_queue.empty() || stop_flag;
            });

            if (stop_flag && task_queue.empty()) {
                return;
            }

            task = task_queue.front();
            task_queue.pop();
        }

        bool ok = handle_request_core(task->req, task->out, task->out_len);
        task->done.set_value(ok);
        delete task;
    }
}

bool Server::handle_request(const EncDBRequest& req, void* out, size_t out_len) {
    Task* task = new Task{req, out, out_len};
    std::future<bool> fut = task->done.get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        task_queue.push(task);
    }

    queue_cv.notify_one();
    return fut.get();
}

bool Server::handle_request_core(const EncDBRequest& req, void* out, size_t out_len) {
    EnclaveInstance* enclave = Server::enclave_pool.acquire(req.client_id);
    if (!enclave) {
        printf("Failed to acquire enclave instance for client_id: %u\n", req.client_id);
        return false;
    }

    sgx_enclave_id_t eid = enclave->eid;

    if (req.type == RequestType::INIT) {
        const InitRequest& init = req.init_req;
        const bool resume = (init.mode == INIT_MODE_RESUME);
        const bool is_new_client = (Server::edb_id_map.find(req.client_id) == Server::edb_id_map.end());
        uint32_t edb_id = 0;
        bool run_update_init = false;

        if (resume) {
            if (init.target_edb_id == 0) {
                printf("INIT resume requires target_edb_id\n");
                Server::enclave_pool.release(enclave, req.client_id);
                return false;
            }
            if (!edb_manager.edb_exists_on_disk(init.target_edb_id)) {
                printf("INIT resume: edb %u not found on disk\n", init.target_edb_id);
                Server::enclave_pool.release(enclave, req.client_id);
                return false;
            }
            edb_id = init.target_edb_id;
            if (!edb_manager.open_edb(edb_id, false)) {
                printf("INIT resume: failed to open edb %u\n", edb_id);
                Server::enclave_pool.release(enclave, req.client_id);
                return false;
            }
            Server::edb_id_map[req.client_id] = edb_id;
        } else if (is_new_client) {
            edb_id = Server::EDB_Register(req.client_id);
            Server::edb_id_map[req.client_id] = edb_id;
            run_update_init = true;
        } else {
            edb_id = Server::edb_id_map[req.client_id];
        }

        const uint8_t create_if_missing = (resume || !is_new_client) ? 0 : 1;
        int schedule_ret = 0;
        sgx_status_t schedule_status = ecall_schedule_context(
            eid,
            &schedule_ret,
            req.client_id,
            edb_id,
            create_if_missing
        );
        if (schedule_status != SGX_SUCCESS || schedule_ret != 0) {
            printf("ecall_schedule_context failed for init client_id=%u status=%#x ret=%d\n",
                req.client_id, schedule_status, schedule_ret);
            Server::enclave_pool.release(enclave, req.client_id);
            return false;
        }

        if (!resume && is_new_client) {
            sgx_status_t key_status = ecall_apply_init_key(eid, init.key);
            if (key_status != SGX_SUCCESS) {
                printf("ecall_apply_init_key failed: %#x\n", key_status);
                Server::enclave_pool.release(enclave, req.client_id);
                return false;
            }
        }

        if (run_update_init) {
            sgx_status_t init_ret = ecall_update_init(eid, edb_id);
            if (init_ret != SGX_SUCCESS) {
                printf("ecall_update_init failed: %#x\n", init_ret);
                Server::enclave_pool.release(enclave, req.client_id);
                return false;
            }
        }

        Server::enclave_pool.release(enclave, req.client_id);
        return true;
    }

    if (req.type == RequestType::SHUTDOWN) {
        const bool ok = flush_client_edb(req.client_id, eid);
        Server::enclave_pool.release(enclave, req.client_id);
        return ok;
    }

    if (Server::edb_id_map.find(req.client_id) == Server::edb_id_map.end()) {
        printf("No EDB registered for client_id: %u\n", req.client_id);
        Server::enclave_pool.release(enclave, req.client_id);
        return false;
    }

    uint32_t edb_id = Server::edb_id_map[req.client_id];
    int schedule_ret = 0;
    sgx_status_t schedule_status = ecall_schedule_context(eid, &schedule_ret, req.client_id, edb_id, 0);
    if (schedule_status != SGX_SUCCESS || schedule_ret != 0) {
        printf("ecall_schedule_context failed for client_id=%u status=%#x ret=%d\n", req.client_id, schedule_status, schedule_ret);
        Server::enclave_pool.release(enclave, req.client_id);
        return false;
    }
    EncDatabase* edb = edb_manager.get_edb(edb_id);
    sgx_status_t ret = ecall_handle_request(
        eid,
        edb_id,
        (uint8_t*)&req,
        sizeof(EncDBRequest),
        out,
        out_len
    );
    (void)edb;
    if (ret != SGX_SUCCESS) {
        printf("ecall_handle_request failed: %#x\n", ret);
        Server::enclave_pool.release(enclave, req.client_id);
        return false;
    }

    Server::enclave_pool.release(enclave, req.client_id);
    return true;
}

// ---------------------------
// Host-side encrypted storage
// ---------------------------

void Server::store_bitmap(EncDatabase& edb, label_struct* outs, cipher_struct* outs_res, size_t out_amt) {
    for (size_t i = 0; i < out_amt; i++) {
        std::string label((char*)outs[i].content, outs[i].content_length);
        std::string cipher((char*)outs_res[i].content, outs_res[i].content_length);
        edb.bitmaps[label] = cipher;
    }
}

std::string Server::load_bitmap(EncDatabase& edb, label_struct* ins) {
    std::string label((char*)ins->content, ins->content_length);
    if (edb.bitmaps.find(label) == edb.bitmaps.end()) {
        printf("Not found!\n");
    }

    std::string cipher = edb.bitmaps[label];
    return cipher;
}

void Server::store_docs(EncDatabase& edb, label_struct* outs, encdoc_struct* outs_res, size_t out_amt) {
    for (int i = 0; i < out_amt; i++) {
        std::string label((char*)outs[i].content, outs[i].content_length);
        std::string cipher((char*)outs_res[i].content, outs_res[i].content_length);
        edb.docs[label] = cipher;
    }
}

std::vector<std::string> Server::load_docs(EncDatabase& edb, label_struct* ins, size_t in_amt) {
    std::vector<std::string> res_list(in_amt);
    for (int i = 0; i < in_amt; i++) {
        std::string label((char*)ins[i].content, ins[i].content_length);
        if (edb.docs.find(label) == edb.docs.end()) {
            printf("Doc Not found!\n");
        } else {
            res_list[i] = edb.docs[label];
        }
    }
    return res_list;
}

void ocall_print_string(const char* str) {
    printf("%s\n", str);
}

void ocall_load_bitmap(uint32_t edb_id, void* ins, void* ins_res, size_t label_size, size_t cipher_size) {
    EncDatabase* edb = g_db_mgr->get_edb(edb_id);
    if (!edb) {
        return;
    }
    std::lock_guard<std::mutex> lock(edb->mtx);
    std::string query_result = Server::load_bitmap(*edb, (label_struct*)ins);
    memcpy(((cipher_struct*)ins_res)->content, query_result.c_str(), query_result.length());
    ((cipher_struct*)ins_res)->content_length = query_result.length();
}

void ocall_store_bitmaps(uint32_t edb_id, void* outs, void* outs_res, size_t out_amt, size_t label_size, size_t cipher_size) {
    EncDatabase* edb = g_db_mgr->get_edb(edb_id);
    if (!edb) {
        return;
    }
    std::lock_guard<std::mutex> lock(edb->mtx);
    Server::store_bitmap(*edb, (label_struct*)outs, (cipher_struct*)outs_res, out_amt);
    for (size_t i = 0; i < out_amt; ++i) {
        label_struct* label = &((label_struct*)outs)[i];
        cipher_struct* cipher = &((cipher_struct*)outs_res)[i];
        const std::string label_str((char*)label->content, label->content_length);
        const std::string cipher_str((char*)cipher->content, cipher->content_length);
        EdbDiskStore::write_bitmap_entry(edb_id, label_str, cipher_str);
    }
}

void ocall_load_docs(uint32_t edb_id, void* ins, void* ins_res, size_t in_amt, size_t label_size, size_t cipher_size) {
    EncDatabase* edb = g_db_mgr->get_edb(edb_id);
    if (!edb) {
        return;
    }
    std::lock_guard<std::mutex> lock(edb->mtx);
    std::vector<std::string> query_result = Server::load_docs(*edb, (label_struct*)ins, in_amt);
    for (int i = 0; i < in_amt; ++i) {
        ((encdoc_struct*)ins_res)[i].content_length = 0;
        memset(((encdoc_struct*)ins_res)[i].content, 0, encdoc_size);
        ((encdoc_struct*)ins_res)[i].content_length = query_result[i].length();
        if (!query_result[i].empty()) {
            memcpy(((encdoc_struct*)ins_res)[i].content, query_result[i].c_str(), query_result[i].length());
        }
    }
}

void ocall_store_docs(uint32_t edb_id, void* outs, void* outs_res, size_t out_amt, size_t label_size, size_t cipher_size) {
    EncDatabase* edb = g_db_mgr->get_edb(edb_id);
    if (!edb) {
        return;
    }
    std::lock_guard<std::mutex> lock(edb->mtx);
    Server::store_docs(*edb, (label_struct*)outs, (encdoc_struct*)outs_res, out_amt);
    for (size_t i = 0; i < out_amt; ++i) {
        label_struct* label = &((label_struct*)outs)[i];
        encdoc_struct* cipher = &((encdoc_struct*)outs_res)[i];
        const std::string label_str((char*)label->content, label->content_length);
        const std::string cipher_str((char*)cipher->content, cipher->content_length);
        EdbDiskStore::write_doc_entry(edb_id, label_str, cipher_str);
    }
}

void ocall_store_context_snapshot(uint32_t edb_id, uint8_t* buf, size_t len) {
    EdbDiskStore::ensure_edb_dirs(edb_id);
    std::ofstream ofs(EdbDiskStore::context_snapshot_path(edb_id), std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<char*>(buf), len);
}

void ocall_load_context_snapshot(uint32_t edb_id, uint8_t* buf, size_t max_len, uint32_t* actual_len) {
    const std::string path = EdbDiskStore::context_snapshot_path(edb_id);
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        *actual_len = 0;
        return;
    }

    ifs.seekg(0, std::ios::end);
    size_t len = ifs.tellg();
    ifs.seekg(0);

    if (len > max_len) {
        *actual_len = 0;
        return;
    }

    ifs.read(reinterpret_cast<char*>(buf), len);
    *actual_len = len;
}

// ---------------------------
// RPC entry
// ---------------------------

Response Server::process(const Request& req) {
    EncDBRequest enc_req = deserialize_encdb_request(req);

    EncDBResponse enc_resp{};
    enc_resp.status = ResponseStatus::OK;

    if (enc_req.type == RequestType::INIT) {
        enc_resp.type = ResponseType::INIT_RESULT;
        bool ok = handle_request(enc_req, nullptr, 0);
        if (!ok) {
            enc_resp.status = ResponseStatus::ERROR;
            enc_resp.init_resp.edb_id = 0;
        } else {
            InitResponse& i = enc_resp.init_resp;
            i.edb_id = Server::edb_id_map[enc_req.client_id];
        }
    } else if (enc_req.type == RequestType::SELECT) {
        enc_resp.type = ResponseType::SELECT_RESULT;

#if ENCDB_SERVER_DEBUG
        fprintf(stderr,
            "[EncDB][SELECT] client_id=%u wire_payload=%zu "
            "select_req.type=%u term_count=%d bool_len=%d\n",
            static_cast<unsigned>(enc_req.client_id),
            req.payload.size(),
            static_cast<unsigned>(enc_req.select_req.type),
            enc_req.select_req.term_count,
            enc_req.select_req.bool_len);
        encdb_debug_hex("select.bool_expr", enc_req.select_req.bool_expr,
            static_cast<size_t>(enc_req.select_req.bool_len));
        if (!req.payload.empty()) {
            const size_t tail_off = req.payload.size() >= 8 ? req.payload.size() - 8 : 0;
            encdb_debug_hex("wire.payload.tail", req.payload.data() + tail_off,
                req.payload.size() - tail_off);
        }
#endif

        result_doc* res_docs = (result_doc*)calloc(1, sizeof(result_doc));
        bool ok = handle_request(enc_req, res_docs, sizeof(result_doc));
        SelectResponse& s = enc_resp.select_resp;
        s.doc_count = 0;
        if (!ok) {
            enc_resp.status = ResponseStatus::ERROR;
#if ENCDB_SERVER_DEBUG
            fprintf(stderr, "[EncDB][SELECT] handle_request failed\n");
#endif
        } else if (enc_req.select_req.type >= 1 && enc_req.select_req.type <= 4) {
            const int agg_value = res_docs->doc_id[0];
            const int match_count = res_docs->doc_id[1];
            const char* op_name = "AGG";
            switch (enc_req.select_req.type) {
            case 1: op_name = "MAX"; break;
            case 2: op_name = "MIN"; break;
            case 3: op_name = "SUM"; break;
            case 4: op_name = "AVG"; break;
            default: break;
            }

#if ENCDB_SERVER_DEBUG
            fprintf(stderr,
                "[EncDB][SELECT] branch=AGGREGATE enclave_out doc_count=%u "
                "doc_id[0]=%d doc_id[1](match_stash)=%d\n",
                res_docs->doc_count,
                res_docs->doc_id[0],
                res_docs->doc_id[1]);
            encdb_debug_cstr("enclave.content[0]", res_docs->content[0]);
#endif

            s.doc_count = 1;
            s.doc_ids[0] = agg_value;
            memset(s.doc_contents[0], 0, MAX_DOC_SIZE);
            const int meta_len = snprintf(
                (char*)s.doc_contents[0],
                MAX_DOC_SIZE,
                "ENCDB_AGG|op=%s|match_count=%d|value=%d",
                op_name,
                match_count,
                agg_value
            );

#if ENCDB_SERVER_DEBUG
            fprintf(stderr,
                "[EncDB][SELECT] snprintf meta_len=%d MAX_DOC_SIZE=%d op=%s "
                "match_count=%d agg_value=%d\n",
                meta_len,
                MAX_DOC_SIZE,
                op_name,
                match_count,
                agg_value);
            encdb_debug_cstr("rpc.doc_contents[0]", s.doc_contents[0]);
            encdb_debug_hex("rpc.doc_contents[0].head", s.doc_contents[0], 48);
#endif
        } else {
#if ENCDB_SERVER_DEBUG
            fprintf(stderr,
                "[EncDB][SELECT] branch=ROWS (select_req.type=%u, not 1-4) "
                "enclave_out doc_count=%u\n",
                static_cast<unsigned>(enc_req.select_req.type),
                res_docs->doc_count);
            if (res_docs->doc_count > 0) {
                encdb_debug_cstr("enclave.content[0]", res_docs->content[0]);
            }
#endif
            s.doc_count = res_docs->doc_count;
            for (int i = 0; i < res_docs->doc_count; i++) {
                s.doc_ids[i] = res_docs->doc_id[i];
                memcpy(s.doc_contents[i], res_docs->content[i], MAX_DOC_SIZE);
            }
#if ENCDB_SERVER_DEBUG
            if (s.doc_count > 0) {
                encdb_debug_cstr("rpc.doc_contents[0]", s.doc_contents[0]);
                encdb_debug_hex("rpc.doc_contents[0].head", s.doc_contents[0], 48);
            }
#endif
        }
        free(res_docs);
    } else if (enc_req.type == RequestType::UPDATE) {
        enc_resp.type = ResponseType::UPDATE_RESULT;
        bool ok = handle_request(enc_req, nullptr, 0);
        UpdateResponse& u = enc_resp.update_resp;
        u.success = ok ? 1 : 0;
        u.doc_id = enc_req.update_req.doc_id;
        if (!ok) {
            enc_resp.status = ResponseStatus::ERROR;
        }
    } else if (enc_req.type == RequestType::SHUTDOWN) {
        enc_resp.type = ResponseType::SHUTDOWN_RESULT;
        const bool ok = handle_request(enc_req, nullptr, 0);
        enc_resp.shutdown_resp.success = ok ? 1 : 0;
        if (!ok) {
            enc_resp.status = ResponseStatus::ERROR;
        }
    } else {
        enc_resp.status = ResponseStatus::ERROR;
        enc_resp.type = ResponseType::INVALID;
    }

    return serialize_encdb_response(enc_resp, req.client_id);
}
