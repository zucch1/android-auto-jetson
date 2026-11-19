// SPDX-License-Identifier: GPL-3.0-or-later
// TEST-ONLY LD_PRELOAD shim: a third-party writer stuck holding a FILE lock.
// At load it starts a thread that fwrite()s 256 KiB into a pipe nobody reads,
// so the thread blocks in write(2) while holding that FILE's lock forever.
// glibc's exit() flushes every open FILE under its lock and would wait for it.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void *hold(void *arg) {
    (void)arg;
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    FILE *stuck = fdopen(fds[1], "w");
    static char big[256 * 1024];
    memset(big, 'x', sizeof big);
    fwrite(big, 1, sizeof big, stuck); /* blocks in write() holding stuck's lock */
    return NULL;
}
__attribute__((constructor)) static void start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, hold, NULL) == 0) pthread_detach(t);
    usleep(100 * 1000);
}
