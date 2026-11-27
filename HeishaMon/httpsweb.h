/*
  HTTPS for the web interface: settings, certificate store and the glue
  between the firmware and src/common/httpscert.* / httpstls.*.

  Only the ESP32 (large board) has HTTPS. The ESP8266 has about 40 kB of
  free RAM in all, and one TLS server connection needs about 40 kB
  (two 16 kB record buffers): its /https page says so, nothing else of
  this is compiled there.
*/
#ifndef _HTTPSWEB_H_
#define _HTTPSWEB_H_

#include <stdint.h>
#include <ArduinoJson.h>
#include "src/common/webserver.h"
#include "src/common/httpscert.h"

#define HTTPS_FILE "/https.pem"      // certificate and private key (httpscert.h store file)
#define HTTPS_FILE_TMP "/https.tmp"

/* Results of POST /https, shown on the page (/https?m=<n>) */
enum {
  HTTPS_MSG_NONE = 0,
  HTTPS_MSG_SAVED,
  HTTPS_MSG_SAVED_RESTART,      // saved; HTTPS starts when the network is up / stops for new connections
  HTTPS_MSG_NEW_CERT,
  HTTPS_MSG_UPLOADED,
  HTTPS_MSG_SAVE_FAILED,
  HTTPS_MSG_INVALID,
  HTTPS_MSG_NOT_AVAILABLE,      // ESP8266
  HTTPS_MSG_UPLOAD_ERROR = 20,  // + (-HTTPSCERT_ERR_x): the upload was refused, the old certificate stays
};

typedef struct https_status_t {
  bool available;     // this board has HTTPS
  bool enabled;       // setting
  uint8_t http_mode;  // WEBAUTH_HTTP_*
  bool running;       // the HTTPS port is open
  bool cert;          // a valid certificate is loaded
  bool pending;       // a new certificate waits for the open TLS connections to end
  httpscert_info_t info;
} https_status_t;

void httpsSetup(void);             // setup(), after loadSettings(): with HTTPS on, mbedtls allocates through calloc (PSRAM)
void httpsLoop(void);              // loop(): starts HTTPS once the network is up, loads a new certificate
bool httpsRunning(void);           // for webauth_env_t.https_on
bool httpsClientOnHotspot(struct webserver_t *client); // for webauth_env_t.hotspot
void httpsStatus(https_status_t *out);
void httpsSettingsJson(JsonDocument &doc);  // /getsettings: no key, only what the certificate says
void httpsStatsText(char *out, size_t size); // the measurements, for the page

/* POST /https (route WEBAUTH_ROUTE_HTTPS_POST) */
int httpsFormStart(struct webserver_t *client);                          // REQUEST accepted; -1: no memory
int httpsFormArgs(struct webserver_t *client, struct arguments_t *args); // body fields (also in parts)
uint8_t httpsFormProcess(struct webserver_t *client);                    // write phase: does it, HTTPS_MSG_*
void httpsFormFree(struct webserver_t *client);                          // idempotent
uint8_t httpsFormResult(struct webserver_t *client);                     // for the Location header
const char *httpsMessage(uint8_t msg, bool *good);                       // PROGMEM text or NULL

#endif
