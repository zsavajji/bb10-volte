#ifndef CARRIER_APPLY_H
#define CARRIER_APPLY_H

int apply_carrier_xml(void);
void fix_published_carrier_xml(void);
void link_carrier_db(void);
int refresh_carrier_db(int force);
int carrier_db_present(void);

#endif /* CARRIER_APPLY_H */
