#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

// HERE ON BYTE BUFFERS WILL BE USED INSTEAD OF STRING BUFFERS

// Maximum size of a single message (32 MB)
constexpr size_t MAX_MSG_SIZE = 32 << 20;

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

static int32_t readFull(int fd, uint8_t* buffer, size_t bytesToRead) {
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
static int32_t writeAll(int fd, const uint8_t* buffer, size_t bytesToWrite) {
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
 * Append raw bytes to the end of a vector, works like a growable byte array.
 */
static void appendBytes(
  vector<uint8_t>& buffer, // writebuffer
  const uint8_t* data, // data to insert
  size_t length // length of data to insert
) {
  buffer.insert(buffer.end(), data, data + length);

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
static int32_t sendRequest(int fd, const uint8_t* message, size_t length) {
  
  if (length > MAX_MSG_SIZE) {
    return -1;
  }
  vector<uint8_t> writeBuffer;

  /*
   * Build the packet to send.
   *
   * Layout:
   * +-------------------------
   * | length |  message... |
   * +-------------------------
   */

  // 4B data length
  appendBytes(
    writeBuffer,
    reinterpret_cast<const uint8_t*>(&length), // size_t -> const uint8_t*
    4
  );

  // data body
  appendBytes(
    writeBuffer,
    message,
    length
  );

  return writeAll(
    fd,
    writeBuffer.data(),
    writeBuffer.size()
  );
}

static int32_t readResponse(int fd) {
  // Read 4B header for length
  // initialize for just 4B, can't know message length beforehand
  vector<uint8_t> readBuffer(4);

  errno = 0;
  
  // Read reply header (4 bytes)
  if (readFull(fd, readBuffer.data(), 4)) {
    if (errno == 0) msg("EOF");
    else msg("read() error");

    return -1;
  }

  // extract response length
  uint32_t length = 0;
  memcpy(&length, readBuffer.data(), 4);
  if (length > MAX_MSG_SIZE) {
    msg("Message length to large.");
  }

  // extract response body
  // need extra space to store the message
  // total space needed = 4 + length -> RESIZE
  readBuffer.resize(4 + length);

  if (readFull(fd, readBuffer.data() + 4, length)) {
    msg("read() error");
    return -1;
  }

  // print response
  cout << "Length: " << length << endl;
  cout << "Response: " << 
    std::string(
      // skip the length part, point to the message body
      reinterpret_cast<char*>(readBuffer.data() + 4),
      min(length, static_cast<uint32_t>(100)) // print the firt 100 chars just as demo
    ) << endl;

  return 0;
}

int main(){
  // Create TCP socket
  int socketFd = socket(AF_INET, SOCK_STREAM, 0);
  if (socketFd < 0){
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
    connect(socketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
  ) {
    die("connect");
  }

  vector<string> requests = {

    "hello1",
    "hello2",
    "hello3",

    // Large request
    string(MAX_MSG_SIZE, 'z'),

    "hello5"
  };

  /*
   * PIPELINING
   *
   * Send ALL requests first...
   */

  for (const auto& req : requests) {
    if (sendRequest(
      socketFd,
      reinterpret_cast<const uint8_t*>(req.data()),
      req.size()
    )) {
      goto DONE;
    }
  }

  /*
   * ...then read ALL responses.
   */
  for (size_t i = 0; i < requests.size(); i++) {

    if (readResponse(socketFd))
        goto DONE;
  }

  DONE:

  close(socketFd);
  return 0;
}