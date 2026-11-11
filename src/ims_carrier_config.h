#ifndef IMS_CARRIER_CONFIG_H
#define IMS_CARRIER_CONFIG_H

#include <stddef.h>

#ifndef CARRIER_IDS_PATH
#define CARRIER_IDS_PATH "/base/etc/carrier/ids/carrierIds.xml"
#endif
#define IMS_CONFIG_DIR   "/var/ims/carrier"

#ifdef __cplusplus
extern "C" {
#endif

/* ims_get_carrier_id(mcc, mnc, gid1) lives in carrier_lookup.h: it also matches the SIM's
   GID, the way carrier_mgr does. This file keeps only the plain, GID-less lookup. */

/**
 * Same as ims_find_carrier_id(), reading the given carrierIds.xml instead of CARRIER_IDS_PATH.
 * Ignores GIDs and IMSI/ICCID ranges, so prefer ims_get_carrier_id() when gid1 is known.
 */
int ims_get_carrier_id_from(const char *ids_path, const char *mcc, const char *mnc);

/**
 * Looks up the carrier's home domain in data_dir/home-domains. That file holds one
 * "<carrier_id> <domain>" per line; blank lines and lines starting with # are ignored.
 * The domain goes into the ims DCA config as Home_network_domain_name, which then makes
 * the dial code build sip:<number>@<domain>;user=phone instead of tel:<number>.
 *
 * @return 0 and the domain in out (NUL-terminated), or -1 if the carrier has no entry
 */
int ims_lookup_home_domain(const char *data_dir, int carrier_id, char *out, size_t max_len);

/**
 * Finds the carrier profile in data_dir. Prefers carrier-<carrier_id>-volte.xml; otherwise
 * falls back to the first carrier-*.xml found (single-profile packages).
 *
 * @return 0 and the full path in out_path, or -1 if data_dir has no carrier-*.xml
 */
int ims_find_carrier_profile(const char *data_dir, int carrier_id, char *out_path, size_t max_len);

/**
 * Fills out with the IR.92 DCA object to write for carrier_id: the contents of
 * data_dir/ir92-<carrier_id> when that file exists and starts with "@ir92", else default_cfg.
 *
 * @return 0 on success, -1 if the chosen config does not fit in max_len
 */
int ims_load_ir92_config(const char *data_dir, int carrier_id, const char *default_cfg,
                         char *out, size_t max_len);

/**
 * Checks that every attribute line of expected (after its "@ir92" header) appears as a whole
 * line in actual, in any order.
 *
 * @return 1 if applied, 0 otherwise
 */
int ims_ir92_applied(const char *actual, const char *expected);

/**
 * Resolves the path to the imsconfig_<carrier_id>.xml file for the given MCC/MNC.
 *
 * @param mcc Mobile Country Code
 * @param mnc Mobile Network Code
 * @param out_path Output buffer to store the generated path
 * @param max_len Maximum size of the out_path buffer
 * @return 0 on success, -1 on error or if the file does not exist
 */
int ims_resolve_config_path(const char *mcc, const char *mnc, const char *gid1, char *out_path, size_t max_len);

/**
 * Sets the value of the <AllowVoLteProvisioning> node to "enabled" in the specified XML file.
 * Uses libxml2 XPath/DOM API.
 *
 * @param xml_path Full path to the imsconfig_*.xml file
 * @return 0 on success, -1 on error or if the node is not found
 */
int ims_enable_volte_provisioning(const char *xml_path);

#ifdef __cplusplus
}
#endif

#endif /* IMS_CARRIER_CONFIG_H */
