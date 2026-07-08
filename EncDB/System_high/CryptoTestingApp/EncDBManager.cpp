#include "EncDBManager.h"

#include "EdbDiskStore.h"

#include <fstream>

namespace {
constexpr const char* kMetaFilePath = "../Databases/meta.dat";
}

uint32_t DatabaseManager::load_next_db_id() {
    std::ifstream ifs(kMetaFilePath, std::ios::binary);
    if (!ifs.good()) {
        return 1;
    }

    uint32_t id = 1;
    ifs.read(reinterpret_cast<char*>(&id), sizeof(id));
    return id;
}

void DatabaseManager::store_next_db_id(uint32_t next_id) {
    EdbDiskStore::ensure_databases_root();
    std::ofstream ofs(kMetaFilePath, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(&next_id), sizeof(next_id));
}

uint32_t DatabaseManager::register_edb() {
    std::lock_guard<std::mutex> lock(mtx);

    const uint32_t new_edb_id = load_next_db_id();
    EdbDiskStore::ensure_edb_dirs(new_edb_id);
    edbs[new_edb_id] = std::make_unique<EncDatabase>();
    store_next_db_id(new_edb_id + 1);

    return new_edb_id;
}

EncDatabase* DatabaseManager::open_edb(uint32_t db_id, bool create_if_missing) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = edbs.find(db_id);
    if (it != edbs.end()) {
        return it->second.get();
    }

    if (!EdbDiskStore::edb_exists_on_disk(db_id)) {
        if (!create_if_missing) {
            return nullptr;
        }
        EdbDiskStore::ensure_edb_dirs(db_id);
        edbs[db_id] = std::make_unique<EncDatabase>();
        return edbs[db_id].get();
    }

    auto db = std::make_unique<EncDatabase>();
    EdbDiskStore::load_edb(db_id, *db);
    EncDatabase* raw = db.get();
    edbs[db_id] = std::move(db);
    return raw;
}

EncDatabase* DatabaseManager::get_edb(uint32_t db_id) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = edbs.find(db_id);
    if (it == edbs.end()) {
        return nullptr;
    }
    return it->second.get();
}

void DatabaseManager::persist_edb(uint32_t db_id) {
    EncDatabase* edb = get_edb(db_id);
    if (!edb) {
        return;
    }
    EdbDiskStore::persist_edb(db_id, *edb);
}

bool DatabaseManager::edb_exists_on_disk(uint32_t db_id) const {
    return EdbDiskStore::edb_exists_on_disk(db_id);
}

std::vector<uint32_t> DatabaseManager::list_edb_ids_on_disk() const {
    return EdbDiskStore::list_edb_ids_on_disk();
}
