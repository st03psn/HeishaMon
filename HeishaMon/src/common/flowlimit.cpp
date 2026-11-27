/*
  Flow temperature limit per heating surface (see flowlimit.h).

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/
#ifdef __linux__
  #include <stdio.h>
  #include <string.h>
  #define PROGMEM
  #define PSTR(x) (x)
  #define snprintf_P snprintf
  #define pgm_read_byte(p) (*(const uint8_t *)(p))
#else
  #include <Arduino.h>
#endif
#include "flowlimit.h"

struct flowlimit_t flowlimit;

#define BYTE_MODE 28
#define BYTE_ZONES 6
#define BYTE_SENSOR 22

static const uint8_t heatCurveByte[2][2] PROGMEM = { { 75, 76 }, { 79, 80 } };
static const uint8_t coolCurveByte[2][2] PROGMEM = { { 86, 87 }, { 90, 91 } };
static const uint8_t heatRequestByte[2] PROGMEM = { 38, 40 };
static const uint8_t coolRequestByte[2] PROGMEM = { 39, 41 };

enum { K_HEAT_CURVE_HIGH, K_HEAT_CURVE_LOW, K_COOL_CURVE_HIGH, K_COOL_CURVE_LOW, K_HEAT_REQUEST, K_COOL_REQUEST };

int flowlimit_default_max(uint8_t surface) {
  return surface == FLOWLIMIT_RADIATOR ? FLOWLIMIT_RADIATOR_MAX : FLOWLIMIT_FLOOR_MAX;
}

void flowlimit_normalize(uint8_t *surface, int *heat_max, int *cool_min) {
  if (*surface > FLOWLIMIT_CUSTOM) *surface = FLOWLIMIT_OFF;
  if (*heat_max < FLOWLIMIT_HEAT_MAX_LOW || *heat_max > FLOWLIMIT_HEAT_MAX_HIGH) *heat_max = flowlimit_default_max(*surface);
  if (*cool_min < FLOWLIMIT_COOL_MIN_LOW || *cool_min > FLOWLIMIT_COOL_MIN_HIGH) *cool_min = FLOWLIMIT_COOL_MIN;
}

void flowlimit_configure(struct flowlimit_t *fl, uint8_t surface, int heat_max, int cool_min) {
  flowlimit_normalize(&surface, &heat_max, &cool_min);
  if (fl->surface != surface || fl->heat_max != heat_max || fl->cool_min != cool_min) {
    fl->surface = surface;
    fl->heat_max = (int8_t)heat_max;
    fl->cool_min = (int8_t)cool_min;
    memset(fl->tries, 0, sizeof(fl->tries));
    fl->given_up = 0;
    fl->pending = 0;
    fl->corrected = 0;
  }
}

int flowlimit_on(const struct flowlimit_t *fl) {
  return fl->surface != FLOWLIMIT_OFF;
}

static void note(char *log, size_t logsize, int kind, int zone, int from, int to) {
  if (log == NULL || logsize == 0) return;
  size_t used = strlen(log);
  if (used + 1 >= logsize) return;
  const char *format;
  switch (kind) {
    case K_HEAT_CURVE_HIGH: format = PSTR(" [limit: Z%d heat curve high %d>%d]"); break;
    case K_HEAT_CURVE_LOW: format = PSTR(" [limit: Z%d heat curve low %d>%d]"); break;
    case K_COOL_CURVE_HIGH: format = PSTR(" [limit: Z%d cool curve high %d>%d]"); break;
    case K_COOL_CURVE_LOW: format = PSTR(" [limit: Z%d cool curve low %d>%d]"); break;
    case K_HEAT_REQUEST: format = PSTR(" [limit: Z%d heat request %d>%d]"); break;
    default: format = PSTR(" [limit: Z%d cool request %d>%d]"); break;
  }
  snprintf_P(log + used, logsize - used, format, zone + 1, from, to);
}

static int clampInt(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

// a curve byte: 0 is "no change", else at most heat_max (heating) / at least cool_min (cooling)
static int clampCurve(uint8_t *cmd, uint8_t address, bool cooling, int limit, int kind, int zone, char *log, size_t logsize) {
  if (cmd[address] == 0) return 0;
  const int value = (int)cmd[address] - 128;
  if (cooling ? value >= limit : value <= limit) return 0;
  cmd[address] = (uint8_t)(limit + 128);
  note(log, logsize, kind, zone, value, limit);
  return 1;
}

// mode set by this datagram (byte 28 field), else the received one, else -1
static int modeOf(const struct flowlimit_t *fl, const uint8_t *cmd, int shift, int received) {
  const int field = (cmd[BYTE_MODE] >> shift) & 0x03;
  if (field == 1 || field == 2) return field - 1;
  if (field == 0 && fl->have_data) return received;
  return -1;
}

// zone sensor set by this datagram (byte 22 nibble, SetZ1SensorSettings), else the received one, else -1
static int sensorOf(const struct flowlimit_t *fl, const uint8_t *cmd, int zone) {
  const int field = (cmd[BYTE_SENSOR] >> (zone * 4)) & 0x0F;
  if (field >= 1 && field <= 4) return field - 1;
  if (field == 0 && fl->have_data) return fl->sensor[zone];
  return -1;
}

static void request(const struct flowlimit_t *fl, uint8_t *cmd, int zone, bool cooling, int mode, int *changes, char *log, size_t logsize) {
  const uint8_t address = pgm_read_byte(cooling ? &coolRequestByte[zone] : &heatRequestByte[zone]);
  if (cmd[address] == 0) return;
  // with an internal thermostat or a thermistor the request is a room setpoint, not a flow temperature;
  // sensor not known: not touched
  const int sensor = sensorOf(fl, cmd, zone);
  if (sensor != 0 && sensor != 1) return;
  const int value = (int)cmd[address] - 128;
  const int limitTemp = cooling ? fl->cool_min : fl->heat_max;
  int limit;
  bool curve = false;
  if (mode == 1) {
    limit = limitTemp;
  } else {
    // the curve of the zone: from this datagram where it sets it, else from the received data
    bool known = mode == 0;
    int extreme = 0;
    for (int i = 0; i < 2 && known; i++) {
      const uint8_t curveByte = pgm_read_byte(cooling ? &coolCurveByte[zone][i] : &heatCurveByte[zone][i]);
      int point;
      if (cmd[curveByte] != 0) {
        point = (int)cmd[curveByte] - 128;
      } else if (fl->have_data && (fl->valid & FLOWLIMIT_VALID_CURVE(zone, cooling ? 1 : 0, i))) {
        point = cooling ? fl->cool_curve[zone][i] : fl->heat_curve[zone][i];
      } else {
        known = false;
        break;
      }
      if (i == 0 || (cooling ? point < extreme : point > extreme)) extreme = point;
    }
    if (known) {
      limit = limitTemp - extreme;  // the largest (heating) / smallest (cooling) shift that keeps the flow inside
      curve = true;
    } else if (value < -5 || value > 5) {
      limit = limitTemp;  // not known whether shift or target: outside the shift range it is a target
    } else {
      return;  // a shift, the curve is not known
    }
  }
  if (cooling ? value >= limit : value <= limit) return;
  if (curve) limit = clampInt(limit, -5, 5);
  // only ever towards the limit: a heat value is only lowered, a cool value only raised
  if (cooling ? value >= limit : value <= limit) return;
  cmd[address] = (uint8_t)(limit + 128);
  note(log, logsize, cooling ? K_COOL_REQUEST : K_HEAT_REQUEST, zone, value, limit);
  (*changes)++;
}

int flowlimit_apply(const struct flowlimit_t *fl, uint8_t *cmd, size_t len, char *log, size_t logsize) {
  if (!flowlimit_on(fl) || cmd == NULL || len < 92) return 0;
  if (cmd[0] != 0xF1 || cmd[1] != 0x6C || cmd[2] != 0x01 || cmd[3] != 0x10) return 0;
  int changes = 0;
  // curves first: the shift below is checked against the curve as it is sent
  for (int zone = 0; zone < 2; zone++) {
    for (int i = 0; i < 2; i++) {
      changes += clampCurve(cmd, pgm_read_byte(&heatCurveByte[zone][i]), false, fl->heat_max, K_HEAT_CURVE_HIGH + i, zone, log, logsize);
      changes += clampCurve(cmd, pgm_read_byte(&coolCurveByte[zone][i]), true, fl->cool_min, K_COOL_CURVE_HIGH + i, zone, log, logsize);
    }
  }
  const int heatingMode = modeOf(fl, cmd, 0, fl->heating_mode);
  const int coolingMode = modeOf(fl, cmd, 2, fl->cooling_mode);
  for (int zone = 0; zone < 2; zone++) {
    request(fl, cmd, zone, false, heatingMode, &changes, log, logsize);
    request(fl, cmd, zone, true, coolingMode, &changes, log, logsize);
  }
  return changes;
}

void flowlimit_update(struct flowlimit_t *fl, const uint8_t *data, size_t len) {
  if (data == NULL || len < 92 || data[0] != 0x71 || data[2] != 0x01 || data[3] != 0x10) return;
  const int heating = (data[BYTE_MODE] & 0x03) - 1;
  const int cooling = ((data[BYTE_MODE] >> 2) & 0x03) - 1;
  fl->heating_mode = (int8_t)((heating == 0 || heating == 1) ? heating : -1);
  fl->cooling_mode = (int8_t)((cooling == 0 || cooling == 1) ? cooling : -1);
  const int zones = (data[BYTE_ZONES] >> 6) - 1;
  fl->zones = zones == 0 ? 1 : (zones == 1 ? 2 : 3);  // unknown: watch both
  const int operating = data[BYTE_ZONES] & 0x3F;
  fl->cooling = (operating == 18 || operating == 33 || operating == 34) ? 0 : 1;  // unknown: watch
  uint16_t valid = 0;
  for (int zone = 0; zone < 2; zone++) {
    const int sensor = ((data[BYTE_SENSOR] >> (zone * 4)) & 0x0F) - 1;
    fl->sensor[zone] = (int8_t)((sensor >= 0 && sensor <= 3) ? sensor : -1);
    for (int i = 0; i < 2; i++) {
      const uint8_t heat = data[pgm_read_byte(&heatCurveByte[zone][i])];
      const uint8_t cool = data[pgm_read_byte(&coolCurveByte[zone][i])];
      fl->heat_curve[zone][i] = (int8_t)((int)heat - 128);
      fl->cool_curve[zone][i] = (int8_t)((int)cool - 128);
      if (heat != 0) valid |= FLOWLIMIT_VALID_CURVE(zone, 0, i);  // a byte 0 is no value (not -128 degrees)
      if (cool != 0) valid |= FLOWLIMIT_VALID_CURVE(zone, 1, i);
    }
    const uint8_t heat = data[pgm_read_byte(&heatRequestByte[zone])];
    const uint8_t cool = data[pgm_read_byte(&coolRequestByte[zone])];
    fl->heat_request[zone] = (int8_t)((int)heat - 128);
    fl->cool_request[zone] = (int8_t)((int)cool - 128);
    if (heat != 0) valid |= FLOWLIMIT_VALID_REQUEST(zone, 0);
    if (cool != 0) valid |= FLOWLIMIT_VALID_REQUEST(zone, 1);
  }
  fl->valid = valid;
  fl->have_data = 1;
}

static void header(uint8_t *cmd) {
  memset(cmd, 0, FLOWLIMIT_DATAGRAM_SIZE);
  cmd[0] = 0xF1;
  cmd[1] = 0x6C;
  cmd[2] = 0x01;
  cmd[3] = 0x10;
}

// the bytes of a part: part = zone * 4 + cooling * 2 + request; the two curve targets, or the request.
// Returns the number of bytes.
static int partBytes(int part, uint8_t *bytes) {
  const int zone = part >> 2;
  const bool cooling = (part >> 1) & 1;
  if (part & 1) {
    bytes[0] = pgm_read_byte(cooling ? &coolRequestByte[zone] : &heatRequestByte[zone]);
    return 1;
  }
  bytes[0] = pgm_read_byte(cooling ? &coolCurveByte[zone][0] : &heatCurveByte[zone][0]);
  bytes[1] = pgm_read_byte(cooling ? &coolCurveByte[zone][1] : &heatCurveByte[zone][1]);
  return 2;
}

// The received (valid) values of the given parts as a datagram, clamped like a command. Curve parts and request
// parts go separately (requests = 1), so a request is judged against the received curve, not a clamped one the
// heat pump may refuse.
static void receivedParts(const struct flowlimit_t *fl, uint8_t parts, int requests, uint8_t *cmd, uint8_t *received, char *log, size_t logsize) {
  header(cmd);
  for (int part = 0; part < FLOWLIMIT_PARTS; part++) {
    if (!((parts >> part) & 1) || (part & 1) != requests) continue;
    const int zone = part >> 2;
    const int cooling = (part >> 1) & 1;
    uint8_t bytes[2];
    const int count = partBytes(part, bytes);
    for (int i = 0; i < count; i++) {
      const bool valid = requests ? (fl->valid & FLOWLIMIT_VALID_REQUEST(zone, cooling)) : (fl->valid & FLOWLIMIT_VALID_CURVE(zone, cooling, i));
      if (!valid) continue;
      int8_t value;
      if (requests) value = cooling ? fl->cool_request[zone] : fl->heat_request[zone];
      else value = cooling ? fl->cool_curve[zone][i] : fl->heat_curve[zone][i];
      cmd[bytes[i]] = (uint8_t)(value + 128);
    }
  }
  memcpy(received, cmd, FLOWLIMIT_DATAGRAM_SIZE);
  flowlimit_apply(fl, cmd, FLOWLIMIT_DATAGRAM_SIZE, log, logsize);
}

int flowlimit_watchdog(struct flowlimit_t *fl, uint32_t now_ms, uint8_t *cmd, char *log, size_t logsize) {
  if (log != NULL && logsize > 0) log[0] = '\0';
  fl->pending = 0;
  if (!flowlimit_on(fl) || !fl->have_data) return 0;

  // which parts are outside the limit: the received values through the same clamp as a command
  uint8_t curves[FLOWLIMIT_DATAGRAM_SIZE], curvesReceived[FLOWLIMIT_DATAGRAM_SIZE];
  uint8_t received[FLOWLIMIT_DATAGRAM_SIZE];
  receivedParts(fl, 0xFF, 0, curves, curvesReceived, NULL, 0);
  receivedParts(fl, 0xFF, 1, cmd, received, NULL, 0);

  const bool due = !fl->corrected || (uint32_t)(now_ms - fl->last_ms) >= FLOWLIMIT_RETRY_MS;
  uint8_t send = 0;
  for (int part = 0; part < FLOWLIMIT_PARTS; part++) {
    const uint8_t *after = (part & 1) ? cmd : curves;
    const uint8_t *before = (part & 1) ? received : curvesReceived;
    uint8_t bytes[2];
    const int count = partBytes(part, bytes);
    bool outside = false;
    for (int i = 0; i < count; i++) outside = outside || after[bytes[i]] != before[bytes[i]];
    if (!outside) {
      fl->tries[part] = 0;
      continue;
    }
    const bool watched = ((fl->zones >> (part >> 2)) & 1) && (!((part >> 1) & 1) || fl->cooling) && !((fl->given_up >> part) & 1);
    if (!watched || !due) continue;
    if (fl->tries[part] >= FLOWLIMIT_MAX_TRIES) {
      // one warning per call; the other parts follow with the next data block
      fl->given_up |= (uint8_t)(1 << part);
      if (log != NULL && logsize > 0) {
        snprintf_P(log, logsize, PSTR("Flow limit: zone %d %s %s is still outside the limit after %d corrections, the heat pump does not take them. Not corrected again until the settings change or a reboot."),
                   (part >> 2) + 1, ((part >> 1) & 1) ? "cooling" : "heating", (part & 1) ? "request" : "curve", FLOWLIMIT_MAX_TRIES);
      }
      return 0;
    }
    send |= (uint8_t)(1 << part);
  }
  if (send == 0) return 0;

  // the correction: the received values of the parts corrected now in one datagram, clamped like a command
  // (a request whose curve is corrected in the same datagram is judged against the corrected curve, else
  // against the received one), then only the changed bytes, everything else 0 = no change
  if (log != NULL && logsize > 0) {
    snprintf_P(log, logsize, PSTR("Flow limit: received value outside the limit (heating max %d, cooling min %d), sending a correction"), fl->heat_max, fl->cool_min);
  }
  receivedParts(fl, send, 0, curves, curvesReceived, NULL, 0);
  receivedParts(fl, send, 1, cmd, received, NULL, 0);
  for (int i = 4; i < FLOWLIMIT_DATAGRAM_SIZE; i++) {
    if (curvesReceived[i] != 0) received[i] = curvesReceived[i];
  }
  memcpy(cmd, received, FLOWLIMIT_DATAGRAM_SIZE);
  flowlimit_apply(fl, cmd, FLOWLIMIT_DATAGRAM_SIZE, log, logsize);
  uint8_t pending = 0;
  for (int part = 0; part < FLOWLIMIT_PARTS; part++) {
    uint8_t bytes[2];
    const int count = partBytes(part, bytes);
    for (int i = 0; i < count; i++) {
      if (((send >> part) & 1) && cmd[bytes[i]] != received[bytes[i]]) pending |= (uint8_t)(1 << part);
    }
  }
  for (int i = 4; i < FLOWLIMIT_DATAGRAM_SIZE; i++) {
    if (cmd[i] == received[i]) cmd[i] = 0;
  }
  fl->pending = pending;
  return pending ? FLOWLIMIT_DATAGRAM_SIZE : 0;
}

void flowlimit_sent(struct flowlimit_t *fl, uint32_t now_ms) {
  if (fl->pending == 0) return;
  for (int part = 0; part < FLOWLIMIT_PARTS; part++) {
    if ((fl->pending >> part) & 1) fl->tries[part]++;
  }
  fl->pending = 0;
  fl->corrected = 1;
  fl->last_ms = now_ms;
}
