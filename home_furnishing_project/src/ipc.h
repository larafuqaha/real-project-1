#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <sys/types.h>

/* Read exactly n bytes (loops on partial reads, retries on EINTR).
 * Returns n on success, 0 on clean EOF, -1 on error.
 * IMPORTANT: returns 0 on EINTR-then-EOF semantics — caller must check global
 * flags (reset_flag, stop_flag) to distinguish "signal woke me" from "peer closed". */
ssize_t read_full(int fd, void* buf, size_t n);

/* Write exactly n bytes. Returns n on success, -1 on error. */
ssize_t write_full(int fd, const void* buf, size_t n);

/* Drain a non-blocking pipe: read until EAGAIN or EOF. Used during round reset. */
void drain_pipe_nonblock(int fd);

/* Make an existing fd non-blocking. */
int set_nonblock(int fd);

/* Make a pipe-fd blocking again (we toggle for drain). */
int set_block(int fd);

#endif
