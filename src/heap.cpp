#include "heap.h"

// Returns the parent index of heap item `i`.
//
// For a zero-based binary heap:
//
//         0
//       /   \
//      1     2
//     / \   / \
//    3   4 5   6
//
// This function must only be called when i > 0.
static size_t heap_parent(size_t i) {
  return (i + 1) / 2 - 1;
}

// Returns the left-child index of heap item `i`.
//
// The index may be outside the heap; callers must compare it with
// the heap length before accessing the array.
static size_t heap_left(size_t i) {
  return i * 2 + 1;
}

// Returns the right-child index of heap item `i`.
//
// The index may be outside the heap; callers must compare it with
// the heap length before accessing the array.
static size_t heap_right(size_t i) {
  return i * 2 + 2;
}

// Moves the item at `pos` upward until min-heap order is restored.
//
// Min-heap rule:
//   Every parent value must be less than or equal to its child values.
//
// Call this after decreasing a[pos].val or after appending a new item.
static void heap_up(HeapItem* heap, size_t pos) {
  // Save the changed item. Its parent nodes are shifted downward
  // until the correct position for this item is found.
  HeapItem item = heap[pos];

  while (
    pos > 0
    && heap[heap_parent(pos)].val > item.val
  ) {
    // Move the parent down into the current position.
    heap[pos] = heap[heap_parent(pos)];

    // Keep the moved parent's external heap-index reference correct.
    *heap[pos].ref = pos;

    // Continue from the parent's former position.
    pos = heap_parent(pos);
  }

  // Place the saved item at its final valid heap position.
  heap[pos] = item;

  // Update this item's externally stored heap index.
  *heap[pos].ref = pos;
}

// Moves the item at `pos` downward until min-heap order is restored.
//
// Min-heap rule:
//   A parent value must be less than or equal to both child values.
//
// Call this after increasing heap[pos].val or after replacing/removing
// an item at this position.
static void heap_down(HeapItem* heap, size_t pos, size_t len) {
  // Save the changed item while smaller child items are shifted upward.
  HeapItem item = heap[pos];

  while (true) {
    // Calculate this node's children in the zero-based heap array.
    size_t left = heap_left(pos);
    size_t right = heap_right(pos);

    // Start by assuming the saved item is already smallest.
    size_t smallest_pos = pos;
    uint64_t smallest_val = item.val;

    // Use the left child when it exists and is smaller.
    if (
      left < len 
      && heap[left].val < smallest_val
    ) {
      smallest_pos = left;
      smallest_val = heap[left].val;
    }

    // Use the right child when it exists and is smaller than both
    // the saved item and the left child selected above.
    if (
      right < len 
      && heap[right].val < smallest_val
    ) {
      smallest_pos = right;
      smallest_val = heap[right].val;
    }

    // Neither child is smaller: heap order is restored.
    if (smallest_pos == pos) break;

    // Move the smaller child upward into the current position.
    heap[pos] = heap[smallest_pos];

    // Keep that moved child's external heap-index reference correct.
    *heap[pos].ref = pos;

    // Continue from the child's former position.
    pos = smallest_pos;
  }

  // Place the saved item in its final valid position.
  heap[pos] = item;

  // Record the saved item's new heap index.
  *heap[pos].ref = pos;
}

// Restores min-heap order after the value at `pos` changes.
//
// If the item became smaller than its parent, move it upward.
// Otherwise, it may be larger than a child, so move it downward.
void heap_update(HeapItem* heap, size_t pos, size_t len) {
  // A non-root item violates heap order upward when its parent
  // has a larger value.
  if (
    pos > 0
    && heap[heap_parent(pos)].val > heap[pos].val
  ) {
    heap_up(heap, pos);
  }
  else {
    // This also safely handles a root item or an unchanged value.
    heap_down(heap, pos, len);
  }
}