/*
  Certificate and key of the HTTPS web interface (ESP32 only).

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.

  Pure code on top of mbedtls, without Arduino calls, so the host tests
  (tests/https) run exactly this:

  - httpscert_params(): what goes into a self-signed certificate (subject,
    names, addresses, validity) from the hostname, the IP addresses and
    the time.
  - httpscert_generate(): a new ECDSA P-256 key and a self-signed X.509
    certificate, written as one store file (see below).
  - httpscert_from_upload(): an own certificate and key (PEM) checked and
    turned into a store file; nothing is written when they do not fit.
  - httpscert_parse(): a store file read back into the mbedtls objects the
    TLS server needs, with the same checks, and the information the
    settings show (fingerprint, subject, validity). The private key is
    never part of that information.

  The store file holds certificate and key together, so replacing both is
  one rename:
      "HMTLS1 generated <hostname>\n" or "HMTLS1 uploaded\n"
      the certificate (PEM, an uploaded one may carry its chain after it)
      the private key (PEM)
*/
#ifndef _HTTPSCERT_H_
#define _HTTPSCERT_H_

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define HTTPSCERT_MAGIC "HMTLS1"
#define HTTPSCERT_HOST_MAX 32           // webauth_hostname_valid(): 1..32
#define HTTPSCERT_IPS_MAX 4
#define HTTPSCERT_VALID_YEARS 10
#define HTTPSCERT_CERT_MAX 6144         // PEM of an uploaded certificate with its chain
#define HTTPSCERT_KEY_MAX 3072          // PEM of an uploaded private key
#define HTTPSCERT_FILE_MAX (64 + HTTPSCERT_CERT_MAX + HTTPSCERT_KEY_MAX)
#define HTTPSCERT_EARLIEST 1704067200L  // 2024-01-01: a clock before this is not set (no NTP yet)

typedef struct httpscert_params_t {
  char host[HTTPSCERT_HOST_MAX + 1];    // lower case
  char subject[64];                     // "CN=<host>.local,O=HeishaMon"
  char dns_local[HTTPSCERT_HOST_MAX + 7]; // "<host>.local"
  uint8_t ip[HTTPSCERT_IPS_MAX][4];
  uint8_t nips;
  char not_before[15];                  // YYYYMMDDhhmmss (UTC)
  char not_after[15];
} httpscert_params_t;

/*
 * hostname: anything; one that is not a valid host name (letters, digits,
 * '-', 1..32, not starting or ending with '-') becomes "heishamon".
 * ips: IPv4 addresses in network order as the ESP keeps them (first octet in
 * the lowest byte), 0 entries are skipped, at most HTTPSCERT_IPS_MAX.
 * now: the clock; before HTTPSCERT_EARLIEST the validity starts 2024-01-01.
 * The validity starts one day before now (clocks of browsers differ) and
 * ends HTTPSCERT_VALID_YEARS later.
 */
void httpscert_params(httpscert_params_t *p, const char *hostname, const uint32_t *ips, uint8_t nips, time_t now);

/* YYYYMMDDhhmmss of a time (UTC); out has room for 15 characters */
void httpscert_time_string(time_t t, char out[15]);

/* Results of generate, upload and parse */
enum {
  HTTPSCERT_OK = 0,
  HTTPSCERT_ERR_MEMORY = -1,     // out of memory or out buffer too small
  HTTPSCERT_ERR_CERT = -2,       // no certificate, or it cannot be read
  HTTPSCERT_ERR_KEY = -3,        // no private key, or it cannot be read (also: protected by a password)
  HTTPSCERT_ERR_MISMATCH = -4,   // key and certificate do not belong together
  HTTPSCERT_ERR_KEYTYPE = -5,    // a key this device does not use (only EC P-256 and RSA 2048)
  HTTPSCERT_ERR_SIZE = -6,       // certificate or key larger than the limits above
  HTTPSCERT_ERR_FORMAT = -7,     // store file without the header line
  HTTPSCERT_ERR_GENERATE = -8,   // mbedtls could not make the key or the certificate
  HTTPSCERT_ERR_IO = -9,         // the store file could not be opened or read (flash)
};
/* A stored file with this result is wrong and is replaced by a new self-signed one; any other error is kept and retried */
bool httpscert_damaged(int err);
const char *httpscert_error_text(int err);

/*
 * f_rng/p_rng: the random generator (mbedtls_ctr_drbg_random with its
 * context). out gets the store file, NUL terminated; the return value is
 * its length (> 0) or an HTTPSCERT_ERR_ value.
 */
typedef int (*httpscert_rng_t)(void *, unsigned char *, size_t);
int httpscert_generate(const httpscert_params_t *p, httpscert_rng_t f_rng, void *p_rng, char *out, size_t outsize);
int httpscert_from_upload(const char *cert_pem, size_t cert_len, const char *key_pem, size_t key_len,
                          httpscert_rng_t f_rng, void *p_rng, char *out, size_t outsize);

typedef struct httpscert_info_t {
  uint8_t generated;              // 1: made by this device, 0: uploaded
  char host[HTTPSCERT_HOST_MAX + 1]; // generated: the hostname it was made for
  char fingerprint[96];           // SHA-256 of the certificate, "AB:CD:..."
  char subject[128];
  char not_before[20];            // "YYYY-MM-DD hh:mm"
  char not_after[20];
  char key_type[24];              // "EC P-256", "RSA 2048"
} httpscert_info_t;

/*
 * Reads a store file (data does not need to be NUL terminated). crt and key
 * must be initialised (mbedtls_x509_crt_init, mbedtls_pk_init) and are
 * filled; on an error the caller frees them. crt/key may be NULL to only
 * check the file and fill info.
 */
struct mbedtls_x509_crt;
struct mbedtls_pk_context;
int httpscert_parse(const char *data, size_t len, httpscert_rng_t f_rng, void *p_rng,
                    struct mbedtls_x509_crt *crt, struct mbedtls_pk_context *key, httpscert_info_t *info);

/* A generated certificate is made again when the hostname changed */
bool httpscert_needs_new(const httpscert_info_t *info, const char *hostname);

#endif
