#include <csignal>
#include <iostream>
#include "PostgreSQLProxy.h"

// Signal handling for graceful shutdown
volatile std::sig_atomic_t graceful_shutdown = 0;

void signal_handler([[maybe_unused]] int signal) {
    graceful_shutdown = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <PgAddress> <PgPort> <ProxyPort>" << std::endl;
        return 1;
    }

    std::string pgAddress = argv[1];
    int pgPort = std::stoi(argv[2]);
    int proxyPort = std::stoi(argv[3]);

    PostgreSQLProxy proxy(pgAddress, pgPort, proxyPort, "log.txt", graceful_shutdown);
    // Setup signal handling for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    proxy.run();
}
