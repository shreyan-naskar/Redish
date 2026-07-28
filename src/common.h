#pragma once

#include <cstddef>
#include <cstdint>

// Hashes `len` bytes starting at `data`.
//
// This preserves your current algorithm and 32-bit intermediate state.
// The return type is uint64_t, but the computed value itself is 32-bit.
static uint64_t str_hash(const uint8_t* data, size_t len) {
    uint32_t hash = 0x811C9DC5;

    for (size_t i = 0; i < len; ++i) {
        // Add this byte, then multiply to spread its bits.
        hash = (hash + data[i]) * 0x01000193;
    }

    return hash;
}

// Given a pointer to a member inside an object, recover the pointer to
// the containing object.
//
// Example:
//   HNode* node = ...;
//   Entry* entry = container_of(node, Entry, node);
//
// `ptr` must genuinely point to `member` within an object of `type`.
#define container_of(ptr, type, member) \
    reinterpret_cast<type*>(             \
        reinterpret_cast<char*>(ptr) - offsetof(type, member))