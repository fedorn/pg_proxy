#include <csignal>
#include "PostgreSQLProxy.h"

// Signal handling for graceful shutdown
volatile std::sig_atomic_t graceful_shutdown = 0;

void signal_handler(int signal) {
    graceful_shutdown = 1;
}

int main() {
    PostgreSQLProxy proxy(5434, graceful_shutdown);
    // Setup signal handling for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    proxy.run();
}
