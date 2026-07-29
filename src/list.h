#pragma once

#include <cstddef>

// One node in a circular doubly linked list.
//
// A list is represented by a DList node. When the list is empty,
// its `next` and `prev` pointers both point back to itself.
struct DList {
  DList* prev = nullptr;
  DList* next = nullptr;
};

// Initializes `node` as an empty circular list sentinel.
inline void dlist_init(DList* node) {
  node->prev = node;
  node->next = node;
}

// Returns true when no nodes are linked after this sentinel.
inline bool dlist_empty(const DList* node) {
  return node->next == node;
}

// Removes `node` from its current list.
//
// This reconnects node's neighbors but intentionally does not reset
// node->prev or node->next. Call dlist_init(node) afterward if needed.
inline void dlist_detach(DList* node) {
  DList* prev = node->prev; // save prev node
  DList* next = node->next; // save next node

  prev->next = next;  // connect prev to next
  next->prev = prev;  // connect next to prev
}

// Inserts `rookie` immediately before `target`.
//
// `target` may be the list sentinel, which appends `rookie` to the list.
inline void dlist_insert_before(DList* target, DList* rookie) {
  DList* prev = target->prev; // save prev node

  prev->next = rookie;  // prev -> rookie
  rookie->prev = prev;  // prev <- rookie

  rookie->next = target;  // rookie -> target
  target->prev = rookie;  // rookie <- target
}