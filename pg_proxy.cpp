#include <csignal>
#include <iostream>
#include "postgreSqlProxy.h"

// Signal handling for graceful shutdown
volatile std::sig_atomic_t gracefulShutdown = 0;

void signalHandler([[maybe_unused]] int signal) {
    gracefulShutdown = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <PgAddress> <PgPort> <ProxyPort>" << std::endl;
        return 1;
    }

    std::string pgAddress = argv[1];
    int pgPort = std::stoi(argv[2]);
    int proxyPort = std::stoi(argv[3]);

    postgreSqlProxy proxy(pgAddress, pgPort, proxyPort, "log.txt", gracefulShutdown);
    // Setup signal handling for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    proxy.run();
}
