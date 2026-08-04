#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstddef>
#include<cmath>
#include <iostream>
#include <vector>
#include<ctime>
#include<map>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "datastructures/hashtable.h"
#include "types/zset.h"
#include "util/common.h"
#include "datastructures/list.h"
#include "datastructures/heap.h"
#include "concurrency/thread_pool.h"

using namespace std;

// -----------------------------------------
// CONSTANTS
// -----------------------------------------

constexpr size_t MAX_MSG_SIZE = 32 << 20;
constexpr size_t READ_BUFFER_SIZE = 64 * 1024; // temp read buffer size
constexpr size_t MAX_ARGUMENTS = 200000; // max command args
constexpr uint64_t IDLE_TIMEOUT_MS = 5 * 1000; // Disconnect a client after five seconds without activity.
// Sentinel meaning: this Entry does not currently have a heap item.
constexpr size_t NO_HEAP_INDEX = static_cast<size_t>(-1);

using Buffer = vector<uint8_t>;

// Shared immutable empty ZSet.
//
// Read-only commands can treat a missing database key as this empty set,
// avoiding special cases for ZSCORE and ZQUERY.
static const ZSet K_EMPTY_ZSET{};

// -----------------------------------------
// ENUMERATORS
// -----------------------------------------

enum ResponseStatus {
  RESPONSE_OK = 0,
  RESPONSE_ERROR = 1,
  RESPONSE_NOT_FOUND = 2
};

// error code for TAG_ERR
enum ErrorCodes {
  ERR_UNKNOWN = 1,    // unknown command
  ERR_TOO_BIG = 2,    // response too big
  ERR_BAD_TYP = 3,    // unexpected value type
  ERR_BAD_ARG = 4,    // bad arguement
};

// data types of serialized data
enum ResponseDataTypes{
  TAG_NIL = 0,    // nil
  TAG_ERR = 1,    // error code + msg
  TAG_STR = 2,    // string
  TAG_INT = 3,    // int64
  TAG_DBL = 4,    // double
  TAG_ARR = 5,    // array
};

// value types

enum ValueTypes {
  T_INIT = 0,
  T_STR = 1,
  T_ZSET = 2,
};

// -----------------------------------------
// STRUCTS
// -----------------------------------------

// State for one connected client.
//
// The event loop keeps one Conn object for every active socket.
struct Conn {
    int fd = -1;  // Client socket file descriptor.

    // Tells the event loop what this connection currently needs.
    bool wantRead = false;   // Wait for incoming request data.
    bool wantWrite = false;  // Wait until pending response data can be sent.
    bool wantClose = false;  // Close and delete this connection.

    // Network buffers.
    Buffer incoming;  // Received bytes waiting to be parsed as requests.
    Buffer outgoing;  // Serialized response bytes waiting to be sent.

    // Idle-timeout tracking.
    //
    // Milliseconds from CLOCK_MONOTONIC when this connection was last active.
    uint64_t last_active_ms = 0;

    // Intrusive node that places this connection in the idle-timeout list.
    // It is initialized with dlist_init(&idle_node).
    DList idle_node;
};


// Global server state.
//
// This single object owns the database, active connections, and timer data.
static struct {
  // Top-level database:
  // key → Entry (string, sorted set, etc.).
  HMap db;

  // Active client connections, indexed by file descriptor.
  //
  // fd2conn[fd] is either a Conn* or nullptr when that descriptor
  // does not belong to an active client.
  vector<Conn*> fdToConn;

  // Circular intrusive list of idle connections.
  //
  // Each Conn contributes its `idle_node` to this list, allowing the
  // server to find and close idle clients efficiently.
  DList idle_list;

  // Binary min-heap of TTL timer items.
  //
  // The item with the earliest expiration is at heap[0].
  vector<HeapItem> heap;

  // Thread pool 
  ThreadPool thread_pool;

} g_data;

// +--------+---------+
// | status | data... |
// +--------+---------+
struct Response {
    uint32_t status = RESPONSE_OK;
    vector<uint8_t> data;
};

// One top-level database entry:
//
//   Redis key → value
//
// `node` lets this Entry live inside the global hash map.
// The actual value is either `str` or `zset`, selected by `type`.
struct Entry {
  HNode node;          // Intrusive node used by the top-level hash map.
  string key;     // Database key.

  // Value type, such as T_STR or T_ZSET.
  uint32_t type = 0;

  // Only one of these is logically active, based on `type`.
  string str;     // Used when type == T_STR.
  ZSet zset;      // Used when type == T_ZSET.

  // for TTL
  size_t heap_index = 1; // array index to heap item
};

// Temporary hash-map key used only for lookup.
//
// It contains an HNode because hm_lookup() works with HNode pointers,
// plus the actual database key used by entry_eq().
struct LookupKey {
  HNode node;
  string key;
};

// -----------------------------------------
// FUNCTION DECLARATIONS
// -----------------------------------------


// Print a simple message.
static void msg(const string& message) {
  cerr << message << '\n';
}

// Print a message along with the current errno value.
static void msgErrno(const string& message) {
  cerr << "[errno: " << errno << "] " << message << '\n';
}

// Print an error and terminate the program.
static void die(const string& message) {
  cerr << "[" << errno << "] "
            << message << ": "
            << strerror(errno) << '\n';

  exit(EXIT_FAILURE);
}

// Returns milliseconds from a monotonic clock.
//
// Unlike wall-clock time, CLOCK_MONOTONIC does not jump when the system
// clock is adjusted, so it is suitable for measuring elapsed time.
static uint64_t get_monotonic_msec() {
  timespec time{};

  // red seconds and nanoseconds from the monotonic clock
  clock_gettime(CLOCK_MONOTONIC, &time);

  // Convert both parts to milliseconds:
  // seconds × 1000 + nanoseconds ÷ 1,000,000.
  return static_cast<uint64_t>(time.tv_sec) * 1000
    + static_cast<uint64_t>(time.tv_nsec) / 1'000'000;    
}

// Make a nonblocking socket

/*
 * By default, sockets are BLOCKING.
 *
 * read() -> (wait...) -> (wait...) -> returns only when data arrives.
 *
 * Not for an event-driven server we don't want that.
 * Need read() to return immediately if no data is available.
 */

static void setNonBlocking(int fd) {
  // get current file descriptor flags
  int flags = fcntl(fd, F_GETFL, 0); // returns a bitmask like 00000010
  if (flags == -1) { // if error sets errno and returns -1
    die("fcntl(FGETFL)");
  }

  // enable O_NONBLOCK while preserving the existing flags
  flags |= O_NONBLOCK; // sets the flag like - flags
  // 00000100 | 00100000 -> 00100100 : prevoius flags remain, adds O_NONBLOCK

  // apply the new flags
  if (fcntl(fd, F_SETFL, flags) == -1) {
    die("fcntl(F_SETFL)");
  }
}

// BUFFER HELPERS

// Append bytes to end of buffer
static void appendBytes(
  Buffer& buffer,
  const uint8_t* data,
  size_t length
) {
  buffer.insert(buffer.end(), data, data + length);
}

// Remove bytes from front of buffer
/* Before:
 *
 * +-------------------------+
 * | req1 | req2 | req3 |
 * +-------------------------+
 *
 * consume(req1)
 *
 * After:
 *
 * +----------------+
 * | req2 | req3 |
 * +----------------+
 */
static void consumeBytes(
  Buffer& buffer,
  size_t length
) {
  buffer.erase(buffer.begin(), buffer.begin() + length);
}

// help functions for the serialization

// append TAGs
static void appendBytes_u8(Buffer &buf, uint8_t data) {
  buf.push_back(data);
}
// append integer: message length or array length
static void appendBytes_u32(Buffer &buf, uint32_t data) {
  appendBytes(buf, (const uint8_t *)&data, 4);
}
// append integer: data
static void appendBytes_i64(Buffer &buf, int64_t data) {
  appendBytes(buf, (const uint8_t *)&data, 8);
}
// append double: data
static void appendBytes_dbl(Buffer &buf, double data) {
  appendBytes(buf, (const uint8_t *)&data, 8);
}

// append serialized data types to the back

// no response body
static void out_nil(Buffer &out) {
    appendBytes_u8(out, TAG_NIL);
}
// string response
static void out_str(Buffer &out, const char *s, size_t size) {
    appendBytes_u8(out, TAG_STR); // data type field
    appendBytes_u32(out, (uint32_t)size); // length of string field
    appendBytes(out, (const uint8_t *)s, size); // response string
}
// integer response
static void out_int(Buffer &out, int64_t val) {
    appendBytes_u8(out, TAG_INT); // data type field
    appendBytes_i64(out, val); // response integer
}
// double response
static void out_dbl(Buffer &out, double val) {
    appendBytes_u8(out, TAG_DBL); // data type field
    appendBytes_dbl(out, val); // response double
}
// error reponse
static void out_err(Buffer &out, uint32_t code, const string &msg) {
    appendBytes_u8(out, TAG_ERR); // data type field
    appendBytes_u32(out, code); // error code field
    appendBytes_u32(out, (uint32_t)msg.size()); // msg size field
    appendBytes(out, (const uint8_t *)msg.data(), msg.size()); // error message, eg. "Not Found"
}
// array response
static void out_arr(Buffer &out, uint32_t n) {
    appendBytes_u8(out, TAG_ARR); // data type field
    appendBytes_u32(out, n); // response array
}

// Starts a serialized array response.
//
// Array format:
//   [TAG_ARR: 1 byte][element count: 4 bytes][elements...]
//
// The number of elements is not known yet, so reserve four zero bytes
// and return their position. Pass that position to out_end_arr() later.
static size_t out_begin_arr(Buffer &out) {
  out.push_back(TAG_ARR); // array bytes field

  // reserve space for element count
  appendBytes_u32(out, 0);

  // return index where the count begins
  return out.size() - sizeof(uint32_t);
}

// Completes an array started by out_begin_arr().
//
// `ctx` is the position returned by out_begin_arr().
// `n` is the number of serialized elements appended to the array.
static void out_end_arr(Buffer &out, size_t ctx, uint32_t n) {
  // byte before the count must be array tag
  assert(out[ctx -1] == TAG_ARR);

  // backfill the reserved 4 byte element count fill
  memcpy(out.data() + ctx, &n, sizeof(n));
}

// Creates a database entry with the requested value type.
//
// The caller later fills in its key and the matching value field
// (`str` for T_STR or `zset` for T_ZSET).
static Entry* entry_new(uint32_t type) {
  Entry* entry = new Entry();
  entry->type = type;
  return entry;
}

// FORWARD DECLARATION
// Sets or removes an Entry's TTL.
//
// A negative TTL means “no expiration”; it removes the Entry from
// the global TTL min-heap when the entry is currently scheduled.
static void entry_set_ttl(Entry* entry, int64_t ttl_ms);

// delete an entry immediately
static void entry_del_sync(Entry* entry) {
  // A sorted set owns manually allocated ZNode objects, so clear them
  // before deleting the Entry itself.
  if (entry->type == T_ZSET) {
    zset_clear(&entry->zset);
  }

  // free the entry object
  delete entry;
}

// Wrapper function for the thread pool.
//
// The thread pool expects a function with
// signature: void (*)(void*)
static void entry_del_func(void* arg) {
  entry_del_sync(static_cast<Entry*>(arg));
}

// Destroys a top-level database entry.
//
// Small entries are deleted immediately.
//
// Large sorted sets are deleted in a worker
// thread so the main server thread does not
// get blocked.
static void entry_del(Entry* entry) {
  // remove the TTL entry from the heap
  entry_set_ttl(entry, -1);

  // determine the size of the container
  size_t set_size = (entry->type == T_ZSET) ? hm_size(&entry->zset.hmap) : 0;

  constexpr size_t K_LARGE_CONTAINER_SIZE = 1000;

  // large container delete in worker thread
  if (set_size > K_LARGE_CONTAINER_SIZE) {
    thread_pool_queue(
      &g_data.thread_pool,
      &entry_del_func,
      entry
    );
  }
  // small container delete immediately
  else {
    entry_del_sync(entry);
  }
}
// Compares a stored Entry against a temporary LookupKey.
//
// `node` belongs to a real Entry already stored in the hash map.
// `key` belongs to the temporary LookupKey used for this search.
static bool entry_eq(HNode* node, HNode* key) {
  Entry* entry = container_of(node, Entry, node);
  LookupKey* lookup_key = container_of(key, LookupKey, node);

  // Hash codes are checked by hm_lookup(); this checks the actual key
  // bytes to handle possible hash collisions.
  return entry->key == lookup_key->key;
}




// Accepts one new client connection and adds it to the server state.
//
// Returns 0 on success and -1 when accept() fails.
static int32_t handleAccept(int listenFd) {
  // accept() fills this sctructure with the connecting client's address
  sockaddr_in client_addr{};
  socklen_t address_length = sizeof(client_addr);

  int clientFd = accept(
    listenFd,
    reinterpret_cast<sockaddr*>(&client_addr),
    &address_length
  );

  if (clientFd < 0) {
    msgErrno("accept() error");
    return -1;
  }

  // print client address
  uint32_t ip = client_addr.sin_addr.s_addr;

  cout
    << "New client from "
    << (ip & 255) << '.'
    << ((ip >> 8) & 255) << '.'
    << ((ip >> 16) & 255) << '.'
    << (ip >> 24)
    << ':'
    << ntohs(client_addr.sin_port)
    << '\n';

  // make client socket nonblocking
  setNonBlocking(clientFd);
  
  // create connection object
  Conn* conn = new Conn();
  conn->fd = clientFd;
  conn->wantRead = true;

  // Record its initial activity time and add it to the idle timer list.
  conn->last_active_ms = get_monotonic_msec();
  dlist_insert_before(&g_data.idle_list, &conn->idle_node);

  // Store the conn at the index matching its file descriptor.
  if (g_data.fdToConn.size() <= static_cast<size_t>(clientFd)) {
    g_data.fdToConn.resize(clientFd + 1);
  }

  assert(!g_data.fdToConn[clientFd]);
  g_data.fdToConn[clientFd] = conn;

  return 0;
}

// Closes and deletes one client connection.
//
// Removes every global reference to the connection before freeing it.
static void conn_destroy(Conn* conn) {
  // Release the socket.
  (void)close(conn->fd);

  // Remove the file-descriptor → Conn mapping.
  g_data.fdToConn[conn->fd] = nullptr;

  // Remove this connection from the idle timer list.
  dlist_detach(&conn->idle_node);

  // Release the connection object and its input/output buffers.
  delete conn;
}

/*
 * Read a 4-byte unsigned integer
 * from the packet.
 *
 * curr is automatically advanced.
 */
static bool readUint32_t(
  // reference to pointer
  const uint8_t* &current, // Bytes already read
  const uint8_t* end, // last position 
  uint32_t& value
) {
  // not enough bytes left
  if ((current + 4) > end) return false;

  memcpy(&value, current, 4);
  current += 4; // move 4B
  return true;
}

/*
 * Read one string of known length.
 */
static bool readString(
  const uint8_t* &current, // Bytes already read
  const uint8_t* end, // last position 
  size_t length, // output bytes to read
  string& output
) {
  if (current + length > end) return false;

  output.assign(
    reinterpret_cast<const char*>(current),
    length
  );

  current += length;
  return true;
}

// parse the req body
// +------+-----+------+-----+------+-----+-----+------+
// | nstr | len | str1 | len | str2 | ... | len | strn |
// +------+-----+------+-----+------+-----+-----+------+

static int32_t parseRequest(
  const uint8_t* data,
  size_t size,
  vector<string>& command
) {
  const uint8_t* end = data + size;

  // number of strings
  uint32_t numStrings = 0;
  if (!readUint32_t(data, end, numStrings)) return -1;

  if (numStrings > MAX_ARGUMENTS) return -1;

  // read every string
  while (command.size() < numStrings) {

    uint32_t length = 0; // string to read

    if (!readUint32_t(data, end, length)) return -1;

    command.emplace_back();

    if (!readString(data, end, length, command.back())) return -1;
  }

  // extra bytes?
  if (data != end) return -1;

  return 0;
}


// Handles: GET <key>
static void do_get(vector<string> &cmd, Buffer &out) {
  // build a temporary key for searching the top-level hash-map
  // swap() moves the req key into key.key
  LookupKey key;
  key.key.swap(cmd[1]);

  // compute the same hash used by stored Entry objects
  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(key.key.data()),
    key.key.size()
  );

  // find the top-level Entry for this key
  HNode* node = hm_lookup(&g_data.db, &key.node, entry_eq);

  // missing key returns a nil response
  if (!node) return out_nil(out);

  // convert HNode to Entry
  Entry* entry = container_of(node, Entry, node);

  // GET only on string not sorted sets
  if (entry->type != T_STR) return out_err(out, ERR_BAD_TYP, "not a string value");

  // serialize the stored string as a TAG_STR response
  return out_str(out, entry->str.data(), entry->str.size());
}

// Handles: SET <key> <value>
static void do_set(vector<string> &cmd, Buffer &out) {
  // build a temp lookup key for req key
  LookupKey key;
  key.key.swap(cmd[1]);

  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(key.key.data()),
    key.key.size()
  );

  // check whether key already exists
  HNode* node = hm_lookup(&g_data.db, &key.node, entry_eq);
  if (node) {
    // existing key: recover its Entry object
    Entry* entry = container_of(node, Entry, node);

    // set only for string not sorted set
    if (entry->type != T_STR) return out_err(out, ERR_BAD_TYP, "not a string value");

    // replace the string
    entry->str.swap(cmd[2]);
  }
  else {
    // new key: allocate a string Entry and insert into database
    Entry *entry = entry_new(T_STR);

    // move key and value out of parsed command
    entry->key.swap(key.key);
    entry->str.swap(cmd[2]);

    // store the already computed hash code in the hash code
    entry->node.hcode = key.node.hcode;

    // add the entry to the top level database hash map
    hm_insert(&g_data.db, &entry->node);
  }

  // SET succeeds with a nil response in this protocol
  return out_nil(out);
}

// Handles: DEL <key>
//
// Removes the key regardless of whether it holds a string or sorted set.
// Returns integer 1 when a key was removed, or 0 when it did not exist.
static void do_del(vector<string> &cmd, Buffer &out) {
    // temporary LookupKey only for lookup
    LookupKey key;

    // O(1) key swap
    key.key.swap(cmd[1]);

    // compute hash
    key.node.hcode = str_hash(
      reinterpret_cast<uint8_t*>(key.key.data()),
      key.key.size()
    );

    // Remove the matching HNode from the top-level hash map.
    // hm_delete() returns nullptr if the key was absent.
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);

    // found
    if (node) { 

      // Recover Entry* from HNode*
      // and delete it.
      entry_del(container_of(node, Entry, node));
    }
    // return 1 if node deleted
    return out_int(out, node ? 1 : 0);
}

// Parses the entire string as a floating-point value.
//
// Returns true only when:
// - strtod() consumed every character, and
// - the result is not NaN.
//
// Examples:
//   "1.5"  -> true,  out = 1.5
//   "abc"  -> false
//   "1.5x" -> false
//   "nan"  -> false
static bool str2dbl(const string& text, double& out) {
  char* end = nullptr;

  // strtod() parses from the C-string representation.
  out = strtod(text.c_str(), &end);

  // `end` points immediately after the parsed number.
  // It must equal the end of the original string.
  return end == text.c_str() + text.size() && !isnan(out);
}

// Parses the entire string as a base-10 signed 64-bit integer.
//
// Returns true only when strtoll() consumed every character.
//
// Examples:
//   "-42" -> true,  out = -42
//   "12x" -> false
//   "abc" -> false
static bool str2int(const string& text, int64_t& out) {
  char* end = nullptr;

  // Parse a base-10 integer.
  out = strtoll(text.c_str(), &end, 10);

  // Require complete consumption: no trailing characters are allowed.
  return end == text.c_str() + text.size();
}


// Callback called once for every entry in the hash map.
//
// `hm_foreach()` accepts a generic void* argument, so `arg` is cast back
// to the Buffer that receives the serialized response.
static bool cb_keys(HNode* node, void* arg) {
    // Recover the output buffer passed from do_keys().
    Buffer& out = *static_cast<Buffer*>(arg);

    // `node` is embedded inside an Entry. Get its containing Entry object,
    // then obtain that entry's key.
    const string& key = container_of(node, Entry, node)->key;

    // Append this key as one serialized string element:
    // [TAG_STR][key length][key bytes]
    out_str(out, key.data(), key.size());

    // Return true to tell hm_foreach() to continue visiting entries.
    return true;
}

// Implements the `keys` command.
static void do_keys(vector<string>&, Buffer& out) {
  // Start a serialized array:
  // [TAG_ARR][number of elements]
  //
  // The number of keys equals the current number of database entries.
  out_arr(out, static_cast<uint32_t>(hm_size(&g_data.db)));

  // Visit every database entry. For each one, cb_keys() appends its key
  // as the next element in the array.
  hm_foreach(&g_data.db, cb_keys, &out);
}

// Handles: ZADD <zset-key> <score> <name>
//
// Returns:
// - 1 when `name` was newly added.
// - 0 when `name` already existed and its score was updated.
static void do_zadd(vector<string>& cmd, Buffer& out) {
  // parse the score arguement first
  double score = 0.0;
  if (!str2dbl(cmd[2], score)) {
    return out_err(out, ERR_BAD_ARG, "expect float");
  }

  // build a temporary top level database key for the req ZSet
  LookupKey key;
  key.key.swap(cmd[1]);

  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(key.key.data()),
    key.key.size()
  );

  // look up the database key
  HNode* hash_node = hm_lookup(
    &g_data.db,
    &key.node,
    entry_eq
  );
  
  Entry* entry = nullptr;

  if (!hash_node) {
    // key doea not yet exist: create a new sorted set Entry
    entry = entry_new(T_ZSET);

    entry->key.swap(key.key);
    entry->node.hcode = key.node.hcode;

    hm_insert(&g_data.db, &entry->node);
  }
  else {
    // key exists: it must already contain a sorted set
    entry = container_of(hash_node, Entry, node);

    if (entry->type != T_ZSET) return out_err(out, ERR_BAD_TYP, "expect sorted set");
  }

  const string &name = cmd[3];

  bool added = zset_insert(
    &entry->zset, name.data(), name.size(), score
  );

  return out_int(out, static_cast<int64_t>(added));
}

// Looks up a top-level database key and returns its ZSet.
//
// Returns:
// - pointer to the stored ZSet, if the key holds a ZSet;
// - pointer to K_EMPTY_ZSET, if the key does not exist;
// - nullptr, if the key exists but holds another type, such as a string.
//
// The returned pointer to K_EMPTY_ZSET must never be modified.
static ZSet* expect_zset(string& text) {
  // build a temporary hashmap lookup key
  LookupKey key;
  key.key.swap(text);

  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(key.key.data()),
    key.key.size()
  );

  HNode *hash_node = hm_lookup(&g_data.db, &key.node, entry_eq);

  // Redis style behavior here: missing ZSet is treated as empty
  if (!hash_node) return const_cast<ZSet*>(&K_EMPTY_ZSET);

  // return the ZSet only when the existing value has correct type
  Entry *entry = container_of(hash_node, Entry, node);

  return (entry->type == T_ZSET) ? &entry->zset : nullptr;
}

// Handles: ZSCORE <zset-key> <name>
//
// Returns the member's score as a double, or nil when the member/key
// does not exist. Returns an error when the key exists but is not a ZSet.
static void do_zscore(vector<string>& cmd, Buffer& out) {
  // lookup the ZSet. A missing key behaves like an empty set
  ZSet* zset = expect_zset(cmd[1]);

  // nullptr means key exists but wrong value type
  if (!zset) return out_err(out, ERR_BAD_TYP, " expect sorted set");

  // lookup the member by its name through the ZSet hmap index
  string &name = cmd[2];

  ZNode* node = zset_lookup(
    zset, name.data(), name.size()
  );

  // return the score when found; otherwise return nil
  return node ? out_dbl(out, node->score) : out_nil(out);
}

// Handles: ZQUERY <zset-key> <score> <name> <offset> <limit>
//
// Finds the first member whose (score, name) tuple is >= the supplied tuple,
// applies `offset`, and returns following members in sorted order.
//
// The response array is flat:
//
//   [name1, score1, name2, score2, ...]
static void do_zquery(vector<string>& cmd, Buffer& out) {
  // parse the starting score 
  double score = 0.0;

  if (!str2dbl(cmd[2], score)) return out_err(out, ERR_BAD_ARG, "expect fp member");

  // `name` is the secondary part of the (score, name) search tuple.
  const string name = cmd[3];

  // parse rank offset and response-element limit
  int64_t offset = 0;
  int64_t limit = 0;

  if (
    !str2int(cmd[4], offset) || !str2int(cmd[5], limit)
  ) return out_err(out, ERR_BAD_ARG, "expect int");

  // lookup th zset or use the shared empty ZSet for a missing key
  ZSet* zset = expect_zset(cmd[1]);

  if (!zset) return out_err(out, ERR_BAD_TYP, "expect sorted set");

  // a non-positive limit always produces an empty array
  if (limit <= 0) return out_arr(out, 0);

  // find the lower bound: first node with (score, name) >= target
  ZNode* node = zset_seekge(
    zset,
    score,
    name.data(),
    name.size()
  );

  // move forward or backward by the req no of nodes
  node = znode_offset(node, offset);
  
  // begin an array whose final element count is not known yet
  size_t ctx = out_begin_arr(out);
  int64_t count = 0;

  // append each member as 2 array elements: name, score
  while (node && count < limit) {
    out_str(out, node->name, node->len);
    out_dbl(out, node->score);

    // advance to the next member in (score, name) order
    node = znode_offset(node, 1);

    // 2 serialized array elements were added
    count += 2;
  }

  // backfill the array's element count field
  out_end_arr(out, ctx, static_cast<uint32_t>(count));

}

// Handles: ZREM <zset-key> <name>
//
// Removes one member from a sorted set.
//
// Returns:
// - 1 when the member existed and was removed.
// - 0 when the member or sorted set did not exist.
static void do_zrem(vector<string>& cmd, Buffer& out) {
  // lookup a ZSet. A missing key is represented as an empty ZSet
  ZSet* zset = expect_zset(cmd[1]);

  // nullptr means the key exists but holds a different type
  if (!zset) return out_err(out, ERR_BAD_TYP, "expect sorted set");

  // find the number through the ZSet's name-based hash-map index
  const string &name = cmd[2];

  ZNode* node = zset_lookup(
    zset, name.data(), name.size()
  );

  if (node) {
    // Remove it from both indexes:
    // - the name → node hash map
    // - the (score, name) AVL tree
    //
    // zset_delete() also frees the ZNode memory.
    zset_delete(zset, node);
  }

  // report whether a member was actually removed
  return out_int(out, node ? 1 : 0);
}

// Removes the heap item at `pos`.
//
// Precondition: pos < heap.size().
//
// The caller should mark the removed item's external index (`*ref`) as
// invalid before or after this call, if the item will no longer be tracked.
static void heap_delete(vector<HeapItem> &heap, size_t pos) {
  // Replace the removed position with the last heap item.
  heap[pos] = heap.back();

  // Remove the duplicate last element.
  heap.pop_back();

  // If the replacement was not already the last item, restore heap order.
  if (pos < heap.size()) {
    heap_update(heap.data(), pos, heap.size());
  }
}

// Adds a heap item or replaces an existing one.
//
// `pos` is the existing item's index. When pos >= heap.size(), `item` is
// appended as a new heap item.
static void heap_upsert(
  std::vector<HeapItem>& heap,
  size_t pos,
  HeapItem item
) {
  if (pos < heap.size()) {
    // Replace an existing timer item.
    heap[pos] = item;
  }
  else {
    // Append a new timer item at the end.
    pos = heap.size();
    heap.push_back(item);
  }

  // The value may have increased or decreased, so choose upward or
  // downward repair as needed.
  heap_update(heap.data(), pos, heap.size());
}

// Adds, updates, or removes an Entry's TTL.
//
// A HeapItem stores a pointer to `entry->heap_idx`, so heap reordering
// automatically keeps the Entry's recorded heap position correct.
static void entry_set_ttl(Entry* entry, int64_t ttl_ms) {
  // a negative TTL removes an existing expiration timer
  if (ttl_ms < 0 && entry->heap_index != NO_HEAP_INDEX) {
    heap_delete(g_data.heap, entry->heap_index);

    // this entry is no longer represented in the TTL heap
    entry->heap_index = NO_HEAP_INDEX;
  }
  // a non negative TTL creates or updates the expiration timer
  else if (ttl_ms >= 0) {
    // store an abosolute expiration time, by monotonicc clock
    uint64_t expire_at = get_monotonic_msec() + static_cast<uint64_t>(ttl_ms);

    HeapItem item{expire_at, &entry->heap_index};

    // entry->heap_index either selects its current heap item or is the
    // sentinel value, which causes heap_upsert() to append a new item
    heap_upsert(g_data.heap, entry->heap_index, item);
  }
}

// Handles: PEXPIRE <key> <ttl-ms>
//
// Returns 1 when the key exists and its TTL was changed; otherwise 0.
static void do_expire(vector<string> &cmd, Buffer&out) {
  // parse the TTL in ms
  int64_t ttl_ms = 0;

  if (!str2int(cmd[2], ttl_ms)) return out_err(out, ERR_BAD_ARG, "expected int TTL");

  // build a temp lookup key for the top level db hash map
  LookupKey key;
  key.key.swap(cmd[1]);

  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(key.key.data()),
    key.key.size()
  );

  HNode* node = hm_lookup(&g_data.db, &key.node, entry_eq);
  if (node) {
    Entry* entry = container_of(node, Entry, node);

    // add, update or remove entry's TTL timer
    entry_set_ttl(entry, ttl_ms);
  }

  return out_int(out, node ? 1 : 0);
}

// Handles: PTTL <key>
//
// Returns:
// - remaining TTL in milliseconds, when the key has a TTL;
// - -1 when the key exists but has no TTL;
// - -2 when the key does not exist.
static void do_ttl(vector<string> &cmd, Buffer &out) {
  // build a temp lookup key
  LookupKey key;
  key.key.swap(cmd[1]);

  key.node.hcode = str_hash(
    reinterpret_cast<const uint8_t*>(key.key.data()),
    key.key.size()
  );

  HNode* node = hm_lookup(&g_data.db, &key.node, entry_eq);

  if (!node) return out_int(out, -2);

  Entry* entry = container_of(node, Entry, node);

  // this entry is not scheduled in the TTL heap
  if (entry->heap_index == NO_HEAP_INDEX) return out_int(out, -1);

  // read the absolute expiration deadline from the heap
  int64_t expire_at = g_data.heap[entry->heap_index].val;
  uint64_t now_ms = get_monotonic_msec();

  // do not return a negative remaining time. Expired key will be
  // deleted by timer processing; until then, return 0;
  return out_int(
    out, 
    expire_at > now_ms
    ? static_cast<int64_t>(expire_at - now_ms)
    : 0
  );
}

// Execute one client command.
// Supports: GET, SET and DEL.
static void do_request(vector<string> &cmd, Buffer &out)
{
    // GET <key>
    if (cmd.size() == 2 && cmd[0] == "get") {
      return do_get(cmd, out);
    }

    // SET <key> <value>
    else if (cmd.size() == 3 && cmd[0] == "set") {
      return do_set(cmd, out);
    }

    // DEL <key>
    else if (cmd.size() == 2 && cmd[0] == "del") {
      return do_del(cmd, out);
    }

    // PEXPIRE <key> <ttl-ms>
    else if (cmd.size() == 3 && cmd[0] == "pexpire") {
      return do_expire(cmd, out);
    }

    // PTTL <key>
    else if (cmd.size() == 2 && cmd[0] == "pttl") {
      return do_ttl(cmd, out);
    }

    // KEYS
    else if (cmd.size() == 1 && cmd[0] == "keys") {
      return do_keys(cmd, out);
    }

    // ZADD <zset-key> <score> <name>
    else if (cmd.size() == 4 && cmd[0] == "zadd") {
      return do_zadd(cmd, out);
    }

    // ZREM <zset-key> <name> 
    else if (cmd.size() == 3 && cmd[0] == "zrem") {
      return do_zrem(cmd, out);
    }

    // ZSCORE <zset-key> <name>
    else if (cmd.size() == 3 && cmd[0] == "zscore") {
      return do_zscore(cmd, out);
    }

    // ZQUERY <zset-key> <score> <name> <offset> <limit>
    else if (cmd.size() == 6 && cmd[0] == "zquery") {
      return do_zquery(cmd, out);
    }

    // Unknown command
    else {
      out_err(out, ERR_UNKNOWN, "unknown command");
    }
}

// Returns the size of this response's body, excluding its 4-byte length header.
//
// `header` is the index where response_begin() reserved the length field:
//
//   header
//     ↓
//   [ length: 4 bytes ][ response body ... ]
//
// So:
//   out.size() - header     = header + body total size
//   out.size() - header - 4 = body size only
static size_t response_size(Buffer &out, size_t header) {
  return out.size() - header - 4;
}

// Starts one framed response message in `out`.
//
// Wire format: [4-byte body length][serialized response body...]
//
// Unknown body length yet
// `do_request()` will append the serialized response afterward
// Reserve four bytes now and fill them in later in response_end().
void response_begin(Buffer &out, size_t &header) {
  // Save where this response starts. `out` may already contain earlier
  // responses when the client sends pipelined requests.
  header = out.size();

  // Reserve the first 4 bytes for the uint32_t message-body length.
  // They are temporarily zero and will be overwritten in response_end().
  appendBytes_u32(out, 0);
}

// Completes the response that began at `header`.
void response_end(Buffer &out, size_t header) {
  // `header` points to the 4-byte length field.
  // Everything after it is the serialized response body.
  size_t body_size = out.size() - header - 4;

  // Do not send a response larger than the protocol's maximum.
  if (body_size > MAX_MSG_SIZE) {
    // Discard the oversized body, but keep its reserved 4-byte header.
    out.resize(header + 4);

    // Write a normal serialized error body in its place.
    out_err(out, ERR_TOO_BIG, "response is too big.");

    // Recalculate because the body is now the error response.
    body_size = out.size() - header - 4;
  }

  // The protocol stores the body size in the 4 bytes at `header`.
  // This cast is safe because body_size is checked against k_max_msg.
  uint32_t length = static_cast<uint32_t>(body_size);

  // Backfill the previously reserved length field.
  memcpy(out.data() + header, &length, sizeof(length));
  
}

// Try to process ONE complete request
/*
 * The incoming buffer may contain:
 *
 * 1. Half a request
 * 2. Exactly one request
 * 3. Multiple requests (pipelining)
 *
 * tryOneRequest(...){...} processes ONE request only.
 *
 * Returns:
 *
 * true  -> one request was processed
 * false -> need more data (or close connection)
 */
static bool tryOneRequest(Conn* connection) {
  // check for a 4B header
  if (connection->incoming.size() < 4) {
    return false;
  }

  // read message length
  uint32_t length = 0;
  memcpy(&length, connection->incoming.data(), 4); // (dest, src, size)
  if ( length > MAX_MSG_SIZE) {
    msg("message too large");
    connection->wantClose = true; // want close

    return false;
  }

  // check for whole message
  if (connection->incoming.size() < 4 + length) {
    return false; // want more read
  }

  // pointer to req body
  const uint8_t* request = connection->incoming.data() + 4;

  // application logic starts
  vector<string> command;
  if (parseRequest(request, length, command)) {
    msg("Bad Request");
    connection->wantClose = true;

    return false;
  }

  // build response packet
  size_t header_pos = 0;
  response_begin(connection->outgoing, header_pos);

  // execute request
  do_request(command, connection->outgoing);

  response_end(connection->outgoing, header_pos);

  // application logic ends

  // remove process requests
  consumeBytes(
    connection->incoming,
    4 + length
  );

  return true;
}

// socket became writable
static void handleWrite(Conn* connection) {
  assert(!connection->outgoing.empty());

  /*
   * Try sending everything currently waiting.
   *
   * write() may send only part of the buffer.
   */

  ssize_t bytesWritten = write(
    connection->fd,
    connection->outgoing.data(),
    connection->outgoing.size()
  );

  // Socket isn't ready
  if (bytesWritten < 0 &&
    errno == EAGAIN) { // try agian later -> connection not closed

    return;
  }

  // real error
  if (bytesWritten < 0) {
    msgErrno("write()");
    connection->wantClose = true;
    return;
  }

  // remove sent bytes
  consumeBytes(
    connection->outgoing,
    static_cast<size_t>(bytesWritten) // bytesWritten is guaranteed to be positive
  );

  /*
   * If nothing is left to send,
   * go back to waiting for requests.
   */

  if (connection->outgoing.empty()) {

      connection->wantRead = true;
      connection->wantWrite = false;
  }

 /*
  * Otherwise...
  *
  * There is still data waiting.
  *
  * Keep waiting for POLLOUT.
  */
}

// socket became readable
static void handleRead(Conn* connection) {
  /*
   * Temporary buffer.
   * Copy the received bytes into connection->incoming afterwards.
   */
  // data flow: socket -> buffer ->incoming
  // incoming doesn't know how many bytes read() will return.
  // too small -> too many system calls
  // too large -> stack ovwerflow
  // MAX_MSG_SIZE: 64MB / READ_BUFFER_SIZE: 64kB -> 512 system calls, not bad
  uint8_t buffer[READ_BUFFER_SIZE]; //  64kB

  // read into temporary buffer
  ssize_t bytesRead = read(
    connection->fd,
    buffer,
    sizeof(buffer)
  );

  // Socket isn't ready
  if (bytesRead < 0 && errno == EAGAIN) {
    return; // try agian later -> connection not closed
  }

  // handle IO error
  if (bytesRead < 0) {
    msgErrno("read() error");
    connection->wantClose = true;
    return; // want close
  }

  // Client diconnected
  if (bytesRead == 0) {
    if (connection->incoming.empty()) msg("Client closed connection.");
    else msg("Unexpected EOF.");

    connection->wantClose = true;
    return;
  }

  // store received bytes
  appendBytes(
    connection->incoming,
    buffer,
    static_cast<size_t>(bytesRead)
  );

  /*
   * Keep parsing requests until no complete request remains.
   *
   * This is important for pipelining.
   */
  // consume all requests from the client one by one
  while (tryOneRequest(connection)) {}

  // processing may have produced replies
  // are replies waiting ?
  if(!connection->outgoing.empty()) {
    // replies waiting -> send responses
    connection->wantRead = false;
    connection->wantWrite = true;
    // socket maybe writable -> try 
    // optimization: just received data -> client alive
    //               -> TCP send buffer is probably empty
    handleWrite(connection);
  } // else wants read
}

// Returns how long poll() should wait before the oldest idle connection
// reaches its timeout.
//
// Returns:
// - -1: no idle connections, so there is no timer deadline.
// -  0: a connection has already reached its deadline.
// - >0: milliseconds until the next idle deadline.
static int32_t next_timer_ms() {

  uint64_t now_ms = get_monotonic_msec();
  uint64_t next_ms = UINT64_MAX; // earliest timer found so far

  /*
   * The idle list is sorted by
   * last activity time.
   *
   * Therefore the first connection
   * is always the one that expires first.
   */
  if (!dlist_empty(&g_data.idle_list)) {
    Conn* connection = container_of(g_data.idle_list.next, Conn, idle_node);
    next_ms = connection->last_active_ms + IDLE_TIMEOUT_MS;
  }

  // Check TTL timers.
  /*
   * The heap is a min-heap.
   *
   * Therefore heap[0] is always the
   * earliest expiring key.
   */
  if (!g_data.heap.empty() && g_data.heap[0].val < next_ms) {
    next_ms = g_data.heap[0].val;
  }

  // No timers exist.
  if (next_ms == UINT64_MAX) return static_cast<uint32_t>(-1);

  // Timer already expired.
  if (next_ms <= now_ms) return 0;

  // Return remaining time.
  return static_cast<uint32_t>(next_ms - now_ms);
}

// Compare two hash nodes.
static bool hnode_same(HNode* node, HNode* key) {
  return node == key;
}


// Removes every idle connection whose timeout has expired.
//
// Call this after poll() wakes up, especially when its timeout expires.
static void process_timers() {
  uint64_t now_ms = get_monotonic_msec();

  // The idle list is ordered from oldest to newest connection.
  // Stop at the first non-expired connection.
  while (!dlist_empty(&g_data.idle_list)) {
    Conn* conn = container_of(
      g_data.idle_list.next,
      Conn,
      idle_node
    );

    uint64_t  deadline_ms = conn->last_active_ms + IDLE_TIMEOUT_MS;
    
    if (deadline_ms >= now_ms) break; // The oldest remaining connection is not expired.

    cerr << "removing idle connection: "
          << conn->fd
          << '\n';

    // Removes it from fd2conn and idle_list, closes its socket,
    // then deletes the Conn object.
    conn_destroy(conn);
  }

  // remove expierd keys
  // TTL timers using a heap
  constexpr size_t K_MAX_WORKS = 2000;

  size_t nworks = 0;
  vector<HeapItem> &heap = g_data.heap;

  while (!heap.empty() && heap[0].val < now_ms) {
    // get entry containing this heap node
    Entry* entry = container_of(heap[0].ref, Entry, heap_index);
    // Remove it from the hash table.
    HNode* node = hm_delete(&g_data.db, &entry->node, &hnode_same);
    
    assert(node == &entry->node);

    // delete the entry itself
    entry_del(entry);

    // Prevent spending too much time
    // deleting expired keys in one go.
    if (nworks++ > K_MAX_WORKS) break;
  }
}

int main() {

  // initialization
  dlist_init(&g_data.idle_list);
  thread_pool_init(&g_data.thread_pool, 4);

  // Create listening socket

  int listenFd = socket(AF_INET, SOCK_STREAM, 0);

  if (listenFd < 0)
      die("socket()");

  // Allow immediate reuse of the port.
  int opt = 1;
  setsockopt(listenFd,
              SOL_SOCKET,
              SO_REUSEADDR,
              &opt,
              sizeof(opt));

  // Configure server address
  sockaddr_in serverAddr{};

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(1234);
  serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

  // Bind socket
  if (bind(
    listenFd,
    reinterpret_cast<sockaddr*>(&serverAddr),
    sizeof(serverAddr)) < 0
  ) {

    die("bind()");
  }

  // Make listening socket non-blocking
  setNonBlocking(listenFd);

  // Start listening
  if (listen(listenFd, SOMAXCONN) < 0) // max SOMAXXCONN number of clients in queue
      die("listen()");

  // poll needs an array describing every fd we want to monitor
  vector<pollfd> pollArgs;

  // EVENT LOOP
  // build poll list -> poll() sleeps -> Kernel wakes up -> handle ready sockets -> repeat
  while (true) {
    // build poll() arguement list
    // remove outdated sockets
    pollArgs.clear();

    /*
     * First entry is always the listening socket.
     *
     * We only care if it becomes readable,
     * meaning a new client wants to connect.
     */
    pollArgs.push_back({
      listenFd, // fd
      POLLIN, // events: listening socket -> only interested in POLLIN
      0 // revents: filled by kernel
    });

    // add every connected client
    for (Conn* connection : g_data.fdToConn) {
      // indexed by fd value -> not continuous -> many values in b/w are nullptr
      if (!connection) continue;

      // new fd to poll
      pollfd pfd{}; // init to 0

      pfd.fd = connection->fd;

      // watch for socket errors: conn reset/ broken pipe/ socket failure
      pfd.events = POLLERR;

      // watch for what fd wants: flip POLLIN bit
      if (connection->wantRead) pfd.events |= POLLIN; // error or readable
      // flip POLLOUT bit
      if (connection->wantWrite) pfd.events |= POLLOUT; // error or writeable

      pollArgs.push_back(pfd);

      // pollArgs looks like
      /*
       * +------------------------------------+
       * | fd=3 | POLLIN              | -> listening socket
       * +------------------------------------+
       * | fd=4 | POLLERR | POLLIN    | -> client socket
       * +------------------------------------+
       * | fd=7 | POLLERR | POLLOUT   |
       * +------------------------------------+
       * 
       * The key idea is that Conn stores the long-lived state of each client.
       * while pollArgs is just a snapshot -> 
       * -> kernel to watch during this one iteration of event loop
       */
    }

    // sleep until something happens
    // wait for readiness
    int32_t timeout_ms = next_timer_ms();
    int readyCount = poll(
      pollArgs.data(),
      pollArgs.size(),
      timeout_ms // TIMEOUT
    );
    // system interrupt -> restart
    if (readyCount < 0 && errno == EINTR) continue;
    // poll failed
    if (readyCount < 0) die("poll()");

    // atleast 1 client is waiting for a connection
    // -> connect front of queue
    // listening socket
    if (pollArgs[0].revents) { // if POLLIN: -> client to connect
      handleAccept(listenFd);
    }

    // client sockets
    for (size_t i = 1; i < pollArgs.size(); i++) { // skip listening fd
      auto ready = pollArgs[i].revents; // ready: bitmask -> POLLERR | POLLIN or POLLERR | POLLOUT

      // not ready
      if (ready == 0) continue;

      Conn* connection = g_data.fdToConn[pollArgs[i].fd];

      // update the idle timer by moving conn to the end of the list
      connection->last_active_ms = get_monotonic_msec();
      dlist_detach(&connection->idle_node);
      dlist_insert_before(&g_data.idle_list, &connection->idle_node);

      // HANDLE IO
      if (ready & POLLIN) handleRead(connection); // & to check if the POLLIN bit is set

      if (ready & POLLOUT) handleWrite(connection); // & to check if the POLLOUT bit is set

      // remove dead connection
      if (
        (ready & POLLERR) || (connection->wantClose)
      ) {
        conn_destroy(connection);
      }
    }

    // handle timers
    process_timers();
  }
  return 0;
}
