#include <assert.h>
#include <stdlib.h>     // calloc(), free()
#include "hashtable.h"

// Initialize a hash table.
// n must be a power of 2.
static void h_init(HTab* htab, size_t n) {
  assert(n > 0 && ((n - 1) & n) == 0);

  // allocate buckets
  htab->tab = (HNode **)calloc(n, sizeof(HNode *));
  htab->mask = n-1; // used for hash & mask
  htab->size = 0;
}

// Insert node at the head of the
// appropriate bucket.
static void h_insert(HTab *htab, HNode *node) {
  size_t pos = node->hcode & htab->mask; // bucket index

  node->next = htab->tab[pos];
  htab->tab[pos] = node;

  htab->size++;
}

// Find a node.
// Returns a pointer to the pointer
// that owns the node.
//
// Useful for deletion.
static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode*, HNode*)) {
  if (!htab->tab) return NULL;

  size_t pos = key->hcode & htab->mask;
  HNode **from = &htab->tab[pos];

  while (*from) {

    HNode *curr = *from;

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
  size_t nwork = 0;

  while (nwork < k_rehashing_work && hmap->older.size > 0) {
    HNode **from = &hmap->older.tab[hmap->migrate_pos];

    if (!*from)  {
      hmap->migrate_pos++;
      continue;
    }

    h_insert(&hmap->newer, h_detach(&hmap->older, from));

    nwork++;
  }

  // old table completely migrated
  if (hmap->older.size == 0 && hmap->older.tab) {
    free(hmap->older.tab);
    hmap->older = HTab();
  }
}

// Start resizing.
//
// Older <- current table
// Newer <- empty larger table
static void hm_trigger_rehashing(HMap *hmap) {
  assert(hmap->older.tab == NULL);

  hmap->older = hmap->newer;

  h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);

  hmap->migrate_pos = 0;
}

// Lookup a node.
// Search newer first,
// then older.

HNode* hm_lookup(HMap *hmap, HNode* key, bool (*eq)(HNode*, HNode*)) {
  hm_help_rehashing(hmap);

  HNode** from = h_lookup(&hmap->newer, key, eq);
  if (!from) {
    from = h_lookup(&hmap->older, key, eq);
  }

  return from ? *from : NULL;
}

// Maximum allowed load factor
const size_t k_max_load_factor = 8;

// Insert into the newer table.
// Trigger resize if needed.
void hm_insert(HMap *hmap, HNode *node) {
  if (!hmap->newer.tab) h_init(&hmap->newer, 4);

  h_insert(&hmap->newer, node);

  if(!hmap->older.tab) {
    size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;

    if (hmap->newer.size >= threshold) hm_trigger_rehashing(hmap);
  }

  hm_help_rehashing(hmap);
}

// delete a node
HNode* hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode*, HNode*)) {
  hm_help_rehashing(hmap);

  if (HNode **from = h_lookup(&hmap->newer, key, eq)){
    return h_detach(&hmap->newer, from);
  }

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
