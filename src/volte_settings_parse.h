#ifndef VOLTE_SETTINGS_PARSE_H
#define VOLTE_SETTINGS_PARSE_H

#include <stddef.h>

/* Pure parsers for the settings UI's two files and for ims_rrm's sip log. No QNX or daemon
 * dependencies, so tests/test_volte_settings.c can feed them text on the host. */

/* "mode=sip-learned\nrefresh=1\nrequest=1234\n" */
int volte_settings_parse_request(const char *text, char *mode, size_t mode_len,
                                 char *request, size_t request_len, int *refresh, int *debug);

/* The operator's own domain from the registered IMPU: P-Associated-URI, <uri>, or the
 * From:/To: of the reg-event NOTIFY. Domains ending in .3gppnetwork.org and
 * anonymous.invalid are skipped; out stays empty when there is none. */
int volte_settings_parse_learned_domain(const char *slog, char *out, size_t out_len);

/* Last outgoing call: "connected", "503 (1:223)", "failed (code)", "in progress", "no call seen",
 * and the domain of its R-URI ("tel:" for a tel URI). Numbers are never copied out. */
void volte_settings_parse_last_call(const char *slog, char *result, size_t result_len,
                                    char *uri_domain, size_t uri_len);

#endif /* VOLTE_SETTINGS_PARSE_H */
