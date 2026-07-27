#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <vector>
#include<map>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "hashtable.h"

using namespace std;

constexpr size_t MAX_MSG_SIZE = 32 << 20;
constexpr size_t READ_BUFFER_SIZE = 64 * 1024; // temp read buffer size
constexpr size_t MAX_ARGUMENTS = 200000; // max command args

// placeholder; implemented later
static struct {
    HMap db;
} g_data;

enum ResponseStatus {
  RESPONSE_OK = 0,
  RESPONSE_ERROR = 1,
  RESPONSE_NOT_FOUND = 2
};

struct Conn {
  // socket file desc for this client
  int fd = -1;
  /*
   * The event loop uses these flags to decide whether to ask poll()
   * to watch for reading or writing.
   */

  bool wantRead = false;
  bool wantWrite = false;
  bool wantClose = false;

  /*
   * Bytes received from the client.
   * The protocol parser reads requests from this buffer.
   */
  vector<uint8_t> incoming;

  /*
   * Bytes waiting to be sent to client
   */
  vector<uint8_t> outgoing;
};

// +--------+---------+
// | status | data... |
// +--------+---------+
struct Response {
    uint32_t status = RESPONSE_OK;
    vector<uint8_t> data;
};

// One key-value pair stored in the hash table.
//
// HNode is embedded so the hash table can link entries
// directly without allocating a separate node.
struct Entry {
    HNode node;          // Hash table metadata
    string key;     // Key
    string val;     // Value
};

#include <cstddef>

#define container_of(ptr, type, member) \
    reinterpret_cast<type*>( \
        reinterpret_cast<char*>(ptr) - offsetof(type, member))

// Print a simple message.
static void msg(const std::string& message) {
  std::cerr << message << '\n';
}

// Print a message along with the current errno value.
static void msgErrno(const std::string& message) {
  std::cerr << "[errno: " << errno << "] " << message << '\n';
}

// Print an error and terminate the program.
static void die(const std::string& message) {
    std::cerr << "[" << errno << "] "
              << message << ": "
              << std::strerror(errno) << '\n';

    std::exit(EXIT_FAILURE);
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
  vector<uint8_t>& buffer,
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
  vector<uint8_t>& buffer,
  size_t length
) {
  buffer.erase(buffer.begin(), buffer.begin() + length);
}

// Accept a new client connection
/*
 * Called when poll() tells us that the listening socket is readable.
 * A listening socket becomes readable when a new client is waiting to connect.
 */
static Conn* handleAccept(int listenFd) {
  sockaddr_in clientAddr{};
  socklen_t clientLength = sizeof(clientAddr);

  int clientFd =
      accept(listenFd,
              reinterpret_cast<sockaddr*>(&clientAddr),
              &clientLength);

  if (clientFd < 0) {
      msgErrno("accept()");
      return nullptr;
  }

  // print client address
  uint32_t ip = clientAddr.sin_addr.s_addr;

  std::cout
    << "New client from "
    << (ip & 255) << '.'
    << ((ip >> 8) & 255) << '.'
    << ((ip >> 16) & 255) << '.'
    << (ip >> 24)
    << ':'
    << ntohs(clientAddr.sin_port)
    << '\n';

  // make client socket nonblocking
  setNonBlocking(clientFd);
  
  // create connection object
  Conn* connection = new Conn;
  connection->fd = clientFd;

  /*
   * Initially expect the client to send us a request.
   */
  connection->wantRead = true;

  return connection;
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

// Equality function used by the hash table.
//
// The hash table only sees HNode*, so convert them back
// to Entry* and compare the actual keys.
static bool entry_eq(HNode *lhs, HNode *rhs) {
    Entry *le = container_of(lhs, Entry, node);
    Entry *re = container_of(rhs, Entry, node);

    return le->key == re->key;
}

// FNV-1a style hash function.
// Converts a string into a 64-bit hash code used to
// choose the bucket.
static uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t h = 0x811C9DC5;

    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }

    return h;
}

// GET command
// Search for the key and return its value.
static void do_get(vector<string> &cmd, Response &out) {
  // temporary entry only for lookup
  Entry key;

  // copy query key - O(1)
  key.key.swap(cmd[1]);

  // compute hash of the key
  key.node.hcode = str_hash(
    (uint8_t*)key.key.data(),
    key.key.size()
  );

  // search hash table
  HNode *node = hm_lookup(
    &g_data.db,
    &key.node,
    &entry_eq
  );

  if (!node) {
    out.status = RESPONSE_NOT_FOUND;
    return;
  }

  // convert HNode* back to Entry* for the value
  const string &val = container_of(node, Entry, node)->val;

  assert(val.size() <= MAX_MSG_SIZE);

  out.data.assign(val.begin(), val.end());

}

// SET command
// Update existing key or insert a new one.
static void do_set(vector<string> &cmd, Response &) {   
    // temporary entry only for lookup
    Entry key;

    // O(1) key swap
    key.key.swap(cmd[1]);

    // compute hash
    key.node.hcode = str_hash(
      (uint8_t*)key.key.data(),
      key.key.size()
    );
    
    // find corresponding
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);

    if (node) {

      // Existing key → replace value.
      container_of(node, Entry, node)
          ->val.swap(cmd[2]);

    } else {

      // New key → allocate a new Entry.
      Entry *ent = new Entry();

      ent->key.swap(key.key);
      ent->node.hcode = key.node.hcode;
      ent->val.swap(cmd[2]);

      // insert into hmap
      hm_insert(&g_data.db, &ent->node);
  }
}

// DEL command
// Remove key from hash table and free memory.

static void do_del(vector<string> &cmd, Response &) {
    // temporary entry only for lookup
    Entry key;

    // O(1) key swap
    key.key.swap(cmd[1]);

    // compute hash
    key.node.hcode = str_hash(
      (uint8_t*)key.key.data(),
      key.key.size()
    );

    // delete node
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);

    // found
    if (node) { 

      // Recover Entry* from HNode*
      // and delete it.
      delete container_of(node, Entry, node);
    }
}

// Execute one client command.
// Supports: GET, SET and DEL.
static void do_request(std::vector<std::string> &cmd,
                       Response &out)
{
    // GET <key>
    if (cmd.size() == 2 && cmd[0] == "get") {
        do_get(cmd, out);
    }

    // SET <key> <value>
    else if (cmd.size() == 3 && cmd[0] == "set") {
        do_set(cmd, out);
    }

    // DEL <key>
    else if (cmd.size() == 2 && cmd[0] == "del") {
        do_del(cmd, out);
    }

    // Unknown command
    else {
        out.status = RESPONSE_ERROR;
    }
}
// build response packet
// +----------------------+
// | Packet Length        |
// +----------------------+
// | Status               |
// +----------------------+
// | Data                 |
// +----------------------+
static void makeResponse(const Response& response, vector<uint8_t>& output) {
  /*
   * Packet:
   *
   * Length
   * Status
   * Data
   */
  uint32_t bodyLength = 4 + response.data.size();

  // bodylength field
  appendBytes(
    output,
    reinterpret_cast<uint8_t*>(&bodyLength),
    4
  );

  // status code field
  appendBytes(
    output,
    reinterpret_cast<const uint8_t*>(&response.status),
    4
  );

  // data field
  appendBytes(
    output,
    response.data.data(),
    response.data.size()
  );
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

  // build response object
  Response response;

  // execute request
  do_request(command, response);

  // make response 
  makeResponse(response, connection->outgoing);
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

int main() {
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

  // Store every active client connection.
  //
  // Index = file descriptor
  // Value = Conn*
  vector<Conn*> fdToConnection; // record fd belongs to which client

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
    for (Conn* connection : fdToConnection) {
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
    int readyCount = poll(
      pollArgs.data(),
      pollArgs.size(),
      -1 // TIMEOUT: block until an event occurs
    );
    // system interrupt -> restart
    if (readyCount < 0 && errno == EINTR) continue;
    // poll failed
    if (readyCount < 0) die("poll()");

    // atleast 1 client is waiting for a connection
    // -> connect front of queue
    // listening socket
    if (pollArgs[0].revents) { // if POLLIN: -> client to connect
      Conn* connection = handleAccept(listenFd); // create connection object

      if (connection) {
        
        // ensure vector is large enough
        if (fdToConnection.size() <= static_cast<size_t>(connection->fd)) {
          // resize to fd value
          fdToConnection.resize(connection->fd + 1);
        }
        // map fd value -> connection object
        assert(!fdToConnection[connection->fd]);
        fdToConnection[connection->fd] = connection;
      }
    }

    // client sockets
    for (size_t i = 1; i < pollArgs.size(); i++) { // skip listening fd
      auto ready = pollArgs[i].revents; // ready: bitmask -> POLLERR | POLLIN or POLLERR | POLLOUT

      // not ready
      if (ready == 0) continue;

      Conn* connection = fdToConnection[pollArgs[i].fd];

      if (ready & POLLIN) handleRead(connection); // & to check if the POLLIN bit is set

      if (ready & POLLOUT) handleWrite(connection); // & to check if the POLLOUT bit is set

      // remove dead connection
      if (
        (ready & POLLERR) || (connection->wantClose)
      ) {
        close(connection->fd);
        fdToConnection[connection->fd] = nullptr;
        delete connection;
      }
    }
  }
  return 0;
}
