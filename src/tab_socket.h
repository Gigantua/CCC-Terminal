#pragma once
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include "input.h"
#include "pacing.h"

namespace tabsocket {

struct Wire { int32_t pid, x, y; };

inline bool send(int s, const Wire& w, int in, int out) {
    iovec io = { const_cast<Wire*>(&w), sizeof w };
    alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))] = {};
    msghdr m = {};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = sizeof control;
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(2 * sizeof(int));
    int fds[2] = { in, out };
    std::memcpy(CMSG_DATA(c), fds, sizeof fds);
#ifdef MSG_NOSIGNAL
    return sendmsg(s, &m, MSG_NOSIGNAL) == (ssize_t)sizeof w;
#else
    return sendmsg(s, &m, 0) == (ssize_t)sizeof w;
#endif
}

inline bool receive(int s, Wire& w, int (&fds)[2]) {
    iovec io = { &w, sizeof w };
    alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))] = {};
    msghdr m = {};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = sizeof control;
    if (recvmsg(s, &m, MSG_WAITALL) != (ssize_t)sizeof w) return false;
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len != CMSG_LEN(2 * sizeof(int))) return false;
    std::memcpy(fds, CMSG_DATA(c), sizeof fds);
    for (int fd : fds) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return true;
}

inline bool give(const sockaddr_un& address, socklen_t len, int type, const TabHandoff& h) {
    int c = socket(AF_UNIX, type, 0);
    if (c < 0) return false;
    fcntl(c, F_SETFD, FD_CLOEXEC);
    timeval limit = { 5, 0 };
    Wire w = { (int32_t)os::processId(h.process), h.x, h.y };
    char accepted = 0;
    bool ok = setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit) == 0 && connect(c, reinterpret_cast<const sockaddr*>(&address), len) == 0 &&
              send(c, w, h.in, h.out) && recv(c, &accepted, 1, 0) == 1 && accepted == 1;
    close(c);
    return ok;
}

template <class Place>
inline void take(int listener, InputQueue& inputs, Place indexAt) {
    int c = accept(listener, nullptr, nullptr);
    if (c < 0) return;
    fcntl(c, F_SETFD, FD_CLOEXEC);
    timeval limit = { 5, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    Wire w = {};
    int fds[2] = { -1, -1 };
    if (receive(c, w, fds)) {
        TabHandoff h;
        h.in = fds[0];
        h.out = fds[1];
        h.process = os::openProcess((unsigned long)w.pid);
        h.x = w.x;
        h.y = w.y;
        h.index = indexAt(w.x, w.y);
        inputs.push(h);
        requestFrame();
        char accepted = 1;
        ::send(c, &accepted, 1, 0);
    }
    close(c);
}

}
