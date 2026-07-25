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

// Maximum size of a single message (excluding the 4-byte header)
constexpr size_t MAX_MSG_SIZE = 4096;

// Print a simple message to stderr
void msg(const string& msg) {
  cerr << msg << '\n';
}

// Print an error message and terminate the program
static void die(const string& msg) {
  cerr << "[" << errno << "]" << msg << ": " << strerror(errno) << endl;
  exit(EXIT_FAILURE);
}

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
    return 0;
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
  return 0;
}

/*
 * Send one request to the server and receive one reply.
 *
 * Packet format:
 * +------------+----------------+
 * | 4-byte len | message bytes  |
 * +------------+----------------+
 *
 * First 4 bytes - bytes to read for the message.
 */
static int32_t query(int fd, const string& text){
  uint32_t length = text.size();
  if (length > MAX_MSG_SIZE) {
    return -1;
  }

  /*
   * Build the packet to send.
   *
   * Layout:
   * +-------------------------
   * | length |  message... |
   * +-------------------------
   */

  char writeBuffer[4 + MAX_MSG_SIZE];

  // msg length in first 4 bytes -- assume little endian
  memcpy(writeBuffer, &length, 4);

  // copy msg after length
  memcpy(writeBuffer + 4, text.data(), length);

  // send entire packet
  if (writeAll(fd, writeBuffer, 4 + length)){
   return -1;
  }

  // Read reply header (4 bytes)
  char readBuffer[4 + MAX_MSG_SIZE]; // maximum possible packet length
  errno = 0;

  if (readFull(fd, readBuffer, 4)) {

    if (errno == 0) msg("EOF");
    else msg("read() error");

    return -1;
  }

  // Extract reply length
  memcpy(&length, readBuffer, 4);

  if (length > MAX_MSG_SIZE) {
    msg("Reply too large.");
    return -1;
  }

  // Read reply body
  if (readFull(fd, readBuffer + 4, length)) {
    msg("read() error");
    return -1;
  }

  // Print server response
  cout << "Server says: " << string(readBuffer + 4, length) << endl;

  return 0;
}

int main(){
  // Create TCP socket
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0){
    die("socket");
  }

  // Server address
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  // Host -> Network byte order
  addr.sin_port = htons(1234);
  // 127.0.0.1
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  // Connect to server
  if(
    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
  ) {
    die("connect");
  }

  // Send multiple requests over the same TCP connection
  if (query(fd, "hello1"))
    goto DONE;

  if (query(fd, "hello2"))
    goto DONE;

  if (query(fd, "hello3"))
    goto DONE;

  DONE:

    close(fd);

    return 0;
}