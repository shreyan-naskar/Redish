#include <cassert>
#include <cstring>
#include <cstdlib>
// proj
#include "zset.h"
#include "common.h"

using namespace std;

// Creates one ZSet member with a variable-length name.
//
// The allocation includes space for the ZNode itself plus `len` bytes
// to store the name after the struct.
static ZNode* znode_new(const char* name, size_t len, double score) {
  // ZNode::name is trailing storage, allocate extra bytes for it
  ZNode* node = static_cast<ZNode*>(
    malloc(sizeof(ZNode) + len)
  );

  // For production code, return nullptr or handle allocation failure
  // instead of aborting through assert().
  assert(node);

  // Initialize the AVL-tree metadata.
  avl_init(&node->tree);

  // initialize hashmap metadata
  node->hmap.next = nullptr;
  node->hmap.hcode = str_hash(
    reinterpret_cast<const uint8_t*> (name),
    len
  );

  // store sorted set data
  node->score = score;
  node->len = len;

  // copy exactly len name bytes
  // stored name doesn't need a trailing \0
  memcpy(node->name, name, len);

  return node;
}

// Releases a ZNode created by znode_new().
static void znode_del(ZNode* node) {
  free(node);
}

// Returns the smaller of two size values.
static size_t min(size_t lhs, size_t rhs) {
  return (lhs < rhs) ? lhs : rhs;
}

// Returns true when the ZNode containing `lhs` sorts before
// the supplied (score, name) lookup tuple.
//
// Ordering rules:
// 1. Lower score comes first.
// 2. When scores are equal, compare names byte-by-byte.
// 3. If one name is a prefix of the other, the shorter name comes first.
static bool zless(AVLNode* lhs, double score, const char* name, size_t len) {
  ZNode* left = container_of(lhs, ZNode, tree);

  // scores are primary sort key
  if (left->score != score) return left->score < score;

  // compare the shared position of both names
  int result = memcmp(left->name, name, min(left->len, len));

  if (result != 0) return result < 0;

  // equal prefix: shorter name sorts first
  return left->len < len;
}

// Compares two AVL nodes by converting them back to ZNode objects.
//
// Returns true when `lhs` sorts before `rhs` by the (score, name) tuple.
static bool zless(AVLNode* lhs, AVLNode* rhs) {
  // Recover rhs's score and name, then reuse the existing tuple comparison.
  ZNode* right = container_of(rhs, ZNode, tree);

  return zless(
    lhs,
    right->score,
    right->name,
    right->len
  );
}

// Inserts `node` into the sorted-set AVL tree.
//
// The tree is ordered by (score, name). Equal keys are placed in the
// right subtree, although names should normally be unique because the
// hash-map index prevents duplicate members.
static void tree_insert(ZSet* zset, ZNode* node) {
  AVLNode* parent = nullptr;
  AVLNode** from = &zset->root;

  // search for an empty child position
  while (*from) {
    parent = *from;

    // smaller tuples go left, equal or greater tuples go right
    from = zless(&node->tree, parent) ? &parent->left : &parent->right;
  }

  // attach AVL node at the empty position
  *from = &node->tree;
  node->tree.parent = parent;

  zset->root = avl_fix(&node->tree);
}

// Updates an existing member's score.
//
// Because the AVL tree is ordered by (score, name), changing a score means
// the node may belong at a different tree position. Remove it, reset its
// AVL links/metadata, then insert it again.
//
// The hash-map entry is unchanged because the member name does not change.
static void zset_update(ZSet* zset, ZNode* node, double score) {
  // No tree work is needed when the score already matches.
  if (node->score == score) {
    return;
  }

  // Remove this node from the score-ordered AVL tree.
  zset->root = avl_del(&node->tree);

  // avl_del() detaches the node but its AVL field contain old links
  // reset them before insertion
  avl_init(&node->tree);

  // change the score, then reinsert at its new sorted position
  node->score = score;
  tree_insert(zset, node);
}

// Adds a new (score, name) member, or updates an existing member's score.
//
// Returns:
// - true:  a new member was created.
// - false: the name already existed; only its score was updated.
bool zset_insert(ZSet* zset, const char* name, size_t len, double score) {
  // names are unique, so look up the hashmap index first
  ZNode* node = zset_lookup(zset, name, len);

  if (node) {
    // existing name: retain node and update only its score
    zset_update(zset, node, score);
    return false;
  }

  // new name: create new znode, then index the same object in both
  // name based hashmap and score based AVL Tree
  node = znode_new(name, len, score);

  hm_insert(&zset->hmap, &node->hmap);
  tree_insert(zset, node);

  return true;
}

// Temporary lookup key used only for hash-map searches.
//
// It has an HNode so it can be passed to hm_lookup(), plus the name bytes
// needed to compare a real ZNode against the requested name.
struct HKey {
  HNode node;
  const char* name = nullptr;
  size_t len = 0;
};

// Compares a stored ZNode against a temporary HKey.
//
// Returns true only when both names have the same length and bytes.
static bool hcmp(HNode* node, HNode* key){
  // convert stored hash node back to its containing ZNode
  ZNode* znode = container_of(node, ZNode, hmap);

  // convert the temp lookup node to its HKey wrapper
  HKey* hkey = container_of(key, HKey, node);

  // Names of different lengths cannot be equal.
  if (znode->len != hkey->len) return false;

  // compare exactly len-bytes; names are not required to end in \0
  return memcmp(znode->name, hkey->name, znode->len) == 0;
}

// Finds a sorted-set member by name.
//
// Returns nullptr when no member has that name.
ZNode* zset_lookup(ZSet* zset, const char* name, size_t len) {
  // empty tree-> empty sorted set
  if (!zset->root) return nullptr;

  // build a temp key with the same hash and comparison data
  // as the member being searched for;
  HKey key;
  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(name),
    len
  );

  key.name = name;
  key.len = len;

  // lookup the matching hash node, then recover its ZNode
  HNode* found = hm_lookup(&zset->hmap, &key.node, hcmp);

  return found ? container_of(found, ZNode, hmap) : nullptr;
}

// Removes `node` from both ZSet indexes, then frees its memory.
//
// Precondition: `node` belongs to `zset`.
void zset_delete(ZSet* zset, ZNode* node) {
  // Build a temporary hash-map key that identifies this node by name.
  HKey key;
  key.node.hcode = node->hmap.hcode;
  key.name = node->name;
  key.len = node->len;

  // Remove the member from the name-based hash-map index.
  HNode* found = hm_delete(&zset->hmap, &key.node, hcmp);

  // The node should exist because the caller supplied a member of zset.
  assert(found);

  // Remove the same member from the score/name-ordered AVL tree.
  zset->root = avl_del(&node->tree);

  // Both indexes no longer refer to it, so its storage is safe to release.
  znode_del(node);
}

// Finds the first member whose (score, name) tuple is >= the requested key.
//
// Returns nullptr when every member sorts before the requested tuple.
ZNode* zset_seekge(ZSet* zset, double score, const char* name, size_t len) {
  AVLNode* found = nullptr;  // Best candidate found so far.

  // Standard lower-bound search in a binary search tree.
  for (AVLNode* node = zset->root; node;) {
    if (zless(node, score, name, len)) {
      // Current node is less than the key, so a valid answer
      // can only be in its right subtree.
      node = node->right;
    }
    else {
      // Current node is >= key. Save it, but look left for a
      // smaller valid candidate.
      found = node;
      node = node->left;
    }
  }

  return found ? container_of(found, ZNode, tree) : nullptr;
}

// Returns the ZNode `offset` positions from `node` in sorted-set order.
//
// Positive offsets move toward larger (score, name) tuples.
// Negative offsets move toward smaller tuples.
// Returns nullptr if `node` is null or the target position is outside
// the AVL tree.
ZNode* znode_offset(ZNode* node, int64_t offset) {
    // avl_offset() operates on the embedded AVLNode.
    AVLNode* tree_node = node
        ? avl_offset(&node->tree, offset)
        : nullptr;

    // Convert the resulting AVL node back to its containing ZNode.
    return tree_node
        ? container_of(tree_node, ZNode, tree)
        : nullptr;
}

// Recursively frees every ZNode reachable from this AVL-tree node.
//
// The hash map has already been cleared, so only the tree still owns
// references to the ZNode objects.
static void tree_dispose(AVLNode* node) {
  if (!node) {
    return;
  }

  // Visit children first because deleting `node` frees its memory.
  tree_dispose(node->left);
  tree_dispose(node->right);

  // Convert the embedded tree node back to ZNode, then release it.
  znode_del(container_of(node, ZNode, tree));
}

// Deletes every member and resets the sorted set to an empty state.
void zset_clear(ZSet* zset) {
  // Free the hash-map bucket arrays. This does not free ZNode objects.
  hm_clear(&zset->hmap);

  // The AVL tree still reaches every ZNode, so free them through it.
  tree_dispose(zset->root);

  // Mark the tree as empty.
  zset->root = nullptr;
}