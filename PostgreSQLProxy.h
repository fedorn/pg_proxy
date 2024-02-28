#ifndef POSTGRESQLPROXY_H
#define POSTGRESQLPROXY_H

#include <unordered_map>
#include <csignal>

class PostgreSQLProxy {
private:
    int listenSock;                                 // Listening socket file descriptor
    int efd;                                       // Epoll file descriptor
    std::unordered_map<int, int> client_to_server; // Map client FD to server FD
    std::unordered_map<int, int> server_to_client; // Map server FD to client FD
    volatile std::sig_atomic_t &graceful_shutdown;

    void handleNewConnection();

    void forwardData(int fd);

    static int set_nonblock(int fd);

public:
    explicit PostgreSQLProxy(int port, volatile std::sig_atomic_t &graceful_shutdown);

    ~PostgreSQLProxy();

    void run();
};

#endif //POSTGRESQLPROXY_H
