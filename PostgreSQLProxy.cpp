#include "PostgreSQLProxy.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int PostgreSQLProxy::set_nonblock(int fd) {
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

PostgreSQLProxy::PostgreSQLProxy(std::string pgAddress, int pgPort, int proxyPort, const std::string &logPath,
                                 volatile std::sig_atomic_t &graceful_shutdown)
        : pgAddress(std::move(pgAddress)), pgPort(pgPort), logFile(logPath, std::ios::app), graceful_shutdown{graceful_shutdown} {
    // Open log file
    if (!logFile.is_open()) {
        std::cerr << "Failed to open log file: " << logPath << std::endl;
        throw std::runtime_error("Failed to open log file.");
    }
    // Initialize listening socket and epoll
    listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == -1) {
        std::cerr << "Failed to create socket descriptor. " << strerror(errno) << std::endl;
        throw std::runtime_error("Create socket failed");
    }
    // Creating socket struct for this socket
    struct sockaddr_in sockAddr{};
    sockAddr.sin_family = AF_INET;
    sockAddr.sin_port = htons(proxyPort);
    sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind socket and address
    if (bind(listenSock, reinterpret_cast<struct sockaddr *>(&sockAddr), sizeof(sockAddr)) == -1) {
        std::cerr << "Bind failed. " << strerror(errno) << std::endl;
        shutdown(listenSock, SHUT_RDWR);
        close(listenSock);
        throw std::runtime_error("Bind failed");
    }

    // Set socket to nonblocking mode
    set_nonblock(listenSock);

    // Set socket as listening for new connections
    if (listen(listenSock, SOMAXCONN) == -1) {
        std::cerr << "Listen failed. " << strerror(errno) << std::endl;
        shutdown(listenSock, SHUT_RDWR);
        close(listenSock);
        throw std::runtime_error("Listen failed");
    }

    // Work with epoll
    struct epoll_event event{};  // event
    event.data.fd = listenSock;  // socket
    event.events = EPOLLIN;      // event type

    // Create epoll descriptor
    efd = epoll_create1(0);
    if (efd == -1) {
        std::cerr << "Epoll create failed. " << strerror(errno) << std::endl;
        shutdown(listenSock, SHUT_RDWR);
        close(listenSock);
        throw std::runtime_error("Epoll create failed");
    }
    if (epoll_ctl(efd, EPOLL_CTL_ADD, listenSock, &event) == -1) {  // add event
        std::cerr << "Epoll add failed. " << strerror(errno) << std::endl;
        shutdown(listenSock, SHUT_RDWR);
        close(listenSock);
        close(efd);
        throw std::runtime_error("Epoll add failed");
    }
}

PostgreSQLProxy::~PostgreSQLProxy() {
    shutdown(listenSock, SHUT_RDWR);
    close(listenSock);
    close(efd);
}

void PostgreSQLProxy::run() {
    // Now we can accept and process connections
    static const int maxEvents = 32;
    while (!graceful_shutdown) {
        struct epoll_event events[maxEvents];  // array for events
        int count = epoll_wait(efd, events, maxEvents, -1); // wait for events

        // Handling events
        for (int i = 0; i < count; ++i) {
            struct epoll_event &e = events[i];

            // We got event from the listening socket
            if (e.data.fd == listenSock) {
                handleNewConnection();
            }
                // We got event from client or postgres socket
            else {
                forwardData(e.data.fd);
            }
        }
    }

    // Close all client/server connections before shutdown
    std::cout << "Shutting down gracefully." << std::endl;
    for (auto &it: client_to_server) {
        shutdown(it.first, SHUT_RDWR);
        close(it.first);
        shutdown(it.second, SHUT_RDWR);
        close(it.second);
    }
    client_to_server.clear();
    server_to_client.clear();
}

void PostgreSQLProxy::handleNewConnection() {
    // Accept new connection and add to epoll
    struct sockaddr_in newAddr{};
    socklen_t length = sizeof(newAddr);
    // Accepting connection
    int newSocket = accept(listenSock,
                           reinterpret_cast<sockaddr *>(&newAddr),
                           &length);
    if (newSocket == -1) {
        std::cerr << "Accept failed. " << strerror(errno) << std::endl;
        return;
    }
    set_nonblock(newSocket); // change to nonblocking mode

    // Add socket to epoll
    struct epoll_event event{};
    event.data.fd = newSocket;
    event.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, newSocket, &event) == -1) {
        std::cerr << "Epoll add failed. " << strerror(errno) << std::endl;
        shutdown(newSocket, SHUT_RDWR);
        close(newSocket);
        return;
    }

    std::cout << "client " << newSocket << " connected." << std::endl;

    // Add new postgres socket
    int pgSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in pgAddr{};
    pgAddr.sin_family = AF_INET;
    pgAddr.sin_port = htons(pgPort);
    pgAddr.sin_addr.s_addr = inet_addr(pgAddress.c_str());

    if (connect(pgSock, (struct sockaddr *) &pgAddr, sizeof(pgAddr)) == -1) {
        std::cerr << "Connection to PostgreSQL failed. " << strerror(errno) << std::endl;
        shutdown(newSocket, SHUT_RDWR);
        close(newSocket);
        shutdown(pgSock, SHUT_RDWR);
        close(pgSock);
        return;
    }
    set_nonblock(pgSock); // change to nonblocking mode

    // Add postgres socket to epoll
    struct epoll_event pgEvent{};
    pgEvent.data.fd = pgSock;
    pgEvent.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, pgSock, &pgEvent) == -1) {
        std::cerr << "Epoll add failed. " << strerror(errno) << std::endl;
        shutdown(newSocket, SHUT_RDWR);
        close(newSocket);
        shutdown(pgSock, SHUT_RDWR);
        close(pgSock);
        return;
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
    if (result <= 0 && errno != EAGAIN) {
        std::cerr << "Received 0 bytes or receive failed, disconnecting. " << strerror(errno) << std::endl;

        // Closing connection
        shutdown(fd, SHUT_RDWR);
        close(fd);

        if (auto itcs = client_to_server.find(fd); itcs != client_to_server.end()) {
            shutdown(itcs->second, SHUT_RDWR);
            close(itcs->second);
            client_to_server.erase(itcs);
        } else if (auto itsc = server_to_client.find(fd); itsc != server_to_client.end()) {
            shutdown(itsc->second, SHUT_RDWR);
            close(itsc->second);
            server_to_client.erase(itsc);
        }
    }
        // Socket received data
    else if (result > 0) {
        if (auto itcs = client_to_server.find(fd); itcs != client_to_server.end()) {
            std::cout << "send to server " << client_to_server[fd] << " from client " << fd << ' ' << result
                      << " bytes : " << buffer << std::endl;
            send(itcs->second, buffer, result, MSG_NOSIGNAL);
            // Ignore startup message that doesn't have initial byte
            if (auto itsi = sent_initial.find(fd); itsi != sent_initial.end()) {
                // Log queries
                if (buffer[0] == 'Q') {
                    if (result < 5) {
                        std::cerr << "Wrong Query message format." << std::endl;
                    } else {
                        buffer[result] = 0;  // Make sure &buffer[5] ends with \0
                        std::cout << "Query: ===" << &buffer[5] << "===" << std::endl;
                        logFile << &buffer[5] << std::endl;
                    }
                }
            }
            else {
                sent_initial.insert(fd);
            }
        } else if (auto itsc = server_to_client.find(fd); itsc != server_to_client.end()) {
            std::cout << "send to client " << server_to_client[fd] << " from server " << fd << ' ' << result
                      << " bytes : " << buffer << std::endl;
            send(itsc->second, buffer, result, MSG_NOSIGNAL);
        } else {
            std::cerr << "Unknown descriptor." << std::endl;
        }
    }
}