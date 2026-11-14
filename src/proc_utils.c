#include "proc_utils.h"
#include "log_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <sys/wait.h>

/* Process execution and control helpers */

/* A spawned child inherits the calling thread's signal mask, and worker threads block the stop
   signals (start_worker). ims_rrm started from the monitor thread ran with SIGTERM blocked, so
   every stop waited out its timeout and ended in SIGKILL. The caller still has to set
   POSIX_SPAWN_SETSIGMASK in the flags. */
void spawnattr_unblock_stop_signals(posix_spawnattr_t *attr)
{
    sigset_t mask;

    pthread_sigmask(SIG_SETMASK, NULL, &mask);
    sigdelset(&mask, SIGTERM);
    sigdelset(&mask, SIGINT);
    sigdelset(&mask, SIGHUP);
    posix_spawnattr_setsigmask(attr, &mask);
}

int run_cmd_spawn(char *const argv[], const char *log_path)
{
    if (!argv || !argv[0])
        return -1;

    int fd = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (log_path)
    {
        fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0)
        {
            posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, fd);
        }
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    spawnattr_unblock_stop_signals(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

    pid_t pid;
    int status = posix_spawnp(&pid, argv[0], &actions, &attr, argv, NULL);

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (fd >= 0)
        close(fd);

    if (status != 0)
        return -1;

    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno != EINTR)
            return -1;
    }

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* 1 when the process's executable path contains name, or equals it with strict. */
static int exe_matches(int pid, const char *name, int strict)
{
    char path[PATH_MAX];
    char exe_path[PATH_MAX];

    snprintf(path, sizeof(path), "/proc/%d/exefile", pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    ssize_t n = read(fd, exe_path, sizeof(exe_path) - 1);
    close(fd);
    if (n <= 0)
        return 0;

    exe_path[n] = '\0';
    exe_path[strcspn(exe_path, "\n")] = '\0';
    return strict ? strcmp(exe_path, name) == 0 : strstr(exe_path, name) != NULL;
}

int find_pid_by_name(const char *name, int strict)
{
    if (!name)
        return -1;

    DIR *d = opendir("/proc");
    if (!d)
        return -1;

    struct dirent *ent;
    int found_pid = -1;

    while ((ent = readdir(d)))
    {
        int pid = atoi(ent->d_name);
        if (pid > 0 && exe_matches(pid, name, strict))
        {
            found_pid = pid;
            break;
        }
    }
    closedir(d);
    return found_pid;
}

/* --- Helpers safe to call from a signal handler: no stdio, no malloc --- */

static void build_exefile_path(char *path, pid_t pid)
{
    char digits[12];
    int count = 0;
    unsigned int value = (unsigned int)pid;

    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    memcpy(path, "/proc/", 6);
    path += 6;
    while (count > 0)
        *path++ = digits[--count];
    memcpy(path, "/exefile", 9);
}

static void read_line_signal_safe(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t len = fd >= 0 ? read(fd, out, size - 1) : -1;

    if (fd >= 0)
        close(fd);

    if (len <= 0)
    {
        memcpy(out, "unknown", 8);
        return;
    }

    for (ssize_t i = 0; i < len; i++)
    {
        if (out[i] == '\n')
        {
            len = i;
            break;
        }
    }
    out[len] = '\0';
}

void exe_name_signal_safe(pid_t pid, char *out, size_t size)
{
    char path[32];

    build_exefile_path(path, pid);
    read_line_signal_safe(path, out, size);
}

/* Matches like find_pid_by_name(name, 0): a command line also names ims_rrm in a tester's
   `slog2info -b ims_rrm`. Returns how many processes were stopped, 0 when none is running,
   -1 on error. */
int slay_by_name_graceful(const char *search_str, int timeout_ms)
{
    if (!search_str || strlen(search_str) == 0)
        return -1;

    DIR *d = opendir("/proc");
    if (!d)
        return -1;

    struct dirent *ent;
    size_t capacity = 16;
    size_t pid_count = 0;
    pid_t *pids = malloc(capacity * sizeof(pid_t));
    if (!pids)
    {
        closedir(d);
        log_msg("Failed to allocate memory for PIDs");
        return -1;
    }

    pid_t self_pid = getpid();

    /* Step 1: Scan /proc and collect matching process IDs */
    while ((ent = readdir(d)))
    {
        int pid = atoi(ent->d_name);
        if (pid <= 0 || pid == (int)self_pid)
            continue;

        if (!exe_matches(pid, search_str, 0))
            continue;

        if (pid_count >= capacity)
        {
            size_t new_capacity = capacity * 2;
            pid_t *new_pids = realloc(pids, new_capacity * sizeof(pid_t));
            if (!new_pids)
            {
                log_msg("Memory reallocation failed, processing partial list (%zu processes)", pid_count);
                break;
            }
            pids = new_pids;
            capacity = new_capacity;
        }
        pids[pid_count++] = (pid_t)pid;
    }
    closedir(d);

    /* Nothing to stop is not a failure: the caller wants it gone, and it is. */
    if (pid_count == 0)
    {
        free(pids);
        return 0;
    }

    /* Step 2: Send SIGTERM signal to all matched processes */
    int active_pids = 0;
    for (size_t i = 0; i < pid_count; i++)
    {
        if (kill(pids[i], SIGTERM) == 0)
        {
            log_msg("Signal SIGTERM sent to PID %d", pids[i]);
            active_pids++;
        }
        else
        {
            log_msg("Failed to send SIGTERM to PID %d: %s", pids[i], strerror(errno));
            pids[i] = -1;
        }
    }

    if (active_pids == 0)
    {
        free(pids);
        return 0;
    }

    /* Step 3: Poll process exit state until timeout */
    int step_us = 50000; /* 50 ms */
    int elapsed_us = 0;
    int timeout_us = timeout_ms * 1000;

    while (elapsed_us < timeout_us)
    {
        usleep(step_us);
        elapsed_us += step_us;

        int still_running = 0;
        for (size_t i = 0; i < pid_count; i++)
        {
            if (pids[i] <= 0)
                continue;

            /* kill(pid, 0) also succeeds on a zombie, so reap our own children first. */
            waitpid(pids[i], NULL, WNOHANG);

            if (kill(pids[i], 0) == 0)
            {
                still_running++;
            }
            else
            {
                pids[i] = -1;
            }
        }

        if (still_running == 0)
            break;
    }

    /* Step 4: Send SIGKILL to non-responsive processes */
    for (size_t i = 0; i < pid_count; i++)
    {
        if (pids[i] > 0 && kill(pids[i], 0) == 0)
        {
            if (kill(pids[i], SIGKILL) == 0)
            {
                log_msg("Process PID %d did not exit after %d ms; sent SIGKILL", pids[i], timeout_ms);
            }
            else
            {
                log_msg("Failed to send SIGKILL to PID %d: %s", pids[i], strerror(errno));
            }
        }
    }

    free(pids);
    return (int)pid_count;
}

int run_cmd_capture(const char *cmd, char *out, size_t max_len)
{
    FILE *p;
    size_t n;

    if (!cmd || !out || max_len == 0)
        return -1;

    p = popen(cmd, "r");
    if (!p)
        return -1;

    n = fread(out, 1, max_len - 1, p);
    out[n] = '\0';
    pclose(p);
    return (int)n;
}
