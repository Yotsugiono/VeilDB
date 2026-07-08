#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// 设为 1 可开启 Server/RPC 调试输出（不影响业务逻辑）
#ifndef ENCDB_SERVER_DEBUG
#define ENCDB_SERVER_DEBUG 0
#endif

#if ENCDB_SERVER_DEBUG

inline void encdb_debug_hex(const char* tag, const void* data, size_t len, size_t max_show = 48) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    fprintf(stderr, "[EncDB][%s] hex(%zu):", tag, len);
    const size_t n = len < max_show ? len : max_show;
    for (size_t i = 0; i < n; ++i) {
        fprintf(stderr, " %02x", bytes[i]);
    }
    if (len > max_show) {
        fprintf(stderr, " ...");
    }
    fprintf(stderr, "\n");
}

inline void encdb_debug_cstr(const char* tag, const void* buf, size_t max_show = 96) {
    const char* s = static_cast<const char*>(buf);
    fprintf(stderr, "[EncDB][%s] str:", tag);
    for (size_t i = 0; i < max_show && s[i] != '\0'; ++i) {
        fputc(s[i], stderr);
    }
    fprintf(stderr, "\n");
}

#else

inline void encdb_debug_hex(const char*, const void*, size_t, size_t = 48) {}
inline void encdb_debug_cstr(const char*, const void*, size_t = 96) {}

#endif
