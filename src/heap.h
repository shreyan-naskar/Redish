#pragma once

#include <cstddef>
#include <cstdint>

// One item stored in a binary min-heap.
//
// `val` is the value used for ordering, such as a timer expiration time.
// `ref` points to external storage that records this item's current
// position in the heap. It is updated whenever the item moves.
struct HeapItem {
  uint64_t val = 0;
  size_t* ref = nullptr;
};

// Restores heap order after the item at index `pos` changes.
//
// `a`   : heap-item array.
// `pos` : index of the changed item.
// `len` : number of active items in the heap.
//
// The function also updates each moved item's external position through
// its `ref` pointer.
void heap_update(HeapItem* a, size_t pos, size_t len);