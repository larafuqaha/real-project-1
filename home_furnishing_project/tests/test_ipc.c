#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "../src/ipc.h"
#include "../src/common.h"

static void test_write_read_roundtrip(void) {
    int fds[2];
    pipe(fds);

    msg_t out;
    memset(&out, 0, sizeof out);
    out.kind        = MSG_PIECE;
    out.serial      = 42;
    out.piece_index = 7;
    out.round       = 3;
    out.direction   = +1;

    ssize_t w = write_full(fds[1], &out, sizeof out);
    assert(w == (ssize_t)sizeof out);

    msg_t in;
    memset(&in, 0, sizeof in);
    ssize_t r = read_full(fds[0], &in, sizeof in);
    assert(r == (ssize_t)sizeof in);

    assert(in.kind        == out.kind);
    assert(in.serial      == out.serial);
    assert(in.piece_index == out.piece_index);
    assert(in.round       == out.round);
    assert(in.direction   == out.direction);

    close(fds[0]);
    close(fds[1]);
    printf("PASS: write_full/read_full roundtrip\n");
}

static void test_read_eof(void) {
    int fds[2];
    pipe(fds);
    close(fds[1]);   /* close write end — reader will see EOF */

    msg_t buf;
    memset(&buf, 0, sizeof buf);
    ssize_t r = read_full(fds[0], &buf, sizeof buf);
    assert(r == 0);

    close(fds[0]);
    printf("PASS: read_full returns 0 on EOF\n");
}

static void test_drain_empties_pipe(void) {
    int fds[2];
    pipe(fds);

    char junk[256];
    memset(junk, 0xAB, sizeof junk);
    write(fds[1], junk, sizeof junk);

    drain_pipe_nonblock(fds[0]);

    /* after drain, a non-blocking read should return EAGAIN */
    set_nonblock(fds[0]);
    char buf[4];
    ssize_t r = read(fds[0], buf, sizeof buf);
    assert(r < 0 && errno == EAGAIN);
    set_block(fds[0]);

    close(fds[0]);
    close(fds[1]);
    printf("PASS: drain_pipe_nonblock empties the pipe\n");
}

static void test_multiple_messages_in_order(void) {
    int fds[2];
    pipe(fds);

    /* write 5 messages */
    for (int i = 0; i < 5; i++) {
        msg_t m;
        memset(&m, 0, sizeof m);
        m.kind   = MSG_PIECE;
        m.serial = i + 1;
        write_full(fds[1], &m, sizeof m);
    }

    /* read them back and verify order */
    for (int i = 0; i < 5; i++) {
        msg_t m;
        memset(&m, 0, sizeof m);
        read_full(fds[0], &m, sizeof m);
        assert(m.serial == i + 1);
    }

    close(fds[0]);
    close(fds[1]);
    printf("PASS: multiple messages arrive in order (FIFO)\n");
}

int main(void) {
    printf("--- IPC Tests ---\n");
    test_write_read_roundtrip();
    test_read_eof();
    test_drain_empties_pipe();
    test_multiple_messages_in_order();
    printf("All IPC tests passed.\n\n");
    return 0;
}
