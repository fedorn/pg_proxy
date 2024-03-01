#ifndef PG_PROXY_POSTGRESQLPROXY_H
#define PG_PROXY_POSTGRESQLPROXY_H

#include <unordered_map>
#include <unordered_set>
#include <csignal>
#include <fstream>

class postgreSqlProxy {
private:
    int listenSock;                               // Listening socket file descriptor
    int efd;                                      // Epoll file descriptor
    std::unordered_map<int, int> clientToServer;  // Map client FD to server FD
    std::unordered_map<int, int> serverToClient;  // Map server FD to client FD
    std::unordered_set<int> sentInitial;          // Clients that already sent initial message
    volatile std::sig_atomic_t &gracefulShutdown; // Reference to variable for graceful shutdown
    std::ofstream logFile;                        // Log file object
    std::string pgAddress;                        // PostgreSQL server address
    int pgPort;                                   // PostgreSQL server port

    void handleNewConnection();

    void forwardData(int fd);

    static int setNonblock(int fd);

    template<typename... Sockets>
    void closeSockets(int sock, Sockets... rest);

public:
    explicit postgreSqlProxy(std::string pgAddress, int pgPort, int proxyPort, const std::string &logPath,
                             volatile std::sig_atomic_t &gracefulShutdown);

    ~postgreSqlProxy();

    void run();
};

#endif //PG_PROXY_POSTGRESQLPROXY_H
