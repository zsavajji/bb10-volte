#ifndef IMS_RECONNECT_H
#define IMS_RECONNECT_H

int wait_connected(void);
int run_berry_check(void);
int reconnect_ims_with_backoff(void);

/* A user-requested restart (the settings app's Apply) may use the radio-cycle recovery
   even if an earlier failure this run already spent it. */
void ims_reconnect_reset_radio_budget(void);
void check_ims_health(int *was_ready);

#endif /* IMS_RECONNECT_H */
