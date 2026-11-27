/*
 * Flow temperature limit per heating surface (underfloor heating, radiators):
 * no command HeishaMon sends to the heat pump can set a heating flow target
 * above the maximum or a cooling flow target below the minimum. Pure code, no
 * hardware: commands.cpp and send_command() pass every main command datagram
 * through flowlimit_apply(), HeishaMon.ino feeds the received data block to
 * flowlimit_update() and sends what flowlimit_watchdog() asks for.
 * Host tests: tests/flowlimit/run_tests.py.
 *
 * Datagram bytes (the same offsets in a command and in the received data block,
 * value = byte - 128 in degrees C, a command byte of 0 means "no change"):
 *   28      bits 1-0 heating mode, bits 3-2 cooling mode (value + 1: 0 = compensation curve, 1 = direct)
 *   38 39   zone 1 heat / cool request (curve mode: shift -5..5, direct mode: the flow target)
 *   40 41   zone 2 heat / cool request
 *   75 76   zone 1 heat curve target high / low      79 80  zone 2
 *   86 87   zone 1 cool curve target high / low      90 91  zone 2
 *   6       bits 7-6 zones state (value + 1: 0 = zone 1, 1 = zone 2, 2 = both), bits 5-0 operating mode
 *           (18 heat, 33 DHW, 34 heat + DHW: no cooling), received data only
 *
 * Rules, only while a surface is chosen (surface 0 = off changes nothing):
 *   - heat curve targets <= heat_max, cool curve targets >= cool_min;
 *   - direct mode: heat request <= heat_max, cool request >= cool_min;
 *   - curve mode: the shift is lowered until highest heat curve target + shift <= heat_max
 *     (cooling: raised until lowest cool curve target + shift >= cool_min), within -5..5. The curve is
 *     taken from the same datagram where it sets it, else from the last received data;
 *   - mode or curve not known (no data yet): a request outside -5..5 counts as a direct target.
 *
 * Watchdog: when the received data shows a value outside the limit (set at the remote controller, by the
 * CZ-TAW1 or before the limit was switched on), one correcting datagram with only the clamped bytes, at most
 * once per FLOWLIMIT_RETRY_MS, for the zones in use (cooling only while the operating mode can cool, so a heat
 * only unit is not bothered with its unused cooling values). Counted per zone, heating / cooling and curve / request: after FLOWLIMIT_MAX_TRIES corrections
 * without effect that part is given up (warning in the log) until the settings change or a reboot; the other
 * parts keep being watched. A received value inside the limit resets the count of its part.
 */

#ifndef _FLOWLIMIT_H_
#define _FLOWLIMIT_H_

#include <stdint.h>
#include <stddef.h>

#define FLOWLIMIT_OFF 0
#define FLOWLIMIT_FLOOR 1     // underfloor heating
#define FLOWLIMIT_RADIATOR 2  // radiators
#define FLOWLIMIT_CUSTOM 3

#define FLOWLIMIT_FLOOR_MAX 35     // recommended maximum flow for underfloor heating
#define FLOWLIMIT_RADIATOR_MAX 55  // a common design value for radiators, check it against the system's design
#define FLOWLIMIT_COOL_MIN 16      // recommended minimum cooling flow (condensation)
#define FLOWLIMIT_HEAT_MAX_LOW 20
#define FLOWLIMIT_HEAT_MAX_HIGH 65
#define FLOWLIMIT_COOL_MIN_LOW 5
#define FLOWLIMIT_COOL_MIN_HIGH 25

#define FLOWLIMIT_RETRY_MS (10UL * 60UL * 1000UL)  // at most one correction per 10 minutes
#define FLOWLIMIT_MAX_TRIES 3                      // corrections without effect before a part is given up
#define FLOWLIMIT_DATAGRAM_SIZE 110                // a main command without checksum

// the parts the watchdog counts separately: bit (zone * 4 + cooling * 2 + request), curve targets and
// request apart, so a curve the heat pump does not take does not stop the watch of the request
#define FLOWLIMIT_PARTS 8

struct flowlimit_t {
  // settings
  uint8_t surface;   // FLOWLIMIT_OFF .. FLOWLIMIT_CUSTOM
  int8_t heat_max;   // degrees C
  int8_t cool_min;   // degrees C
  // last received data block
  uint8_t have_data;      // 0: nothing received yet, everything below is unknown
  int8_t heating_mode;    // 0 = curve, 1 = direct, -1 = unknown
  int8_t cooling_mode;
  uint8_t zones;          // bit 0 zone 1 in use, bit 1 zone 2 in use
  uint8_t cooling;        // 1 unless the operating mode has no cooling (heat, DHW, heat + DHW)
  int8_t heat_curve[2][2];  // [zone][0 = target high, 1 = target low]
  int8_t cool_curve[2][2];
  int8_t heat_request[2];
  int8_t cool_request[2];
  // watchdog
  uint8_t tries[FLOWLIMIT_PARTS];  // corrections sent while the part stayed outside the limit
  uint8_t given_up;                // bit per part
  uint8_t corrected;               // a correction was sent, last_ms is valid
  uint32_t last_ms;
};

extern struct flowlimit_t flowlimit;

// The maximum heating flow that belongs to a surface (the custom one starts as underfloor heating).
int flowlimit_default_max(uint8_t surface);
// Bring settings into range: an unknown surface is off, a maximum or minimum out of range becomes the
// default of the surface (never clamped to the nearest end, a typo must not loosen the limit).
void flowlimit_normalize(uint8_t *surface, int *heat_max, int *cool_min);
// Take over (normalized) settings. A change resets the watchdog.
void flowlimit_configure(struct flowlimit_t *fl, uint8_t surface, int heat_max, int cool_min);
// 1 while a surface is chosen
int flowlimit_on(const struct flowlimit_t *fl);

// Clamp a main command datagram (F1 6C 01 10 ...) in place. Anything else, or the limit off: untouched.
// Appends what was clamped to log (a string of logsize bytes, may be NULL). Returns the number of changed bytes.
int flowlimit_apply(const struct flowlimit_t *fl, uint8_t *cmd, size_t len, char *log, size_t logsize);

// Remember the values of a received data block (71 C8 01 10 ..., at least 92 bytes).
void flowlimit_update(struct flowlimit_t *fl, const uint8_t *data, size_t len);

// After flowlimit_update(): fills cmd (FLOWLIMIT_DATAGRAM_SIZE bytes) with a correction and returns its
// length, or 0 when nothing is to be sent. log gets the reason or the warning (also when 0 is returned
// and a part is given up now); log[0] == 0 when there is nothing to say.
int flowlimit_watchdog(struct flowlimit_t *fl, uint32_t now_ms, uint8_t *cmd, char *log, size_t logsize);

#endif
