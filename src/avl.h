#pragma once

#include <cstddef>
#include <cstdint>

// One node in an AVL tree.
//
// The tree stores links to the parent and both children.
// `height` helps keep the tree balanced.
// `cnt` stores the number of nodes in this node's subtree.
struct AVLNode {
  AVLNode *parent = nullptr;
  AVLNode *left = nullptr;
  AVLNode *right = nullptr;

  uint32_t height = 0; // height subtree rooted at this node
  uint32_t cnt = 0; // # of nodes in this node's subtree
};

// init node before insertion
inline void avl_init(AVLNode *node) {
  node->parent = nullptr;
  node->left = nullptr;
  node->right = nullptr;

  // A newly created node has no children
  node->height = 1; // just itself
  node->cnt = 1; // just itself
}

// Returns a node's subtree height.
// A missing child contributes height 0.
inline uint32_t avl_height(const AVLNode* node) {
  return node ? node->height : 0;
}

// Returns a node's subtree size.
// A missing child contributes zero nodes.
inline uint32_t avl_cnt(const AVLNode* node) {
  return node ? node->cnt : 0;
}

// Restores AVL metadata/balance starting at `node`.
// Returns the new root of the repaired subtree.
AVLNode* avl_fix(AVLNode* node);

// Removes `node` from its AVL tree.
// Returns the new root of the affected subtree/tree.
AVLNode* avl_del(AVLNode* node);