/*
 * Host test of the boot button and setup hotspot decisions
 * (HeishaMon/src/common/setupap.cpp). Run: python3 tests/setupap/run_tests.py
 */

#include <climits>
#include <initializer_list>
#include <cstdio>
#include <cstring>

#include "setupap.h"

static int checks = 0, failed = 0;
#define CHECK(cond, ...) \
  do { \
    checks++; \
    if(!(cond)) { \
      failed++; \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while(0)

/* ---- boot button ---- */

struct press_result_t {
  int shorts, resets;
  unsigned long reset_at;  // time since the press began
};

/* polls every `step` ms: pressed from 0 for `hold` ms, then released for 2 s */
static press_result_t press(bootbutton_t *b, unsigned long *now, unsigned long hold, unsigned long step = 5) {
  press_result_t r = { 0, 0, 0 };
  unsigned long start = *now;
  for(unsigned long t = 0; t <= hold + 2000; t += step) {
    *now = start + t;
    uint8_t e = bootbutton_update(b, t < hold, *now);
    if(e == BOOTBUTTON_SHORT) {
      r.shorts++;
    } else if(e == BOOTBUTTON_FACTORY_RESET) {
      r.resets++;
      r.reset_at = t;
    }
  }
  return r;
}

static void test_button() {
  bootbutton_t b;
  unsigned long now = 1000;
  press_result_t r;

  // nothing pressed: nothing happens, also at the very first polls and for a long time
  memset(&b, 0, sizeof(b));
  for(int i = 0; i < 100000; i++) {
    now += 7;
    CHECK(bootbutton_update(&b, false, now) == BOOTBUTTON_NONE, "idle");
  }

  // short presses of every sensible length: exactly one event, never a reset
  for(unsigned long hold : { 60UL, 100UL, 300UL, 1000UL, 2500UL, 2990UL }) {
    memset(&b, 0, sizeof(b));
    r = press(&b, &now, hold);
    CHECK(r.shorts == 1 && r.resets == 0, "hold %lu: %d short, %d resets", hold, r.shorts, r.resets);
  }
  // too short (bounce, noise): nothing
  for(unsigned long hold : { 5UL, 20UL, 45UL }) {
    memset(&b, 0, sizeof(b));
    r = press(&b, &now, hold);
    CHECK(r.shorts == 0 && r.resets == 0, "hold %lu: %d short, %d resets", hold, r.shorts, r.resets);
  }
  // between "short" and "factory reset": nothing at all (somebody changed his mind)
  for(unsigned long hold : { 3100UL, 5000UL, 8000UL, 9900UL }) {
    memset(&b, 0, sizeof(b));
    r = press(&b, &now, hold);
    CHECK(r.shorts == 0 && r.resets == 0, "hold %lu: %d short, %d resets", hold, r.shorts, r.resets);
  }
  // held for more than 10 s: one factory reset while still held, no short press at the release
  for(unsigned long hold : { 10100UL, 15000UL, 60000UL }) {
    memset(&b, 0, sizeof(b));
    r = press(&b, &now, hold);
    CHECK(r.shorts == 0 && r.resets == 1, "hold %lu: %d short, %d resets", hold, r.shorts, r.resets);
    CHECK(r.reset_at > BOOTBUTTON_FACTORY_RESET_MS && r.reset_at <= BOOTBUTTON_FACTORY_RESET_MS + 100, "reset after %lu ms", r.reset_at);
  }

  // a bouncing release must not give a second short press (it would stop the hotspot again)
  memset(&b, 0, sizeof(b));
  int shorts = 0;
  for(unsigned long t = 0; t < 3000; t++) {
    now++;
    bool pressed = t < 300 || (t >= 302 && t < 380) || (t >= 400 && t < 470);  // 300 ms press, then two bounces of 70-80 ms
    shorts += bootbutton_update(&b, pressed, now) == BOOTBUTTON_SHORT;
  }
  CHECK(shorts == 1, "bounce: %d short presses", shorts);
  // a second press a few seconds later counts again
  r = press(&b, &now, 200);
  CHECK(r.shorts == 1, "second press: %d", r.shorts);

  // many short presses in a row never add up to a factory reset
  memset(&b, 0, sizeof(b));
  int resets = 0;
  for(int i = 0; i < 200; i++) {
    r = press(&b, &now, 2900);
    resets += r.resets;
  }
  CHECK(resets == 0, "200 short presses: %d resets", resets);

  // loop() did not run for a while (WiFi scan, MQTT connect): the gap never counts as held.
  // The old code compared with the last time the pin was seen high: one poll before a 12 s
  // gap and a tap at the first poll after it was a factory reset.
  memset(&b, 0, sizeof(b));
  CHECK(bootbutton_update(&b, false, now) == BOOTBUTTON_NONE, "before the gap");
  now += 12000;
  CHECK(bootbutton_update(&b, true, now) == BOOTBUTTON_NONE, "pressed right after a 12 s gap");
  now += 100;
  CHECK(bootbutton_update(&b, true, now) == BOOTBUTTON_NONE, "still pressed");
  now += 5;
  CHECK(bootbutton_update(&b, false, now) == BOOTBUTTON_SHORT, "short press after the gap");
  // pressed, then a 12 s gap, pin low again at the next poll (pressed twice, or held): not a reset yet
  memset(&b, 0, sizeof(b));
  now += 5000;
  bootbutton_update(&b, true, now);
  now += 12000;
  CHECK(bootbutton_update(&b, true, now) == BOOTBUTTON_NONE, "low before and after a 12 s gap");
  // ... it still needs 10 s of watched time
  r.resets = 0;
  unsigned long watched = 0;
  while(watched < 20000 && r.resets == 0) {
    now += 10;
    watched += 10;
    r.resets += bootbutton_update(&b, true, now) == BOOTBUTTON_FACTORY_RESET;
  }
  CHECK(r.resets == 1 && watched > 10000 && watched <= 10100, "reset after %lu ms of watched time", watched);
  // a press whose release fell into a gap: the length is unknown, so no short press
  memset(&b, 0, sizeof(b));
  now += 5000;
  bootbutton_update(&b, true, now);
  now += 60;
  bootbutton_update(&b, true, now);
  now += 2500;
  CHECK(bootbutton_update(&b, false, now) == BOOTBUTTON_NONE, "release inside a gap");
  // a hold with a 2 s gap in it (the WiFi retry of the ESP8266) still becomes a reset, 2 s later
  memset(&b, 0, sizeof(b));
  now += 5000;
  unsigned long start = now;
  int fired = 0;
  for(unsigned long t = 0; t < 30000 && !fired; t += (t == 4000 ? 2000 : 10)) {  // one 2 s gap after 4 s
    now = start + t;
    fired = bootbutton_update(&b, true, now) == BOOTBUTTON_FACTORY_RESET;
  }
  CHECK(fired && now - start > 12000 && now - start <= 12100, "hold with gaps: reset after %lu ms", now - start);

  // across the wrap of millis()
  memset(&b, 0, sizeof(b));
  now = ULONG_MAX - 100;
  r = press(&b, &now, 400);
  CHECK(r.shorts == 1 && r.resets == 0, "short press across the wrap: %d %d", r.shorts, r.resets);
  memset(&b, 0, sizeof(b));
  now = ULONG_MAX - 5000;
  r = press(&b, &now, 12000);
  CHECK(r.shorts == 0 && r.resets == 1, "long press across the wrap: %d %d", r.shorts, r.resets);
}

/* ---- setup hotspot ---- */

static setupap_input_t input(bool configured, bool net_up, bool allow_factory = true, bool auto_on_loss = false, uint8_t clients = 0, bool eth_seen = false) {
  setupap_input_t in;
  in.wifi_configured = configured;
  in.net_up = net_up;
  in.eth_seen = eth_seen;
  in.allow_factory = allow_factory;
  in.auto_on_loss = auto_on_loss;
  in.clients = clients;
  return in;
}

static void test_hotspot() {
  setupap_t s;
  setupap_input_t in;
  unsigned long now = 5000;

  // 1. factory state: open, else the first setup is impossible
  memset(&s, 0, sizeof(s));
  in = input(false, false);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_FACTORY, "factory state");
  now += 24UL * 3600 * 1000;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_FACTORY, "factory state has no timeout");
  // ... unless switched off (ethernet install), or ethernet is there
  in = input(false, false, false);
  memset(&s, 0, sizeof(s));
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "factory hotspot switched off");
  in = input(false, false, true, false, 0, true);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "no WiFi stored, ethernet cable linked or seen before");
  in = input(false, true, true, false, 0, true);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "no WiFi stored, ethernet up");

  // 2. configured: never by itself, however long the WiFi is gone
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  for(int i = 0; i < 100000; i++) {
    now += 1000;
    CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "WiFi lost for %d s", i);
    if(failed) {
      break;
    }
  }
  in = input(true, true);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "WiFi connected");
  // the setting that only covers the factory state changes nothing here
  for(int allow = 0; allow < 2; allow++) {
    in = input(true, false, allow);
    CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "configured, hotspot=%d", allow);
  }

  // 3. the old behaviour is an explicit setting
  memset(&s, 0, sizeof(s));
  in = input(true, false, true, true);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_AUTO, "automatic when lost");
  in = input(true, true, true, true);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "automatic, network up");
  in = input(false, false, true, true, 0, true);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_AUTO, "automatic, ethernet install without its cable");

  // 4. the button: up until nobody was connected for 10 minutes
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  CHECK(setupap_button(&s, &in, now) == SETUPAP_EVENT_REQUESTED, "request");
  unsigned long start = now;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_BUTTON, "requested");
  now = start + SETUPAP_IDLE_MS - 1;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_BUTTON && s.event == SETUPAP_EVENT_NONE, "just before the timeout");
  now = start + SETUPAP_IDLE_MS;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF && s.event == SETUPAP_EVENT_TIMEOUT, "timeout without a client");
  now += 1000;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF && s.event == SETUPAP_EVENT_NONE, "stays off, the event is reported once");

  // a connected client keeps it open, but never longer than 30 minutes after the press
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  setupap_button(&s, &in, now);
  start = now;
  in.clients = 1;
  unsigned long up = 0;
  while(setupap_update(&s, &in, now) == SETUPAP_BUTTON) {
    now += 1000;
    up = now - start;
    if(up > 4 * SETUPAP_MAX_MS) {
      break;
    }
  }
  CHECK(up == SETUPAP_MAX_MS, "with a client connected all the time: up for %lu s", up / 1000);
  // client leaves after 5 minutes: 10 more minutes
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  setupap_button(&s, &in, now);
  start = now;
  up = 0;
  for(;;) {
    in.clients = (now - start) <= 5UL * 60 * 1000;
    if(setupap_update(&s, &in, now) != SETUPAP_BUTTON) {
      break;
    }
    now += 1000;
    up = now - start;
  }
  CHECK(up == 15UL * 60 * 1000, "client left after 5 min: up for %lu s", up / 1000);

  // the network is back: over, and it does not come back when the network is lost again
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  setupap_button(&s, &in, now);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_BUTTON, "requested");
  in.net_up = 1;
  now += 20000;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF && s.event == SETUPAP_EVENT_NETWORK, "network back");
  in.net_up = 0;
  now += 20000;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF && s.event == SETUPAP_EVENT_NONE, "lost again: no hotspot without a new press");

  // a second press stops it; a press while the network is up does nothing
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  setupap_button(&s, &in, now);
  CHECK(setupap_button(&s, &in, now + 5000) == SETUPAP_EVENT_CANCELLED, "second press");
  CHECK(setupap_update(&s, &in, now + 5000) == SETUPAP_OFF, "cancelled");
  in = input(true, true);
  CHECK(setupap_button(&s, &in, now) == SETUPAP_EVENT_IGNORED, "press while the network is up");
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "ignored");
  in.net_up = 0;
  CHECK(setupap_update(&s, &in, now + 1000) == SETUPAP_OFF, "the ignored press is not remembered");
  // the button also works when the factory hotspot is switched off (ethernet install, cable gone)
  memset(&s, 0, sizeof(s));
  in = input(false, false, false, false, 0, true);
  CHECK(setupap_button(&s, &in, now) == SETUPAP_EVENT_REQUESTED && setupap_update(&s, &in, now) == SETUPAP_BUTTON, "button with hotspot=off");

  // 5. first setup with a wrong WiFi password: the hotspot must come back (like a request), not be gone for good
  memset(&s, 0, sizeof(s));
  in = input(false, false, true, false, 1);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_FACTORY, "factory, a client connected");
  in = input(true, false);  // the settings were saved: a WiFi name is stored, not connected (yet)
  now += 100;
  start = now;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_BUTTON && s.event == SETUPAP_EVENT_KEPT, "kept after the save");
  now += 60000;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_BUTTON && s.event == SETUPAP_EVENT_NONE, "still there a minute later");
  now = start + SETUPAP_IDLE_MS;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF && s.event == SETUPAP_EVENT_TIMEOUT, "and it times out like a request");
  // with the right password: over as soon as the WiFi is connected
  memset(&s, 0, sizeof(s));
  in = input(false, false);
  setupap_update(&s, &in, now);
  in = input(true, false);
  setupap_update(&s, &in, now + 100);
  in = input(true, true);
  CHECK(setupap_update(&s, &in, now + 5000) == SETUPAP_OFF && s.event == SETUPAP_EVENT_NETWORK, "connected after the save");
  in = input(true, false);
  CHECK(setupap_update(&s, &in, now + 500000) == SETUPAP_OFF, "WiFi lost later: no hotspot");
  // same with a button hotspot on a configured device (new router): the request survives the save
  memset(&s, 0, sizeof(s));
  in = input(true, false, true, false, 1);
  setupap_button(&s, &in, now);
  CHECK(setupap_update(&s, &in, now + 60000) == SETUPAP_BUTTON, "button hotspot in use");
  in.clients = 0;  // setupWifi() took the hotspot down to try the new WiFi
  CHECK(setupap_update(&s, &in, now + 80000) == SETUPAP_BUTTON, "wanted again after the WiFi change");
  // switching the factory hotspot off on the factory hotspot does not start a request
  memset(&s, 0, sizeof(s));
  in = input(false, false);
  setupap_update(&s, &in, now);
  in = input(false, false, false);
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF && s.event == SETUPAP_EVENT_NONE, "factory hotspot switched off by the user");

  // 6. the request across the wrap of millis()
  memset(&s, 0, sizeof(s));
  in = input(true, false);
  now = ULONG_MAX - 1000;
  setupap_button(&s, &in, now);
  now += 2000;  // wrapped
  CHECK(setupap_update(&s, &in, now) == SETUPAP_BUTTON, "across the wrap");
  now += SETUPAP_IDLE_MS;
  CHECK(setupap_update(&s, &in, now) == SETUPAP_OFF, "timeout across the wrap");

  // 7. every combination: the hotspot is never wanted while the network is up, and for a
  // configured device without the automatic setting only with a request
  for(int m = 0; m < 64; m++) {
    for(int req = 0; req < 2; req++) {
      memset(&s, 0, sizeof(s));
      in = input(m & 1, m & 2, m & 4, m & 8, (m & 16) ? 2 : 0, m & 32);
      now = 100000;
      if(req) {
        setupap_button(&s, &in, now);
      }
      uint8_t r = setupap_update(&s, &in, now + 10);
      if(in.net_up) {
        CHECK(r == SETUPAP_OFF, "network up, combination %d/%d: %d", m, req, r);
      } else if((in.wifi_configured || in.eth_seen || !in.allow_factory) && !in.auto_on_loss) {
        CHECK(r == (req ? SETUPAP_BUTTON : SETUPAP_OFF), "combination %d/%d: %d", m, req, r);
      } else {
        CHECK(r != SETUPAP_OFF, "combination %d/%d: %d", m, req, r);
      }
    }
  }
}

int main() {
  test_button();
  test_hotspot();
  printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
  return failed ? 1 : 0;
}
