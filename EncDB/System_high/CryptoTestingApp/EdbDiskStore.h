#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EncDatebase.h"

namespace EdbDiskStore {

std::string databases_root();
void ensure_databases_root();
std::string edb_dir(uint32_t edb_id);
std::string bitmaps_dir(uint32_t edb_id);
std::string docs_dir(uint32_t edb_id);
std::string context_snapshot_path(uint32_t edb_id);

void ensure_edb_dirs(uint32_t edb_id);
bool edb_exists_on_disk(uint32_t edb_id);

std::string label_to_hex(const std::string& label);
void write_blob_file(const std::string& path, const std::string& data);
bool read_blob_file(const std::string& path, std::string& out);

void persist_bitmaps(uint32_t edb_id, const EncDatabase& edb);
void persist_docs(uint32_t edb_id, const EncDatabase& edb);
void persist_edb(uint32_t edb_id, EncDatabase& edb);

void load_bitmaps(uint32_t edb_id, EncDatabase& edb);
void load_docs(uint32_t edb_id, EncDatabase& edb);
void load_edb(uint32_t edb_id, EncDatabase& edb);

void write_bitmap_entry(uint32_t edb_id, const std::string& label, const std::string& cipher);
void write_doc_entry(uint32_t edb_id, const std::string& label, const std::string& cipher);

std::vector<uint32_t> list_edb_ids_on_disk();

}  // namespace EdbDiskStore
