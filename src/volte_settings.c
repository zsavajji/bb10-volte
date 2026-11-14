#include "volte_settings.h"
#include "volte_settings_parse.h"
#include "ims_control.h"
#include "ims_carrier_config.h"
#include "carrier_lookup.h"
#include "ims_reconnect.h"
#include "system_state.h"
#include "sys_utils.h"
#include "zip_writer.h"
#include "ims_patch.h"
#include "log_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DOCS_DIR "/accounts/1000/shared/documents"
#define REQUEST_PATH DOCS_DIR "/volte-domain.request"
#define STATUS_PATH DOCS_DIR "/volte-domain.status"
#define PHONE_STATUS "/pps/services/phone/public/status"
#define DEBUG_OUT "/var/tmp/volte-debug-bundle"
#define LOG_MAX (768u * 1024u)

static char g_mode[16] = "tel";
static char g_learned[128];
static char g_imsi[64];
static volte_call_t g_call = {"no call seen"};
static char g_last_request[32];
static char g_debug_state[16] = "idle";
static char g_debug_file[64];
static char g_message[160];
static char g_selection[8] = "auto";     /* "auto" or "manual" */
static char g_failed[4];                 /* letters of modes refused while automatic */
static char g_auto_state[12] = "testing"; /* "testing", "confirmed", "off" */
static char g_auto_note[160];            /* last automatic event, memory only */
static unsigned long g_last_call_id;     /* last outgoing call already evaluated */
static volatile int g_phone_changed;
static int g_carrier_id = -1;
static int g_inited = 0;

/* --- per-carrier data files (dial-modes / learned-domains) --- */

static int read_kv(const char *path, int id, char *out, size_t out_len)
{
    char buf[16384];
    char *line, *save = NULL;
    int found = 0;

    if (read_file(path, buf, sizeof(buf)) < 0)
        return -1;

    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char *sp;
        size_t n;

        if (atoi(line) != id)
            continue;
        sp = strpbrk(line, " \t");
        if (!sp)
            continue;
        while (*sp == ' ' || *sp == '\t')
            sp++;
        n = strcspn(sp, " \t\r\n");
        if (n == 0 || n >= out_len)
            continue;
        memcpy(out, sp, n);
        out[n] = '\0';
        found = 1;
    }
    return found ? 0 : -1;
}

static int write_kv(const char *path, int id, const char *value)
{
    char in[16384];
    char out[16384];
    size_t used = 0;

    in[0] = '\0';
    read_file(path, in, sizeof(in));

    char *line, *save = NULL;
    for (line = strtok_r(in, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        if (atoi(line) == id)
            continue;
        int n = snprintf(out + used, sizeof(out) - used, "%s\n", line);
        if (n < 0 || (size_t)n >= sizeof(out) - used)
            return -1;
        used += (size_t)n;
    }
    int n = snprintf(out + used, sizeof(out) - used, "%d %s\n", id, value);
    if (n < 0 || (size_t)n >= sizeof(out) - used)
        return -1;
    used += (size_t)n;
    return write_file(path, out, 0644);
}

/* --- automatic selection state, <carrier_id> <token> in dial-auto ---
   Tokens: manual | ok (confirmed) |
   off (VoLTE off, once "done") | auto | auto-<failed letters> (testing). */

static void save_auto(void)
{
    char path[512], tok[16];

    if (g_carrier_id <= 0)
        return;
    if (strcmp(g_selection, "manual") == 0)
        snprintf(tok, sizeof(tok), "manual");
    else if (strcmp(g_auto_state, "confirmed") == 0)
        snprintf(tok, sizeof(tok), "ok");
    else if (strcmp(g_auto_state, "off") == 0)
        snprintf(tok, sizeof(tok), "off");
    else if (g_failed[0])
        snprintf(tok, sizeof(tok), "auto-%s", g_failed);
    else
        snprintf(tok, sizeof(tok), "auto");
    snprintf(path, sizeof(path), "%s/dial-auto", g_data_path);
    write_kv(path, g_carrier_id, tok);
}

/* A missing or unknown token means automatic, nothing tried yet. */
static void load_auto(const char *tok)
{
    snprintf(g_selection, sizeof(g_selection), "auto");
    snprintf(g_auto_state, sizeof(g_auto_state), "testing");
    g_failed[0] = '\0';

    if (strcmp(tok, "manual") == 0)
        snprintf(g_selection, sizeof(g_selection), "manual");
    else if (strcmp(tok, "ok") == 0)
        snprintf(g_auto_state, sizeof(g_auto_state), "confirmed");
    else if (strcmp(tok, "off") == 0 || strcmp(tok, "done") == 0)
        snprintf(g_auto_state, sizeof(g_auto_state), "off");
    else if (strncmp(tok, "auto-", 5) == 0)
        snprintf(g_failed, sizeof(g_failed), "%s", tok + 5);
}

/* --- status file --- */

static void write_status(const char *state, const char *message)
{
    char buf[2048];

    if (message)
        snprintf(g_message, sizeof(g_message), "%s", message);

    snprintf(buf, sizeof(buf),
             "request=%s\n"
             "carrier=%d\n"
             "mode=%s\n"
             "state=%s\n"
             "imsi_domain=%s\n"
             "learned_domain=%s\n"
             "last_result=%s\n"
             "last_uri_domain=%s\n"
             "debug_state=%s\n"
             "debug_file=%s\n"
             "message=%s\n"
             "selection=%s\n"
             "auto_state=%s\n"
             "auto_note=%s\n"
             "updated=%ld\n",
             g_last_request, g_carrier_id, g_mode, state, g_imsi, g_learned,
             g_call.result, g_call.uri, g_debug_state, g_debug_file, g_message,
             g_selection, g_auto_state, g_auto_note, (long)time(NULL));
    write_file(STATUS_PATH, buf, 0666);
}

static int call_active(void)
{
    char buf[2048];
    if (read_file(PHONE_STATUS, buf, sizeof(buf)) < 0)
        return 0;
    return strstr(buf, "phone_active:b:true") != NULL;
}

/* --- sip log, read only when the app asks --- */

static void scan_log(void)
{
    char *buf = malloc(LOG_MAX);
    const char *cmd;
    int n;

    if (!buf)
        return;

    cmd = access("/usr/bin/slog2info", X_OK) == 0 ? "/usr/bin/slog2info -b ims_rrm"
                                                  : "slog2info -b ims_rrm";
    n = run_cmd_capture(cmd, buf, LOG_MAX);
    if (n > 0)
    {
        char domain[128];

        if (volte_settings_parse_learned_domain(buf, domain, sizeof(domain)))
        {
            if (strcmp(domain, g_learned) != 0)
            {
                snprintf(g_learned, sizeof(g_learned), "%s", domain);
                if (g_carrier_id > 0)
                {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/learned-domains", g_data_path);
                    write_kv(path, g_carrier_id, domain);
                }
            }
        }
        volte_settings_parse_last_call(buf, &g_call);
    }
    free(buf);
}

/* --- debug bundle --- */

static void run_debug(void)
{
    char zip[512];
    char name[64];
    char cmd[1024];
    char date[16];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    int rc;

    if (tm)
        strftime(date, sizeof(date), "%Y%m%d", tm);
    else
        snprintf(date, sizeof(date), "00000000");
    snprintf(name, sizeof(name), "volte-debug-%s.zip", date);
    snprintf(zip, sizeof(zip), "%s/%s", DOCS_DIR, name);

    snprintf(g_debug_state, sizeof(g_debug_state), "running");
    g_debug_file[0] = '\0';
    write_status("applying", "Collecting debug log...");

    char *rm_argv[] = {"rm", "-rf", DEBUG_OUT, NULL};
    run_cmd_spawn(rm_argv, NULL);
    mkdir_p(DEBUG_OUT, 0755);

    snprintf(cmd, sizeof(cmd),
             "PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin; export PATH; "
             "cat \"%s/volte-debug.sh\" | sh -s \"%s\"", g_data_path, DEBUG_OUT);
    char *sh_argv[] = {"/bin/sh", "-c", cmd, NULL};
    rc = run_cmd_spawn(sh_argv, NULL);
    if (rc == 0)
        rc = zip_directory(DEBUG_OUT, zip);

    if (rc == 0)
    {
        snprintf(g_debug_state, sizeof(g_debug_state), "done");
        snprintf(g_debug_file, sizeof(g_debug_file), "%s", name);
        char msg[160];
        snprintf(msg, sizeof(msg), "Debug log saved: %s", name);
        write_status("idle", msg);
        run_cmd_spawn(rm_argv, NULL);
    }
    else
    {
        snprintf(g_debug_state, sizeof(g_debug_state), "error");
        write_status("error", "Debug log failed");
    }
}

/* --- public --- */

void volte_settings_init(void)
{
    char tok[16];

    g_carrier_id = ims_get_carrier_id(g_mcc, g_mnc, g_gid1);
    snprintf(g_imsi, sizeof(g_imsi), "ims.mnc%03d.mcc%s.3gppnetwork.org", atoi(g_mnc), g_mcc);

    g_mode[0] = '\0';
    g_learned[0] = '\0';
    if (g_carrier_id > 0)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/dial-modes", g_data_path);
        read_kv(path, g_carrier_id, g_mode, sizeof(g_mode));
        snprintf(path, sizeof(path), "%s/learned-domains", g_data_path);
        read_kv(path, g_carrier_id, g_learned, sizeof(g_learned));
        snprintf(path, sizeof(path), "%s/dial-auto", g_data_path);
        tok[0] = '\0';
        read_kv(path, g_carrier_id, tok, sizeof(tok));
        load_auto(tok);
    }

    if (g_mode[0] == '\0')
    {
        char domain[128];
        if (g_carrier_id > 0 &&
            ims_lookup_home_domain(g_data_path, g_carrier_id, domain, sizeof(domain)) == 0)
        {
            snprintf(g_mode, sizeof(g_mode), "sip-learned");
            snprintf(g_learned, sizeof(g_learned), "%s", domain);
        }
        else
        {
            snprintf(g_mode, sizeof(g_mode), "tel");
        }
    }

    /* The IMSI-derived domain is always known from the SIM: use it as the initial learned
       domain so the toggle is available on carriers whose IMPU lives in 3gppnetwork.org.
       A scan that finds the operator's own domain replaces it. */
    if (g_learned[0] == '\0' && g_imsi[0] != '\0')
        snprintf(g_learned, sizeof(g_learned), "%s", g_imsi);

    /* sip-learned without a domain would dial an empty R-URI domain: keep tel until one is seen.
       With the IMSI domain it dials the same as sip-imsi, only through the patched setter. */
    if (strcmp(g_mode, "sip-learned") == 0 && g_learned[0] == '\0')
        snprintf(g_mode, sizeof(g_mode), "tel");
    else if (strcmp(g_mode, "sip-learned") == 0 && strcmp(g_learned, g_imsi) == 0)
        snprintf(g_mode, sizeof(g_mode), "sip-imsi");

    g_ims_patch_home_domain = strcmp(g_mode, "sip-learned") == 0;

    /* A request left from before this start was already handled (or is stale, like a debug
       run): only a newer one, written while the daemon runs, counts. */
    {
        char text[1024], m[16], req[32] = {0};
        int r, d;
        if (read_file(REQUEST_PATH, text, sizeof(text)) >= 0 &&
            volte_settings_parse_request(text, m, sizeof(m), req, sizeof(req), &r, &d))
            snprintf(g_last_request, sizeof(g_last_request), "%s", req);
    }

    /* Calls from before this start are never evaluated: remember the last one in the log. */
    scan_log();
    g_last_call_id = g_call.id;

    mkdir_p(DOCS_DIR, 0777);
    g_inited = 1;
    write_status("idle", "");
}

/* Stores the mode and pushes the DCA config. The caller decides whether IMS restarts. */
static int store_mode(const char *mode)
{
    snprintf(g_mode, sizeof(g_mode), "%s", mode);
    g_ims_patch_home_domain = strcmp(g_mode, "sip-learned") == 0;
    if (g_carrier_id > 0)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/dial-modes", g_data_path);
        write_kv(path, g_carrier_id, g_mode);
    }

    write_dca_configs();
    return push_dca_configs();
}

/* Stores the mode and restarts IMS through the daemon's own path. */
static void apply_mode(const char *mode, const char *done_message)
{
    if (store_mode(mode) != 0)
    {
        write_status("error", "Could not load the new config (dca failed)");
        return;
    }
    write_status("applying", "Restarting IMS...");

    /* The daemon's own path: it stops ims_rrm, clears stale routes, restarts, and runs
       reset_radio_if_stalled() when a restart does not register. A raw stop/start left
       the phone needing airplane mode, because restarting ims_rrm alone does not clear
       the same-IP/port collision. A user Apply gets a fresh reset budget. */
    ims_reconnect_reset_radio_budget();
    int ok = reconnect_ims_with_backoff();
    write_status(ok ? "idle" : "error", ok ? done_message : "IMS did not connect");
}

/* After a call ends: in automatic mode, a refused address moves on to the next dial format
   and a connected call confirms the current one. */
static void auto_check_last_call(void)
{
    const char *used, *next;
    char letter, old[16];
    int learned_differs;

    scan_log();
    if (g_call.id == 0 || g_call.id == g_last_call_id || g_call.in_progress)
        return;
    g_last_call_id = g_call.id;

    if (strcmp(g_selection, "auto") != 0)
    {
        write_status("idle", NULL);
        return;
    }

    /* A call dialled with another mode than the current one predates a switch. */
    used = volte_call_mode(&g_call, g_imsi);
    if (!used || strcmp(used, g_mode) != 0)
        return;

    if (g_call.connected)
    {
        if (strcmp(g_auto_state, "confirmed") != 0)
        {
            snprintf(g_auto_state, sizeof(g_auto_state), "confirmed");
            save_auto();
            snprintf(g_auto_note, sizeof(g_auto_note), "Calls connect with %s", g_mode);
        }
        write_status("idle", NULL);
        return;
    }
    if (strcmp(g_auto_state, "testing") != 0 || !volte_call_refused(&g_call))
    {
        write_status("idle", NULL);
        return;
    }

    letter = volte_mode_letter(g_mode);
    if (letter && !strchr(g_failed, letter) && strlen(g_failed) < sizeof(g_failed) - 1)
    {
        size_t n = strlen(g_failed);
        g_failed[n] = letter;
        g_failed[n + 1] = '\0';
    }
    learned_differs = g_learned[0] && strcmp(g_learned, g_imsi) != 0;
    next = volte_auto_next(g_failed, learned_differs);

    if (!next)
    {
        /* The network takes no VoLTE call from this phone. Leave ims_rrm stopped so calls use
           3G or 2G, and keep the mode testing started with (the default, or a seeded one) for when
           VoLTE is turned back on. Nothing reconnects while off (voice_off() gates it). */
        const char *first = volte_mode_from_letter(g_failed[0]);

        snprintf(g_auto_state, sizeof(g_auto_state), "off");
        snprintf(g_auto_note, sizeof(g_auto_note),
                 "Network refused every dial format; VoLTE is off on this network and calls use 3G or 2G");
        save_auto();
        log_msg("[BBE] VOLTE_OFF: %s", g_auto_note);
        if (first && strcmp(first, g_mode) != 0 && store_mode(first) != 0)
            log_msg("[BBE] DCA_FAILED: VoLTE off");
        stop_ims();
        write_status("idle", NULL);
        return;
    }

    snprintf(old, sizeof(old), "%s", g_mode);
    snprintf(g_auto_note, sizeof(g_auto_note),
             "Network refused a call with %d using %s; switched to %s", g_call.code, old, next);
    save_auto();
    log_msg("[BBE] DIAL_AUTO: %s", g_auto_note);
    apply_mode(next, "Switched automatically");
}

void volte_settings_poll(void)
{
    char text[1024];
    char mode[16] = {0};
    char request[32] = {0};
    int refresh = 0, debug = 0;

    if (!g_inited)
        return;

    /* ims_rrm logs the dialog end slightly after the phone state changes. */
    if (g_phone_changed && !call_active())
    {
        g_phone_changed = 0;
        sleep(1);
        auto_check_last_call();
    }

    if (read_file(REQUEST_PATH, text, sizeof(text)) < 0)
        return;
    if (!volte_settings_parse_request(text, mode, sizeof(mode), request, sizeof(request),
                                      &refresh, &debug))
        return;
    if (request[0] && strcmp(request, g_last_request) == 0)
        return;
    snprintf(g_last_request, sizeof(g_last_request), "%s", request);

    (void)refresh;
    scan_log();

    /* Apply always reapplies, even the current mode: it is the user's way to bring the live
       config back in line with the stored mode. */
    if (mode[0])
    {
        if (strcmp(mode, "auto") == 0)
        {
            int was_off = volte_settings_voice_off();

            snprintf(g_selection, sizeof(g_selection), "auto");
            g_failed[0] = '\0';
            snprintf(g_auto_state, sizeof(g_auto_state), "testing");
            save_auto();
            if (!was_off)
            {
                write_status("idle", "Automatic selection on");
                return;
            }

            /* IMS was left stopped: bring it back, with a fresh reset budget like Apply. */
            g_auto_note[0] = '\0';
            write_status("applying", "Turning VoLTE back on...");
            ims_reconnect_reset_radio_budget();
            int ok = reconnect_ims_with_backoff();
            write_status(ok ? "idle" : "error", ok ? "VoLTE is back on" : "IMS did not connect");
            return;
        }
        if (strcmp(mode, "tel") != 0 && strcmp(mode, "sip-imsi") != 0 &&
            strcmp(mode, "sip-learned") != 0)
        {
            write_status("error", "Unknown dial mode");
            return;
        }
        if (strcmp(mode, "sip-learned") == 0 && g_learned[0] == '\0')
        {
            write_status("error", "No learned domain seen on this network");
            return;
        }
        if (strcmp(mode, "sip-learned") == 0 && strcmp(g_learned, g_imsi) == 0)
        {
            write_status("error", "The learned domain is the IMSI domain: use sip-imsi");
            return;
        }
        if (call_active())
        {
            write_status("error", "A call is in progress; try again");
            return;
        }

        snprintf(g_selection, sizeof(g_selection), "manual");
        snprintf(g_auto_state, sizeof(g_auto_state), "testing");
        save_auto();
        apply_mode(mode, "Applied");
        return;
    }

    if (debug)
    {
        run_debug();
        return;
    }

    write_status("idle", "");
}

void volte_settings_phone_changed(void)
{
    g_phone_changed = 1;
}

const char *volte_settings_mode(void)
{
    return g_mode;
}

const char *volte_settings_domain(void)
{
    return strcmp(g_mode, "sip-learned") == 0 ? g_learned : "";
}

int volte_settings_voice_off(void)
{
    return strcmp(g_selection, "auto") == 0 && strcmp(g_auto_state, "off") == 0;
}
