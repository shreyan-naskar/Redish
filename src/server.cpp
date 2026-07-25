#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

constexpr size_t MAX_MSG_SIZE = 32 << 20;
constexpr size_t READ_BUFFER_SIZE = 64 * 1024;
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
    connection->wantClose = true;

    return false;
  }

  // check for whole message
  if (connection->incoming.size() < 4 + length) {
    return false;
  }

  // pointer to req body
  const uint8_t* request = connection->incoming.data() + 4;

  // print client msg
  std::cout
    << "Client says: "
    << string(
      reinterpret_cast<const char*>(request),
      std::min(length, static_cast<uint32_t>(100)))
    << '\n';

  // Build reply: echo the req
  appendBytes(
    connection->outgoing,
    reinterpret_cast<uint8_t*>(&length),
    4
  );

  appendBytes(
    connection->outgoing,
    request,
    length
  );

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
    READ_BUFFER_SIZE
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
  }
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
  vector<Conn*> fdToConnection;

  // poll needs an array describing eery fd we want to monitor
  vector<pollfd> pollArgs;

  // EVENT LOOP
  while (true) {
    // build poll() arguement list
    pollArgs.clear();

    /*
     * First entry is always the listening socket.
     *
     * We only care if it becomes readable,
     * meaning a new client wants to connect.
     */
    pollArgs.push_back({
      listenFd, POLLIN, 0
    });

    // add every connected client
    for (Conn* connection : fdToConnection) {
      if (!connection) continue;

      pollfd pfd{};

      pfd.fd = connection->fd;

      // watch for socket errors
      pfd.events = POLLERR;

      // watch for what application wants
      if (connection->wantRead) pfd.events |= POLLIN;

      if (connection->wantWrite) pfd.events |= POLLOUT;

      pollArgs.push_back(pfd);
    }

    // sleep until something happens
    int readyCount = poll(
      pollArgs.data(),
      pollArgs.size(),
      -1
    );

    if (readyCount < 0 && errno == EINTR) continue;

    if (readyCount < 0) die("poll()");

    // listening socket
    if (pollArgs[0].revents) {
      Conn* connection = handleAccept(listenFd);

      if (connection) {
        
        // ensure vector is large enough
        if (fdToConnection.size() <= static_cast<size_t>(connection->fd)) {

          fdToConnection.resize(connection->fd + 1);
        }

        fdToConnection[connection->fd] = connection;
      }
    }

    // client sockets
    for (size_t i = 1; i < pollArgs.size(); i++) {
      auto ready = pollArgs[i].revents;

      if (ready == 0) continue;

      Conn* connection = fdToConnection[pollArgs[i].fd];

      // std::cout << "fd = "
      //     << pollArgs[i].fd
      //     << '\n';

      // std::cout << "vector size = "
      //     << fdToConnection.size()
      //     << '\n';

      // std::cout << "pointer = "
      //     << fdToConnection[pollArgs[i].fd]
      //     << '\n';

      if (ready & POLLIN) handleRead(connection);

      if (ready & POLLOUT) handleWrite(connection);

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
}
