#include <assert.h>
#include <stdlib.h>     // calloc(), free()
#include "hashtable.h"

// open chaining for hash tables

// Initialize a hash table.
// n must be a power of 2.
static void h_init(HTab* htab, size_t n) {
  assert(n > 0 && ((n - 1) & n) == 0); // verify n is a power of 2

  // allocate buckets
  // each element is a pointer to the head of a linked list
  htab->tab = (HNode **)calloc(n, sizeof(HNode *)); 
  htab->mask = n-1; // used for hash & mask equivalent to hash%mask
  htab->size = 0;
}

// Insert node at the HEAD of the
// appropriate bucket.
static void h_insert(HTab *htab, HNode *node) {
  size_t pos = node->hcode & htab->mask; // bucket index
  
  // old: bucket -> A-> B
  // new node: C
  node->next = htab->tab[pos];
  // C -> A -> B
  htab->tab[pos] = node;
  // now: bucket -> C -> A ->B
  htab->size++;
}

// Find a node.
// Returns a POINTER to the POINTER that owns the node. -> HNode**
//
// Useful for deletion.
// Why? 
//
// if curr -> B is returned --> need to know who points to B to delete
// hence pointer to pointer ownning the node is returned
// thus, deletion becomes: *from = curr->next
static HNode** h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode*, HNode*)) {
  // bucket array not allocated
  if (!htab->tab) return NULL;

  // find bucket of key
  size_t pos = key->hcode & htab->mask;
  HNode **from = &htab->tab[pos];
  // HNode** from -> bucket -> A

  while (*from) { // pointer to A and traverse

    HNode *curr = *from;

    // compare hash and actual key
    if (curr->hcode == key->hcode && eq(curr, key)) return from;

    from = &curr->next;
  }
  return NULL;
}

// Remove a node from a bucket
static HNode *h_detach(HTab *htab, HNode **from) {
  HNode *node = *from;
  *from = node->next;
  htab->size--;

  return node;

}

// Number of buckets migrated
// per operation.
const size_t k_rehashing_work = 128;

// Move a few nodes from the old
// table into the new table.
static void hm_help_rehashing(HMap *hmap) {
  size_t nwork = 0; // nodes migrated

  while (nwork < k_rehashing_work && hmap->older.size > 0) {
    // current bucket
    HNode **from = &hmap->older.tab[hmap->migrate_pos];
    // empty bucket
    if (!*from)  {
      hmap->migrate_pos++; // next bucket
      continue;
    }
    // delete head node from current bucket
    // insert into new bucket
    // Example:
    // old hmap: bucket_4- > B -> C -> D
    // delete: bucket_4 -> C -> D
    // new hmap: bucket_6 -> B
    h_insert(&hmap->newer, h_detach(&hmap->older, from));

    nwork++; // one node moved
  }

  // old table completely migrated
  if (hmap->older.size == 0 && hmap->older.tab) {
    free(hmap->older.tab);
    hmap->older = HTab(); // reset
  }
}

// Start resizing.
//
// Older <- current table
// Newer <- empty larger table
static void hm_trigger_rehashing(HMap *hmap) {
  // ensure older is empty
  // else resizing ongoing
  assert(hmap->older.tab == NULL);

  // copy newer to older
  // both point to same bucket array
  hmap->older = hmap->newer;

  // new size: new capacity = 2 * old capacity
  // old capacity = oldmask + 1
  h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);

  hmap->migrate_pos = 0; // resest migrate pos fro new table
}

// Lookup a node.
// Search newer first,
// then older.
HNode* hm_lookup(HMap *hmap, HNode* key, bool (*eq)(HNode*, HNode*)) {
  // perform migrations
  hm_help_rehashing(hmap);

  // search in new table
  HNode** from = h_lookup(&hmap->newer, key, eq);
  // not found
  if (!from) {
    // search in old table
    from = h_lookup(&hmap->older, key, eq);
  }
  // return node if found else NULL
  return from ? *from : NULL;
}

// Maximum allowed load factor
const size_t k_max_load_factor = 8;

// Insert into the newer table.
// Trigger resize if needed.
void hm_insert(HMap *hmap, HNode *node) {
  // if first insertion, create hmap
  if (!hmap->newer.tab) h_init(&hmap->newer, 4);

  // even if resizing in progress
  // always insert into new table
  h_insert(&hmap->newer, node);

  // if older.tab != nullptr, a resize is already underway
  if(!hmap->older.tab) {
    // capacity * load_factor(average nodes per bucket)
    size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;

    // size exceeds threshold
    // trigger resizing; prep new and old hmaps
    if (hmap->newer.size >= threshold) hm_trigger_rehashing(hmap);
  }
  // attempt migration if needed
  hm_help_rehashing(hmap);
}

// delete a node
HNode* hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode*, HNode*)) {
  // do some migrations
  hm_help_rehashing(hmap);

  // find in new hmpa
  if (HNode **from = h_lookup(&hmap->newer, key, eq)){
    return h_detach(&hmap->newer, from);
  }

  // find in old hmap
  if (HNode **from = h_lookup(&hmap->older, key, eq)) {
    return h_detach(&hmap->older, from);
  }

  return NULL;
}


// Free all allocated buckets.
void hm_clear(HMap *hmap)
{
    free(hmap->newer.tab);
    free(hmap->older.tab);

    *hmap = HMap{};
}

// Total number of elements.
size_t hm_size(HMap *hmap)
{
    return hmap->newer.size +
           hmap->older.size;
}

// Traverse one hash table.
//
// Calls function 'f' for every node.
// Stops immediately if 'f' returns false.
static bool h_foreach(HTab *htab, bool (*f)(HNode *, void *), void *arg) {

  // Visit every bucket.
  for (size_t i = 0; htab->mask != 0 && i <= htab->mask; i++) {

    // Traverse the linked list in this bucket.
    for (HNode *node = htab->tab[i]; node != nullptr; node = node->next) {
      
      // Apply callback to the current node.
      // Stop traversal if callback returns false.
      if (!f(node, arg))
          return false;
    }
  }

  return true;
}

// Traverse the entire hash map.
//
// During rehashing, nodes may exist in both
// newer and older tables, so visit both.
void hm_foreach(
    HMap *hmap,
    bool (*f)(HNode *, void *),
    void *arg)
{
    h_foreach(&hmap->newer, f, arg) &&
    h_foreach(&hmap->older, f, arg);
}
