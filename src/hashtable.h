#pragma once

#include <stddef.h>
#include <stdint.h>

// One node in the hash table.
// This is embedded inside the actual user object
// (e.g., a key-value Entry).
struct HNode {
    HNode *next = NULL;   // Next node in the bucket (collision chain)
    uint64_t hcode = 0;   // Cached hash value of the key
};

// One fixed-size hash table.
struct HTab {
    HNode **tab = NULL;   // Array of bucket pointers
    size_t mask = 0;      // bucket_index = hash & mask (capacity = mask + 1)
    size_t size = 0;      // Number of nodes currently stored
};

// Hash map interface.
//
// Uses two hash tables for
// incremental (progressive)
// rehashing during resizing.
struct HMap {
    HTab newer;               // Current hash table
    HTab older;               // Old table being migrated
    size_t migrate_pos = 0;   // Next bucket to migrate from older -> newer
};

// Search for a node matching 'key'.
// Returns pointer to the node if found,
// otherwise nullptr.
// 'eq' is a user-supplied equality function.
HNode *hm_lookup(HMap *hmap,
                 HNode *key,
                 bool (*eq)(HNode *, HNode *));

// Insert a node into the hash map.
void hm_insert(HMap *hmap,
               HNode *node);

// Delete a node matching 'key'.
// Returns the removed node,
// or nullptr if not found.
HNode *hm_delete(HMap *hmap,
                 HNode *key,
                 bool (*eq)(HNode *, HNode *));

// Remove all nodes and free
// internal hash-table storage.
void hm_clear(HMap *hmap);

// Return total number of nodes
// in the hash map.
size_t hm_size(HMap *hmap);

// invoke the callback on each node until it returns false
void   hm_foreach(HMap *hmap, bool (*f)(HNode *, void *), void *arg);