/* CA trust anchors for HTTPS (generated into tls_ca.c by mktrust.py). A curated
 * subset of the public root store, enough to validate the common CAs. */
#ifndef PC64_TLS_CA_H
#define PC64_TLS_CA_H
#include "bearssl.h"
extern const br_x509_trust_anchor uno_tls_tas[];
extern const size_t uno_tls_tas_num;
#endif
