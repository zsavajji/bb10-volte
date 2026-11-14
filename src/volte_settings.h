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
/* For loops that block the monitor: VoLTE off and the debug report only, the rest is "busy". */
void volte_settings_poll_busy(void);

/* The phone status PPS changed: after the next call ends, check how its dial format fared. */
void volte_settings_phone_changed(void);

/* For write_dca_configs(): "tel", "sip-imsi" or "sip-learned", and the learned domain. */
const char *volte_settings_mode(void);
const char *volte_settings_domain(void);

/* 1 while VoLTE is off, either turned off by the user or because automatic selection found every
   dial format refused: IMS stays stopped, calls use 3G. */
int volte_settings_voice_off(void);

/* "auto", "manual" or "off" (turned off by the user). */
const char *volte_settings_selection(void);

#endif /* VOLTE_SETTINGS_H */
