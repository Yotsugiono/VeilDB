#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "EncDatebase.h"

class DatabaseManager {
public:
    uint32_t register_edb();
    EncDatabase* open_edb(uint32_t db_id, bool create_if_missing = false);
    EncDatabase* get_edb(uint32_t db_id);
    void persist_edb(uint32_t db_id);
    bool edb_exists_on_disk(uint32_t db_id) const;
    std::vector<uint32_t> list_edb_ids_on_disk() const;

private:
    std::mutex mtx;
    std::unordered_map<uint32_t, std::unique_ptr<EncDatabase>> edbs;

    uint32_t load_next_db_id();
    void store_next_db_id(uint32_t next_id);
};
