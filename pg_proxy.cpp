#include <sys/epoll.h>
#include <netinet/in.h>
#include <unordered_map>
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

int set_nonblock(int fd) {
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}

class PostgreSQLProxy {
private:
    int listen_fd;                                 // Listening socket file descriptor
    int efd;                                       // Epoll file descriptor
    std::unordered_map<int, int> client_to_server; // Map client FD to server FD
    std::unordered_map<int, int> server_to_client; // Map server FD to client FD

public:
    explicit PostgreSQLProxy(int port);

    ~PostgreSQLProxy();

    [[noreturn]] void run();

    void handleNewConnection();

    void forwardData(int fd);
};

PostgreSQLProxy::PostgreSQLProxy(int port) {
    // Initialize listening socket and epoll
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // Creating socket struct for this socket
    struct sockaddr_in sockAddr{};
    sockAddr.sin_family = AF_INET;
    sockAddr.sin_port = htons(port);
    sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind socket and address
    if (bind(listen_fd, reinterpret_cast<struct sockaddr *>(&sockAddr), sizeof(sockAddr)) == -1) {
        close(listen_fd);
        throw std::runtime_error("Bind failed");
    }

    // Set socket to nonblocking mode
    set_nonblock(listen_fd);

    // Set socket as listening for new connections
    if (listen(listen_fd, SOMAXCONN) == -1) {
        close(listen_fd);
        throw std::runtime_error("Listen failed");
    }

    // Work with epoll
    struct epoll_event event{};  // event
    event.data.fd = listen_fd;   // socket
    event.events = EPOLLIN;      // event type

    // Create epoll descriptor
    efd = epoll_create1(0);
    if (efd == -1) {
        close(listen_fd);
        throw std::runtime_error("Epoll create failed");
    }
    epoll_ctl(efd, EPOLL_CTL_ADD, listen_fd, &event); // add event
}

PostgreSQLProxy::~PostgreSQLProxy() {
    close(efd);
    close(listen_fd);
}

[[noreturn]] void PostgreSQLProxy::run() {
    // Now we can accept and process connections
    static const int maxEvents = 32;
    while (true) {
        struct epoll_event events[maxEvents];  // array for events
        int count = epoll_wait(efd, events, maxEvents, -1); // wait for events

        // Handling events
        for (int i = 0; i < count; ++i) {
            struct epoll_event &e = events[i];

            // We got event from the listening socket
            if (e.data.fd == listen_fd) {
                handleNewConnection();
            }
                // We got event from client or postgres socket
            else {
                forwardData(e.data.fd);
            }
        }
    }
}

void PostgreSQLProxy::handleNewConnection() {
    // Accept new connection and add to epoll
    struct sockaddr_in newAddr{};
    socklen_t length = sizeof(newAddr);
    // Accepting connection
    int newSocket = accept(listen_fd,
                           reinterpret_cast<sockaddr *>(&newAddr),
                           &length);
    if (newSocket == -1) {
        throw std::runtime_error("Accept client failed");
    }
    set_nonblock(newSocket); // change to nonblocking mode

    // Add socket to epoll
    struct epoll_event event{};
    event.data.fd = newSocket;
    event.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, newSocket, &event) == -1) {
        throw std::runtime_error("Epoll add failed");
    }

    std::cout << "client " << newSocket << " connected." << std::endl;

    // Add new postgres socket
    int pgSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in pgAddr
            {
            };
    pgAddr.sin_family = AF_INET;
    pgAddr.sin_port = htons(5432);
    pgAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(pgSock, (struct sockaddr *) &pgAddr, sizeof(pgAddr)) == -1) {
        throw std::runtime_error("Connection to PostgreSQL failed");
    }
    set_nonblock(pgSock); // change to nonblocking mode

    // Add postgres socket to epoll
    struct epoll_event pgEvent{};
    pgEvent.data.fd = pgSock;
    pgEvent.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, pgSock, &pgEvent) == -1) {
        throw std::runtime_error("Epoll add failed");
    }

    client_to_server[newSocket] = pgSock;
    server_to_client[pgSock] = newSocket;
}

void PostgreSQLProxy::forwardData(int fd) {
    // Read data from fd, log it, and forward to the corresponding FD
    // Buffer and length for receiving data
    static const size_t length = 1024;
    static char buffer[length];

    // Receiving
    ssize_t result = recv(fd, buffer, length - 1, MSG_NOSIGNAL);

    // Client disconnected
    if (result == 0 && errno != EAGAIN) {
        // Closing connection
        shutdown(fd, SHUT_RDWR);
        close(fd);

        if (auto itcs = client_to_server.find(fd); itcs != client_to_server.end()) {
            shutdown(itcs->second, SHUT_RDWR);
            close(itcs->second);
            std::cout << "client " << fd << "and server " << itcs->second << " disconnected." << std::endl;
            client_to_server.erase(itcs);
        } else if (auto itsc = server_to_client.find(fd); itsc != server_to_client.end()) {
            shutdown(itsc->second, SHUT_RDWR);
            close(itsc->second);
            std::cout << "client " << fd << "and server " << itsc->second << " disconnected." << std::endl;
            server_to_client.erase(itsc);
        }
    }
        // Socket received data
    else if (result > 0) {
        if (client_to_server.find(fd) != client_to_server.end()) {
            std::cout << "send to server " << client_to_server[fd] << " from client " << fd << ' ' << result
                      << " bytes : " << buffer << std::endl;
            send(client_to_server[fd], buffer, result, MSG_NOSIGNAL);
            // Log queries
            // TODO: ignore startup message that doesn't have initial byte
            // TODO: log to file
            // TODO: make sure &buffer[5] ends with \0
            if (buffer[0] == 'Q') {
                std::cout << "Query: ===" << &buffer[5] << "===" << std::endl;
            }
        } else if (server_to_client.find(fd) != server_to_client.end()) {
            std::cout << "send to client " << server_to_client[fd] << " from server " << fd << ' ' << result
                      << " bytes : " << buffer << std::endl;
            send(server_to_client[fd], buffer, result, MSG_NOSIGNAL);
        } else {
            throw std::runtime_error("Unknown descriptor");
        }
    }
}

int main() {
    PostgreSQLProxy proxy(5434);
    proxy.run();
}
