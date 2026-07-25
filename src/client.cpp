#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

void die(const string& msg){
  cerr << msg << ": " << strerror(errno) << '\n';
}

int main(){
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0){
    die("socket");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(
    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
  )
    die("connect");
  
  string msg = "hello";
  write(fd, msg.c_str(), msg.size());

  char buffer[64]{};
  ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
  if (n < 0){
    die("read");
  }
  cout << "Server says: " << buffer << '\n';

  close(fd);
}