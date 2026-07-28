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

enum {
    TAG_NIL = 0,    // nil
    TAG_ERR = 1,    // error code + msg
    TAG_STR = 2,    // string
    TAG_INT = 3,    // int64
    TAG_DBL = 4,    // double
    TAG_ARR = 5,    // array
};

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

// Parse and print one serialized response.
//
// Returns:
//   > 0 : Number of bytes consumed.
//   -1  : Invalid response.
//
// Response format:
//
// +---------+---------------------------+
// | 1B Tag  | Payload (depends on tag)  |
// +---------+---------------------------+
//
// Tags:
//   NIL  -> no payload
//   ERR  -> error code + string
//   STR  -> string
//   INT  -> 64-bit integer
//   DBL  -> double
//   ARR  -> array of serialized objects
static int32_t printResponse(const uint8_t *data, size_t size) {
  // every res must contain atleast tag
  if (size < 1) {
    msg("bad response");
    return -1;
  }
  switch (data[0]) {
    // NIL
    //
    // Format:
    // +------+ 
    // | TAG  |
    // +------+
    case TAG_NIL:
      printf("(nil)\n");
      return 1;

    // ERROR
    //
    // Format:
    // +------+-----------+------------+----------------------+
    // | TAG  | Code      | Str Length | Message              |
    // | 1B   | 4B (404)  | 4B (9)     | N bytes "Not Found"  |
    // +------+-----------+------------+----------------------+
    case TAG_ERR: {

      if (size < 9) {
        msg("Bad response");
        return -1;
      }

      // read error code
      int32_t code = 0;
      memcpy(&code, &data[1], 4);

      // read msg length
      uint32_t msgLen = 0;
      memcpy(&msgLen, &data[5], 4);

      // ensure complete message exists.
      if (size < 9 + msgLen) {
        msg("bad response");
        return -1;
      }

      // print respnse
      // TAG_ERR
      cout << "(err): " << code << ' ';
      cout.write(reinterpret_cast<const char*>(data + 9), msgLen);
      cout << '\n';

      // Bytes consumed:
      // tag + code + length + string.
      return 9 + msgLen;
    }

    // STRING
    //
    // Format:
    //
    // +------+-----------+-----------+
    // | TAG  | Length(4B)| Characters|
    // +------+-----------+-----------+
    //
    // Example:
    //
    // TAG_STR
    // len = 5
    // data = "hello"
    case TAG_STR: {
      // need atleasat tag + length
      if (size < 5) {
        msg("bad response");
        return -1;
      }

      uint32_t msgLen = 0;
      memcpy(&msgLen, &data[1], 4);

      if (size < 5 + msgLen) {
        msg("Bad Response");
        return -1;
      }

      // print string
      cout << "(str): ";
      cout.write(reinterpret_cast<const char*>(data + 5), msgLen);
      cout << '\n';

      // bytes consumed
      return 5 + msgLen;
    }
    // INTEGER
    //
    // Format:
    //
    // +------+-------------+
    // | TAG  | int64_t(8B) |
    // +------+-------------+
    case TAG_INT : {
    // need atleasat tag + 8B integer
      if (size < 9) {
        msg("bad response");
        return -1;
      }

      uint64_t value = 0;
      memcpy(&value, &data[1], 8);

      // print string
      cout << "(int): " << (long long)value << endl;

      // bytes consumed
      return 9;
    }
    // DOUBLE
    //
    // Format:
    //
    // +------+------------+
    // | TAG  | double(8B) |
    // +------+------------+
    case TAG_DBL : {
    // need atleasat tag + *B integer
      if (size < 9) {
        msg("bad response");
        return -1;
      }

      double value = 0;
      memcpy(&value, &data[1], 8);

      // print string
      cout << "(int): " << value << endl;

      // bytes consumed
      return 9;
    }
    // ARRAY
    //
    // Format:
    //
    // +------+-----------+-----------------------------+
    // | TAG  | Count(4B) | Serialized element 1        |
    // |      |           | Serialized element 2        |
    // |      |           | ...                         |
    // +------+-----------+-----------------------------+
    //
    // Each element is itself a complete serialized
    // response (string/int/error/array/etc.).
    //
    // Therefore we recursively call print_response().
    case TAG_ARR: {
      // need tag + element count(4B)
      if (size < 5) {
        msg("Bad response");
        return -1;
      }

      // read element count
      uint32_t arrLen = 0;
      memcpy(&arrLen, &data[1], 4);

      cout << "(arr): len = " << arrLen << endl;

      size_t bytesConsumed = 5; // 1B (code) + 4B (array length)
      // parse every array length
      for (uint32_t i = 0; i < arrLen; i++) {

        // parse one serialized object at current position
        int32_t consumed = printResponse(
          &data[bytesConsumed],
          size - bytesConsumed
        );
        // Child parser failed.
        if (consumed < 0)
            return -1;

        // Move to the next serialized object.
        bytesConsumed += consumed;
      }

      cout <<"(arr): end" << endl;

      // Return total bytes consumed by
      // the entire array.
      return (int32_t)bytesConsumed;
    }
    // Unknown tag
    default:
        msg("bad response");
        return -1;
        
  } // switch-case ends
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

  // Parse and print the response.
  int32_t parsedLength = printResponse(
      reinterpret_cast<uint8_t*>(readBuffer + 4),
      bodyLength
  );

  // Ensure the parser consumed the entire response.
  if (parsedLength > 0 && static_cast<uint32_t>(parsedLength) != bodyLength) {
      msg("bad response");
      return -1;
  }

  return parsedLength;
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