#pragma once

#include <cstddef>
#include <cstdint>

#include "datastructures/avl.h"
#include "datastructures/hashtable.h"

// Sorted set.
//
// It maintains two indexes over the same ZNode objects:
// - `root`: AVL tree ordered by (score, name)
// - `hmap`: hash map indexed by name
struct ZSet {
    AVLNode* root = nullptr;
    HMap hmap;
};

// One sorted-set entry.
struct ZNode {
    AVLNode tree;  // Tree index metadata.
    HNode hmap;    // Hash-map index metadata.

    double score = 0.0;  // Numeric score used for ordering.
    size_t len = 0;      // Number of bytes in `name`.

    // Trailing storage for a variable-length name.
    //
    // `char name[0]` is a compiler extension, not standard C++.
    // Use one byte here and allocate extra trailing bytes when creating
    // a ZNode. The name is length-prefixed by `len`, not null-terminated.
    char name[1];
};

// Insert a member, or update it if the name already exists.
// Returns true when a new member was added.
bool zset_insert(ZSet* zset, const char* name, size_t len, double score);

// Find a member by its name. Returns nullptr when absent.
ZNode* zset_lookup(ZSet* zset, const char* name, size_t len);

// Remove `node` from both the AVL tree and hash-map indexes.
void zset_delete(ZSet* zset, ZNode* node);

// Find the first member whose (score, name) is >= the supplied position.
// Returns nullptr when no such member exists.
ZNode* zset_seekge(
    ZSet* zset,
    double score,
    const char* name,
    size_t len
);

// Delete every member and clear both indexes.
void zset_clear(ZSet* zset);

// Return the member `offset` positions from `node` in sorted-set order.
// Returns nullptr when that position is outside the set.
ZNode* znode_offset(ZNode* node, int64_t offset);