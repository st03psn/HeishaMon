/*
  HTTPS for the web interface (ESP32 only), see httpsweb.h.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/
#include "httpsweb.h"
#include "webfunctions.h"
#include "src/common/progmem.h"
#include "src/common/webauth.h"
#include <LittleFS.h>
#if defined(ESP32)
  #include <WiFi.h>
  #include <ETH.h>
  #include <esp_heap_caps.h>
  #include "src/common/httpstls.h"
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif

extern settingsStruct heishamonSettings;
void log_message(char *string);

/* ---- POST /https: the fields of the forms, collected while the body comes in ---- */

struct https_form_t {
  char action[16];
  char https[12];
  char mode[4];
  char *cert;           // upload: the certificate (PEM), allocated on the first part
  size_t cert_len;
  char *key;            // upload: the private key (PEM)
  size_t key_len;
  uint8_t overflow;     // a field was longer than allowed
  uint8_t result;
};

static void field_add(char *dst, size_t size, const struct arguments_t *args, uint8_t *overflow) {
  size_t have = strlen(dst);
  if (have + args->len >= size) {
    *overflow = 1;
    return;
  }
  memcpy(dst + have, args->value, args->len);
  dst[have + args->len] = 0;
}

#if defined(ESP32)
static void blob_add(char **dst, size_t *len, size_t max, const struct arguments_t *args, uint8_t *overflow) {
  if (*len + args->len > max) {
    *overflow = 1;
    return;
  }
  if (*dst == NULL) {
    *dst = (char *)malloc(max + 1);  // above 4 kB: PSRAM on the ESP32
    if (*dst == NULL) {
      *overflow = 1;
      return;
    }
  }
  memcpy(*dst + *len, args->value, args->len);
  *len += args->len;
  (*dst)[*len] = 0;
}

#endif

static void wipe_free(char *p, size_t len) {
  if (p != NULL) {
    volatile char *v = p;
    for (size_t i = 0; i < len; i++) {
      v[i] = 0;
    }
    free(p);
  }
}

int httpsFormStart(struct webserver_t *client) {
  struct https_form_t *f = (struct https_form_t *)calloc(1, sizeof(struct https_form_t));
  if (f == NULL) {
    return -1;
  }
  client->userdata = f;
  return 0;
}

int httpsFormArgs(struct webserver_t *client, struct arguments_t *args) {
  struct https_form_t *f = (struct https_form_t *)client->userdata;
  if (f == NULL || args->name == NULL || args->value == NULL) {
    return 0;
  }
  const char *n = (const char *)args->name;
  if (strcmp_P(n, PSTR("action")) == 0) {
    field_add(f->action, sizeof(f->action), args, &f->overflow);
  } else if (strcmp_P(n, PSTR("https")) == 0) {
    field_add(f->https, sizeof(f->https), args, &f->overflow);
  } else if (strcmp_P(n, PSTR("http_mode")) == 0) {
    field_add(f->mode, sizeof(f->mode), args, &f->overflow);
#if defined(ESP32)  // the ESP8266 has no RAM to spare for an upload it cannot use
  } else if (strcmp_P(n, PSTR("cert")) == 0) {
    blob_add(&f->cert, &f->cert_len, HTTPSCERT_CERT_MAX, args, &f->overflow);
  } else if (strcmp_P(n, PSTR("key")) == 0) {
    blob_add(&f->key, &f->key_len, HTTPSCERT_KEY_MAX, args, &f->overflow);
#endif
  }
  return 0;
}

void httpsFormFree(struct webserver_t *client) {
  struct https_form_t *f = (struct https_form_t *)client->userdata;
  if (f != NULL) {
    free(f->cert);
    wipe_free(f->key, f->key_len);
    free(f);
    client->userdata = NULL;
  }
}

const char *httpsMessage(uint8_t msg, bool *good) {
  *good = true;
  switch (msg) {
    case HTTPS_MSG_SAVED: return PSTR("Saved.");
    case HTTPS_MSG_SAVED_RESTART: return PSTR("Saved. HTTPS starts as soon as the network is up (or stops taking new connections).");
    case HTTPS_MSG_NEW_CERT: return PSTR("A new certificate was made. The browser warns again: compare the fingerprint below and accept it.");
    case HTTPS_MSG_UPLOADED: return PSTR("Your certificate is stored and used for new connections.");
  }
  *good = false;
  switch (msg) {
    case HTTPS_MSG_SAVE_FAILED: return PSTR("Could not write to the flash: nothing was changed.");
    case HTTPS_MSG_INVALID: return PSTR("Invalid input.");
    case HTTPS_MSG_NOT_AVAILABLE: return PSTR("HTTPS is only available on the large board (ESP32-S3).");
    case HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_MEMORY: return PSTR("Upload refused: out of memory. The old certificate stays.");
    case HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_CERT: return PSTR("Upload refused: the certificate cannot be read (PEM expected). The old certificate stays.");
    case HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_KEY: return PSTR("Upload refused: the private key cannot be read (PEM without a password expected). The old certificate stays.");
    case HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_MISMATCH: return PSTR("Upload refused: the private key does not belong to the certificate. The old certificate stays.");
    case HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_KEYTYPE: return PSTR("Upload refused: only EC P-256 and RSA 2048 keys are supported. The old certificate stays.");
    case HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_SIZE: return PSTR("Upload refused: certificate or key too large. The old certificate stays.");
  }
  if (msg >= HTTPS_MSG_UPLOAD_ERROR) {
    return PSTR("Upload refused. The old certificate stays.");
  }
  return NULL;
}

#if defined(ESP32)

static void saveHttpsSettings(void) {
  JsonDocument jsonDoc;
  settingsToJson(jsonDoc, &heishamonSettings);
  saveJsonToFile(jsonDoc, "/config.json");
}

static https_status_t state;          // what the page and /getsettings show (never the key)
static bool callocInstalled = false;
static bool reloadPending = false;    // a new store file waits for the open TLS connections to end
static unsigned long lastTry = 0;
static bool triedOnce = false;
#define HTTPS_RETRY_MS 60000UL

unsigned long httpstls_platform_millis(void) {
  return millis();
}

bool webserver_tls_admit(void) {
  size_t internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t psram = psramFound() ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : 0;
  return httpstls_admit(httpstls_stats.active, internal, psram, callocInstalled && psram > 0);
}

static bool networkUp(void) {
  return (uint32_t)WiFi.localIP() != 0 || ETH.hasIP();
}

static uint8_t deviceIps(uint32_t *ips) {
  uint8_t n = 0;
  if ((uint32_t)WiFi.localIP() != 0) {
    ips[n++] = (uint32_t)WiFi.localIP();
  }
  if (ETH.hasIP()) {
    ips[n++] = (uint32_t)ETH.localIP();
  }
  return n;
}

/* The whole store file into a buffer (PSRAM when there is one); NULL when there is none */
static char *readStore(size_t *len) {
  *len = 0;
  if (!LittleFS.exists(HTTPS_FILE)) {
    return NULL;
  }
  File f = LittleFS.open(HTTPS_FILE, "r");
  if (!f) {
    return NULL;
  }
  size_t size = f.size();
  char *buf = (size > 0 && size <= HTTPSCERT_FILE_MAX) ? (char *)malloc(size + 1) : NULL;
  if (buf != NULL) {
    if (f.read((uint8_t *)buf, size) != size) {
      free(buf);
      buf = NULL;
    } else {
      buf[size] = 0;
      *len = size;
    }
  }
  f.close();
  return buf;
}

/* Temporary file, then rename: a power cut leaves the old or the new file, never half of one */
static bool writeStore(const char *data, size_t len) {
  bool ok = false;
  File f = LittleFS.open(HTTPS_FILE_TMP, "w");
  if (f) {
    ok = f.write((const uint8_t *)data, len) == len;
    f.close();
    if (ok) {
      LittleFS.remove(HTTPS_FILE);
      ok = LittleFS.rename(HTTPS_FILE_TMP, HTTPS_FILE);
    } else {
      LittleFS.remove(HTTPS_FILE_TMP);
    }
  }
  if (!ok) {
    log_message(_F("HTTPS: could not write " HTTPS_FILE));
  }
  return ok;
}

static int generateStore(void) {
  void *rng = httpstls_random_ctx();
  if (rng == NULL) {
    return HTTPSCERT_ERR_GENERATE;
  }
  uint32_t ips[HTTPSCERT_IPS_MAX];
  uint8_t nips = deviceIps(ips);
  httpscert_params_t p;
  httpscert_params(&p, heishamonSettings.wifi_hostname, ips, nips, time(NULL));
  char *out = (char *)malloc(HTTPSCERT_FILE_MAX);
  if (out == NULL) {
    return HTTPSCERT_ERR_MEMORY;
  }
  unsigned long t = millis();
  int n = httpscert_generate(&p, httpstls_random, rng, out, HTTPSCERT_FILE_MAX);
  char msg[96];
  snprintf_P(msg, sizeof(msg), PSTR("HTTPS: new self-signed certificate for %s: %s, %lu ms"), p.host, n > 0 ? "ok" : httpscert_error_text(n), millis() - t);
  log_message(msg);
  int r = n;
  if (n > 0) {
    r = writeStore(out, (size_t)n) ? HTTPSCERT_OK : HTTPSCERT_ERR_MEMORY;
  }
  memset(out, 0, HTTPSCERT_FILE_MAX);  // the private key
  free(out);
  return r;
}

/* Load the store file into the TLS server (only while no TLS connection is open) */
static bool loadServer(void) {
  size_t len = 0;
  char *store = readStore(&len);
  int r = HTTPSCERT_ERR_FORMAT;
  if (store != NULL) {
    r = httpscert_parse(store, len, httpstls_random, httpstls_random_ctx(), NULL, NULL, &state.info);
    if (r == HTTPSCERT_OK) {
      r = httpstls_server_init(store, len);
    }
    memset(store, 0, len);
    free(store);
  }
  state.cert = (r == HTTPSCERT_OK);
  if (!state.cert) {
    memset(&state.info, 0, sizeof(state.info));
    char msg[96];
    snprintf_P(msg, sizeof(msg), PSTR("HTTPS: certificate not usable: %s"), httpscert_error_text(r));
    log_message(msg);
  }
  return state.cert;
}

/* Start: certificate (made when there is none, or for an old hostname), server, port */
static void startHttps(void) {
  if (!LittleFS.begin()) {
    return;
  }
  if (!LittleFS.exists(HTTPS_FILE) && LittleFS.exists(HTTPS_FILE_TMP)) {
    LittleFS.rename(HTTPS_FILE_TMP, HTTPS_FILE);  // power lost between remove and rename
  }
  uint32_t heapBefore = ESP.getFreeHeap();
  if (!LittleFS.exists(HTTPS_FILE)) {
    generateStore();
  }
  if (loadServer() && httpscert_needs_new(&state.info, heishamonSettings.wifi_hostname)) {
    log_message(_F("HTTPS: the hostname changed, making a new certificate"));
    httpstls_server_free();
    generateStore();
    loadServer();
  }
  if (!state.cert) {
    // a damaged or foreign file: make a new one (an uploaded one that broke is replaced, the log says so)
    log_message(_F("HTTPS: making a new certificate instead of the stored one"));
    generateStore();
    loadServer();
  }
  if (state.cert && webserver_tls_start(HTTPSTLS_PORT) == 0) {
    state.running = true;
  }
  char msg[128];
  snprintf_P(msg, sizeof(msg), PSTR("HTTPS: %s, heap %u -> %u (internal), PSRAM for TLS buffers: %s"),
             state.running ? "running on port 443" : "not started", (unsigned int)heapBefore, (unsigned int)ESP.getFreeHeap(),
             callocInstalled && psramFound() ? "yes" : "no");
  log_message(msg);
}

void httpsSetup(void) {
  state.available = true;
  state.enabled = heishamonSettings.https;
  state.http_mode = webauth_http_mode(heishamonSettings.http_mode);
  if (!heishamonSettings.https) {
    return;  // nothing changes for an installation without HTTPS
  }
  httpstls_use_calloc();  // record buffers (2 x 16.7 kB per connection) to PSRAM; also for MQTT TLS
  callocInstalled = true;
}

void httpsLoop(void) {
  state.enabled = heishamonSettings.https;
  state.http_mode = webauth_http_mode(heishamonSettings.http_mode);
  if (reloadPending && httpstls_stats.active == 0) {
    reloadPending = false;
    state.pending = false;
    httpstls_server_free();
    if (loadServer() && heishamonSettings.https && webserver_tls_start(HTTPSTLS_PORT) == 0) {
      state.running = true;
    }
    return;
  }
  if (!heishamonSettings.https) {
    if (state.running) {
      webserver_tls_stop();  // open connections end normally
      state.running = false;
    }
    return;
  }
  // the hostname was changed while HTTPS runs: a new self-signed certificate (once per name)
  static char checkedHost[sizeof(heishamonSettings.wifi_hostname)] = "";
  if (state.running && state.cert && strcmp(checkedHost, heishamonSettings.wifi_hostname) != 0) {
    snprintf(checkedHost, sizeof(checkedHost), "%s", heishamonSettings.wifi_hostname);
    if (httpscert_needs_new(&state.info, heishamonSettings.wifi_hostname)) {
      log_message(_F("HTTPS: the hostname changed, making a new certificate"));
      if (generateStore() == HTTPSCERT_OK) {
        reloadPending = true;
        webserver_tls_stop();
        state.running = false;
      }
      return;
    }
  }
  if (!state.running && !reloadPending && networkUp() && (!triedOnce || (unsigned long)(millis() - lastTry) > HTTPS_RETRY_MS)) {
    triedOnce = true;
    lastTry = millis();
    if (!callocInstalled) {
      httpstls_use_calloc();  // switched on at runtime
      callocInstalled = true;
    }
    if (httpstls_stats.active == 0) {
      startHttps();
    }
  }
}

bool httpsRunning(void) {
  return state.running && webserver_tls_running();
}

bool httpsClientOnHotspot(struct webserver_t *client) {
  if (!(WiFi.getMode() & WIFI_AP) || client == NULL || client->client == NULL) {
    return false;
  }
  // the local address of the socket: a LAN client cannot fake it (Host header or 192.168.4.x as source can be)
  return client->client->localIP() == WiFi.softAPIP();
}

void httpsStatus(https_status_t *out) {
  *out = state;
  out->running = httpsRunning();
  out->pending = reloadPending;
}

void httpsSettingsJson(JsonDocument &doc) {
  doc[F("https")] = state.enabled ? "enabled" : "disabled";
  doc[F("http_mode")] = state.http_mode;
  doc[F("https_running")] = httpsRunning() ? 1 : 0;
  if (state.cert) {
    doc[F("https_cert_source")] = state.info.generated ? "generated" : "uploaded";
    doc[F("https_cert_subject")] = state.info.subject;
    doc[F("https_cert_valid_from")] = state.info.not_before;
    doc[F("https_cert_valid_to")] = state.info.not_after;
    doc[F("https_cert_fingerprint")] = state.info.fingerprint;
    doc[F("https_cert_key")] = state.info.key_type;
  }
}

void httpsStatsText(char *out, size_t size) {
  snprintf_P(out, size,
             PSTR("TLS connections now %u (most %u, at most %u at a time); handshakes %lu, failed %lu, refused (limit) %lu, refused (memory) %lu; "
                  "last handshake %lu ms, longest %lu ms; longest single step (loop() blocked) last %lu ms, ever %lu ms; "
                  "free internal RAM %u (largest block %u), free PSRAM %u"),
             httpstls_stats.active, httpstls_stats.peak, HTTPSTLS_MAX_CONN, (unsigned long)httpstls_stats.handshakes,
             (unsigned long)httpstls_stats.failed, (unsigned long)httpstls_stats.refused_cap, (unsigned long)httpstls_stats.refused_memory,
             (unsigned long)httpstls_stats.last_ms, (unsigned long)httpstls_stats.max_ms, (unsigned long)httpstls_stats.last_step_ms,
             (unsigned long)httpstls_stats.max_step_ms, (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned int)ESP.getFreePsram());
}

uint8_t httpsFormProcess(struct webserver_t *client) {
  struct https_form_t *f = (struct https_form_t *)client->userdata;
  if (f == NULL) {
    return HTTPS_MSG_INVALID;
  }
  if (f->overflow) {
    // a too large upload is a size error, not a crash
    f->result = (f->cert_len > 0 || f->key_len > 0 || strcmp_P(f->action, PSTR("upload")) == 0) ? (uint8_t)(HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_SIZE) : (uint8_t)HTTPS_MSG_INVALID;
    return f->result;
  }
  if (strcmp_P(f->action, PSTR("settings")) == 0) {
    bool on = strcmp_P(f->https, PSTR("enabled")) == 0;
    char *end = NULL;
    long mode = strtol(f->mode, &end, 10);
    if (f->mode[0] == 0 || *end != 0 || webauth_http_mode(mode) != mode) {
      f->result = HTTPS_MSG_INVALID;
      return f->result;
    }
    bool changed = on != heishamonSettings.https;
    heishamonSettings.https = on;
    heishamonSettings.http_mode = (uint8_t)mode;
    saveHttpsSettings();
    if (changed) {
      triedOnce = false;  // httpsLoop() starts or stops it
    }
    f->result = changed ? HTTPS_MSG_SAVED_RESTART : HTTPS_MSG_SAVED;
    return f->result;
  }
  if (strcmp_P(f->action, PSTR("regenerate")) == 0) {
    // made now (a few hundred ms); loaded when the TLS connections (this one, too) have ended
    int r = generateStore();
    if (r == HTTPSCERT_OK) {
      reloadPending = true;
      webserver_tls_stop();
      state.running = false;
      f->result = HTTPS_MSG_NEW_CERT;
    } else {
      f->result = HTTPS_MSG_SAVE_FAILED;
    }
    return f->result;
  }
  if (strcmp_P(f->action, PSTR("upload")) == 0) {
    char *out = (char *)malloc(HTTPSCERT_FILE_MAX);
    if (out == NULL) {
      f->result = HTTPS_MSG_UPLOAD_ERROR - HTTPSCERT_ERR_MEMORY;
      return f->result;
    }
    int n = httpscert_from_upload(f->cert, f->cert_len, f->key, f->key_len, httpstls_random, httpstls_random_ctx(), out, HTTPSCERT_FILE_MAX);
    if (n > 0) {
      if (writeStore(out, (size_t)n)) {
        reloadPending = true;
        webserver_tls_stop();
        state.running = false;
        f->result = HTTPS_MSG_UPLOADED;
        log_message(_F("HTTPS: own certificate stored"));
      } else {
        f->result = HTTPS_MSG_SAVE_FAILED;
      }
    } else {
      f->result = (uint8_t)(HTTPS_MSG_UPLOAD_ERROR - n);
      char msg[128];
      snprintf_P(msg, sizeof(msg), PSTR("HTTPS: upload refused, %s"), httpscert_error_text(n));
      log_message(msg);
    }
    memset(out, 0, HTTPSCERT_FILE_MAX);
    free(out);
    return f->result;
  }
  f->result = HTTPS_MSG_INVALID;
  return f->result;
}

#else  // ESP8266: no HTTPS

void httpsSetup(void) {}
void httpsLoop(void) {}
bool httpsRunning(void) {
  return false;
}
bool httpsClientOnHotspot(struct webserver_t *client) {
  return false;
}
void httpsStatus(https_status_t *out) {
  memset(out, 0, sizeof(*out));
}
void httpsSettingsJson(JsonDocument &doc) {}
void httpsStatsText(char *out, size_t size) {
  if (size > 0) {
    out[0] = 0;
  }
}
uint8_t httpsFormProcess(struct webserver_t *client) {
  struct https_form_t *f = (struct https_form_t *)client->userdata;
  if (f != NULL) {
    f->result = HTTPS_MSG_NOT_AVAILABLE;
  }
  return HTTPS_MSG_NOT_AVAILABLE;
}

#endif

uint8_t httpsFormResult(struct webserver_t *client) {
  struct https_form_t *f = (struct https_form_t *)client->userdata;
  return f ? f->result : (uint8_t)HTTPS_MSG_INVALID;
}
