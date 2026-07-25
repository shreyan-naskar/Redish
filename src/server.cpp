#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <string>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

// Print a message to stderr
static void msg(const string& msg){
  cerr << msg << endl;
}

// Print an error message and terminate the program
static void die(const string& msg){
  cerr << "[" << errno << "]" << msg << strerror(errno) << endl;
  exit(EXIT_FAILURE);
}

// Maximum allowed message size (excluding the 4-byte header)
constexpr size_t MAX_MSG_SIZE = 4096;

/*
 * Read exactly n bytes from the socket.
 *
 * A single read() call is NOT guaranteed to return all requested bytes.
 *
 * Example:
 * We ask for 100 bytes. // read(fd, buf, 100)
 *
 * The kernel may return: 25 bytes + 40 bytes + 35 bytes
 *
 * because TCP is a byte stream, not a message protocol.
 *
 * Repeatedly call read() until all bytes have been received.
 */

static int32_t readFull(int fd, char* buffer, size_t bytesToRead) {
  // read all bytes  
  while (bytesToRead > 0) {
      ssize_t bytesRead = read(fd, buffer, bytesToRead);

      // bytesRead == 0  -> peer closed the connection
      // bytesRead < 0   -> read() failed
      // also ensured bytesRead is POSITIVE
      if (bytesRead <= 0)
        return -1;

      // bytesRead is ssize_t -> signed integer
      // bytesToRead is size_t -> unsigned integer
      // thus static_cast on bytesRead -> after confirming it is POSITIVE 
      //                               -> safe to treat as an unsigned integer
      assert(static_cast<size_t>(bytesRead) <= bytesToRead);

      bytesToRead -= bytesRead;
      // move pointer to start of remaining part
      buffer += bytesRead;
    }
  return 0; // successful
}

/*
 * Write exactly n bytes to the socket.
 *
 * Like read(), write() is also allowed to write only part of the data
 * Keep writing until everything is sent.
 */
static int32_t writeAll(int fd, char* buffer, size_t bytesToWrite) {
  // write all bytes
  while (bytesToWrite > 0) {

    ssize_t bytesWritten = write(fd, buffer, bytesToWrite);

    // bytesWritten == 0  -> peer closed the connection
    // bytesWritten < 0   -> read() failed
    // also ensured bytesWritten is POSITIVE
    if (bytesWritten <= 0) {
      return -1;
    }

    // bytesWritten is ssize_t -> signed integer
    // bytesToWrite is size_t -> unsigned integer
    // thus static_cast on bytesWritten -> after confirming it is POSITIVE 
    //                               -> safe to treat as an unsigned integer
    assert(static_cast<size_t>(bytesWritten) <= bytesToWrite);

    bytesToWrite -= bytesWritten;
    buffer += bytesWritten;
  }
  return 0; // successful
}

/*
 * Process one client request.
 *
 * Request format:
 * +------------+----------------+
 * | 4B len     | message bytes  |
 * +------------+----------------+
 *
 * Response uses exactly the same format.
 */
static int32_t handleRequest(int clientFd) {

  // Read 4-byte header
  char readBuffer[4 + MAX_MSG_SIZE];

  errno = 0;

  if (readFull(clientFd, readBuffer, 4)) {
    if (errno == 0) msg("EOF");
    else msg("read() error");

    return -1;
  }

  // extract message length
  uint32_t length = 0;
  memcpy(&length, readBuffer, 4);
  if (length > MAX_MSG_SIZE) {
    msg("Message too large");
    return -1;
  }

  // Read the message body
  if (readFull(clientFd, readBuffer + 4, length)) {
    msg("read() error");
    return -1;
  }

  // print client message
  cout << "Client says: " << string(readBuffer + 4, length) << endl;

  // build response packet
  string reply = "world";

  char writeBuffer[4 + MAX_MSG_SIZE];

  length = reply.size();

  // Header
  memcpy(writeBuffer, &length, 4);

  // Body
  memcpy(writeBuffer + 4, reply.data(), length);

  // send response
  return writeAll(clientFd, writeBuffer, 4 + length);
}

int main() {
  // Create TCP socket
  int serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd < 0)
    die("socket()");

  // Allow immediate reuse of the port
  int opt = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // Configure server address
  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(1234);
  serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

  // Bind socket to address
  if (bind(serverFd,
            reinterpret_cast<sockaddr*>(&serverAddr),
            sizeof(serverAddr)) < 0)
      die("bind()");
  
  // Start listening
  if (listen(serverFd, SOMAXCONN) < 0)
      die("listen()");

  while (true) {
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);

    int clientFd = accept(serverFd,
                          reinterpret_cast<sockaddr*>(&clientAddr),
                          &clientLen);

    if (clientFd < 0)
      continue;

    /*
     * Keep serving requests from this client
     * until the client disconnects or an error occurs.
     *
     * One TCP connection can carry many requests.
     */
    while (true) {
      if (handleRequest(clientFd))
          break;
    }

        std::cout << "Client disconnected.\n";
  }
  close(serverFd);
}



