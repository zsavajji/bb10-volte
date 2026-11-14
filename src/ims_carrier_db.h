#ifndef IMS_CARRIER_DB_H
#define IMS_CARRIER_DB_H

#include <stddef.h>

#include "carrier_fetch.h"

#define CARRIER_DB_NAME    "carrierIds.xml"
#define CARRIER_STOCK_NAME "carrierIds.stock.xml"
#define CARRIER_APNS_NAME  "carrier-apns.tsv"

/* Bump CARRIER_LIST_GEN when carrier_rows.c or carrier_apns.c would write a different list or APN
   table from the same download; the next start then downloads again instead of waiting out the
   30 days. */
#define CARRIER_GEN_NAME   "carrier-list.gen"
#define CARRIER_LIST_GEN   "1"

/* New carriers get this plus their AOSP canonical_id, so IDs survive a refresh. */
#define AOSP_ID_BASE 30000

/* Copies the stock carrierIds.xml, which the link hides once it exists. */
int ims_carrier_db_save_stock(const char *src, const char *dst);

/**
 * Appends the AOSP carriers the stock list lacks, leaving stock rows byte-identical, and writes
 * the Android APNs of every carrier that has usable ones, stock carriers included.
 *
 * @param textpb    carrier_list.textpb contents, NUL terminated
 * @param apns_xml  apns-full-conf.xml contents
 * @return 0 on success, -1 on error; on error the output files are untouched
 */
int ims_carrier_db_generate(const char *textpb, const char *apns_xml, size_t apns_len,
                            const char *stock_path, const char *out_db, const char *out_apns);

#endif /* IMS_CARRIER_DB_H */
