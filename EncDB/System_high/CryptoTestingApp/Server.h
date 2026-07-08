#ifndef SERVER_H
#define SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sgx_urts.h"
#include <csignal>
#include "EncDBManager.h"
#include "EncDBRequest.h"
#include "EncDatebase.h"
#include "RPC.h"
#include "../common/data_type.h"

struct Task {
    EncDBRequest req;
    void* out;
    size_t out_len;
    std::promise<bool> done;
};

class EnclaveInstance {
public:
    sgx_enclave_id_t eid;
    std::atomic<bool> busy{false};
    int last_client_id = -1;

    bool try_acquire() {
        bool expected = false;
        return busy.compare_exchange_strong(expected, true);
    }

    void release() {
        busy.store(false);
    }
};

class EnclavePool {
public:
    ~EnclavePool();

    void init(size_t pool_size);
    EnclaveInstance* acquire(int client_id);
    void release(EnclaveInstance* instance, int client_id);

private:
    std::mutex pool_mtx;
    std::unordered_map<int, EnclaveInstance*> client_enclave_map;
    std::vector<std::unique_ptr<EnclaveInstance>> enclaves;
    std::queue<EnclaveInstance*> available_enclaves;
};

class Server {
public:
    Server();
    ~Server();

    void init(int enclave_num = 4, int worker_num = 4);
    void release();
    void shutdown_flush();

    uint32_t EDB_Register(uint8_t client_id);
    bool flush_client_edb(uint8_t client_id, sgx_enclave_id_t eid);

    Response process(const Request& req);

    static void store_bitmap(EncDatabase& edb, label_struct* outs, cipher_struct* outs_res, size_t out_amt);
    static std::string load_bitmap(EncDatabase& edb, label_struct* ins);
    static void store_docs(EncDatabase& edb, label_struct* outs, encdoc_struct* outs_res, size_t out_amt);
    static std::vector<std::string> load_docs(EncDatabase& edb, label_struct* ins, size_t in_amt);
    static std::vector<std::string> Retrieve_Encrypted_Docs(uint32_t edb_id, uint8_t* bitmap, size_t len);

    void worker_loop();
    bool handle_request(const EncDBRequest& req, void* out, size_t out_len);
    bool handle_request_core(const EncDBRequest& req, void* out, size_t out_len);

private:
    DatabaseManager edb_manager;
    EnclavePool enclave_pool;

    std::unordered_map<uint8_t, uint32_t> edb_id_map;

    std::queue<Task*> task_queue;
    std::mutex queue_mtx;
    std::condition_variable queue_cv;
    std::vector<std::thread> workers;
    bool stop_flag = false;
};

extern volatile sig_atomic_t g_shutdown_requested;
void handle_shutdown_signal(int signo);

#endif
