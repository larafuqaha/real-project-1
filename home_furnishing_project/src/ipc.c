#include "ipc.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

ssize_t read_full(int fd, void* buf, size_t n) {
    size_t got = 0;
    char*  p   = (char*)buf;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return (ssize_t)got;          /* EOF */
        if (r < 0) {
            if (errno == EINTR) return -1;        /* let caller handle the signal */
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

ssize_t write_full(int fd, const void* buf, size_t n) {
    size_t put = 0;
    const char* p = (const char*)buf;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        put += (size_t)w;
    }
    return (ssize_t)put;
}

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int set_block(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

void drain_pipe_nonblock(int fd) {
    if (fd < 0) return;
    if (set_nonblock(fd) < 0) return;
    char buf[1024];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r > 0) continue;
        if (r == 0) break;            /* EOF */
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        break;
    }
    set_block(fd);
}
