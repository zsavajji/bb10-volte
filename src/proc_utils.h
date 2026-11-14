#ifndef PROC_UTILS_H
#define PROC_UTILS_H

#include <stddef.h>
#include <spawn.h>
#include <sys/types.h>

/* Process & system control */
int run_cmd_spawn(char *const argv[], const char *log_path);

/* Runs cmd through /bin/sh and copies up to max_len-1 bytes of its stdout into out.
 * Returns the number of bytes read, or -1 on error. */
int run_cmd_capture(const char *cmd, char *out, size_t max_len);
int find_pid_by_name(const char *name, int strict);
int slay_by_cmdline_graceful(const char *search_str, int timeout_ms);

/* Sets the child's signal mask to the caller's without SIGTERM, SIGINT and SIGHUP, which worker
 * threads block. Add POSIX_SPAWN_SETSIGMASK to the attribute's flags. */
void spawnattr_unblock_stop_signals(posix_spawnattr_t *attr);

/* Async-signal-safe: writes the process's executable path, or "unknown". size must be at least 8. */
void exe_name_signal_safe(pid_t pid, char *out, size_t size);

#endif /* PROC_UTILS_H */
