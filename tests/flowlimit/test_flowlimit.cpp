// Host test of the flow temperature limit (HeishaMon/src/common/flowlimit.cpp). Run: python3 tests/flowlimit/run_tests.py
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "flowlimit.h"

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const int N = FLOWLIMIT_DATAGRAM_SIZE;

// an empty main command (every byte 0 = no change)
static void command(uint8_t *cmd) {
  memset(cmd, 0, N);
  cmd[0] = 0xF1; cmd[1] = 0x6C; cmd[2] = 0x01; cmd[3] = 0x10;
}
static uint8_t t(int celsius) { return (uint8_t)(celsius + 128); }

// a received data block (203 bytes) with the given modes (0 = curve, 1 = direct), zones state (0 = z1, 1 = z2, 2 = both)
struct block_t { uint8_t b[203]; };
static block_t received(int heatMode, int coolMode, int zones) {
  block_t d;
  memset(d.b, 0, sizeof(d.b));
  d.b[0] = 0x71; d.b[1] = 0xC8; d.b[2] = 0x01; d.b[3] = 0x10;
  d.b[6] = (uint8_t)((zones + 1) << 6) | 25;  // operating mode auto: heating and cooling
  d.b[28] = (uint8_t)((heatMode + 1) | ((coolMode + 1) << 2) | 0x50);
  d.b[22] = 0x11;  // both zones: water temperature
  // compliant values for underfloor heating, max 35 / min 16
  d.b[75] = t(33); d.b[76] = t(25); d.b[79] = t(32); d.b[80] = t(24);
  d.b[86] = t(18); d.b[87] = t(20); d.b[90] = t(17); d.b[91] = t(19);
  // requests: a shift of 0 in curve mode, a target in direct mode
  d.b[38] = d.b[40] = t(heatMode == 1 ? 30 : 0);
  d.b[39] = d.b[41] = t(coolMode == 1 ? 18 : 0);
  return d;
}

// the watchdog, with its correction accepted by send_command()
static int wd(flowlimit_t *fl, uint32_t now, uint8_t *cmd, char *log, size_t logsize) {
  const int len = flowlimit_watchdog(fl, now, cmd, log, logsize);
  if (len > 0) flowlimit_sent(fl, now);
  return len;
}

static flowlimit_t limit(uint8_t surface, int max, int min) {
  flowlimit_t fl;
  memset(&fl, 0, sizeof(fl));
  flowlimit_configure(&fl, surface, max, min);
  return fl;
}

static void test_off_changes_nothing() {
  flowlimit_t fl;
  memset(&fl, 0, sizeof(fl));  // as the global after boot
  CHECK(!flowlimit_on(&fl));
  block_t d = received(1, 1, 2);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  srand(1);
  for (int round = 0; round < 2000; round++) {
    uint8_t cmd[N], copy[N];
    command(cmd);
    for (int i = 4; i < N; i++) cmd[i] = (uint8_t)rand();
    memcpy(copy, cmd, N);
    char log[256] = "x";
    CHECK(flowlimit_apply(&fl, cmd, N, log, sizeof(log)) == 0);
    CHECK(memcmp(cmd, copy, N) == 0);
    CHECK(strcmp(log, "x") == 0);
  }
  uint8_t cmd[N];
  char log[256];
  CHECK(wd(&fl, 0, cmd, log, sizeof(log)) == 0 && log[0] == 0);
  // configured off with values: still nothing
  fl = limit(FLOWLIMIT_OFF, 35, 16);
  command(cmd); cmd[75] = t(60); cmd[38] = t(60);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[75] == t(60) && cmd[38] == t(60));
}

static void test_curve_bytes() {
  flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
  const int heat[] = { 75, 76, 79, 80 }, cool[] = { 86, 87, 90, 91 };
  for (int address : heat) {
    uint8_t cmd[N];
    command(cmd); cmd[address] = t(45);
    char log[256] = "SetCurves JSON received ok";
    CHECK(flowlimit_apply(&fl, cmd, N, log, sizeof(log)) == 1);
    CHECK(cmd[address] == t(35));
    for (int i = 4; i < N; i++) if (i != address) CHECK(cmd[i] == 0);
    CHECK(strstr(log, "SetCurves JSON received ok [limit: Z") == log && strstr(log, "45>35]"));
    command(cmd); cmd[address] = t(35);  // at the limit: untouched
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == t(35));
    command(cmd); cmd[address] = t(10);  // low heating curve values pass
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == t(10));
    command(cmd);  // 0 = no change stays
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == 0);
  }
  for (int address : cool) {
    uint8_t cmd[N];
    command(cmd); cmd[address] = t(7);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[address] == t(16));
    command(cmd); cmd[address] = t(16);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == t(16));
    command(cmd); cmd[address] = t(22);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == t(22));
  }
  // the names in the log
  uint8_t cmd[N];
  char log[256] = "";
  command(cmd); cmd[80] = t(50); cmd[87] = t(10);
  CHECK(flowlimit_apply(&fl, cmd, N, log, sizeof(log)) == 2);
  CHECK(strcmp(log, " [limit: Z1 cool curve low 10>16] [limit: Z2 heat curve low 50>35]") == 0);
  // the radiator limit
  fl = limit(FLOWLIMIT_RADIATOR, 55, 16);
  command(cmd); cmd[75] = t(50); cmd[76] = t(60);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[75] == t(50) && cmd[76] == t(55));
  // a datagram that is not a main command is never changed
  uint8_t query[N];
  command(query); query[0] = 0x71; query[75] = t(60);
  CHECK(flowlimit_apply(&fl, query, N, NULL, 0) == 0 && query[75] == t(60));
  command(query); query[75] = t(60);
  CHECK(flowlimit_apply(&fl, query, 91, NULL, 0) == 0 && query[75] == t(60));
  CHECK(flowlimit_apply(&fl, NULL, N, NULL, 0) == 0);
}

static void test_direct_requests() {
  flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t d = received(1, 1, 2);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  const int heat[] = { 38, 40 }, cool[] = { 39, 41 };
  for (int address : heat) {
    uint8_t cmd[N];
    command(cmd); cmd[address] = t(40);
    char log[256] = "SetZ1HeatRequestTemperature set to 40";
    CHECK(flowlimit_apply(&fl, cmd, N, log, sizeof(log)) == 1 && cmd[address] == t(35));
    CHECK(strstr(log, "heat request 40>35]") != NULL);
    command(cmd); cmd[address] = t(30);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == t(30));
  }
  for (int address : cool) {
    uint8_t cmd[N];
    command(cmd); cmd[address] = t(10);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[address] == t(16));
    command(cmd); cmd[address] = t(18);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[address] == t(18));
  }
}

static void test_shift() {
  flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t d = received(0, 0, 2);  // curve mode, z1 heat curve 33/25, z2 32/24, z1 cool 18/20, z2 17/19
  flowlimit_update(&fl, d.b, sizeof(d.b));
  uint8_t cmd[N];
  // readback curve: highest 33, so at most +2
  command(cmd); cmd[38] = t(5);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(2));
  command(cmd); cmd[38] = t(2);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(2));
  command(cmd); cmd[38] = t(-3);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(-3));
  command(cmd); cmd[40] = t(5);  // zone 2: highest 32, +3
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[40] == t(3));
  // a direct value sent in curve mode is no shift: lowered to the largest allowed shift, within -5..5
  command(cmd); cmd[38] = t(40);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(2));
  // curve from the same datagram: high 30 (and low from the readback 25): +5 passes
  command(cmd); cmd[75] = t(30); cmd[38] = t(5);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(5));
  // a curve point of the datagram that is clamped first: 45 -> 35, so the shift goes to 0
  command(cmd); cmd[75] = t(45); cmd[38] = t(4);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 2 && cmd[75] == t(35) && cmd[38] == t(0));
  // the low point counts as well: datagram low 34 with readback high 33 -> +1
  command(cmd); cmd[76] = t(34); cmd[38] = t(5);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(1));
  // a readback curve above the limit: the shift goes to -5 at most (the watchdog corrects the curve)
  d.b[75] = t(45);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  command(cmd); cmd[38] = t(0);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(-5));
  command(cmd); cmd[38] = t(-5);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(-5));
  d = received(0, 0, 2);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  // cooling: lowest z1 cool point 18, min 16: at least -2
  command(cmd); cmd[39] = t(-5);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[39] == t(-2));
  command(cmd); cmd[39] = t(-1);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[39] == t(-1));
  command(cmd); cmd[41] = t(-5);  // z2 lowest 17: -1
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[41] == t(-1));
  command(cmd); cmd[90] = t(25); cmd[91] = t(26); cmd[41] = t(-5);  // curve from the datagram: -5 passes
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[41] == t(-5));
  // the mode set by the same datagram wins over the received one
  command(cmd); cmd[28] = 0x02; cmd[38] = t(40);  // heating direct
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(35));
  flowlimit_t direct = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t dd = received(1, 1, 2);
  flowlimit_update(&direct, dd.b, sizeof(dd.b));
  command(cmd); cmd[28] = 0x01; cmd[38] = t(5);  // heating curve: shift checked against the curve, 33 -> +2
  CHECK(flowlimit_apply(&direct, cmd, N, NULL, 0) == 1 && cmd[38] == t(2));
}

static void test_unknown() {
  flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);  // no data received: the zone sensor is not known
  uint8_t cmd[N];
  command(cmd); cmd[38] = t(40); cmd[39] = t(8); cmd[75] = t(45);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(40) && cmd[39] == t(8) && cmd[75] == t(35));
  // the datagram itself says water temperature (SetZ1SensorSettings 0): the unknown mode rule
  command(cmd); cmd[22] = 0x01; cmd[38] = t(40);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(35));
  // received data with water sensors, but mode and curve not known (mode bits invalid)
  block_t u = received(0, 0, 2);
  u.b[28] = 0x00;
  flowlimit_update(&fl, u.b, sizeof(u.b));
  command(cmd); cmd[38] = t(40);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(35));
  command(cmd); cmd[40] = t(11);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[40] == t(11));
  command(cmd); cmd[38] = t(5);  // a shift passes
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(5));
  command(cmd); cmd[39] = t(8);  // outside -5..5: a direct cooling target
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[39] == t(16));
  command(cmd); cmd[39] = t(-5);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[39] == t(-5));
  // known data, but the mode bits are invalid: the same rule
  block_t d = received(0, 0, 2);
  d.b[28] = 0x00;
  flowlimit_update(&fl, d.b, sizeof(d.b));
  CHECK(fl.have_data && fl.heating_mode == -1 && fl.cooling_mode == -1);
  command(cmd); cmd[38] = t(50); cmd[41] = t(3);
  CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(35) && cmd[41] == t(3));
  // a block that is not the main data block is ignored
  flowlimit_t other = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t x = received(1, 1, 2);
  x.b[3] = 0x21;
  flowlimit_update(&other, x.b, sizeof(x.b));
  CHECK(!other.have_data);
}

static void test_watchdog() {
  flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
  uint8_t cmd[N];
  char log[256];
  CHECK(wd(&fl, 1000, cmd, log, sizeof(log)) == 0 && log[0] == 0);  // no data yet
  block_t d = received(0, 0, 2);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  CHECK(wd(&fl, 1000, cmd, log, sizeof(log)) == 0 && log[0] == 0);  // compliant

  // the remote controller sets the zone 1 heat curve to 45
  d.b[75] = t(45);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  uint32_t now = 5000;
  CHECK(wd(&fl, now, cmd, log, sizeof(log)) == N);
  CHECK(cmd[0] == 0xF1 && cmd[1] == 0x6C && cmd[2] == 0x01 && cmd[3] == 0x10);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 75 ? t(35) : 0));  // only the clamped byte, shift 0 is fine
  CHECK(strstr(log, "Flow limit: received value outside the limit") == log && strstr(log, "Z1 heat curve high 45>35"));
  // the next data blocks: not before 10 minutes
  for (uint32_t later = now + 5000; later < now + FLOWLIMIT_RETRY_MS; later += 60000) {
    CHECK(wd(&fl, later, cmd, log, sizeof(log)) == 0 && log[0] == 0);
  }
  now += FLOWLIMIT_RETRY_MS;
  CHECK(wd(&fl, now, cmd, log, sizeof(log)) == N && cmd[75] == t(35));  // second
  now += FLOWLIMIT_RETRY_MS;
  CHECK(wd(&fl, now, cmd, log, sizeof(log)) == N);  // third
  CHECK(wd(&fl, now + 1000, cmd, log, sizeof(log)) == 0);
  // still not taken after three corrections: given up, with a warning, once
  now += FLOWLIMIT_RETRY_MS;
  CHECK(wd(&fl, now, cmd, log, sizeof(log)) == 0);
  CHECK(strstr(log, "zone 1 heating curve is still outside the limit after 3 corrections") != NULL);
  CHECK(fl.given_up == 1);
  // the curve is given up, the request is still watched and judged against the real curve (45): the shift
  // goes down as far as it can, -5 (a partial correction is better than none); the curve is not sent again
  now += FLOWLIMIT_RETRY_MS;
  CHECK(wd(&fl, now, cmd, log, sizeof(log)) == N);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(-5) : 0));
  CHECK(strstr(log, "Z1 heat request 0>-5") != NULL);
  d.b[38] = t(-5);  // taken
  flowlimit_update(&fl, d.b, sizeof(d.b));
  for (int i = 1; i <= 20; i++) {
    CHECK(wd(&fl, now + i * FLOWLIMIT_RETRY_MS, cmd, log, sizeof(log)) == 0 && log[0] == 0);
  }
  CHECK(fl.tries[1] == 0 && fl.given_up == 1);
  now += 21 * FLOWLIMIT_RETRY_MS;
  // the other parts are still watched: zone 2 cooling curve 10
  d.b[91] = t(10);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  CHECK(wd(&fl, now, cmd, log, sizeof(log)) == N);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 91 ? t(16) : 0));
  // the heat pump takes it: the count of that part starts again
  d.b[91] = t(16);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  CHECK(wd(&fl, now + 1000, cmd, log, sizeof(log)) == 0);
  CHECK(fl.tries[6] == 0 && fl.given_up == 1);
  // a direct target of 50 set at the remote is corrected, the curve is still given up
  d.b[28] = (uint8_t)(2 | (1 << 2) | 0x50);  // heating direct
  d.b[38] = t(50);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  CHECK(wd(&fl, now + FLOWLIMIT_RETRY_MS, cmd, log, sizeof(log)) == N);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(35) : 0));
  d.b[38] = t(0);  // back to curve mode, shift 0
  d.b[28] = (uint8_t)(1 | (1 << 2) | 0x50);
  flowlimit_update(&fl, d.b, sizeof(d.b));
  now += 2 * FLOWLIMIT_RETRY_MS;
  // a curve corrected in the same datagram: the request is judged against the corrected curve (no extra shift)
  {
    flowlimit_t both = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t db = received(0, 0, 0);
    db.b[75] = t(45); db.b[38] = t(1);
    flowlimit_update(&both, db.b, sizeof(db.b));
    CHECK(wd(&both, 0, cmd, log, sizeof(log)) == N);
    for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 75 ? t(35) : (i == 38 ? t(0) : 0)));
    CHECK(both.tries[0] == 1 && both.tries[1] == 1);
    db.b[75] = t(45); db.b[38] = t(0);  // the shift is taken, the curve is not
    flowlimit_update(&both, db.b, sizeof(db.b));
    CHECK(wd(&both, FLOWLIMIT_RETRY_MS, cmd, log, sizeof(log)) == N);
    for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 75 ? t(35) : 0));  // with the curve corrected, shift 0 is fine
  }
  // a correction that send_command() did not accept is not counted and is tried again with the next data block
  {
    flowlimit_t busy = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t db = received(0, 0, 0);
    db.b[76] = t(40);
    flowlimit_update(&busy, db.b, sizeof(db.b));
    for (int i = 0; i < 5; i++) CHECK(flowlimit_watchdog(&busy, 1000 + i * 5000, cmd, log, sizeof(log)) == N);
    CHECK(busy.tries[0] == 0 && busy.corrected == 0);
    flowlimit_sent(&busy, 30000);
    CHECK(busy.tries[0] == 1 && busy.corrected == 1);
    CHECK(flowlimit_watchdog(&busy, 35000, cmd, log, sizeof(log)) == 0);
    flowlimit_sent(&busy, 35000);  // nothing pending: no count
    CHECK(busy.tries[0] == 1 && busy.last_ms == 30000);
  }
  // saving the same settings again does not reset, changed settings do
  flowlimit_configure(&fl, FLOWLIMIT_FLOOR, 35, 16);
  CHECK(fl.given_up == 1);
  flowlimit_configure(&fl, FLOWLIMIT_CUSTOM, 40, 16);
  CHECK(fl.given_up == 0 && fl.corrected == 0);
  CHECK(wd(&fl, now + 2000, cmd, log, sizeof(log)) == N && cmd[75] == t(40));

  // direct mode: a request target above the limit; curve mode: a shift that takes the flow above it
  flowlimit_t direct = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t dd = received(1, 1, 2);
  dd.b[38] = t(45); dd.b[41] = t(12);
  flowlimit_update(&direct, dd.b, sizeof(dd.b));
  CHECK(wd(&direct, 0, cmd, log, sizeof(log)) == N);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(35) : (i == 41 ? t(16) : 0)));
  CHECK(strstr(log, "Z1 heat request 45>35") && strstr(log, "Z2 cool request 12>16"));
  flowlimit_t shift = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t ds = received(0, 0, 0);
  ds.b[38] = t(4);  // curve 33 + 4 = 37
  flowlimit_update(&shift, ds.b, sizeof(ds.b));
  CHECK(wd(&shift, 0, cmd, log, sizeof(log)) == N);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(2) : 0));

  // zone 2 not in use: its values are not corrected
  flowlimit_t zone1 = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t dz = received(0, 0, 0);
  dz.b[79] = t(55);
  flowlimit_update(&zone1, dz.b, sizeof(dz.b));
  CHECK(wd(&zone1, 0, cmd, log, sizeof(log)) == 0 && log[0] == 0);
  dz.b[6] = (uint8_t)(3 << 6);  // both zones
  flowlimit_update(&zone1, dz.b, sizeof(dz.b));
  CHECK(wd(&zone1, 0, cmd, log, sizeof(log)) == N && cmd[79] == t(35));

  // an operating mode without cooling (heat only unit): its cooling values are not corrected, heating is
  flowlimit_t heat = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t dh = received(0, 0, 2);
  dh.b[6] = (uint8_t)(3 << 6) | 34;  // heat + DHW
  dh.b[86] = t(5);
  flowlimit_update(&heat, dh.b, sizeof(dh.b));
  CHECK(wd(&heat, 0, cmd, log, sizeof(log)) == 0 && log[0] == 0);
  dh.b[80] = t(40);
  flowlimit_update(&heat, dh.b, sizeof(dh.b));
  CHECK(wd(&heat, 0, cmd, log, sizeof(log)) == N);
  for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 80 ? t(35) : 0));
  dh.b[6] = (uint8_t)(3 << 6) | 19;  // cool
  flowlimit_update(&heat, dh.b, sizeof(dh.b));
  CHECK(wd(&heat, FLOWLIMIT_RETRY_MS, cmd, log, sizeof(log)) == N && cmd[86] == t(16));

  // millis() wraps around
  flowlimit_t wrap = limit(FLOWLIMIT_FLOOR, 35, 16);
  block_t dw = received(0, 0, 2);
  dw.b[76] = t(40);
  flowlimit_update(&wrap, dw.b, sizeof(dw.b));
  CHECK(wd(&wrap, 0xFFFFF000UL, cmd, log, sizeof(log)) == N);
  CHECK(wd(&wrap, 0x00001000UL, cmd, log, sizeof(log)) == 0);
  CHECK(wd(&wrap, (uint32_t)(0xFFFFF000UL + FLOWLIMIT_RETRY_MS), cmd, log, sizeof(log)) == N);

  // a correction run through the limit again changes nothing (send_command() applies it once more)
  uint8_t copy[N];
  memcpy(copy, cmd, N);
  CHECK(flowlimit_apply(&wrap, cmd, N, NULL, 0) == 0 && memcmp(copy, cmd, N) == 0);
  // the limit off: no watchdog
  flowlimit_configure(&wrap, FLOWLIMIT_OFF, 35, 16);
  CHECK(wd(&wrap, 0x7FFFFFFFUL, cmd, log, sizeof(log)) == 0);
}

static void test_settings() {
  CHECK(flowlimit_default_max(FLOWLIMIT_FLOOR) == 35);
  CHECK(flowlimit_default_max(FLOWLIMIT_RADIATOR) == 55);
  CHECK(flowlimit_default_max(FLOWLIMIT_CUSTOM) == 35);
  struct { int surface, max, min, surfaceOut, maxOut, minOut; } cases[] = {
    { 0, 35, 16, 0, 35, 16 },     // the defaults of a new or an updated install: off
    { 0, 0, 0, 0, 35, 16 },       // keys missing in config.json
    { 1, 35, 16, 1, 35, 16 },
    { 1, 40, 18, 1, 40, 18 },     // editable
    { 2, 55, 16, 2, 55, 16 },
    { 2, 0, 0, 2, 55, 16 },
    { 3, 20, 5, 3, 20, 5 },       // the ends of the ranges
    { 3, 65, 25, 3, 65, 25 },
    { 1, 19, 4, 1, 35, 16 },      // out of range: the default of the surface, never the nearest end
    { 1, 350, 26, 1, 35, 16 },
    { 2, 66, -3, 2, 55, 16 },
    { 4, 40, 16, 0, 40, 16 },     // unknown surface: off
    { 255, 50, 16, 0, 50, 16 },
  };
  for (auto &c : cases) {
    uint8_t surface = (uint8_t)c.surface;
    int max = c.max, min = c.min;
    flowlimit_normalize(&surface, &max, &min);
    if (surface != c.surfaceOut || max != c.maxOut || min != c.minOut) {
      printf("FAIL normalize(%d, %d, %d) = (%d, %d, %d)\n", c.surface, c.max, c.min, surface, max, min);
      failures++;
    }
    // the stored values read back unchanged (settings -> config.json -> settings)
    uint8_t surface2 = surface;
    int max2 = max, min2 = min;
    flowlimit_normalize(&surface2, &max2, &min2);
    CHECK(surface2 == surface && max2 == max && min2 == min);
    flowlimit_t fl;
    memset(&fl, 0, sizeof(fl));
    flowlimit_configure(&fl, (uint8_t)c.surface, c.max, c.min);
    CHECK(fl.surface == surface && fl.heat_max == max && fl.cool_min == min);
  }
  // the global starts off
  CHECK(flowlimit.surface == FLOWLIMIT_OFF && !flowlimit_on(&flowlimit));
}

// scenarios of the independent review of PR #23 (probes rv23/t.cpp and t2.cpp)
static void test_review_probes() {
  uint8_t cmd[N];
  char log[256];
  // room sensors (internal thermostat 2, thermistor 3): the request is a room setpoint, never touched
  for (int sensor = 2; sensor <= 3; sensor++) {
    flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t d = received(0, 0, 2);
    d.b[22] = (uint8_t)(((sensor + 1) << 4) | (sensor + 1));
    d.b[38] = t(21); d.b[40] = t(22); d.b[39] = t(24); d.b[41] = t(26);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(fl.sensor[0] == sensor && fl.sensor[1] == sensor);
    CHECK(wd(&fl, 0, cmd, log, sizeof(log)) == 0 && log[0] == 0);
    command(cmd); cmd[38] = t(21); cmd[40] = t(40); cmd[39] = t(10); cmd[41] = t(-5);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(21) && cmd[40] == t(40) && cmd[39] == t(10) && cmd[41] == t(-5));
    // direct mode as well
    d.b[28] = (uint8_t)(2 | (2 << 2));
    flowlimit_update(&fl, d.b, sizeof(d.b));
    command(cmd); cmd[38] = t(40);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(40));
    // the curve is still clamped and watched
    command(cmd); cmd[75] = t(45);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[75] == t(35));
    d.b[79] = t(50);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 0, cmd, log, sizeof(log)) == N);
    for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 79 ? t(35) : 0));
  }
  // the sensor per zone: zone 1 low nibble, zone 2 high nibble (TOP111 / TOP112)
  {
    flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t d = received(1, 1, 2);
    d.b[22] = 0x41;  // zone 1 water, zone 2 thermistor
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(fl.sensor[0] == 0 && fl.sensor[1] == 3);
    command(cmd); cmd[38] = t(40); cmd[40] = t(40);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[38] == t(35) && cmd[40] == t(40));
    d.b[22] = 0x12;  // zone 1 external thermostat (a flow request), zone 2 water
    flowlimit_update(&fl, d.b, sizeof(d.b));
    command(cmd); cmd[38] = t(40); cmd[40] = t(40);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 2 && cmd[38] == t(35) && cmd[40] == t(35));
    d.b[22] = 0x00;  // not known: left alone
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(fl.sensor[0] == -1 && fl.sensor[1] == -1);
    command(cmd); cmd[38] = t(40);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(40));
    d.b[38] = t(45);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 0, cmd, log, sizeof(log)) == 0);
  }
  // a received byte 0 is no value: cool curve bytes 0 must not turn a cool request of -2 into +5
  {
    flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t d = received(0, 0, 2);
    d.b[86] = d.b[87] = d.b[90] = d.b[91] = 0;
    d.b[39] = t(-2);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 5, cmd, log, sizeof(log)) == 0 && log[0] == 0);
    command(cmd); cmd[39] = t(-2); cmd[41] = t(-5);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[39] == t(-2) && cmd[41] == t(-5));
    command(cmd); cmd[39] = t(8);  // curve unknown: outside -5..5 it is a target
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 1 && cmd[39] == t(16));
    // heating: heat curve bytes 0, shift -3 passes, nothing sent
    d = received(0, 0, 2);
    d.b[75] = d.b[76] = 0;
    d.b[38] = t(-3);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 5, cmd, log, sizeof(log)) == 0);
    command(cmd); cmd[38] = t(5);
    CHECK(flowlimit_apply(&fl, cmd, N, NULL, 0) == 0 && cmd[38] == t(5));
    // zone 2 curve and request bytes 0: nothing for zone 2
    d = received(0, 0, 2);
    d.b[79] = d.b[80] = d.b[40] = 0;
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 5, cmd, log, sizeof(log)) == 0);
  }
  // radiators 55, curve 55, shift +5: corrected to 0
  {
    flowlimit_t fl = limit(FLOWLIMIT_RADIATOR, 55, 16);
    block_t d = received(0, 0, 0);
    d.b[75] = t(55); d.b[38] = t(5);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 5, cmd, log, sizeof(log)) == N);
    for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(0) : 0));
  }
  // direct mode in operating mode DHW: a heat request 45 is corrected, cooling not watched
  {
    flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t d = received(1, 0, 0);
    d.b[6] = (uint8_t)(1 << 6) | 33;
    d.b[38] = t(45); d.b[86] = t(5);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 5, cmd, log, sizeof(log)) == N);
    for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(35) : 0));
  }
  // invalid mode bits, water sensor: a request 40 is a target, corrected
  {
    flowlimit_t fl = limit(FLOWLIMIT_FLOOR, 35, 16);
    block_t d = received(0, 0, 0);
    d.b[28] = 0; d.b[38] = t(40);
    flowlimit_update(&fl, d.b, sizeof(d.b));
    CHECK(wd(&fl, 5, cmd, log, sizeof(log)) == N);
    for (int i = 4; i < N; i++) CHECK(cmd[i] == (i == 38 ? t(35) : 0));
  }
}

// Invariant over random states and datagrams: values only ever move towards the limit (a heating value is only
// lowered, a cooling value only raised), a 0 stays 0, nothing else changes, room setpoints are never touched,
// and a watchdog correction only holds such values.
static bool heatByte(int i) { return i == 38 || i == 40 || i == 75 || i == 76 || i == 79 || i == 80; }
static bool coolByte(int i) { return i == 39 || i == 41 || i == 86 || i == 87 || i == 90 || i == 91; }
static uint8_t randomTemp() {
  switch (rand() % 6) {
    case 0: return 0;
    case 1: return (uint8_t)rand();
    case 2: return t(rand() % 11 - 5);
    default: return t(rand() % 70 - 5);
  }
}

// an independent model of the request rule (flowlimit.h), for the random test: the expected request byte
static int modelRequest(const flowlimit_t *fl, const block_t *d, bool received, const uint8_t *cmd, int zone, bool cooling) {
  const int address = 38 + zone * 2 + (cooling ? 1 : 0);
  if (cmd[address] == 0) return 0;
  int field = (cmd[22] >> (zone * 4)) & 0x0F;
  int sensor = (field >= 1 && field <= 4) ? field - 1 : -1;
  if (field == 0 && received) { int r = ((d->b[22] >> (zone * 4)) & 0x0F); sensor = (r >= 1 && r <= 4) ? r - 1 : -1; }
  if (sensor != 0 && sensor != 1) return cmd[address];
  const int shift = cooling ? 2 : 0;
  int m = (cmd[28] >> shift) & 3;
  int mode = (m == 1 || m == 2) ? m - 1 : -1;
  if (m == 0 && received) { int r = (d->b[28] >> shift) & 3; mode = (r == 1 || r == 2) ? r - 1 : -1; }
  const int value = cmd[address] - 128;
  const int bound = cooling ? fl->cool_min : fl->heat_max;
  int target;
  if (mode == 1) {
    target = bound;
  } else {
    const int first = cooling ? (zone ? 90 : 86) : (zone ? 79 : 75);
    bool known = mode == 0;
    int points[2];
    for (int i = 0; i < 2 && known; i++) {
      // the curve byte as it is sent (already clamped) or the received one; a 0 is no value
      int sent = cmd[first + i];
      if (sent != 0) points[i] = cooling ? (sent - 128 < bound ? bound : sent - 128) : (sent - 128 > bound ? bound : sent - 128);
      else if (received && d->b[first + i] != 0) points[i] = d->b[first + i] - 128;
      else known = false;
    }
    if (known) {
      const int extreme = cooling ? (points[0] < points[1] ? points[0] : points[1]) : (points[0] > points[1] ? points[0] : points[1]);
      target = bound - extreme;
      if (cooling ? value >= target : value <= target) return cmd[address];
      target = target < -5 ? -5 : (target > 5 ? 5 : target);
    } else if (value < -5 || value > 5) {
      target = bound;
    } else {
      return cmd[address];
    }
  }
  if (cooling ? value >= target : value <= target) return cmd[address];
  return target + 128;
}
static void test_invariant() {
  srand(23);
  for (int round = 0; round < 20000; round++) {
    flowlimit_t fl;
    memset(&fl, 0, sizeof(fl));
    flowlimit_configure(&fl, (uint8_t)(1 + rand() % 3), 20 + rand() % 46, 5 + rand() % 21);
    block_t d;
    memset(d.b, 0, sizeof(d.b));
    d.b[0] = 0x71; d.b[1] = 0xC8; d.b[2] = 0x01; d.b[3] = 0x10;
    d.b[6] = (uint8_t)rand(); d.b[22] = (uint8_t)rand(); d.b[28] = (uint8_t)rand();
    if (rand() % 2) d.b[22] = (uint8_t)(((rand() % 2 + 1) << 4) | (rand() % 2 + 1));  // often water / external
    for (int i = 4; i < 110; i++) if (heatByte(i) || coolByte(i)) d.b[i] = randomTemp();
    const bool got = rand() % 8 != 0;
    if (got) flowlimit_update(&fl, d.b, sizeof(d.b));
    uint8_t cmd[N], before[N];
    command(cmd);
    if (rand() % 2) cmd[22] = (uint8_t)rand();
    if (rand() % 2) cmd[28] = (uint8_t)rand();
    for (int i = 4; i < N; i++) if (heatByte(i) || coolByte(i)) cmd[i] = randomTemp();
    memcpy(before, cmd, N);
    flowlimit_apply(&fl, cmd, N, NULL, 0);
    for (int zone = 0; zone < 2; zone++) {
      for (int cooling = 0; cooling < 2; cooling++) {
        const int address = 38 + zone * 2 + cooling;
        const int expected = modelRequest(&fl, &d, got, before, zone, cooling != 0);
        if (cmd[address] != expected) {
          printf("FAIL model round %d byte %d: %d -> %d, expected %d\n", round, address, before[address] - 128, cmd[address] - 128, expected - 128);
          failures++;
          return;
        }
      }
    }
    for (int i = 0; i < N; i++) {
      if (cmd[i] == before[i]) continue;
      bool ok = before[i] != 0 && ((heatByte(i) && cmd[i] < before[i]) || (coolByte(i) && cmd[i] > before[i]));
      if (i >= 38 && i <= 41) {
        const int zone = (i - 38) / 2;
        int field = (before[22] >> (zone * 4)) & 0x0F;
        int sensor = field ? field - 1 : (fl.have_data ? fl.sensor[zone] : -1);
        ok = ok && (sensor == 0 || sensor == 1);
      }
      if (!ok) {
        printf("FAIL invariant apply round %d byte %d: %d -> %d\n", round, i, before[i] - 128, cmd[i] - 128);
        failures++;
        return;
      }
    }
    char log[256];
    const int len = flowlimit_watchdog(&fl, (uint32_t)rand(), cmd, log, sizeof(log));
    if (len == 0) continue;
    CHECK(cmd[0] == 0xF1 && cmd[1] == 0x6C && cmd[2] == 0x01 && cmd[3] == 0x10);
    for (int i = 4; i < N; i++) {
      if (cmd[i] == 0) continue;
      bool ok = d.b[i] != 0 && ((heatByte(i) && cmd[i] < d.b[i]) || (coolByte(i) && cmd[i] > d.b[i]));
      if (i >= 38 && i <= 41) ok = ok && (fl.sensor[(i - 38) / 2] == 0 || fl.sensor[(i - 38) / 2] == 1);
      if (!ok) {
        printf("FAIL invariant watchdog round %d byte %d: received %d, sent %d\n", round, i, d.b[i] - 128, cmd[i] - 128);
        failures++;
        return;
      }
    }
  }
}

int main() {
  test_off_changes_nothing();
  test_curve_bytes();
  test_direct_requests();
  test_shift();
  test_unknown();
  test_watchdog();
  test_settings();
  test_review_probes();
  test_invariant();
  if (failures) {
    printf("%d checks failed\n", failures);
    return 1;
  }
  printf("PASS: flow limit (clamping, shift, unknown state, watchdog, settings)\n");
  return 0;
}
