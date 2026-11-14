/* Run with `make test`. Builds proc_utils.c without QNX's slog2. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
#define LOG_VERBOSE(...) ((void)0)
void log_msg(const char *fmt, ...);

#include "proc_utils.c"

void log_msg(const char *fmt, ...)
{
    (void)fmt;
}

static void check_path(pid_t pid, const char *expected)
{
    char path[32];

    build_exefile_path(path, pid);
    assert(strcmp(path, expected) == 0);
}

/* Reads a file holding `content` through a buffer of `size` bytes. */
static void read_back(const char *content, size_t size, char *out)
{
    char name[] = "build/stop_sender_XXXXXX";
    int fd = mkstemp(name);

    assert(fd >= 0);
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);

    read_line_signal_safe(name, out, size);
    unlink(name);
}

int main(void)
{
    char out[256];
    char small[8];

    check_path(0, "/proc/0/exefile");
    check_path(7, "/proc/7/exefile");
    check_path(14774363, "/proc/14774363/exefile");

    read_back("/base/usr/bin/slay\nextra\n", sizeof(out), out);
    assert(strcmp(out, "/base/usr/bin/slay") == 0);

    read_back("/base/usr/bin/slay", sizeof(out), out);
    assert(strcmp(out, "/base/usr/bin/slay") == 0);

    read_back("/base/usr/bin/slay", sizeof(small), small);
    assert(strcmp(small, "/base/u") == 0);

    read_back("", sizeof(out), out);
    assert(strcmp(out, "unknown") == 0);

    read_line_signal_safe("build/stop_sender_missing", out, sizeof(out));
    assert(strcmp(out, "unknown") == 0);

    /* A child must get the stop signals even when the spawning thread blocks them, as the
       daemon's worker threads do: this shell ends by its own SIGTERM, not by reaching exit 0. */
    sigset_t stop, old_mask;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stop, &old_mask);

    char *term_self[] = {"sh", "-c", "kill -TERM $$; exit 0", NULL};
    char *exit_ok[] = {"sh", "-c", "exit 0", NULL};
    assert(run_cmd_spawn(term_self, NULL) == -1);
    assert(run_cmd_spawn(exit_ok, NULL) == 0);

    /* The caller's own mask is left alone. */
    sigset_t now;
    pthread_sigmask(SIG_SETMASK, &old_mask, &now);
    assert(sigismember(&now, SIGTERM) && sigismember(&now, SIGHUP));

    puts("stop sender tests passed");
    return 0;
}
