#ifndef VOLTE_SETTINGS_H
#define VOLTE_SETTINGS_H

/* The root side of the settings app. The app only writes a request file in the shared
 * documents folder and reads a status file; this module does the privileged work: hold the
 * dial mode for the current carrier, learn the operator domain from ims_rrm's sip log, write
 * the DCA config and restart ims_rrm when the mode changes, and build the debug zip.
 *
 * The sip log is read only on a new request (app open, Refresh, Apply), never periodically. */

void volte_settings_init(void);
void volte_settings_poll(void);

/* For write_dca_configs(): "tel", "sip-imsi" or "sip-learned", and the learned domain. */
const char *volte_settings_mode(void);
const char *volte_settings_domain(void);

#endif /* VOLTE_SETTINGS_H */
