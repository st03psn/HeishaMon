/*
 * The boot button and the setup hotspot (the open access point
 * "HeishaMon-Setup"): the decisions only, no hardware. HeishaMon.ino reads
 * the pin and the network state, asks here and starts or stops the access
 * point. Host tests: tests/setupap/run_tests.py.
 *
 * Boot button (pin low while pressed, polled from loop()):
 *   - a short press (pressed 50 ms to 3 s, then released) asks for the setup
 *     hotspot, or stops the one a short press started;
 *   - held for more than 10 s: factory reset (as before);
 *   - released between 3 s and 10 s: nothing.
 *   Only time during which the pin was really watched counts: when loop()
 *   did not run for a while (WiFi scan, MQTT connect), that gap is not
 *   counted as "held", so a long blocking call can never turn a short press
 *   into a factory reset.
 *
 * Setup hotspot, only while the device has no network (no WiFi connection
 * and no Ethernet address):
 *   - factory state (no WiFi name stored, no Ethernet seen): open, else the
 *     first setup is impossible (setting "hotspot", on by default);
 *   - WiFi name stored: NOT by itself. The device keeps trying its WiFi. A
 *     short press of the boot button opens the hotspot until the network is
 *     back, until nobody was connected to it for 10 minutes, or 30 minutes
 *     after the press at the latest;
 *   - setting "hotspot_auto" (off by default, less secure): also whenever
 *     the network is lost, as older versions did;
 *   - WiFi name saved from the factory hotspot: the hotspot comes back like
 *     after a button press when that WiFi cannot be reached (wrong password).
 */

#ifndef _SETUPAP_H_
#define _SETUPAP_H_

#include <stdint.h>

#define BOOTBUTTON_SHORT_MIN_MS 50UL          // shorter: bounce or noise
#define BOOTBUTTON_SHORT_MAX_MS 3000UL        // a longer press is not a short press
#define BOOTBUTTON_FACTORY_RESET_MS 10000UL   // held longer: factory reset
#define BOOTBUTTON_REARM_MS 1000UL            // no second short press within this time (bounce of the release)
#define BOOTBUTTON_GAP_MS 1000UL              // two polls further apart: the time between them does not count as held

#define SETUPAP_IDLE_MS (10UL * 60UL * 1000UL)  // a requested hotspot stops when nobody was connected for this long
#define SETUPAP_MAX_MS (30UL * 60UL * 1000UL)   // and at the latest this long after the request

enum {
  BOOTBUTTON_NONE = 0,
  BOOTBUTTON_SHORT,          // short press, reported once at the release
  BOOTBUTTON_FACTORY_RESET   // reported once, while still held
};

struct bootbutton_t {
  uint8_t down;              // pressed at the last poll
  uint8_t fired;             // this press already had its factory reset
  uint8_t armed;             // 0: a short press happened less than BOOTBUTTON_REARM_MS ago
  unsigned long last;        // time of the last poll while pressed
  unsigned long held;        // watched time of this press
  unsigned long last_short;
};

/* one poll of the pin; returns BOOTBUTTON_* */
uint8_t bootbutton_update(struct bootbutton_t *b, bool pressed, unsigned long now);

enum {
  SETUPAP_OFF = 0,
  SETUPAP_FACTORY,   // nothing configured yet
  SETUPAP_AUTO,      // setting: automatically when the network is lost
  SETUPAP_BUTTON     // asked for at the device (or kept after a WiFi change made on the factory hotspot)
};

enum {
  SETUPAP_EVENT_NONE = 0,
  SETUPAP_EVENT_REQUESTED,   // short press: hotspot asked for
  SETUPAP_EVENT_CANCELLED,   // short press: the requested hotspot is stopped
  SETUPAP_EVENT_IGNORED,     // short press while the network is up: nothing to do
  SETUPAP_EVENT_KEPT,        // WiFi saved on the factory hotspot: kept like a request
  SETUPAP_EVENT_NETWORK,     // request over: the network is up
  SETUPAP_EVENT_TIMEOUT      // request over: nobody connected for SETUPAP_IDLE_MS, or SETUPAP_MAX_MS
};

struct setupap_input_t {
  uint8_t wifi_configured;   // a WiFi name is stored
  uint8_t net_up;            // WiFi connected or Ethernet has an address
  uint8_t eth_seen;          // Ethernet cable linked now, or Ethernet had an address since the start
  uint8_t allow_factory;     // setting "hotspot": open while nothing is configured
  uint8_t auto_on_loss;      // setting "hotspot_auto"
  uint8_t clients;           // stations connected to the hotspot
};

struct setupap_t {
  uint8_t requested;         // a request is running
  uint8_t reason;            // SETUPAP_* of the last update
  uint8_t event;             // SETUPAP_EVENT_* of the last update (one update long)
  unsigned long request_time;
  unsigned long seen_time;   // last time a station was connected (or the request)
};

/* a short press of the boot button; returns SETUPAP_EVENT_REQUESTED, _CANCELLED or _IGNORED */
uint8_t setupap_button(struct setupap_t *s, const struct setupap_input_t *in, unsigned long now);

/* should the hotspot be up now? returns SETUPAP_*, s->event says what changed */
uint8_t setupap_update(struct setupap_t *s, const struct setupap_input_t *in, unsigned long now);

#endif
