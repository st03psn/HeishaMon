/*
  TLS for the web server (ESP32 only): one mbedtls server configuration and
  the TLS state of each HTTPS connection.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.

  The web server (webserver.cpp) keeps one request path for HTTP and HTTPS:
  a TLS connection is a normal client slot whose bytes go through this
  module (httpstls_read() instead of the socket read, httpstls_write()
  instead of the socket send). The HTTP parser, webauth_request() and the
  routes do not know the difference, except for the tls flag of the
  request (Secure cookie, https Origin, redirect decision).

  Everything runs in the loop() task, non-blocking:
  - httpstls_conn_new() at accept: only below HTTPSTLS_MAX_CONN connections
    and when httpstls_admit() finds enough memory; NULL means "close the
    socket", nothing is half set up.
  - httpstls_handshake(): one step of the handshake per call. The web server
    makes at most one step per webserver_loop() pass over all connections,
    so loop() is blocked at most for the longest single step (the ECDHE and
    signature step, see the stats).
  - session resumption (session cache): the web server closes the
    connection after every answer, so without it every request of a page
    would need a full handshake.

  No Arduino calls here: the host tests (tests/https) run this code over real
  sockets. The firmware provides the httpstls_platform_ functions.
*/
#ifndef _HTTPSTLS_H_
#define _HTTPSTLS_H_

#include <stdint.h>
#include <stddef.h>

#define HTTPSTLS_PORT 443
#define HTTPSTLS_MAX_CONN 3                    // TLS connections at the same time (of WEBSERVER_MAX_CLIENTS: the rest stays for HTTP)
#define HTTPSTLS_HANDSHAKE_TIMEOUT_MS 10000UL  // a handshake that is not done by then is closed
#define HTTPSTLS_CACHE_ENTRIES 6               // sessions kept for resumption
#define HTTPSTLS_CACHE_TIMEOUT_S 3600          // and for how long
#define HTTPSTLS_FULL_MAX 6                    // full handshakes (ECC work) per window; resumed sessions do not count
#define HTTPSTLS_FULL_WINDOW_MS 10000UL
#define HTTPSTLS_RELOAD_FORCE_MS 12000UL       // a new certificate waits at most this long for open connections, then closes them
/*
 * Memory a connection needs (mbedtls 3.6 of the ESP32 core, fixed
 * MBEDTLS_SSL_MAX_CONTENT_LEN 16384): two record buffers of about 16.7 kB,
 * plus about 4-6 kB for the context, the handshake and the keys.
 */
#define HTTPSTLS_BUFFER_BYTES 17000UL          // one record buffer (in or out), one block
#define HTTPSTLS_CONN_BYTES 40960UL            // a connection with everything in internal RAM
#define HTTPSTLS_CONN_INTERNAL_BYTES 8192UL    // the internal RAM of a connection whose record buffers are in PSRAM
#define HTTPSTLS_INTERNAL_RESERVE 24576UL      // internal RAM that always stays free (WiFi, MQTT, websocket, serial)

/* Results of handshake, read and write */
#define HTTPSTLS_DONE 0
#define HTTPSTLS_AGAIN 1
#define HTTPSTLS_FAIL -1

typedef struct httpstls_stats_t {
  uint32_t handshakes;        // finished handshakes
  uint32_t failed;            // failed or timed out handshakes
  uint32_t refused_cap;       // connections closed at accept: HTTPSTLS_MAX_CONN reached
  uint32_t refused_memory;    // connections closed at accept: not enough memory
  uint32_t refused_rate;      // full handshakes refused after the ClientHello: HTTPSTLS_FULL_MAX per window reached
  uint32_t resumed;           // handshakes that resumed a session (no ECC work)
  uint32_t last_ms;           // duration of the last handshake (first byte to finished, with the network)
  uint32_t max_ms;
  uint32_t last_step_ms;      // longest single step of the last handshake (time loop() was blocked)
  uint32_t max_step_ms;       // longest single step of all handshakes
  int32_t last_error;         // mbedtls error of the last failed handshake (negative), for the log
  uint8_t active;             // TLS connections now
  uint8_t peak;               // most TLS connections at the same time
} httpstls_stats_t;

extern httpstls_stats_t httpstls_stats;

/*
 * May one more TLS connection start? active: TLS connections now.
 * internal_largest: largest free block of internal RAM. psram_largest:
 * largest free block of PSRAM (0 without). psram_buffers: mbedtls takes
 * large blocks from PSRAM (httpstls_use_psram() was called).
 */
bool httpstls_admit(uint8_t active, size_t internal_largest, size_t psram_largest, bool psram_buffers);

/*
 * Load the store file (httpscert.h) and set up the server configuration.
 * Returns HTTPSCERT_OK or an HTTPSCERT_ERR_ value; on an error nothing is
 * active (httpstls_ready() false).
 */
struct httpscert_info_t;
int httpstls_server_init(const char *store, size_t len, struct httpscert_info_t *info); // info may be NULL
void httpstls_server_free(void);   // only when no connection is open
bool httpstls_ready(void);

/* The random generator of this module (also used to make certificates) */
int httpstls_random(void *ctx, unsigned char *buf, size_t len);
void *httpstls_random_ctx(void);   // NULL when it could not be seeded

/*
 * mbedtls allocates through calloc()/free() from now on, so on the ESP32
 * blocks larger than CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 kB) go to PSRAM
 * (the default of the core is: everything internal). This affects every
 * mbedtls user (MQTT TLS, WiFi), so it is only called when HTTPS is on.
 */
void httpstls_use_calloc(void);

struct httpstls_conn_t;
struct httpstls_conn_t *httpstls_conn_new(int fd);  // NULL: refused or no memory, close the socket
void httpstls_conn_free(struct httpstls_conn_t *conn); // sends close_notify (best effort), frees; the socket stays for the caller
int httpstls_handshake(struct httpstls_conn_t *conn);  // one step: HTTPSTLS_DONE, HTTPSTLS_AGAIN or HTTPSTLS_FAIL
bool httpstls_handshake_done(const struct httpstls_conn_t *conn);
unsigned long httpstls_conn_age(const struct httpstls_conn_t *conn); // ms since httpstls_conn_new()
/* > 0: bytes read, 0: nothing now, HTTPSTLS_FAIL: closed or an error */
int httpstls_read(struct httpstls_conn_t *conn, unsigned char *buf, size_t len);
/*
 * > 0: bytes of buf taken, 0: not now, call again with the same data (buf
 * may have grown at the end in between), HTTPSTLS_FAIL: an error.
 */
int httpstls_write(struct httpstls_conn_t *conn, const unsigned char *buf, size_t len);

/*
 * Rate of full handshakes: may one more start at `now`? Counts it when yes.
 * Called after the ServerHello of a connection that did not resume a session,
 * before the expensive step; a refused one is closed without the ECC work.
 */
bool httpstls_full_allowed(unsigned long now);
void httpstls_full_reset(void);  // tests

/* Loading a new certificate: what to do now, `waited` ms after it was asked for */
enum {
  HTTPSTLS_RELOAD_WAIT = 0,   // close idle TLS connections, wait for the others
  HTTPSTLS_RELOAD_FORCE,      // waited long enough: close every TLS connection
  HTTPSTLS_RELOAD_NOW,        // no TLS connection open: load
};
uint8_t httpstls_reload_step(unsigned long waited, uint8_t active);

/* Implemented by the firmware (and the tests) */
unsigned long httpstls_platform_millis(void);

#endif
