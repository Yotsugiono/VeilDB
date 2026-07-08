#include "EdbDiskStore.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <cstdint>
#include <fstream>

namespace EdbDiskStore {
namespace {

constexpr const char* kDatabasesRoot = "../Databases";

bool mkdir_one(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

void persist_map_dir(
    uint32_t edb_id,
    const char* subdir,
    const std::unordered_map<std::string, std::string>& entries
) {
    ensure_edb_dirs(edb_id);
    const std::string dir = std::string(edb_dir(edb_id)) + "/" + subdir;
    for (const auto& entry : entries) {
        write_blob_file(dir + "/" + label_to_hex(entry.first) + ".bin", entry.second);
    }
}

void load_map_dir(
    uint32_t edb_id,
    const char* subdir,
    std::unordered_map<std::string, std::string>& out
) {
    const std::string dir = std::string(edb_dir(edb_id)) + "/" + subdir;
    DIR* dp = opendir(dir.c_str());
    if (!dp) {
        return;
    }

    while (dirent* ent = readdir(dp)) {
        const char* name = ent->d_name;
        if (name[0] == '.') {
            continue;
        }
        std::string filename(name);
        if (filename.size() < 5 || filename.substr(filename.size() - 4) != ".bin") {
            continue;
        }
        const std::string hex_label = filename.substr(0, filename.size() - 4);
        if (hex_label.size() % 2 != 0) {
            continue;
        }
        std::string label;
        label.reserve(hex_label.size() / 2);
        for (size_t i = 0; i < hex_label.size(); i += 2) {
            const std::string byte_str = hex_label.substr(i, 2);
            label.push_back(static_cast<char>(std::stoi(byte_str, nullptr, 16)));
        }
        std::string cipher;
        if (read_blob_file(dir + "/" + filename, cipher)) {
            out[label] = std::move(cipher);
        }
    }
    closedir(dp);
}

}  // namespace

std::string databases_root() {
    return kDatabasesRoot;
}

void ensure_databases_root() {
    mkdir_one(databases_root());
}

std::string edb_dir(uint32_t edb_id) {
    return databases_root() + "/edb_" + std::to_string(edb_id);
}

std::string bitmaps_dir(uint32_t edb_id) {
    return edb_dir(edb_id) + "/bitmaps";
}

std::string docs_dir(uint32_t edb_id) {
    return edb_dir(edb_id) + "/docs";
}

std::string context_snapshot_path(uint32_t edb_id) {
    return edb_dir(edb_id) + "/context.dat";
}

void ensure_edb_dirs(uint32_t edb_id) {
    mkdir_one(databases_root());
    mkdir_one(edb_dir(edb_id));
    mkdir_one(bitmaps_dir(edb_id));
    mkdir_one(docs_dir(edb_id));
}

bool edb_exists_on_disk(uint32_t edb_id) {
    struct stat st {};
    return stat(context_snapshot_path(edb_id).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string label_to_hex(const std::string& label) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(label.size() * 2);
    for (unsigned char c : label) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0f]);
    }
    return out;
}

void write_blob_file(const std::string& path, const std::string& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.good()) {
        printf("Warning: failed to write blob file %s\n", path.c_str());
        return;
    }
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool read_blob_file(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        return false;
    }
    ifs.seekg(0, std::ios::end);
    const std::streamsize len = ifs.tellg();
    if (len < 0) {
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(len));
    if (len > 0) {
        ifs.read(&out[0], len);
    }
    return true;
}

void persist_bitmaps(uint32_t edb_id, const EncDatabase& edb) {
    persist_map_dir(edb_id, "bitmaps", edb.bitmaps);
}

void persist_docs(uint32_t edb_id, const EncDatabase& edb) {
    persist_map_dir(edb_id, "docs", edb.docs);
}

void persist_edb(uint32_t edb_id, EncDatabase& edb) {
    std::lock_guard<std::mutex> lock(edb.mtx);
    persist_bitmaps(edb_id, edb);
    persist_docs(edb_id, edb);
}

void load_bitmaps(uint32_t edb_id, EncDatabase& edb) {
    load_map_dir(edb_id, "bitmaps", edb.bitmaps);
}

void load_docs(uint32_t edb_id, EncDatabase& edb) {
    load_map_dir(edb_id, "docs", edb.docs);
}

void load_edb(uint32_t edb_id, EncDatabase& edb) {
    std::lock_guard<std::mutex> lock(edb.mtx);
    edb.bitmaps.clear();
    edb.docs.clear();
    load_bitmaps(edb_id, edb);
    load_docs(edb_id, edb);
}

void write_bitmap_entry(uint32_t edb_id, const std::string& label, const std::string& cipher) {
    ensure_edb_dirs(edb_id);
    write_blob_file(bitmaps_dir(edb_id) + "/" + label_to_hex(label) + ".bin", cipher);
}

void write_doc_entry(uint32_t edb_id, const std::string& label, const std::string& cipher) {
    ensure_edb_dirs(edb_id);
    write_blob_file(docs_dir(edb_id) + "/" + label_to_hex(label) + ".bin", cipher);
}

std::vector<uint32_t> list_edb_ids_on_disk() {
    std::vector<uint32_t> ids;
    DIR* dp = opendir(databases_root().c_str());
    if (!dp) {
        return ids;
    }

    while (dirent* ent = readdir(dp)) {
        const char* name = ent->d_name;
        if (strncmp(name, "edb_", 4) != 0) {
            continue;
        }
        char* end = nullptr;
        const unsigned long id = strtoul(name + 4, &end, 10);
        if (end == name + 4 || *end != '\0') {
            continue;
        }
        if (id == 0 || id > UINT32_MAX) {
            continue;
        }
        if (edb_exists_on_disk(static_cast<uint32_t>(id))) {
            ids.push_back(static_cast<uint32_t>(id));
        }
    }
    closedir(dp);
    return ids;
}

}  // namespace EdbDiskStore
