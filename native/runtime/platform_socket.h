// Sockets on Windows (Winsock) and on Android/iOS (POSIX), behind one set of names.
// On Windows each function makes exactly the calls the code made before this layer existed.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <thread>

namespace plat {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kNoSocket = INVALID_SOCKET;
using SockLen = int;
#else
using Socket = int;
inline constexpr Socket kNoSocket = -1;
using SockLen = socklen_t;
#endif

// As stored in plain integers (lobby.h, net.h): INVALID_SOCKET stays ~0, -1 becomes ~0 too.
inline std::uintptr_t to_handle(Socket s) { return std::uintptr_t(s); }
inline Socket from_handle(std::uintptr_t h) { return static_cast<Socket>(h); }

// Once per program; true if sockets can be used.
inline bool net_ready() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
#else
    return true;
#endif
}

inline void close_socket(Socket s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

inline void set_blocking(Socket s, bool blocking) {
#ifdef _WIN32
    u_long nonblocking = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

inline void set_option(Socket s, int level, int name, bool on) {
#ifdef _WIN32
    BOOL value = on ? TRUE : FALSE;
#else
    int value = on ? 1 : 0;
#endif
    setsockopt(s, level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

// A writer to a closed connection must get an error, not a signal that ends the program.
inline void no_sigpipe([[maybe_unused]] Socket s) {
#ifdef SO_NOSIGPIPE
    set_option(s, SOL_SOCKET, SO_NOSIGPIPE, true);
#endif
}

#if defined(MSG_NOSIGNAL)
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

#ifdef _WIN32
inline int send_some(Socket s, const void* data, int n) { return ::send(s, static_cast<const char*>(data), n, 0); }
inline int receive_some(Socket s, void* buffer, int n) { return ::recv(s, static_cast<char*>(buffer), n, 0); }
#else
inline int send_some(Socket s, const void* data, int n) {
    return int(::send(s, data, std::size_t(n), kSendFlags));
}
inline int receive_some(Socket s, void* buffer, int n) { return int(::recv(s, buffer, std::size_t(n), 0)); }
#endif

// The last call only could not go on yet (non-blocking socket); a connect still under way counts.
inline bool would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

// The first argument of select(): Windows ignores it, POSIX needs the highest socket plus one.
inline int select_width(Socket highest) {
#ifdef _WIN32
    (void)highest;
    return 0;
#else
    return highest + 1;
#endif
}

// After a select() that had a connecting socket in `writable` and `failed`: -1 failed, 0 still
// connecting, 1 connected. Windows reports a failed connect in `failed`, POSIX as writable with an error.
inline int connect_state(Socket s, fd_set& writable, fd_set& failed) {
    if (FD_ISSET(s, &failed)) return -1;
    if (!FD_ISSET(s, &writable)) return 0;
#ifndef _WIN32
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) return -1;
#endif
    return 1;
}

inline void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(DWORD(ms));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

// Milliseconds of a steady clock (QueryPerformanceCounter on Windows, as before).
inline std::uint64_t steady_ms() {
#ifdef _WIN32
    static const LONGLONG frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return std::uint64_t(counter.QuadPart / frequency * 1000 + counter.QuadPart % frequency * 1000 / frequency);
#else
    using namespace std::chrono;
    return std::uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

}  // namespace plat
