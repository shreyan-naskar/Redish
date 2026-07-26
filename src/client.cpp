#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

// Maximum size of a single message 4kB
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
static int32_t writeAll(int fd, const char* buffer, size_t bytesToWrite) {
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
 * First 4 bytes - number of bytes to read for the message.
 */
static int32_t sendRequest(int fd, const vector<string>& command) {
  /*
   * Packet format:
   *
   * +----------------------+
   * | Total Length (4B)    |
   * +----------------------+
   * | Num Strings (4B)     |
   * +----------------------+
   * | Len | String         |
   * +----------------------+
   * | Len | String         |
   * +----------------------+
   * ...
   */
  uint32_t bodyLength = 4; // init with size of total-length field

  for (const auto& word : command) {

    bodyLength += (4 + word.size()); // 4B for string len field + size of string    
  }

  if (bodyLength > MAX_MSG_SIZE) {
    return -1;
  }

  // message buffer
  char writeBuffer[4 + MAX_MSG_SIZE];

  // add total length field
  memcpy(writeBuffer, &bodyLength, 4);

  // add nstr field
  uint32_t nstr = command.size();
  memcpy(writeBuffer + 4, &nstr, 4);

  size_t current = 8; // 4B totalLen + 4B nstr
  // add the strings
  for (const auto& word : command) {

    // len field
    uint32_t wordLen = word.size();
    memcpy(
      writeBuffer + current, // start of len for this word
      &wordLen,
      4
    );

    // string field
    memcpy(
      writeBuffer + current + 4,
      word.data(),
      word.size()
    );

    current += 4 + word.size();
    
  }

  // send request
  return writeAll(fd, writeBuffer, 4 + bodyLength);
}

// READ SERVER RESPONSE
static int32_t readResponse(int fd) {
  char readBuffer[4 + MAX_MSG_SIZE];

  // read packet length
  errno = 0;
  if (readFull(fd, readBuffer, 4)) {
    if (errno == 0) msg("EOF");
    else msg("read() error");
    return -1;
  }

  // extract body length
  uint32_t bodyLength = 0;
  memcpy(&bodyLength, readBuffer, 4);
  if (bodyLength > MAX_MSG_SIZE) {
    msg("Message size too large");
    return -1;
  }

  // read response body
  if (readFull(fd, readBuffer + 4, bodyLength)) {
    msg("read() error");
    return -1;
  }

  // check for valid response
  if (bodyLength < 4) {
    msg("bad response");
    return -1;
  }

  // Response format:
  //
  // Result Code (4B) - Message

  uint32_t resultCode = 0;
  memcpy(&resultCode, readBuffer + 4, 4);

  std::cout
    << "Result Code : "
    << resultCode
    << '\n';

  std::cout
    << "Response    : "
    << std::string(
            readBuffer + 8, // skip 4B totalLength + 4B status code
            bodyLength - 4  // bodyLength - 4B statusCode
      )
    << '\n';

  return 0;
}

int main(int argc, char* argv[]){
  // Create TCP socket
  int clientFd = socket(AF_INET, SOCK_STREAM, 0);
  if (clientFd < 0){
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
    connect(clientFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
  ) {
    die("connect");
  }

  // Build command from command-line arguments
  std::vector<std::string> command;

  for (int i = 1; i < argc; i++)
      command.push_back(argv[i]);

  // Send request
  if (sendRequest(clientFd, command))
      goto DONE;

  // Read reply
  if (readResponse(clientFd))
      goto DONE;
  
  DONE:

  close(clientFd);

  return 0;
}