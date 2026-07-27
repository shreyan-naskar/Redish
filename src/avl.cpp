#include <assert.h>
#include "avl.h"

static uint32_t max(uint32_t lhs, uint32_t rhs) {
  return lhs < rhs ? rhs : lhs;
}

// Recalculates metadata for `node` after its children change.
static void avl_update(AVLNode *node) {
  node->height = 1 + max(
    avl_height(node->left),
    avl_height(node->right)
  );
  node->cnt = 1 + // for this node
    avl_cnt(node->left) + // all nodes in the left
    avl_cnt(node->right); // all nodes in the right
}

// Performs a left rotation around `node`.
//
// Before:
//       node                    new_node
//      /    \                  /        \
//     A   new_node    ->    node         C
//        /      \            /  \
//    inner       C          A   inner
//
// Returns `new_node`, the new root of this subtree.
//
// Note: The caller must reconnect the returned subtree root to the
// original grandparent, if one exists.
static AVLNode* rot_left(AVLNode *node) {
  AVLNode *parent = node->parent; // org parent of this subtree
  AVLNode *new_node = node->right; // right child becomes new node
  AVLNode *inner = new_node->left; // subtree to move

  // move inner from now_node's left to node's right
  node->right = inner;
  if (inner) inner->parent = node;

  // preeserve org parent relationship for new subtree root
  new_node->parent = parent;

  // make old root the left child of the new root
  new_node->left = node;
  node->parent = new_node;

  // recalculate 
  // Recalculate bottom-up: node changed first, then new_node.
  avl_update(node);
  avl_update(new_node);

  return new_node;
}

// Performs a right rotation around `node`.
//
// Before:
//          node                 new_node
//         /    \               /        \
//   new_node     C    ->      A        node
//    /      \                          /   \
//   A      inner                    inner   C
//
// Returns `new_node`, the new root of this subtree.
//
// The caller must reconnect this returned root to the original
// grandparent, when one exists.
static AVLNode* rot_right(AVLNode *node) {
  AVLNode *parent = node->parent; // org parent of this subtree
  AVLNode *new_node = node->left; // left child becomes new node
  AVLNode *inner = new_node->right; // subtree to move

  //move inner from from new_node's right side to node's left side.
  node->left = inner;
  if (inner) inner->parent = node;

  // Preserve the original parent relationship for the new subtree root.
  new_node->parent = parent;

  // Make the old root the right child of the new root.
  new_node->right = node;
  node->parent = new_node;

  // Recalculate metadata from the bottom upward.
  avl_update(node);
  avl_update(new_node);
  
  return new_node;
}

// Repairs a node whose left subtree is two levels taller than its right.
//
// Handles both left-left and left-right imbalance:
//
// Left-left:               Left-right:          After:
//       node                    node            left
//       /                       /                /   \
//    left                    left            child   node
//    /                         \
// child                       child
//
// For left-right, rotate the left child left first.
// Then rotate `node` right in both cases.
static AVLNode* avl_fix_left(AVLNode *node) {
  // If left's right subtree is taller, this is the left-right case.
  if (avl_height(node->left->left) < avl_height(node->left->right)) {
      // First rotation: turn left-right into left-left.
      node->left = rot_left(node->left);
  }
  // Final rotation: the left child becomes this subtree's root.
  return rot_right(node);
}

// Repairs a node whose right subtree is two levels taller than its left.
//
// Handles both right-right and right-left imbalance:
//
// Right-right:              Right-left:        After:
//   node                       node            right
//     \                          \              /   \
//     right                      right       node    child
//       \                       /
//      child                  child
//
// For right-left, rotate the right child right first.
// Then rotate `node` left in both cases.
static AVLNode* avl_fix_right(AVLNode* node) {
  // If right's left subtree is taller, this is the right-left case.
  if (avl_height(node->right->right) < avl_height(node->right->left)) {
    // First rotation: turn right-left into right-right.
    node->right = rot_right(node->right);
  }

  // Final rotation: the right child becomes this subtree's root.
  return rot_left(node);
}

// Walks upward from `node` to the tree root.
//
// At each level it:
// 1. Recalculates height and subtree count.
// 2. Rotates if the node is unbalanced.
// 3. Continues with the parent, whose metadata may have changed.
//
// Returns the final root of the AVL tree.
AVLNode* avl_fix(AVLNode *node) {
  while (true) {
    // `from` is the pointer that owns this subtree's root.
    //
    // If `node` has a parent, it points to either parent->left or
    // parent->right. If node is the overall root, it points to the
    // local `node` variable, so a root rotation can replace it.
    AVLNode **from = &node;

    AVLNode *parent = node->parent;
    if (parent) {
      // find which child pointer of parent refers to node
      from = (parent->left == node) ? &parent->left : &parent->right;
    }

    // update this node after one of its children may have changed.
    avl_update(node);

    // compare heights of this node after one of its children may have changed
    uint32_t left_height = avl_height(node->left);
    uint32_t right_height = avl_height(node->right);

    // AVL permits a height difference of atmost 1
    if (left_height == right_height + 2) {
      // left imbalance. Rotation may replace this subtree root
      *from = avl_fix_left(node);
    }
    if (right_height == left_height + 2) {
      // right imbalance. Rotation may replace this subtree root
      *from = avl_fix_right(node);
    }

    // No parent means this was the overall tree root.
    // Return either the original root or the root produced by rotation.
    if (!parent) {
      return *from;
    }

    // Continue upward because the parent's height/count may now differ.
    node = parent;
  }
}

// Removes `node` when it has at most one child.
//
// This is the simple deletion case:
//
//   No children:         One child:
//       node                node
//       /  \                /
//    null  null          child
//
// The caller is responsible for freeing the object that contains `node`
// after this function detaches it.
//
// Returns the new root of the AVL tree.
static AVLNode* avl_del_one(AVLNode *node) {
  // This helper only handles leaves and nodes with one child.
  assert(!node->left || !node->right);

  // use existing child, if any. A leaf produces nullptr here
  AVLNode *child = node->left ? node->left : node->right;

  // save the parent as node will be detatched
  AVLNode *parent = node->parent;

  // The child takes node's former position, so update its parent link.
  if (child) child->parent = parent;

  // if node was tree root, child becomes new root
  if (!parent) return child;

  // find parent's pointer that currently points at node
  AVLNode **from = (parent->left == node) ? &parent->left : &parent->right;

  // replace node with child
  *from = child;

  // removing node can cause imbalance -> restore balance
  return avl_fix(parent);
}

// Removes `node` from the AVL tree.
//
// If node has two children, replace it with its in-order successor:
// the smallest node in node's right subtree.
//
// Returns the new root of the AVL tree.
//
// The caller must free the object containing the original `node` after
// this function detaches it.
AVLNode* avl_del(AVLNode *node) {
  // a leaf or a node with one child uses avl_del_one
  if (!node->left || !node->right) return avl_del_one(node);

  // Find the in-order successor:
  // start in the right subtree, then keep moving left.
  AVLNode *victim = node->right;
  while (victim->left) {
    victim = victim->left;
  }

  // successor can't have a left child (while loop condition)
  // delete; successor is new root
  AVLNode *root = avl_del_one(victim);

  // Copy node's AVL links and metadata into the detached successor.
  //
  // This is intentional: victim now takes node's former position in the
  // tree. Only AVLNode fields are copied, not the key/value object fields
  // in the containing structure.
  *victim = *node;

  // victim now owns node's former children, so repair their parent links
  if (victim->left) victim->left->parent = victim;
  if (victim->right) victim->right->parent = victim;

  // Find the pointer that previously referred to `node`.
  //
  // If node was the root, update `root`; otherwise update the appropriate
  // child pointer in node's current parent.
  AVLNode **from = &root;
  AVLNode *parent = node->parent;

  if (parent) {
    from = (parent->left == node) ? &parent->left : &parent->right;
  }

  // replace the removed node with its successor
  *from = victim;

  return root;
}