// EncDatebase.h
#pragma once
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <string>
#include <mutex>


struct EncDatabase {
    std::mutex mtx;
    std::unordered_map<std::string, std::string> bitmaps;
    std::unordered_map<std::string, std::string> docs;
};
