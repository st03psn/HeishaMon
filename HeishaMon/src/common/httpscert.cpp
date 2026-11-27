/*
  Certificate and key of the HTTPS web interface (ESP32 only), see httpscert.h.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/
#if defined(ESP32) || defined(HTTPS_HOST_TEST)

#include "httpscert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/build_info.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "mbedtls/sha256.h"
#include "mbedtls/asn1.h"
#include "mbedtls/platform_util.h"

static bool host_valid(const char *name) {
  if(name == NULL) {
    return false;
  }
  size_t len = strlen(name);
  if(len < 1 || len > HTTPSCERT_HOST_MAX || name[0] == '-' || name[len - 1] == '-') {
    return false;
  }
  for(size_t i = 0; i < len; i++) {
    char c = name[i];
    if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
  }
  return true;
}

static void lower_copy(char *out, size_t size, const char *in) {
  size_t i = 0;
  for(; in[i] != 0 && i + 1 < size; i++) {
    char c = in[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  out[i] = 0;
}

void httpscert_time_string(time_t t, char out[15]) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
           tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
  memcpy(out, tmp, 14);
  out[14] = 0;
}

void httpscert_params(httpscert_params_t *p, const char *hostname, const uint32_t *ips, uint8_t nips, time_t now) {
  memset(p, 0, sizeof(*p));
  lower_copy(p->host, sizeof(p->host), host_valid(hostname) ? hostname : "heishamon");
  snprintf(p->dns_local, sizeof(p->dns_local), "%s.local", p->host);
  snprintf(p->subject, sizeof(p->subject), "CN=%s,O=HeishaMon", p->dns_local);  // the name the owner types: heishamon.local
  for(uint8_t i = 0; ips != NULL && i < nips && p->nips < HTTPSCERT_IPS_MAX; i++) {
    uint32_t a = ips[i];
    if(a == 0) {
      continue;
    }
    bool dup = false;
    uint8_t b[4] = { (uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 24) & 0xFF) };
    for(uint8_t j = 0; j < p->nips; j++) {
      if(memcmp(p->ip[j], b, 4) == 0) {
        dup = true;
      }
    }
    if(!dup) {
      memcpy(p->ip[p->nips++], b, 4);
    }
  }
  time_t start = (now < (time_t)HTTPSCERT_EARLIEST) ? (time_t)HTTPSCERT_EARLIEST : now - 24 * 60 * 60;
  httpscert_time_string(start, p->not_before);
  struct tm tm;
  gmtime_r(&start, &tm);
  int year = tm.tm_year + 1900 + HTTPSCERT_VALID_YEARS;
  int day = (tm.tm_mon == 1 && tm.tm_mday == 29) ? 28 : tm.tm_mday; // 29 February
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d", year % 10000, (tm.tm_mon + 1) % 100, day % 100,
           tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
  memcpy(p->not_after, tmp, 14);
  p->not_after[14] = 0;
}

const char *httpscert_error_text(int err) {
  switch(err) {
    case HTTPSCERT_OK: return "ok";
    case HTTPSCERT_ERR_MEMORY: return "out of memory";
    case HTTPSCERT_ERR_CERT: return "the certificate cannot be read (PEM expected)";
    case HTTPSCERT_ERR_KEY: return "the private key cannot be read (PEM without a password expected)";
    case HTTPSCERT_ERR_MISMATCH: return "the private key does not belong to the certificate";
    case HTTPSCERT_ERR_KEYTYPE: return "only EC P-256 and RSA 2048 keys are supported";
    case HTTPSCERT_ERR_SIZE: return "certificate or key too large";
    case HTTPSCERT_ERR_FORMAT: return "the stored file is not valid";
    case HTTPSCERT_ERR_GENERATE: return "the certificate could not be made";
    case HTTPSCERT_ERR_IO: return "the stored file could not be read";
  }
  return "unknown error";
}

/* Only keys whose private operation fits into one short step of loop() */
static bool key_type_ok(const mbedtls_pk_context *key, char *name, size_t size) {
  mbedtls_pk_type_t type = mbedtls_pk_get_type(key);
  if(type == MBEDTLS_PK_ECKEY || type == MBEDTLS_PK_ECDSA) {
    mbedtls_ecp_group_id gid = mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(*key));
    if(gid == MBEDTLS_ECP_DP_SECP256R1) {
      snprintf(name, size, "EC P-256");
      return true;
    }
    snprintf(name, size, "EC (other curve)");
    return false;
  }
  if(type == MBEDTLS_PK_RSA) {
    size_t bits = mbedtls_pk_get_bitlen(key);
    snprintf(name, size, "RSA %u", (unsigned int)bits);
    return bits == 2048;
  }
  snprintf(name, size, "other");
  return false;
}

/* header + certificate + key */
static int write_store(char *out, size_t outsize, const char *header, const char *cert, size_t cert_len, const char *key, size_t key_len) {
  size_t hlen = strlen(header);
  size_t need = hlen + cert_len + 1 + key_len + 1 + 1;
  if(need > outsize) {
    return HTTPSCERT_ERR_MEMORY;
  }
  size_t n = 0;
  memcpy(out + n, header, hlen);
  n += hlen;
  memcpy(out + n, cert, cert_len);
  n += cert_len;
  if(n > 0 && out[n - 1] != '\n') {
    out[n++] = '\n';
  }
  memcpy(out + n, key, key_len);
  n += key_len;
  if(n > 0 && out[n - 1] != '\n') {
    out[n++] = '\n';
  }
  out[n] = 0;
  return (int)n;
}

int httpscert_generate(const httpscert_params_t *p, httpscert_rng_t f_rng, void *p_rng, char *out, size_t outsize) {
  int ret = 0;
  int result = HTTPSCERT_ERR_GENERATE;
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_x509_san_list san[2 + HTTPSCERT_IPS_MAX];
  mbedtls_asn1_sequence eku;
  unsigned char serial[16];
  char header[24 + HTTPSCERT_HOST_MAX];
  const size_t cert_size = 2048, key_size = 1024;
  unsigned char *cert_pem = NULL, *key_pem = NULL;

  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  memset(san, 0, sizeof(san));
  memset(&eku, 0, sizeof(eku));

  cert_pem = (unsigned char *)calloc(1, cert_size);
  key_pem = (unsigned char *)calloc(1, key_size);
  if(cert_pem == NULL || key_pem == NULL) {
    result = HTTPSCERT_ERR_MEMORY;
    goto done;
  }

  if((ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
     (ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), f_rng, p_rng)) != 0) {
    goto done;
  }

  if((ret = f_rng(p_rng, serial, sizeof(serial))) != 0) {
    goto done;
  }
  serial[0] &= 0x7F;  // a positive number
  serial[0] |= 0x40;  // of full length, never zero

  // subjectAltName: hostname, hostname.local, the IP addresses
  {
    uint8_t n = 0;
    san[n].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[n].node.san.unstructured_name.p = (unsigned char *)p->host;
    san[n].node.san.unstructured_name.len = strlen(p->host);
    n++;
    san[n].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[n].node.san.unstructured_name.p = (unsigned char *)p->dns_local;
    san[n].node.san.unstructured_name.len = strlen(p->dns_local);
    n++;
    for(uint8_t i = 0; i < p->nips && i < HTTPSCERT_IPS_MAX; i++) {
      san[n].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
      san[n].node.san.unstructured_name.p = (unsigned char *)p->ip[i];
      san[n].node.san.unstructured_name.len = 4;
      n++;
    }
    for(uint8_t i = 0; i + 1 < n; i++) {
      san[i].next = &san[i + 1];
    }
  }

  eku.buf.tag = MBEDTLS_ASN1_OID;
  eku.buf.p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH;
  eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
  eku.next = NULL;

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  if((ret = mbedtls_x509write_crt_set_subject_name(&crt, p->subject)) != 0 ||
     (ret = mbedtls_x509write_crt_set_issuer_name(&crt, p->subject)) != 0 ||
     (ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0 ||
     (ret = mbedtls_x509write_crt_set_validity(&crt, p->not_before, p->not_after)) != 0 ||
     (ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0 ||
     (ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) != 0 ||
     (ret = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku)) != 0 ||
     (ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san[0])) != 0) {
    goto done;
  }

  if((ret = mbedtls_x509write_crt_pem(&crt, cert_pem, cert_size, f_rng, p_rng)) != 0 ||
     (ret = mbedtls_pk_write_key_pem(&key, key_pem, key_size)) != 0) {
    goto done;
  }

  snprintf(header, sizeof(header), HTTPSCERT_MAGIC " generated %s\n", p->host);
  result = write_store(out, outsize, header, (const char *)cert_pem, strlen((const char *)cert_pem),
                       (const char *)key_pem, strlen((const char *)key_pem));

done:
  if(key_pem != NULL) {
    mbedtls_platform_zeroize(key_pem, key_size);
    free(key_pem);
  }
  free(cert_pem);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  return result;
}

/* text between the first "-----BEGIN <what>" and the end of its "-----END ...-----" line */
static const char *find_block(const char *data, size_t len, const char *what, size_t *blen) {
  const char *begin = NULL;
  size_t wlen = strlen(what);
  for(size_t i = 0; i + 11 + wlen <= len; i++) {
    if(memcmp(data + i, "-----BEGIN ", 11) == 0) {
      // "-----BEGIN " then the label, which must contain `what` before the closing dashes
      size_t j = i + 11;
      while(j < len && data[j] != '-' && data[j] != '\n') {
        j++;
      }
      size_t lab = j - (i + 11);
      bool match = false;
      for(size_t k = 0; k + wlen <= lab; k++) {
        if(memcmp(data + i + 11 + k, what, wlen) == 0) {
          match = true;
          break;
        }
      }
      if(match) {
        begin = data + i;
        break;
      }
    }
  }
  if(begin == NULL) {
    return NULL;
  }
  const char *end = NULL;
  size_t rest = len - (size_t)(begin - data);
  for(size_t i = 0; i + 9 <= rest; i++) {
    if(memcmp(begin + i, "-----END ", 9) == 0) {
      size_t j = i + 9;
      while(j < rest && begin[j] != '\n') {
        j++;
      }
      if(j < rest) {
        j++;  // with the newline
      }
      end = begin + j;
      break;
    }
  }
  if(end == NULL) {
    return NULL;
  }
  *blen = (size_t)(end - begin);
  return begin;
}

/* all CERTIFICATE blocks one after the other (an uploaded chain), up to the first other block */
static const char *find_certs(const char *data, size_t len, size_t *clen) {
  size_t first_len = 0;
  const char *first = find_block(data, len, "CERTIFICATE", &first_len);
  if(first == NULL) {
    return NULL;
  }
  const char *end = first + first_len;
  for(;;) {
    size_t rest = len - (size_t)(end - data);
    const char *p = end;
    while(rest > 0 && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) {
      p++;
      rest--;
    }
    if(rest < 27 || memcmp(p, "-----BEGIN CERTIFICATE-----", 27) != 0) {
      break;
    }
    size_t blen = 0;
    const char *b = find_block(p, rest, "CERTIFICATE", &blen);
    if(b == NULL) {
      break;
    }
    end = b + blen;
  }
  *clen = (size_t)(end - first);
  return first;
}

/* mbedtls wants PEM NUL terminated, with the terminator counted */
static char *nul_copy(const char *data, size_t len) {
  char *c = (char *)malloc(len + 1);
  if(c != NULL) {
    memcpy(c, data, len);
    c[len] = 0;
  }
  return c;
}

static void format_time(const mbedtls_x509_time *t, char *out, size_t size) {
  char tmp[40];
  snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d", t->year % 10000, t->mon % 100, t->day % 100, t->hour % 100, t->min % 100);
  snprintf(out, size, "%.16s", tmp);
}

/*
 * The checks of an upload and of a stored file. cert/key: PEM text (not
 * terminated). crt/pk: filled when not NULL.
 */
static int check_pair(const char *cert, size_t cert_len, const char *key, size_t key_len,
                      httpscert_rng_t f_rng, void *p_rng,
                      mbedtls_x509_crt *crt_out, mbedtls_pk_context *pk_out, httpscert_info_t *info) {
  int result = HTTPSCERT_OK;
  mbedtls_x509_crt crt_local;
  mbedtls_pk_context pk_local;
  mbedtls_x509_crt *crt = crt_out ? crt_out : &crt_local;
  mbedtls_pk_context *pk = pk_out ? pk_out : &pk_local;
  char *cert_z = NULL, *key_z = NULL;
  char key_type[24] = "";

  if(crt_out == NULL) {
    mbedtls_x509_crt_init(&crt_local);
  }
  if(pk_out == NULL) {
    mbedtls_pk_init(&pk_local);
  }

  if(cert_len > HTTPSCERT_CERT_MAX || key_len > HTTPSCERT_KEY_MAX) {
    result = HTTPSCERT_ERR_SIZE;
    goto done;
  }
  cert_z = nul_copy(cert, cert_len);
  key_z = nul_copy(key, key_len);
  if(cert_z == NULL || key_z == NULL) {
    result = HTTPSCERT_ERR_MEMORY;
    goto done;
  }
  if(mbedtls_x509_crt_parse(crt, (const unsigned char *)cert_z, cert_len + 1) != 0 || crt->raw.len == 0) {
    result = HTTPSCERT_ERR_CERT;
    goto done;
  }
  if(mbedtls_pk_parse_key(pk, (const unsigned char *)key_z, key_len + 1, NULL, 0, f_rng, p_rng) != 0) {
    result = HTTPSCERT_ERR_KEY;
    goto done;
  }
  if(!key_type_ok(pk, key_type, sizeof(key_type))) {
    result = HTTPSCERT_ERR_KEYTYPE;
    goto done;
  }
  // the first certificate is the one of this server; the key must be its key
  if(mbedtls_pk_check_pair(&crt->pk, pk, f_rng, p_rng) != 0) {
    result = HTTPSCERT_ERR_MISMATCH;
    goto done;
  }
  if(info != NULL) {
    uint8_t hash[32];
    mbedtls_sha256(crt->raw.p, crt->raw.len, hash, 0);
    size_t n = 0;
    for(uint8_t i = 0; i < 32 && n + 4 <= sizeof(info->fingerprint); i++) {
      n += snprintf(info->fingerprint + n, sizeof(info->fingerprint) - n, i ? ":%02X" : "%02X", hash[i]);
    }
    if(mbedtls_x509_dn_gets(info->subject, sizeof(info->subject), &crt->subject) < 0) {
      info->subject[0] = 0;
    }
    format_time(&crt->valid_from, info->not_before, sizeof(info->not_before));
    format_time(&crt->valid_to, info->not_after, sizeof(info->not_after));
    snprintf(info->key_type, sizeof(info->key_type), "%s", key_type);
  }

done:
  if(key_z != NULL) {
    mbedtls_platform_zeroize(key_z, key_len + 1);
    free(key_z);
  }
  free(cert_z);
  if(crt_out == NULL) {
    mbedtls_x509_crt_free(&crt_local);
  }
  if(pk_out == NULL) {
    mbedtls_pk_free(&pk_local);
  }
  return result;
}

int httpscert_from_upload(const char *cert_pem, size_t cert_len, const char *key_pem, size_t key_len,
                          httpscert_rng_t f_rng, void *p_rng, char *out, size_t outsize) {
  if(cert_pem == NULL || key_pem == NULL) {
    return cert_pem == NULL ? HTTPSCERT_ERR_CERT : HTTPSCERT_ERR_KEY;
  }
  if(cert_len > HTTPSCERT_CERT_MAX || key_len > HTTPSCERT_KEY_MAX) {
    return HTTPSCERT_ERR_SIZE;
  }
  size_t clen = 0, klen = 0;
  const char *cert = find_certs(cert_pem, cert_len, &clen);
  if(cert == NULL) {
    return HTTPSCERT_ERR_CERT;
  }
  const char *key = find_block(key_pem, key_len, "PRIVATE KEY", &klen);
  if(key == NULL) {
    return HTTPSCERT_ERR_KEY;
  }
  int r = check_pair(cert, clen, key, klen, f_rng, p_rng, NULL, NULL, NULL);
  if(r != HTTPSCERT_OK) {
    return r;
  }
  return write_store(out, outsize, HTTPSCERT_MAGIC " uploaded\n", cert, clen, key, klen);
}

int httpscert_parse(const char *data, size_t len, httpscert_rng_t f_rng, void *p_rng,
                    struct mbedtls_x509_crt *crt, struct mbedtls_pk_context *key, httpscert_info_t *info) {
  httpscert_info_t local;
  if(info == NULL) {
    info = &local;
  }
  memset(info, 0, sizeof(*info));
  if(data == NULL || len > HTTPSCERT_FILE_MAX) {
    return HTTPSCERT_ERR_FORMAT;
  }
  // header line
  size_t ml = strlen(HTTPSCERT_MAGIC);
  if(len < ml + 2 || memcmp(data, HTTPSCERT_MAGIC " ", ml + 1) != 0) {
    return HTTPSCERT_ERR_FORMAT;
  }
  const char *nl = (const char *)memchr(data, '\n', len);
  if(nl == NULL) {
    return HTTPSCERT_ERR_FORMAT;
  }
  const char *kind = data + ml + 1;
  size_t kl = (size_t)(nl - kind);
  if(kl >= 10 && memcmp(kind, "generated ", 10) == 0) {
    size_t hl = kl - 10;
    if(hl < 1 || hl > HTTPSCERT_HOST_MAX) {
      return HTTPSCERT_ERR_FORMAT;
    }
    info->generated = 1;
    memcpy(info->host, kind + 10, hl);
    info->host[hl] = 0;
  } else if(kl == 8 && memcmp(kind, "uploaded", 8) == 0) {
    info->generated = 0;
  } else {
    return HTTPSCERT_ERR_FORMAT;
  }
  const char *body = nl + 1;
  size_t blen = len - (size_t)(body - data);
  size_t clen = 0, klen = 0;
  const char *cert = find_certs(body, blen, &clen);
  if(cert == NULL) {
    return HTTPSCERT_ERR_CERT;
  }
  const char *k = find_block(body, blen, "PRIVATE KEY", &klen);
  if(k == NULL) {
    return HTTPSCERT_ERR_KEY;
  }
  return check_pair(cert, clen, k, klen, f_rng, p_rng, crt, key, info);
}

bool httpscert_damaged(int err) {
  // only a file that was read completely and is wrong; out of memory, a read error or a missing
  // random generator are passing troubles, and an uploaded certificate must survive them
  return err == HTTPSCERT_ERR_FORMAT || err == HTTPSCERT_ERR_CERT || err == HTTPSCERT_ERR_KEY ||
         err == HTTPSCERT_ERR_MISMATCH || err == HTTPSCERT_ERR_KEYTYPE || err == HTTPSCERT_ERR_SIZE;
}

bool httpscert_needs_new(const httpscert_info_t *info, const char *hostname) {
  if(!info->generated) {
    return false;  // an uploaded certificate is the owner's choice
  }
  char want[HTTPSCERT_HOST_MAX + 1];
  lower_copy(want, sizeof(want), host_valid(hostname) ? hostname : "heishamon");
  return strcmp(want, info->host) != 0;
}

#endif
