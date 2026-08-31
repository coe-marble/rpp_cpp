#pragma once

#include <cstdint>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

inline uint16_t get_available_port()
{
#ifdef _WIN32
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        throw std::runtime_error("Unable to create port discovery socket.");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket_handle);
        throw std::runtime_error("Unable to bind port discovery socket.");
    }
    int length = sizeof(address);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(socket_handle);
        throw std::runtime_error("Unable to inspect port discovery socket.");
    }
    closesocket(socket_handle);
    return ntohs(address.sin_port);
#else
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) {
        throw std::runtime_error("Unable to create port discovery socket.");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_handle);
        throw std::runtime_error("Unable to bind port discovery socket.");
    }
    socklen_t length = sizeof(address);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close(socket_handle);
        throw std::runtime_error("Unable to inspect port discovery socket.");
    }
    close(socket_handle);
    return ntohs(address.sin_port);
#endif
}
