#ifdef ESP32
// THROWAWAY MEASUREMENT CODE, not meant for a pull request. See TlsSpike.h.
//
// One TLS session at a time, polled from loop() like the normal web server:
//   GET /      -> text with the measurements of this session and of the device
//   GET /big   -> 256 kB of data, to measure the send speed
//   POST /...  -> the body is read and thrown away, to measure the receive speed (firmware upload)
// The connection stays open for more requests until it is idle for 5 seconds.
// The certificate is a self-signed EC P-256 one, generated in RAM at start (nothing is stored).

#include <Arduino.h>
#include <errno.h>
#include <stdarg.h>
#include <strings.h>
#include <lwip/sockets.h>
#include "TlsSpike.h"

#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#ifdef MBEDTLS_PSA_CRYPTO_C
#include "psa/crypto.h"
#endif

extern void log_message(char *string);
extern unsigned long goodreads;
extern unsigned long totalreads;

#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_X509_CRT_WRITE_C) && defined(MBEDTLS_ECP_C) && defined(MBEDTLS_PK_C)
#define TLS_SPIKE_POSSIBLE 1
#endif

#define SPIKE_START_MS 20000UL   // start after the device is up
#define SPIKE_IDLE_MS 5000UL     // close an idle session
#define SPIKE_BIG_BYTES 262144UL
#define SPIKE_CHUNK 1400

static char spikeLog[256];
static void spikeSay(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(spikeLog, sizeof(spikeLog), fmt, args);
  va_end(args);
  log_message(spikeLog);
}

#ifndef TLS_SPIKE_POSSIBLE

void tlsSpikeLoop() {
  static bool said = false;
  if (said || millis() < SPIKE_START_MS) return;
  said = true;
  spikeSay("[TLSSPIKE] not possible with this core: SSL_SRV_C=%d X509_CRT_WRITE_C=%d ECP_C=%d PK_C=%d",
#ifdef MBEDTLS_SSL_SRV_C
           1,
#else
           0,
#endif
#ifdef MBEDTLS_X509_CRT_WRITE_C
           1,
#else
           0,
#endif
#ifdef MBEDTLS_ECP_C
           1,
#else
           0,
#endif
#ifdef MBEDTLS_PK_C
           1
#else
           0
#endif
          );
}

#else

enum { SPIKE_OFF, SPIKE_FAILED, SPIKE_LISTEN, SPIKE_HANDSHAKE, SPIKE_REQUEST, SPIKE_BODY, SPIKE_SEND };

static int spikeState = SPIKE_OFF;
static int listenFd = -1;
static int clientFd = -1;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_pk_context serverKey;
static mbedtls_x509_crt serverCert;
static mbedtls_ssl_config sslConf;
static mbedtls_ssl_context ssl;

// measurements
static unsigned long keyGenMs = 0, certGenMs = 0;
static unsigned long sessions = 0, requests = 0;
static unsigned long hsStart = 0, hsTotalMs = 0, hsMaxCallMs = 0, hsCalls = 0;
static unsigned long hsWorstTotalMs = 0, hsWorstCallMs = 0;
static uint32_t heapBeforeSession = 0, heapAfterHandshake = 0;
static unsigned long loopGapMaxMs = 0, loopGapMaxSinceReportMs = 0, lastLoopCall = 0;
static unsigned long lastActivity = 0;

// request / response state
static char reqBuf[1024];
static size_t reqLen = 0;
static unsigned long bodyLeft = 0, bodyTotal = 0, bodyStart = 0;
static char outBuf[SPIKE_CHUNK];
static size_t outLen = 0, outSent = 0;
static unsigned long bigLeft = 0, bigStart = 0;
static bool reportBig = false;

static int spikeSend(void *ctx, const unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  int ret = lwip_send(fd, buf, len, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return ret;
}

static int spikeRecv(void *ctx, unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  int ret = lwip_recv(fd, buf, len, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return ret;
}

static void spikeError(const char *what, int ret) {
  char text[80];
  mbedtls_strerror(ret, text, sizeof(text));
  spikeSay("[TLSSPIKE] %s failed: -0x%04x %s", what, (unsigned int)(-ret), text);
}

static bool spikeMakeCert() {
  int ret;
#ifdef MBEDTLS_PSA_CRYPTO_C
  psa_crypto_init();
#endif
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_pk_init(&serverKey);
  mbedtls_x509_crt_init(&serverCert);
  if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"heishamon", 9)) != 0) {
    spikeError("drbg seed", ret);
    return false;
  }

  unsigned long t = millis();
  if ((ret = mbedtls_pk_setup(&serverKey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
      (ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(serverKey), mbedtls_ctr_drbg_random, &drbg)) != 0) {
    spikeError("key generation", ret);
    return false;
  }
  keyGenMs = millis() - t;

  t = millis();
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  unsigned char serial[8];
  mbedtls_ctr_drbg_random(&drbg, serial, sizeof(serial));
  serial[0] &= 0x7F; // positive number
  serial[0] |= 0x01; // never all zero
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &serverKey);
  mbedtls_x509write_crt_set_issuer_key(&crt, &serverKey);
  unsigned char *der = (unsigned char *)malloc(1024);
  ret = (der == NULL) ? MBEDTLS_ERR_X509_ALLOC_FAILED : 0;
  if (ret == 0) ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=heishamon.local,O=HeishaMon");
  if (ret == 0) ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=heishamon.local,O=HeishaMon");
  if (ret == 0) ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
  if (ret == 0) ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20450101000000");
  if (ret == 0) ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
  if (ret == 0) {
    ret = mbedtls_x509write_crt_der(&crt, der, 1024, mbedtls_ctr_drbg_random, &drbg);
    if (ret > 0) {
      int derLen = ret;
      ret = mbedtls_x509_crt_parse_der(&serverCert, der + 1024 - derLen, derLen);
      if (ret == 0) spikeSay("[TLSSPIKE] certificate: %d bytes DER", derLen);
    }
  }
  mbedtls_x509write_crt_free(&crt);
  free(der);
  if (ret != 0) {
    spikeError("certificate", ret);
    return false;
  }
  certGenMs = millis() - t;

  mbedtls_ssl_config_init(&sslConf);
  if ((ret = mbedtls_ssl_config_defaults(&sslConf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
    spikeError("ssl config", ret);
    return false;
  }
  mbedtls_ssl_conf_rng(&sslConf, mbedtls_ctr_drbg_random, &drbg);
  if ((ret = mbedtls_ssl_conf_own_cert(&sslConf, &serverCert, &serverKey)) != 0) {
    spikeError("own cert", ret);
    return false;
  }
  return true;
}

static bool spikeListen() {
  listenFd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd < 0) return false;
  int yes = 1;
  lwip_setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(443);
  if (lwip_bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || lwip_listen(listenFd, 2) != 0) {
    lwip_close(listenFd);
    listenFd = -1;
    return false;
  }
  lwip_fcntl(listenFd, F_SETFL, lwip_fcntl(listenFd, F_GETFL, 0) | O_NONBLOCK);
  return true;
}

static void spikeClose(const char *why) {
  spikeSay("[TLSSPIKE] session %lu closed (%s), %lu requests so far", sessions, why, requests);
  mbedtls_ssl_free(&ssl);
  if (clientFd >= 0) lwip_close(clientFd);
  clientFd = -1;
  spikeState = SPIKE_LISTEN;
}

static void spikeAccept() {
  int fd = lwip_accept(listenFd, NULL, NULL);
  if (fd < 0) return;
  lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  clientFd = fd;
  sessions++;
  heapBeforeSession = ESP.getFreeHeap();
  mbedtls_ssl_init(&ssl);
  int ret = mbedtls_ssl_setup(&ssl, &sslConf);
  if (ret != 0) {
    spikeError("ssl setup", ret);
    spikeClose("setup failed");
    return;
  }
  mbedtls_ssl_set_bio(&ssl, &clientFd, spikeSend, spikeRecv, NULL);
  hsStart = millis();
  hsMaxCallMs = 0;
  hsCalls = 0;
  lastActivity = millis();
  spikeState = SPIKE_HANDSHAKE;
}

static void spikeHandshake() {
  unsigned long t = millis();
  int ret = mbedtls_ssl_handshake(&ssl);
  unsigned long used = millis() - t;
  hsCalls++;
  if (used > hsMaxCallMs) hsMaxCallMs = used;
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    if (millis() - lastActivity > SPIKE_IDLE_MS) spikeClose("handshake timeout");
    return;
  }
  if (ret != 0) {
    spikeError("handshake", ret);
    spikeClose("handshake failed");
    return;
  }
  hsTotalMs = millis() - hsStart;
  heapAfterHandshake = ESP.getFreeHeap();
  if (hsTotalMs > hsWorstTotalMs) hsWorstTotalMs = hsTotalMs;
  if (hsMaxCallMs > hsWorstCallMs) hsWorstCallMs = hsMaxCallMs;
  spikeSay("[TLSSPIKE] handshake %lu ms, longest block %lu ms in %lu calls, %s %s, heap %lu -> %lu",
           hsTotalMs, hsMaxCallMs, hsCalls, mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl),
           (unsigned long)heapBeforeSession, (unsigned long)heapAfterHandshake);
  reqLen = 0;
  lastActivity = millis();
  spikeState = SPIKE_REQUEST;
}

static void spikeQueue(size_t len) {
  outLen = len;
  outSent = 0;
  spikeState = SPIKE_SEND;
}

static void spikeReport(const char *extra) {
  char body[900];
  int n = snprintf(body, sizeof(body),
                   "HeishaMon TLS measurement (throwaway build)\n"
                   "session: %lu  request: %lu\n"
                   "this session: handshake %lu ms, longest single block %lu ms, %lu handshake calls\n"
                   "worst so far: handshake %lu ms, longest single block %lu ms\n"
                   "protocol: %s  cipher: %s\n"
                   "heap: before session %lu, after handshake %lu, session cost %ld, now %lu, lowest ever %lu, largest block %lu\n"
                   "psram free: %lu\n"
                   "loop task stack: lowest free %lu bytes\n"
                   "longest gap between loop() passes: %lu ms since last report, %lu ms since boot\n"
                   "certificate at start: key %lu ms, certificate %lu ms\n"
                   "heat pump reads: %lu good of %lu\n"
                   "uptime: %lu s\n"
                   "%s",
                   sessions, requests, hsTotalMs, hsMaxCallMs, hsCalls, hsWorstTotalMs, hsWorstCallMs,
                   mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl),
                   (unsigned long)heapBeforeSession, (unsigned long)heapAfterHandshake,
                   (long)heapBeforeSession - (long)heapAfterHandshake, (unsigned long)ESP.getFreeHeap(),
                   (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                   (unsigned long)ESP.getFreePsram(),
                   (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                   loopGapMaxSinceReportMs, loopGapMaxMs, keyGenMs, certGenMs, goodreads, totalreads,
                   millis() / 1000, extra);
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof(body)) n = sizeof(body) - 1;
  loopGapMaxSinceReportMs = 0;
  int h = snprintf(outBuf, sizeof(outBuf),
                   "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", n);
  memcpy(outBuf + h, body, n);
  spikeQueue(h + n);
}

static void spikeRequestDone() {
  requests++;
  bool post = (strncmp(reqBuf, "POST ", 5) == 0);
  bool big = (strncmp(reqBuf, "GET /big", 8) == 0);
  char *end = strstr(reqBuf, "\r\n\r\n");
  size_t headerLen = (end - reqBuf) + 4;
  if (post) {
    bodyTotal = 0;
    for (char *p = reqBuf; p < end; p++) {
      if (strncasecmp(p, "\r\nContent-Length:", 17) == 0) {
        bodyTotal = strtoul(p + 17, NULL, 10);
        break;
      }
    }
    size_t already = reqLen - headerLen;
    bodyLeft = (bodyTotal > already) ? bodyTotal - already : 0;
    bodyStart = millis();
    reqLen = 0;
    if (bodyLeft > 0) {
      spikeState = SPIKE_BODY;
    } else {
      spikeReport("received: body fitted in the first read\n");
    }
  } else if (big) {
    reqLen = 0;
    bigLeft = SPIKE_BIG_BYTES;
    bigStart = millis();
    reportBig = true;
    int h = snprintf(outBuf, sizeof(outBuf),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lu\r\nConnection: keep-alive\r\n\r\n",
                     SPIKE_BIG_BYTES);
    spikeQueue(h);
  } else {
    reqLen = 0;
    spikeReport("");
  }
}

static void spikeRequest() {
  int ret = mbedtls_ssl_read(&ssl, (unsigned char *)reqBuf + reqLen, sizeof(reqBuf) - 1 - reqLen);
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    if (millis() - lastActivity > SPIKE_IDLE_MS) spikeClose("idle");
    return;
  }
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
  if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return;
#endif
  if (ret <= 0) {
    spikeClose(ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? "closed by client" : "read error");
    return;
  }
  lastActivity = millis();
  reqLen += ret;
  reqBuf[reqLen] = '\0';
  if (strstr(reqBuf, "\r\n\r\n") != NULL) {
    spikeRequestDone();
  } else if (reqLen >= sizeof(reqBuf) - 1) {
    spikeClose("request header too long");
  }
}

static void spikeBody() {
  unsigned char sink[SPIKE_CHUNK];
  int ret = mbedtls_ssl_read(&ssl, sink, sizeof(sink));
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    if (millis() - lastActivity > SPIKE_IDLE_MS) spikeClose("idle in body");
    return;
  }
  if (ret <= 0) {
    spikeClose("read error in body");
    return;
  }
  lastActivity = millis();
  bodyLeft = (bodyLeft > (unsigned long)ret) ? bodyLeft - ret : 0;
  if (bodyLeft == 0) {
    unsigned long ms = millis() - bodyStart;
    char extra[96];
    snprintf(extra, sizeof(extra), "received: %lu bytes in %lu ms (%lu kB/s)\n", bodyTotal, ms, ms ? bodyTotal / ms : 0);
    spikeSay("[TLSSPIKE] %s", extra);
    spikeReport(extra);
  }
}

static void spikeSendStep() {
  if (outSent < outLen) {
    int ret = mbedtls_ssl_write(&ssl, (unsigned char *)outBuf + outSent, outLen - outSent);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      if (millis() - lastActivity > SPIKE_IDLE_MS) spikeClose("idle in send");
      return;
    }
    if (ret <= 0) {
      spikeClose("write error");
      return;
    }
    lastActivity = millis();
    outSent += ret;
    return;
  }
  if (bigLeft > 0) {
    size_t n = (bigLeft > SPIKE_CHUNK) ? SPIKE_CHUNK : bigLeft;
    memset(outBuf, 'x', n);
    bigLeft -= n;
    outLen = n;
    outSent = 0;
    return;
  }
  if (reportBig) {
    reportBig = false;
    unsigned long ms = millis() - bigStart;
    spikeSay("[TLSSPIKE] sent %lu bytes in %lu ms (%lu kB/s)", SPIKE_BIG_BYTES, ms, ms ? SPIKE_BIG_BYTES / ms : 0);
  }
  spikeState = SPIKE_REQUEST;
}

void tlsSpikeLoop() {
  unsigned long now = millis();
  if (lastLoopCall != 0) {
    unsigned long gap = now - lastLoopCall;
    if (gap > loopGapMaxMs) loopGapMaxMs = gap;
    if (gap > loopGapMaxSinceReportMs) loopGapMaxSinceReportMs = gap;
  }
  lastLoopCall = now;

  switch (spikeState) {
    case SPIKE_OFF:
      if (now < SPIKE_START_MS) return;
      if (spikeMakeCert() && spikeListen()) {
        spikeState = SPIKE_LISTEN;
        spikeSay("[TLSSPIKE] listening on port 443, key %lu ms, certificate %lu ms, heap %lu", keyGenMs, certGenMs, (unsigned long)ESP.getFreeHeap());
      } else {
        spikeState = SPIKE_FAILED;
        spikeSay("[TLSSPIKE] could not start");
      }
      lastLoopCall = millis(); // do not count the start itself as a loop gap
      break;
    case SPIKE_LISTEN:
      spikeAccept();
      break;
    case SPIKE_HANDSHAKE:
      spikeHandshake();
      break;
    case SPIKE_REQUEST:
      spikeRequest();
      break;
    case SPIKE_BODY:
      spikeBody();
      break;
    case SPIKE_SEND:
      spikeSendStep();
      break;
    default:
      break;
  }
}

#endif // TLS_SPIKE_POSSIBLE
#endif // ESP32
