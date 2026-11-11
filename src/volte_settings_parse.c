#include "volte_settings_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '"')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '"'))
        *--end = '\0';
    return s;
}

int volte_settings_parse_request(const char *text, char *mode, size_t mode_len,
                                 char *request, size_t request_len, int *refresh, int *debug)
{
    char *copy, *line, *save = NULL;
    int have_request = 0;

    if (mode_len)
        mode[0] = '\0';
    if (request_len)
        request[0] = '\0';
    if (refresh)
        *refresh = 0;
    if (debug)
        *debug = 0;
    if (!text)
        return 0;

    copy = strdup(text);
    if (!copy)
        return 0;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char *eq = strchr(line, '=');
        char *key, *value;

        if (!eq)
            continue;
        *eq = '\0';
        key = trim(line);
        value = trim(eq + 1);

        if (strcmp(key, "mode") == 0)
            snprintf(mode, mode_len, "%s", value);
        else if (strcmp(key, "request") == 0)
        {
            snprintf(request, request_len, "%s", value);
            have_request = 1;
        }
        else if (strcmp(key, "refresh") == 0 && refresh)
            *refresh = atoi(value) != 0;
        else if (strcmp(key, "debug") == 0 && debug)
            *debug = atoi(value) != 0;
    }

    free(copy);
    return have_request;
}

/* First URI in the line. *is_3gpp says whether its domain is the IMSI-derived one. */
static int domain_from_line(const char *line, char *out, size_t out_len, int *is_3gpp)
{
    const char *p = strstr(line, "sip:");

    while (p)
    {
        const char *at = strchr(p, '@');
        if (at)
        {
            const char *d = at + 1;
            size_t n = strcspn(d, ">;\"' \t\r\n");
            if (n > 3 && n < out_len)
            {
                if (n == 17 && strncmp(d, "anonymous.invalid", 17) == 0)
                    ;
                else
                {
                    memcpy(out, d, n);
                    out[n] = '\0';
                    *is_3gpp = n > 16 && strncmp(d + n - 16, ".3gppnetwork.org", 16) == 0;
                    return 1;
                }
            }
        }
        p = strstr(p + 4, "sip:");
    }
    return 0;
}

int volte_settings_parse_learned_domain(const char *slog, char *out, size_t out_len)
{
    char *copy, *line, *save = NULL;
    char fallback[256] = "";
    int found = 0;

    if (!slog || !out || out_len == 0)
        return 0;
    out[0] = '\0';

    copy = strdup(slog);
    if (!copy)
        return 0;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char cand[256];
        int is_3gpp = 0;

        if (!(strstr(line, "P-Associated-URI") || strstr(line, "<uri") ||
              strstr(line, "From: <sip:") || strstr(line, "To: <sip:")))
            continue;
        if (!domain_from_line(line, cand, sizeof(cand), &is_3gpp))
            continue;

        if (!is_3gpp)
        {
            /* The operator's own domain wins over the IMSI-derived one. */
            snprintf(out, out_len, "%s", cand);
            found = 1;
            break;
        }
        if (fallback[0] == '\0')
            snprintf(fallback, sizeof(fallback), "%s", cand);
    }

    /* No operator domain in the log: the IMPU uses the IMSI domain, which is what the
       pre-2.33 sip: form sent. Still a valid "learned" domain for the toggle. */
    if (!found && fallback[0])
    {
        snprintf(out, out_len, "%s", fallback);
        found = 1;
    }

    free(copy);
    return found;
}

static void uri_domain(const char *line, char *out, size_t out_len)
{
    const char *r = strstr(line, "R-URI: ");
    if (!r)
        return;
    r += 7;

    if (strncmp(r, "tel:", 4) == 0 || strncmp(r, "Tel:", 4) == 0)
    {
        snprintf(out, out_len, "tel:");
        return;
    }

    int is_3gpp = 0;
    domain_from_line(r, out, out_len, &is_3gpp);
}

void volte_settings_parse_last_call(const char *slog, char *result, size_t result_len,
                                    char *uri, size_t uri_len)
{
    char *copy, *line, *save = NULL;
    int have_call = 0, connected = 0, in_progress = 0, code = 0;
    int detail_1223 = 0;
    int reg_subscribe = 0;

    if (result_len)
        result[0] = '\0';
    if (uri_len)
        uri[0] = '\0';
    if (!slog)
        return;

    copy = strdup(slog);
    if (!copy)
        return;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        /* After each registration ims_rrm subscribes to its own reg event, and that SUBSCRIBE
           logs the same createOutDialog line as a call: skip the one that follows it. */
        if (strstr(line, "SubscribeToRegInfo("))
        {
            reg_subscribe = 1;
        }
        else if (strstr(line, "createOutDialog(): R-URI:") && reg_subscribe)
        {
            reg_subscribe = 0;
        }
        else if (strstr(line, "createOutDialog(): R-URI:"))
        {
            have_call = 1;
            connected = 0;
            in_progress = 1;
            code = 0;
            uri_domain(line, uri, uri_len);
        }
        else if (strstr(line, "Service Unavailable (1:223)"))
        {
            detail_1223 = 1;
        }
        else if (strstr(line, "onDialogStateChange") && strstr(line, "=> 4"))
        {
            connected = 1;
            in_progress = 0;
        }
        else if (strstr(line, "onDialogEnded"))
        {
            const char *c = strstr(line, "code ");
            if (c)
                code = atoi(c + 5);
            in_progress = 0;
        }
    }

    if (!have_call)
        snprintf(result, result_len, "no call seen");
    else if (in_progress)
        snprintf(result, result_len, "in progress");
    else if (connected)
        snprintf(result, result_len, "connected");
    else if (code == 503 && detail_1223)
        snprintf(result, result_len, "503 (1:223)");
    else if (code)
        snprintf(result, result_len, "failed (%d)", code);
    else
        snprintf(result, result_len, "unknown");

    free(copy);
}
