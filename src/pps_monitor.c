#include "pps_monitor.h"
#include "sys_utils.h"
#include "berry_monitor.h"
#include "carrier_apply.h"
#include "daemon_stats.h"
#include "ims_control.h"
#include "ims_reconnect.h"
#include "ims_routes.h"
#include "system_state.h"
#include "voice_flag.h"
#include "volte_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

enum
{
    FD_RADIO = 0,
    FD_UICC,
    FD_DATA,
    FD_PHONE,
    FD_COUNT
};

#define CARRIER_DB_RETRY_SEC 300 /* between automatic downloads while the carrier list is missing */

static pthread_t g_lte_thread;
volatile int g_lte_monitor_running = 0;

/* Starts the whole startup path again in the same process: the new SIM, or a carrier list that
   was just downloaded, need their XML built and reloaded. */
void request_daemon_restart(void)
{
    g_restart_requested = 1;
    kill(getpid(), SIGHUP); /* wakes the main thread's sleep() */
}

/* --- Background Multi-PPS Monitor Thread --- */

static void *detailed_pps_monitor_thread(void *arg)
{
    (void)arg;
    struct pollfd pfds[FD_COUNT];
    const char *paths[FD_COUNT] = {PPS_RADIO_PATH, PPS_UICC_PATH, PPS_DATA_PATH,
                                   "/pps/services/phone/public/status"};

    for (int i = 0; i < FD_COUNT; i++)
    {
        pfds[i].fd = open(paths[i], O_RDONLY | O_NONBLOCK);
        pfds[i].events = POLLIN | POLLPRI;

        if (pfds[i].fd < 0)
        {
            /* Only the automatic dial-mode check needs the phone status: run without it. */
            if (i == FD_PHONE)
            {
                log_msg("[BBE] PPS_OPEN_FAILED: %s", paths[i]);
                continue;
            }
            LOG_VERBOSE("Failed to open PPS: %s", paths[i]);
            for (int j = 0; j < i; j++)
                close(pfds[j].fd);
            return NULL;
        }
    }

    g_current_state = get_current_system_state();
    int was_ready = is_state_ready(&g_current_state) && ims_connected_pps();

    /* No drop will be recorded for this outage, so time recovery from here. */
    if (!was_ready)
        clock_gettime(CLOCK_MONOTONIC, &g_stats.drop_time);

    LOG_VERBOSE("Detailed PPS monitor started. Initial state -> LTE:%d, SIM:%d, Data:%d",
                g_current_state.lte_ok, g_current_state.sim_ok, g_current_state.data_ok);

    char force_path[512];
    snprintf(force_path, sizeof(force_path), "%s/force-ims-voice", g_data_path);
    g_force_ims_voice = access(force_path, F_OK) == 0;

    while (g_lte_monitor_running)
    {
        if (g_reset_stats_requested)
        {
            g_reset_stats_requested = 0;
            stats_reset();
            stats_export_to_file();
        }

        /* Right after a reboot the phone service may not have published its status yet
           (thomas-6: PPS_OPEN_FAILED at 14:02:49): keep trying, it costs one open(). */
        if (pfds[FD_PHONE].fd < 0)
        {
            pfds[FD_PHONE].fd = open(paths[FD_PHONE], O_RDONLY | O_NONBLOCK);
            if (pfds[FD_PHONE].fd >= 0)
                log_msg("[BBE] PPS_OPENED: %s", paths[FD_PHONE]);
        }

        add_missing_ims_routes();
        volte_settings_poll();
        if (g_restart_requested)
            break;

        /* The list is fetched once at start; if that failed (data off, no APN yet) nothing else
           asks again, and without it the SIM gets no profile and IMS never registers.
           ponytail: the download blocks this thread for up to ~20 s offline, every 5 minutes;
           move it to its own thread if that matters. */
        static time_t last_db_try = 0;
        time_t now = time(NULL);
        if (!carrier_db_present() && is_state_ready(&g_current_state) &&
            (last_db_try == 0 || now - last_db_try >= CARRIER_DB_RETRY_SEC))
        {
            last_db_try = now;
            if (refresh_carrier_db(0) > 0)
            {
                log_msg("[BBE] CARRIER_DB_RETRY: downloaded, restarting");
                request_daemon_restart();
                break;
            }
        }

        sync_voice_flag(1);
        check_ims_health(&was_ready);

        /* PPS changes wake this at once; the timeout only drives the periodic checks.
           Poll each second only while waiting for IMS, so STATS_OK times recovery. The
           timeout also bounds how long stop_lte_monitor() waits. */
        int timeout_ms = (!was_ready && is_state_ready(&g_current_state) && !volte_settings_voice_off())
                             ? 1000 : 5000;
        int poll_res = poll(pfds, FD_COUNT, timeout_ms);

        if (poll_res < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (poll_res == 0)
            continue;

        for (int i = 0; i < FD_COUNT; i++)
        {
            if (pfds[i].fd >= 0 && (pfds[i].revents & (POLLIN | POLLPRI)))
            {
                char buf[1024];
                ssize_t bytes = read(pfds[i].fd, buf, sizeof(buf) - 1);
                (void)bytes;
                if (i == FD_PHONE)
                    volte_settings_phone_changed();
            }
        }

        system_state_t new_state = get_current_system_state();

        /* Another carrier's SIM needs the whole startup path: its XML, the reload and a new ims_rrm. */
        if (new_state.sim_ok && sim_carrier_changed())
        {
            request_daemon_restart();
            break;
        }

        if (new_state.lte_ok != g_current_state.lte_ok ||
            new_state.sim_ok != g_current_state.sim_ok ||
            new_state.data_ok != g_current_state.data_ok)
        {
            log_state_transition(g_current_state, new_state);

            int is_ready = is_state_ready(&new_state);

            if (was_ready && !is_ready)
            {
                stats_on_drop();
                /* A network drop leaves ims_rrm running: it registers again by itself, and
                   check_ims_health() restarts it if not. Stopping it drops its IPsec keys
                   first, so its FIN leaves in cleartext and goes unanswered. That socket
                   then blocks the next REGISTER from the same address with "Address already
                   in use" for up to about 2 minutes. */
                if (!new_state.sim_ok || !new_state.data_ok)
                    stop_ims();
                was_ready = 0;
            }
            else if (!was_ready && is_ready && find_pid_by_name("ims_rrm", 0) <= 0 &&
                     !volte_settings_voice_off())
            {
                log_msg("[BBE] RESTORED: STARTING_RECONNECT");

                if (reconnect_ims_with_backoff())
                {
                    stats_on_success();
                    was_ready = 1;
                }
                else
                {
                    stats_on_failure();
                    was_ready = 0;
                }
            }

            g_current_state = new_state;
        }
    }

    sync_voice_flag(0);

    for (int i = 0; i < FD_COUNT; i++)
    {
        if (pfds[i].fd >= 0)
            close(pfds[i].fd);
    }

    return NULL;
}

void start_lte_monitor(void)
{
    if (!g_lte_monitor_running)
    {
        g_lte_monitor_running = 1;
        start_worker(&g_lte_thread, detailed_pps_monitor_thread);
    }
}

static void stop_lte_monitor(void)
{
    if (g_lte_monitor_running)
    {
        g_lte_monitor_running = 0;
        pthread_join(g_lte_thread, NULL);
    }
}

/* Main thread only: joins the worker threads. */
void exit_if_stopped(void)
{
    if (g_stop_requested)
    {
        log_stop_sender();
        stop_lte_monitor();
        stop_berry();
        exit(0);
    }
}

/* with_berry: the berry thread ending (a second daemon holds its lock) also ends the daemon.
   The VoLTE-off start runs without it. */
int run_until_stopped(int with_berry)
{
    /* A stop signal interrupts sleep(), since only this thread takes it (start_worker).
       ponytail: a signal landing between the check and sleep() waits out the 60 s;
       sigsuspend() with the signal blocked closes that if it ever matters. */
    while ((g_berry_running || !with_berry) && !g_stop_requested && !g_restart_requested)
    {
        sleep(60);
    }

    if (g_stop_requested)
        log_stop_sender();
    stop_lte_monitor();
    stop_berry();

    if (g_restart_requested && !g_stop_requested)
    {
        /* Same process and pid: the startup path then waits for the new SIM or list, builds and
           reloads its XML and restarts ims_rrm, as a reboot would. */
        char self[600];
        snprintf(self, sizeof(self), "%s/ims_manager", g_init_path);
        char *args[] = {self, "-a", "enable", "-r", NULL};
        log_msg("[BBE] RESTARTING: startup path again (new SIM, new carrier list or settings app)");
        execv(self, args);
        log_msg("[BBE] RESTART_FAILED: %d", errno);
    }
    return 0;
}
