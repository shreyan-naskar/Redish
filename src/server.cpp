#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

static void msg(const string& msg){
  cerr << msg << endl;
}

static void die(const string& msg){
  cerr << "[" << errno << "]" << msg << strerror(errno) << endl;
  exit(EXIT_FAILURE);
}

static void doSomething(int connfd) {
    char buffer[64]{};

    ssize_t n = read(connfd, buffer, sizeof(buffer) - 1);
    if (n < 0) {
        msg("read() error");
        return;
    }

    std::cout << "Client says: " << buffer << '\n';

    std::string reply = "world";
    write(connfd, reply.c_str(), reply.size());
}

int main() {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
        die("socket()");

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(1234);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(serverFd,
             reinterpret_cast<sockaddr*>(&serverAddr),
             sizeof(serverAddr)) < 0)
        die("bind()");

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

        doSomething(clientFd);
        close(clientFd);
    }

    close(serverFd);
}



