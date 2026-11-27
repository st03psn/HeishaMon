/*
  TLS for the web server (ESP32 only), see httpstls.h.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/
#if defined(ESP32) || defined(HTTPS_HOST_TEST)

#include "httpstls.h"
#include "httpscert.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP32)
  #include "lwip/sockets.h"
#else
  #include <sys/socket.h>
  #include <sys/types.h>
#endif

#include "mbedtls/build_info.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cache.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#if defined(MBEDTLS_PSA_CRYPTO_C)
  #include "psa/crypto.h"
#endif

#ifdef MSG_NOSIGNAL
  #define SEND_FLAGS MSG_NOSIGNAL  // a closed peer is an error code, not a signal (host)
#else
  #define SEND_FLAGS 0
#endif

httpstls_stats_t httpstls_stats;

struct httpstls_conn_t {
  int fd;
  uint8_t done;            // handshake finished
  unsigned long start;
  uint32_t step_max;       // longest step of this handshake
  size_t pending;          // a record of this many plaintext bytes is partly sent: mbedtls wants the same length again
  mbedtls_ssl_context ssl;
};

static uint8_t rng_ready = 0;
static uint8_t server_ready = 0;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_ssl_config conf;
static mbedtls_x509_crt server_crt;
static mbedtls_pk_context server_key;
#if defined(MBEDTLS_SSL_CACHE_C)
static mbedtls_ssl_cache_context cache;
#endif

/* Forward secrecy and AEAD only; mbedtls picks the ones that fit the key */
static const int ciphersuites[] = {
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
  0
};

#if defined(MBEDTLS_PLATFORM_MEMORY) && !defined(MBEDTLS_PLATFORM_CALLOC_MACRO)
static void *plain_calloc(size_t n, size_t size) {
  return calloc(n, size);
}

static void plain_free(void *p) {
  free(p);
}
#endif

void httpstls_use_calloc(void) {
#if defined(MBEDTLS_PLATFORM_MEMORY) && !defined(MBEDTLS_PLATFORM_CALLOC_MACRO)
  mbedtls_platform_set_calloc_free(plain_calloc, plain_free);
#endif
}

static bool rng_init(void) {
  if(rng_ready) {
    return true;
  }
#if defined(MBEDTLS_PSA_CRYPTO_C)
  psa_crypto_init();
#endif
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  static const unsigned char pers[] = "heishamon-https";
  if(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, pers, sizeof(pers) - 1) != 0) {
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return false;
  }
  rng_ready = 1;
  return true;
}

int httpstls_random(void *ctx, unsigned char *buf, size_t len) {
  return mbedtls_ctr_drbg_random(ctx, buf, len);
}

void *httpstls_random_ctx(void) {
  return rng_init() ? &drbg : NULL;
}

bool httpstls_admit(uint8_t active, size_t internal_largest, size_t psram_largest, bool psram_buffers) {
  if(active >= HTTPSTLS_MAX_CONN) {
    return false;
  }
  if(psram_buffers && psram_largest >= 2 * HTTPSTLS_BUFFER_BYTES) {
    return internal_largest >= HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_INTERNAL_BYTES;
  }
  return internal_largest >= HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_BYTES;
}

bool httpstls_ready(void) {
  return server_ready != 0;
}

void httpstls_server_free(void) {
  if(!server_ready) {
    return;
  }
  mbedtls_ssl_config_free(&conf);
  mbedtls_x509_crt_free(&server_crt);
  mbedtls_pk_free(&server_key);
#if defined(MBEDTLS_SSL_CACHE_C)
  mbedtls_ssl_cache_free(&cache);
#endif
  server_ready = 0;
}

int httpstls_server_init(const char *store, size_t len, struct httpscert_info_t *info) {
  httpstls_server_free();
  if(!rng_init()) {
    return HTTPSCERT_ERR_GENERATE;
  }
  mbedtls_x509_crt_init(&server_crt);
  mbedtls_pk_init(&server_key);
  mbedtls_ssl_config_init(&conf);
#if defined(MBEDTLS_SSL_CACHE_C)
  mbedtls_ssl_cache_init(&cache);
#endif
  int r = httpscert_parse(store, len, httpstls_random, &drbg, &server_crt, &server_key, info);
  if(r == HTTPSCERT_OK) {
    if(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
      r = HTTPSCERT_ERR_MEMORY;
    }
  }
  if(r == HTTPSCERT_OK) {
    mbedtls_ssl_conf_rng(&conf, httpstls_random, &drbg);
    mbedtls_ssl_conf_ciphersuites(&conf, ciphersuites);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2); // the only one of the ESP32 core
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE); // no client certificates
#if defined(MBEDTLS_SSL_RENEGOTIATION)
    mbedtls_ssl_conf_renegotiation(&conf, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
#if defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_cache_set_max_entries(&cache, HTTPSTLS_CACHE_ENTRIES);
#if defined(MBEDTLS_HAVE_TIME)
    mbedtls_ssl_cache_set_timeout(&cache, HTTPSTLS_CACHE_TIMEOUT_S);
#endif
    mbedtls_ssl_conf_session_cache(&conf, &cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
#endif
    // the chain of an uploaded certificate is sent with it (server_crt.next)
    if(mbedtls_ssl_conf_own_cert(&conf, &server_crt, &server_key) != 0) {
      r = HTTPSCERT_ERR_MEMORY;
    }
  }
  if(r != HTTPSCERT_OK) {
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&server_crt);
    mbedtls_pk_free(&server_key);
#if defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_cache_free(&cache);
#endif
    return r;
  }
  server_ready = 1;
  return HTTPSCERT_OK;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  int fd = ((struct httpstls_conn_t *)ctx)->fd;
  int n = send(fd, buf, len, MSG_DONTWAIT | SEND_FLAGS);
  if(n < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOMEM) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
  int fd = ((struct httpstls_conn_t *)ctx)->fd;
  int n = recv(fd, buf, len, MSG_DONTWAIT);
  if(n < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return n;  // 0: the peer closed, mbedtls reports MBEDTLS_ERR_SSL_CONN_EOF
}

struct httpstls_conn_t *httpstls_conn_new(int fd) {
  if(!server_ready || fd < 0 || httpstls_stats.active >= HTTPSTLS_MAX_CONN) {
    return NULL;
  }
  struct httpstls_conn_t *c = (struct httpstls_conn_t *)calloc(1, sizeof(struct httpstls_conn_t));
  if(c == NULL) {
    return NULL;
  }
  c->fd = fd;
  c->start = httpstls_platform_millis();
  mbedtls_ssl_init(&c->ssl);
  // the record buffers are allocated here; out of memory is an error code, never a crash
  if(mbedtls_ssl_setup(&c->ssl, &conf) != 0) {
    mbedtls_ssl_free(&c->ssl);
    free(c);
    return NULL;
  }
  mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
  httpstls_stats.active++;
  if(httpstls_stats.active > httpstls_stats.peak) {
    httpstls_stats.peak = httpstls_stats.active;
  }
  return c;
}

void httpstls_conn_free(struct httpstls_conn_t *c) {
  if(c == NULL) {
    return;
  }
  if(c->done) {
    mbedtls_ssl_close_notify(&c->ssl); // one try, non-blocking
  } else {
    httpstls_stats.failed++;            // closed before the handshake finished
  }
  mbedtls_ssl_free(&c->ssl);
  free(c);
  if(httpstls_stats.active > 0) {
    httpstls_stats.active--;
  }
}

bool httpstls_handshake_done(const struct httpstls_conn_t *c) {
  return c != NULL && c->done;
}

unsigned long httpstls_conn_age(const struct httpstls_conn_t *c) {
  return (unsigned long)(httpstls_platform_millis() - c->start);
}

int httpstls_handshake(struct httpstls_conn_t *c) {
  if(c->done) {
    return HTTPSTLS_DONE;
  }
  unsigned long t = httpstls_platform_millis();
  int ret = mbedtls_ssl_handshake_step(&c->ssl);
  uint32_t used = (uint32_t)(httpstls_platform_millis() - t);
  if(used > c->step_max) {
    c->step_max = used;
  }
  if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    return HTTPSTLS_AGAIN;
  }
  if(ret != 0) {
    httpstls_stats.last_error = ret;
    return HTTPSTLS_FAIL;
  }
  if(!mbedtls_ssl_is_handshake_over(&c->ssl)) {
    return HTTPSTLS_AGAIN;
  }
  c->done = 1;
  httpstls_stats.handshakes++;
  httpstls_stats.last_ms = (uint32_t)(httpstls_platform_millis() - c->start);
  if(httpstls_stats.last_ms > httpstls_stats.max_ms) {
    httpstls_stats.max_ms = httpstls_stats.last_ms;
  }
  httpstls_stats.last_step_ms = c->step_max;
  if(c->step_max > httpstls_stats.max_step_ms) {
    httpstls_stats.max_step_ms = c->step_max;
  }
  return HTTPSTLS_DONE;
}

int httpstls_read(struct httpstls_conn_t *c, unsigned char *buf, size_t len) {
  if(!c->done) {
    return 0;
  }
  int ret = mbedtls_ssl_read(&c->ssl, buf, len);
  if(ret > 0) {
    return ret;
  }
  if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    return 0;
  }
  return HTTPSTLS_FAIL;  // 0 (closed), close_notify, or an error
}

int httpstls_write(struct httpstls_conn_t *c, const unsigned char *buf, size_t len) {
  if(!c->done) {
    return 0;
  }
  if(c->pending > 0) {
    if(len < c->pending) {
      return HTTPSTLS_FAIL;  // the data mbedtls is still sending is gone
    }
    len = c->pending;
  }
  if(len == 0) {
    return 0;
  }
  int ret = mbedtls_ssl_write(&c->ssl, buf, len);
  if(ret > 0) {
    c->pending = 0;
    return ret;
  }
  if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    c->pending = len;
    return 0;
  }
  return HTTPSTLS_FAIL;
}

#endif
