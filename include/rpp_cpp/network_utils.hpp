#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

uint16_t get_available_port() {
    // 1. Kreiraj standardni TCP socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Slušaj na svim sučeljima
    addr.sin_port = htons(0);          // Ključni dio: Port 0 traži slobodan port od kernela!

    // 2. Veži socket (sustav ovdje dodjeljuje slobodan port)
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    // 3. Pročitaj koji je port sustav stvarno dodijelio
    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &len) < 0) {
        close(sock);
        return -1;
    }

    // 4. Zatvori socket i oslobodi port za ponovnu upotrebu
    close(sock);

    // Pretvori iz mrežnog (network byte order) u lokalni format (host byte order)
    return ntohs(addr.sin_port);
}