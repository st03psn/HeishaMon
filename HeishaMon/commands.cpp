#include "commands.h"
#include <LittleFS.h>

//removed checksum from default query, is calculated in send_command
byte initialQuery[] = {0x31, 0x05, 0x10, 0x01, 0x00, 0x00, 0x00};
byte panasonicQuery[] = {0x71, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
byte optionalPCBQuery[] = {0xF1, 0x11, 0x01, 0x50, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0xE5, 0xFF, 0xFF, 0x00, 0xFF, 0xEB, 0xFF, 0xFF, 0x00, 0x00};
byte panasonicSendQuery[] PROGMEM = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#ifdef ESP32
extern QueueHandle_t pcbQueue;
#endif

// last complete data block received from the heat pump (HeishaMon.ino); only read here, for the
// nibble commands that must write back the unchanged other nibble of a shared byte
extern char actData[DATASIZE];

const char* mqtt_topic_values PROGMEM = "main";
const char* mqtt_topic_xvalues PROGMEM = "extra";
const char* mqtt_topic_commands PROGMEM = "commands";
const char* mqtt_topic_pcbvalues PROGMEM = "optional";
const char* mqtt_topic_1wire PROGMEM = "1wire";
const char* mqtt_topic_s0 PROGMEM = "s0";
const char* mqtt_logtopic PROGMEM = "log";

const char* mqtt_willtopic PROGMEM = "LWT";
const char* mqtt_iptopic PROGMEM = "ip";

const char* mqtt_send_raw_value_topic PROGMEM = "SendRawValue";

static unsigned int temp2hex(float temp) {
  int hextemp = 0;
  if (temp > 120) {
    hextemp = 0;
  } else if (temp < -78) {
    hextemp = 255;
  } else {
    byte Uref = 255;
    int constant = 3695;
    int R25 = 6340;
    byte T25 = 25;
    int Rf = 6480;
    float K = 273.15;
    float RT = R25 * exp(constant * (1 / (temp + K) - 1 / (T25 + K)));
    hextemp = Uref * (RT / (Rf + RT));
  }
  return hextemp;
}


unsigned int set_heatpump_state(char *msg, unsigned char *cmd, char *log_msg) {
  byte heatpump_state = 1;
  String set_heatpump_state_string(msg);

  if ( set_heatpump_state_string.toInt() == 1 ) {
    heatpump_state = 2;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heatpump state to %d"), heatpump_state - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[4] = heatpump_state;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_pump(char *msg, unsigned char *cmd, char *log_msg) {

  String set_pump_string(msg);

  byte pump_state = 16;
  if ( set_pump_string.toInt() == 1 ) {
    pump_state = 32;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set pump state to %d"), (pump_state / 16) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[4] = pump_state;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_max_pump_duty(char *msg, unsigned char *cmd, char *log_msg) {

  String set_pumpduty_string(msg);

  byte pumpduty = set_pumpduty_string.toInt() + 1;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set max pump duty to %d"), pumpduty - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[45] = pumpduty;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_quiet_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_quiet_mode_string(msg);

  byte quiet_mode = (set_quiet_mode_string.toInt() + 1) * 8;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set Quiet mode to %d"), quiet_mode / 8 - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[7] = quiet_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z1_heat_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z1 heat request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[38] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z1_cool_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z1 cool request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[39] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z2_heat_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z2 heat request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[40] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z2_cool_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z2 cool request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[41] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_bivalent_start_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent start temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[65] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_bivalent_ap_start_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent ap start temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[66] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_bivalent_ap_stop_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent stop ap temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[68] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_DHW(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_DHW_string(msg);

  byte force_DHW_mode = 64; //hex 0x40
  if ( set_force_DHW_string.toInt() == 1 ) {
    force_DHW_mode = 128; //hex 0x80
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force DHW mode to %d"), (force_DHW_mode / 64) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[4] = force_DHW_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_defrost(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_defrost_string(msg);

  byte force_defrost_mode = 0;
  if ( set_force_defrost_string.toInt() == 1 ) {
    force_defrost_mode = 2; //hex 0x02
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force defrost mode to %d"), force_defrost_mode / 2);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[8] = force_defrost_mode;
  }

  return sizeof(panasonicSendQuery);
}

// Validate the complete payload before touching the command buffer. The limits
// below describe the nonzero wire encoding; the heat pump may impose narrower
// model-specific installer-menu limits.
static unsigned int set_integer_field(char *msg, unsigned char *cmd, char *log_msg,
                                     byte address, int minimum, int maximum,
                                     int step, int divisor, int offset, int multiplier) {
  const size_t length = msg == NULL ? 0 : strlen(msg);
  bool valid = length > 0 && length <= 5;
  size_t index = valid && msg[0] == '-' ? 1 : 0;
  const bool negative = index == 1;
  valid = valid && index < length;
  int value = 0;
  for (; valid && index < length; ++index) {
    valid = msg[index] >= '0' && msg[index] <= '9';
    if (valid) value = value * 10 + msg[index] - '0';
  }
  if (negative) value = -value;
  if (!valid || value < minimum || value > maximum || value % step != 0) {
    snprintf_P(log_msg, 256, PSTR("Invalid setting for byte %u: expected integer %d..%d, step %d"),
               address, minimum, maximum, step);
    return 0;
  }
  memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
  cmd[address] = (value / divisor + offset) * multiplier;
  snprintf_P(log_msg, 256, PSTR("set byte %u to value %d"), address, value);
  return sizeof(panasonicSendQuery);
}

unsigned int set_heat_to_cool_temp(char *msg, unsigned char *cmd, char *log_msg) {
  return set_integer_field(msg, cmd, log_msg, 95, -127, 127, 1, 1, 128, 1);
}

unsigned int set_cool_to_heat_temp(char *msg, unsigned char *cmd, char *log_msg) {
  return set_integer_field(msg, cmd, log_msg, 96, -127, 127, 1, 1, 128, 1);
}

unsigned int set_room_operation_max_time(char *msg, unsigned char *cmd, char *log_msg) {
  return set_integer_field(msg, cmd, log_msg, 97, 30, 7620, 30, 30, 1, 1);
}

unsigned int set_dhw_heatup_max_time(char *msg, unsigned char *cmd, char *log_msg) {
  return set_integer_field(msg, cmd, log_msg, 98, 1, 254, 1, 1, 1, 1);
}

unsigned int set_quiet_mode_schedule(char *msg, unsigned char *cmd, char *log_msg) {
  return set_integer_field(msg, cmd, log_msg, 7, 0, 1, 1, 1, 1, 64);
}

// Bit-pair settings. Payload 0/1 is sent as b01/b10 in the pair; all other bits of the byte stay 0
// ("no change"), like SetQuietModeSchedule. Anything but "0" or "1" is rejected without sending.
unsigned int set_anti_freeze_mode(char *msg, unsigned char *cmd, char *log_msg) { // byte 20 bits 3-2, TOP109
  return set_integer_field(msg, cmd, log_msg, 20, 0, 1, 1, 1, 1, 4);
}

unsigned int set_liquid_type(char *msg, unsigned char *cmd, char *log_msg) { // byte 20 bits 7-6, TOP107
  return set_integer_field(msg, cmd, log_msg, 20, 0, 1, 1, 1, 1, 64);
}

unsigned int set_demand_control_enable(char *msg, unsigned char *cmd, char *log_msg) { // byte 27 bits 1-0, TOP150
  return set_integer_field(msg, cmd, log_msg, 27, 0, 1, 1, 1, 1, 1);
}

unsigned int set_heating_water_mode(char *msg, unsigned char *cmd, char *log_msg) { // byte 28 bits 1-0, TOP76
  return set_integer_field(msg, cmd, log_msg, 28, 0, 1, 1, 1, 1, 1);
}

unsigned int set_dhw_defrost(char *msg, unsigned char *cmd, char *log_msg) { // byte 30 bits 1-0, TOP151
  return set_integer_field(msg, cmd, log_msg, 30, 0, 1, 1, 1, 1, 1);
}

unsigned int set_static_pressure(char *msg, unsigned char *cmd, char *log_msg) { // byte 31 bits 5-4, TOP170
  return set_integer_field(msg, cmd, log_msg, 31, 0, 1, 1, 1, 1, 16);
}

// Zone 1 type: the whole byte 21 is the zone configuration code (TOP146). Only the single-zone codes
// 21 (0x15, Z1 room) and 25 (0x19, Z1 pool) are known for this write, see ProtocolByteDecrypt.md.
unsigned int set_zone_type(char *msg, unsigned char *cmd, char *log_msg) {
  const unsigned int length = set_integer_field(msg, cmd, log_msg, 21, 0, 1, 1, 1, 0, 1);
  if (length == 0) return 0;
  cmd[21] = cmd[21] == 0 ? 21 : 25;
  snprintf_P(log_msg, 256, PSTR("set zone 1 type to %s (byte 21 = %u)"), cmd[21] == 21 ? "room" : "pool", (unsigned int)cmd[21]);
  return length;
}

// Whole-number settings (same style as SetHeatToCoolTemp). The limits are the nonzero wire range.
unsigned int set_pool_delta(char *msg, unsigned char *cmd, char *log_msg) { // byte 58, K + 128, TOP153
  return set_integer_field(msg, cmd, log_msg, 58, -127, 127, 1, 1, 128, 1);
}

unsigned int set_heating_on_outdoor_temp(char *msg, unsigned char *cmd, char *log_msg) { // byte 108, degC + 128, TOP171
  return set_integer_field(msg, cmd, log_msg, 108, -127, 127, 1, 1, 128, 1);
}

unsigned int set_heating_on_delay(char *msg, unsigned char *cmd, char *log_msg) { // byte 109, minutes / 30 + 1, TOP172
  return set_integer_field(msg, cmd, log_msg, 109, 30, 7620, 30, 30, 1, 1);
}

unsigned int set_dhw_booster_delay(char *msg, unsigned char *cmd, char *log_msg) { // byte 60, minutes + 1, TOP173
  return set_integer_field(msg, cmd, log_msg, 60, 1, 254, 1, 1, 1, 1);
}

// Nibble fields share a byte with another setting. It is NOT verified that a zero nibble in the
// command frame means "no change" (the raw tests always wrote whole bytes), so the other nibble is
// never sent as zero: the new byte is built from the validated nibble and the unchanged other nibble
// of the last data block received from the heat pump. Without a data block nothing is sent.
// cmd[address] must already hold the new nibble in its final position (set_integer_field with
// multiplier 1 or 16) and the rest of cmd must be the empty command template.
static unsigned int merge_other_nibble(unsigned char *cmd, char *log_msg, byte address, bool newIsHigh) {
  if (!(actData[0] == 0x71 && actData[1] == (char)0xc8 && actData[2] == 0x01)) {
    snprintf_P(log_msg, 256, PSTR("Cannot set byte %u: no data block from the heat pump yet, the other nibble would be unknown"), address);
    return 0;
  }
  const byte current = (byte)actData[address];
  cmd[address] |= newIsHigh ? (current & 0x0F) : (current & 0xF0);
  return sizeof(panasonicSendQuery);
}

static unsigned int set_nibble_field(char *msg, unsigned char *cmd, char *log_msg, byte address, bool high,
                                     int minimum, int maximum, int step, int divisor, int offset) {
  unsigned char tmp[sizeof(panasonicSendQuery)];
  if (set_integer_field(msg, tmp, log_msg, address, minimum, maximum, step, divisor, offset, high ? 16 : 1) == 0) return 0;
  if (merge_other_nibble(tmp, log_msg, address, high) == 0) return 0;
  memcpy(cmd, tmp, sizeof(tmp));
  return sizeof(panasonicSendQuery);
}

// efficiency stage times, minutes / 5 + 1 per nibble: stage 1 = byte 33 low, stage 2 = byte 33 high, stage 3 = byte 34 low
unsigned int set_efficiency_stage1_time(char *msg, unsigned char *cmd, char *log_msg) { // TOP174
  return set_nibble_field(msg, cmd, log_msg, 33, false, 0, 70, 5, 5, 1);
}

unsigned int set_efficiency_stage2_time(char *msg, unsigned char *cmd, char *log_msg) { // TOP175
  return set_nibble_field(msg, cmd, log_msg, 33, true, 0, 70, 5, 5, 1);
}

unsigned int set_efficiency_stage3_time(char *msg, unsigned char *cmd, char *log_msg) { // TOP176, cloud range 0..30
  return set_nibble_field(msg, cmd, log_msg, 34, false, 0, 30, 5, 5, 1);
}

// sensor offsets in K, nibble = offset / 0.5 + 8 (8 = 0 K), whole or half degrees from -3 to 3
static bool parse_half_degrees(const char *msg, int &halfDegrees) {
  const size_t length = msg == NULL ? 0 : strlen(msg);
  size_t index = 0;
  bool negative = false;
  if (length > 0 && msg[0] == '-') { negative = true; index = 1; }
  if (index >= length || msg[index] < '0' || msg[index] > '9') return false;
  int whole = msg[index++] - '0';
  int half = 0;
  if (index < length) {
    if (msg[index] != '.' || index + 2 != length || (msg[index + 1] != '0' && msg[index + 1] != '5')) return false;
    half = msg[index + 1] == '5' ? 1 : 0;
  }
  halfDegrees = (whole * 2 + half) * (negative ? -1 : 1);
  return true;
}

static unsigned int set_sensor_offset(char *msg, unsigned char *cmd, char *log_msg, byte address, bool high) {
  int halfDegrees = 0;
  if (!parse_half_degrees(msg, halfDegrees) || halfDegrees < -6 || halfDegrees > 6) {
    snprintf_P(log_msg, 256, PSTR("Invalid sensor offset for byte %u: expected -3 to 3 K in steps of 0.5"), address);
    return 0;
  }
  unsigned char tmp[sizeof(panasonicSendQuery)];
  memcpy_P(tmp, panasonicSendQuery, sizeof(tmp));
  tmp[address] = (halfDegrees + 8) * (high ? 16 : 1);
  if (merge_other_nibble(tmp, log_msg, address, high) == 0) return 0;
  memcpy(cmd, tmp, sizeof(tmp));
  snprintf_P(log_msg, 256, PSTR("set sensor offset in byte %u to %d half degrees"), address, halfDegrees);
  return sizeof(panasonicSendQuery);
}

unsigned int set_sensor_offset_remote_th1(char *msg, unsigned char *cmd, char *log_msg) { // byte 35 high nibble, TOP177
  return set_sensor_offset(msg, cmd, log_msg, 35, true);
}

unsigned int set_sensor_offset_outdoor(char *msg, unsigned char *cmd, char *log_msg) { // byte 35 low nibble, TOP178
  return set_sensor_offset(msg, cmd, log_msg, 35, false);
}

// ---- Settings in bytes that carry several fields (read-modify-write) --------------------------------
// Bytes 25, 26, 27 and 31 hold more than one setting. Data byte 0 means "no change", but only whole
// bit pairs were verified that way, so these commands never rely on it: the new byte is built from
// the validated field and the unchanged remaining bits of the last data block from the heat pump, and
// the whole byte is sent (as in the raw write tests). Nothing is sent while no data block has been
// received (or the byte is still 0), and some commands additionally need a state in which the cloud
// offered the field; see MQTT-Topics.md.
static bool current_data_byte(char *log_msg, byte address, byte &current) {
  if (!(actData[0] == 0x71 && actData[1] == (char)0xc8 && actData[2] == 0x01) || actData[address] == 0) {
    snprintf_P(log_msg, 256, PSTR("Cannot set byte %u: no data block from the heat pump yet, the other bits would be unknown"), address);
    return false;
  }
  current = (byte)actData[address];
  return true;
}

// the cloud only offered the field in this state: (actData[address] & mask) == value
static bool require_state(char *log_msg, byte address, byte mask, byte value, const char *requirement) {
  if (((byte)actData[address] & mask) != value) {
    snprintf_P(log_msg, 256, PSTR("Cannot set this field now: it needs %s (byte %u)"), requirement, address);
    return false;
  }
  return true;
}

// bit pair at shift 0, 2, 4 or 6: payload 0..maximum is sent as b01, b10, b11
static unsigned int set_pair_field(char *msg, unsigned char *cmd, char *log_msg, byte address, byte shift, int maximum,
                                   byte requireAddress = 0, byte requireMask = 0, byte requireValue = 0, const char *requirement = NULL) {
  unsigned char tmp[sizeof(panasonicSendQuery)];
  if (set_integer_field(msg, tmp, log_msg, address, 0, maximum, 1, 1, 1, 1 << shift) == 0) return 0;
  byte current = 0;
  if (!current_data_byte(log_msg, address, current)) return 0;
  if (requireMask != 0 && !require_state(log_msg, requireAddress, requireMask, requireValue, requirement)) return 0;
  tmp[address] = (unsigned char)((current & ~(0x03 << shift)) | tmp[address]);
  memcpy(cmd, tmp, sizeof(tmp));
  return sizeof(panasonicSendQuery);
}

// Bivalent: byte 26 bits 1-0 (SetBivalentControl) and 3-2 (SetBivalentMode) already have commands.
// "Heating" (bits 5-4, TOP132, b01 no / b10 yes) was only seen in the cloud with advanced parallel (b11 in bits 3-2).
unsigned int set_bivalent_ap_heat(char *msg, unsigned char *cmd, char *log_msg) {
  return set_pair_field(msg, cmd, log_msg, 26, 4, 1, 26, 0x0C, 0x0C, "bivalent mode advanced parallel");
}

// byte 27 bits 5-4: b01 auto, b10 SG ready, b11 intelligent (TOP180). Bits 1-0 (demand control) and 3-2
// (SG ready) of the same byte are kept as read.
unsigned int set_bivalent_operating_mode(char *msg, unsigned char *cmd, char *log_msg) {
  return set_pair_field(msg, cmd, log_msg, 27, 4, 2);
}

// byte 27 bits 7-6: external pump b01 off / b10 on (TOP181), only seen in the cloud with bivalent mode alternative (b01 in bits 3-2 of byte 26)
unsigned int set_bivalent_external_pump(char *msg, unsigned char *cmd, char *log_msg) {
  return set_pair_field(msg, cmd, log_msg, 27, 6, 1, 26, 0x0C, 0x04, "bivalent mode alternative");
}

unsigned int set_bivalent_ap_start_delay(char *msg, unsigned char *cmd, char *log_msg) { // byte 67, minutes + 1, TOP136
  return set_integer_field(msg, cmd, log_msg, 67, 1, 254, 1, 1, 1, 1);
}

unsigned int set_bivalent_ap_stop_delay(char *msg, unsigned char *cmd, char *log_msg) { // byte 69, minutes + 1, TOP137
  return set_integer_field(msg, cmd, log_msg, 69, 1, 254, 1, 1, 1, 1);
}

// Extra pump, byte 31 low nibble. Only four complete values were ever read or written by the cloud, so
// only those are sent: 0x5 not used, 0xA heating (efficiency), 0xB DHW (efficiency), 0x7 DHW (comfort).
// The split into use (bits 1-0: b01 not used, b10 heating, b11 DHW) and mode (bits 3-2: b01 comfort,
// b10 efficiency) is derived from these four values. Bits 5-4 (static pressure) and 7-6 are kept as read.
// The cloud only allowed the setting with a DHW tank connected (bits 1-0 of byte 24 = b10), so that is required here too.
static unsigned int set_extra_pump_nibble(char *log_msg, unsigned char *cmd, byte nibble) {
  byte current = 0;
  if (!current_data_byte(log_msg, 31, current)) return 0;
  if (!require_state(log_msg, 24, 0x03, 0x02, "a connected DHW tank")) return 0;
  unsigned char tmp[sizeof(panasonicSendQuery)];
  memcpy_P(tmp, panasonicSendQuery, sizeof(tmp));
  tmp[31] = (unsigned char)((current & 0xF0) | nibble);
  memcpy(cmd, tmp, sizeof(tmp));
  snprintf_P(log_msg, 256, PSTR("set extra pump nibble of byte 31 to %u"), (unsigned int)nibble);
  return sizeof(panasonicSendQuery);
}

unsigned int set_extra_pump(char *msg, unsigned char *cmd, char *log_msg) { // 0 = not used, 1 = heating, 2 = DHW; TOP182
  unsigned char tmp[sizeof(panasonicSendQuery)];
  if (set_integer_field(msg, tmp, log_msg, 31, 0, 2, 1, 1, 0, 1) == 0) return 0;
  byte nibble = 0x5;
  if (tmp[31] == 1) nibble = 0xA;
  if (tmp[31] == 2) nibble = ((byte)actData[31] & 0x03) == 0x03 ? ((byte)actData[31] & 0x0F) : 0xB; // keep comfort/efficiency when already DHW
  return set_extra_pump_nibble(log_msg, cmd, nibble);
}

unsigned int set_extra_pump_mode(char *msg, unsigned char *cmd, char *log_msg) { // 0 = comfort, 1 = efficiency; TOP183; only with DHW use
  unsigned char tmp[sizeof(panasonicSendQuery)];
  if (set_integer_field(msg, tmp, log_msg, 31, 0, 1, 1, 1, 0, 1) == 0) return 0;
  byte current = 0;
  if (!current_data_byte(log_msg, 31, current)) return 0;
  if (!require_state(log_msg, 31, 0x03, 0x03, "the extra pump set to DHW")) return 0;
  return set_extra_pump_nibble(log_msg, cmd, tmp[31] == 0 ? 0x7 : 0xB);
}

// Back-up heater capacity, byte 25 bits 3-2 (TOP147): 3 kW = b01, 6 kW = b10. 9 kW (b11) is only
// assumed from the read side and is not accepted. Bits 1-0 (tank heater type) and 7-4 are sent as read.
unsigned int set_internal_heater_capacity(char *msg, unsigned char *cmd, char *log_msg) {
  unsigned char tmp[sizeof(panasonicSendQuery)];
  if (set_integer_field(msg, tmp, log_msg, 25, 3, 6, 3, 3, 0, 4) == 0) return 0;
  byte current = 0;
  if (!current_data_byte(log_msg, 25, current)) return 0;
  tmp[25] = (unsigned char)((current & ~0x0C) | tmp[25]);
  memcpy(cmd, tmp, sizeof(tmp));
  return sizeof(panasonicSendQuery);
}

// Single-byte temperature settings (degC or K + 128), same style as SetPoolDelta
unsigned int set_solar_on_delta(char *msg, unsigned char *cmd, char *log_msg) { // byte 61, TOP102
  return set_integer_field(msg, cmd, log_msg, 61, -127, 127, 1, 1, 128, 1);
}

unsigned int set_solar_off_delta(char *msg, unsigned char *cmd, char *log_msg) { // byte 62, TOP103
  return set_integer_field(msg, cmd, log_msg, 62, -127, 127, 1, 1, 128, 1);
}

unsigned int set_solar_frost_protection(char *msg, unsigned char *cmd, char *log_msg) { // byte 63, TOP104
  return set_integer_field(msg, cmd, log_msg, 63, -127, 127, 1, 1, 128, 1);
}

unsigned int set_room_holiday_shift_temp(char *msg, unsigned char *cmd, char *log_msg) { // byte 43, TOP45
  return set_integer_field(msg, cmd, log_msg, 43, -127, 127, 1, 1, 128, 1);
}

unsigned int set_sterilization_temp(char *msg, unsigned char *cmd, char *log_msg) {
  if (msg == NULL || strlen(msg) != 2 || msg[0] < '0' || msg[0] > '9' || msg[1] < '0' || msg[1] > '9') {
    snprintf_P(log_msg, 256, PSTR("Invalid sterilization temperature: expected integer 55-75 C"));
    return 0;
  }
  const int temperature = (msg[0] - '0') * 10 + (msg[1] - '0');
  if (temperature < 55 || temperature > 75) {
    snprintf_P(log_msg, 256, PSTR("Invalid sterilization temperature: expected integer 55-75 C"));
    return 0;
  }

  memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
  cmd[100] = temperature + 128;
  snprintf_P(log_msg, 256, PSTR("set sterilization temperature to %d C"), temperature);
  return sizeof(panasonicSendQuery);
}

unsigned int set_sterilization_max_time(char *msg, unsigned char *cmd, char *log_msg) {
  int minutes = 0;
  const size_t length = msg == NULL ? 0 : strlen(msg);
  if (length > 0 && length <= 2) {
    for (size_t i = 0; i < length; ++i) {
      if (msg[i] < '0' || msg[i] > '9') {
        minutes = 0;
        break;
      }
      minutes = minutes * 10 + (msg[i] - '0');
    }
  }
  if (minutes < 5 || minutes > 60 || minutes % 5 != 0) {
    snprintf_P(log_msg, 256, PSTR("Invalid sterilization holding time: expected multiple of 5 minutes from 5 to 60"));
    return 0;
  }

  memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
  cmd[101] = minutes + 1;
  snprintf_P(log_msg, 256, PSTR("set sterilization holding time to %d minutes"), minutes);
  return sizeof(panasonicSendQuery);
}

unsigned int set_force_sterilization(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_sterilization_string(msg);

  byte force_sterilization_mode = 0;
  if ( set_force_sterilization_string.toInt() == 1 ) {
    force_sterilization_mode = 4; //hex 0x04
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force sterilization mode to %d"), force_sterilization_mode / 4);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[8] = force_sterilization_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_heater(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_heater_string(msg);

  byte force_heater_mode = 4; //hex 0x04
  if ( set_force_heater_string.toInt() == 1 ) {
    force_heater_mode = 8; //hex 0x08
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force heater mode to %d"), (force_heater_mode / 4) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[5] = force_heater_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_holiday_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_holiday_string(msg);

  byte set_holiday = 16; //hex 0x10
  if ( set_holiday_string.toInt() == 1 ) {
    set_holiday = 32; //hex 0x20
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set holiday mode to %d"), (set_holiday / 16) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[5] = set_holiday;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_powerful_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_powerful_string(msg);

  byte set_powerful = (set_powerful_string.toInt() + 1) & 0b111;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set powerful mode to %d"), set_powerful - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[7] = set_powerful;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_DHW_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_DHW_temp_string(msg);

  byte set_DHW_temp = set_DHW_temp_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set DHW temperature to %d"), set_DHW_temp - 128);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[42] = set_DHW_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_operation_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_mode_string(msg);

  byte set_mode;
  switch (set_mode_string.toInt()) {
    case 0: set_mode = 18; break;
    case 1: set_mode = 19; break;
    case 2: set_mode = 24; break;
    case 3: set_mode = 33; break;
    case 4: set_mode = 34; break;
    case 5: set_mode = 35; break;
    case 6: set_mode = 40; break;
    default: set_mode = 0; break;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heat pump mode to %d"), set_mode);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[6] = set_mode;
  }

  return sizeof(panasonicSendQuery);
}


unsigned int set_bivalent_control(char *msg, unsigned char *cmd, char *log_msg) {

  byte set_bcontrol = 1;
  String set_bivalent_control_string(msg);

  if ( set_bivalent_control_string.toInt() == 1 ) {
    set_bcontrol = 2;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent control to %d"), set_bcontrol - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[26] = set_bcontrol;
  }

  return sizeof(panasonicSendQuery);
}


unsigned int set_bivalent_mode(char *msg, unsigned char *cmd, char *log_msg) {

  byte set_bmode = 4; // alternative mode
  String set_bivalent_mode_string(msg);

  if ( set_bivalent_mode_string.toInt() == 1 ) { //parallel mode
    set_bmode = 8;
  }
  if ( set_bivalent_mode_string.toInt() == 2 ) { //advanced parallel mode
    set_bmode = 12;
  }
  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent mode to %d"), (set_bmode / 4) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[26] = set_bmode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_curves(char *msg, unsigned char *cmd, char *log_msg) {
  memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));

  JsonDocument jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, msg);
  if (!error) {
    char tmpmsg[256] = { 0 };
    JsonVariant jsonValue;
    snprintf(tmpmsg, 255, "SetCurves JSON received ok");
    memcpy(log_msg, tmpmsg, sizeof(tmpmsg));
    //set correct bytes according to the values in json and if not exists keep default 0x00 value which keeps current setting for this byte
    jsonValue = jsonDoc["zone1"]["heat"]["target"]["high"]; if (!jsonValue.isNull()) cmd[75] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["heat"]["target"]["low"]; if (!jsonValue.isNull()) cmd[76] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["heat"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[77] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["heat"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[78] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["target"]["high"]; if (!jsonValue.isNull()) cmd[79] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["target"]["low"]; if (!jsonValue.isNull()) cmd[80] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[81] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[82] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["target"]["high"]; if (!jsonValue.isNull()) cmd[86] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["target"]["low"]; if (!jsonValue.isNull()) cmd[87] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[88] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[89] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["target"]["high"]; if (!jsonValue.isNull()) cmd[90] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["target"]["low"]; if (!jsonValue.isNull()) cmd[91] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[92] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[93] = jsonValue.as<int>() + 128;
  } else {
    char tmpmsg[256] = { 0 };
    snprintf(tmpmsg, 255, "SetCurves JSON decode failed!");
    memcpy(log_msg, tmpmsg, sizeof(tmpmsg));
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_zones(char *msg, unsigned char *cmd, char *log_msg) {

  String set_mode_string(msg);

  byte set_mode;
  switch (set_mode_string.toInt()) {
    case 0: set_mode = 64; break;
    case 1: set_mode = 128; break;
    case 2: set_mode = 192; break;
    default: set_mode = 0; break;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set zones active state to %d"), set_mode);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[6] = set_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_floor_heat_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set floor heat delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[84] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_floor_cool_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set floor cool delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[94] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_dhw_heat_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set DHW heat delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[99] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_reset(char *msg, unsigned char *cmd, char *log_msg) {
  byte resetRequest = 0;
  String set_reset_string(msg);

  if ( set_reset_string.toInt() == 1 ) {
    resetRequest = 1;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set reset to %d"), resetRequest);
    memcpy(log_msg, tmp, sizeof(tmp));
  }


  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[8] = resetRequest;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_heater_delay_time(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 1;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater delay time to %d"), byteValue - 1 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[104] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_heater_start_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater start delta to %d"), byteValue - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[105] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_heater_stop_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater stop delta to %d"), byteValue - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[106] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_main_schedule(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = 64; //hex 0x40

  if ( stringValue.toInt() == 1 ) {
    byteValue = 128; //hex 0x80
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set main schedule to %d"), (byteValue / 64) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[5] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_alt_external_sensor(char *msg, unsigned char *cmd, char *log_msg) {

  String set_alt_string(msg);

  byte set_alt = 16;
  if ( set_alt_string.toInt() == 1 ) {
    set_alt = 32;
  }

    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set alternative external sensor to %d"), ((set_alt / 16) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[20] = set_alt;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_external_pad_heater(char *msg, unsigned char *cmd, char *log_msg) {

  String set_pad_string(msg);

  byte set_pad = 16;
  if ( set_pad_string.toInt() == 1 ) {
    set_pad = 32;
  }
  if ( set_pad_string.toInt() == 2 ) {
    set_pad = 48;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external pad heater to %d"), ((set_pad / 16) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[25] = set_pad;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_buffer_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set buffer tank delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[59] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_buffer(char *msg, unsigned char *cmd, char *log_msg) {

  String set_buffer_string(msg);

  byte set_buffer = 4;
  if ( set_buffer_string.toInt() == 1 ) {
    set_buffer = 8;
  }

    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set buffer enabled to %d"), ((set_buffer / 4) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[24] = set_buffer;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_heatingoffoutdoortemp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_heatingoffoutdoortemp_string(msg);

  byte request_temp = set_heatingoffoutdoortemp_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heating off outdoor temp %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[83] = request_temp;
  }

  return sizeof(panasonicSendQuery);
  
}

unsigned int set_external_control(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=1;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external control enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

unsigned int set_external_heat_cool_control(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=4;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external cool/heat control enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

unsigned int set_external_error(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=16;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external error signal enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

unsigned int set_heatingcontrol(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=30;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heating control %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 2;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_smart_dhw(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=24;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set smart dhw %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 6;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_quiet_mode_priority(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=11;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set quiet mode priority %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 4;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_pump_flowrate_mode(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=29;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set pump flowrate mode %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 4;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_dhw_sensor_selection(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address = 11;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set dhw sensor selection %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_dhw_heater_state(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address = 9;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set dhw heater state %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 2;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_room_heater_state(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address = 9;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set room heater state %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_heater_on_outdoor_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater on outdoor temp to %d"), byteValue - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[85] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_external_compressor_control(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=64;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external compressor control enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

//start of optional pcb commands
unsigned int set_byte_6(int val, int base, int bit, char *log_msg, const char *func) {
  unsigned char hex = (optionalPCBQuery[6] & ~(base << bit)) | (val << bit);

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to state %d (result byte 6: %02x)"), func, val, hex);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[6] = hex;
  }

  return sizeof(optionalPCBQuery);
}

unsigned int set_byte_9(char *msg, char *log_msg) {
  String set_pcb_string(msg);

  byte set_pcb_value = set_pcb_string.toInt();

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to %02x"), __FUNCTION__, set_pcb_value);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[9] = set_pcb_value;
  }
  return sizeof(optionalPCBQuery);
}

unsigned int set_heat_cool_mode(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = (set_pcb_string.toInt() == 1);

  return set_byte_6(set_pcb_value, 0b1, 7, log_msg, __FUNCTION__);
}

unsigned int set_compressor_state(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = (set_pcb_string.toInt() == 1);

  return set_byte_6(set_pcb_value, 0b1, 6, log_msg, __FUNCTION__);
}

unsigned int set_smart_grid_mode(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = set_pcb_string.toInt();

  if (set_pcb_value < 4) {
    return set_byte_6(set_pcb_value, 0b11, 4, log_msg, __FUNCTION__);
  } else {
    return 0;
  }
}

unsigned int set_external_thermostat_1_state(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = set_pcb_string.toInt();

  if (set_pcb_value < 4) {
    return set_byte_6(set_pcb_value, 0b11, 2, log_msg, __FUNCTION__);
  } else {
    return 0;
  }
}

unsigned int set_external_thermostat_2_state(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = set_pcb_string.toInt();

  if (set_pcb_value < 4) {
    return set_byte_6(set_pcb_value, 0b11, 0, log_msg, __FUNCTION__);
  } else {
    return 0;
  }
}

unsigned int set_demand_control(char *msg, char *log_msg) {
  String set_pcb_string(msg);

  byte set_pcb_value = set_pcb_string.toInt();

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to %02x"), __FUNCTION__, set_pcb_value);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[14] = set_pcb_value;
  }

  return sizeof(optionalPCBQuery);
}

unsigned int set_xxx_temp(char *msg, char *log_msg, int byte, const char *func) {
  String set_pcb_string(msg);

  float temp = set_pcb_string.toFloat();

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to temp %.2f (%02x)"), func, temp, temp2hex(temp));
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[byte] = temp2hex(temp);
  }

  return sizeof(optionalPCBQuery);
}

unsigned int set_pool_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 7, __FUNCTION__);
}

unsigned int set_buffer_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 8, __FUNCTION__);
}

unsigned int set_z1_room_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 10, __FUNCTION__);
}

unsigned int set_z1_water_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 16, __FUNCTION__);
}

unsigned int set_z2_room_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 11, __FUNCTION__);
}

unsigned int set_z2_water_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 15, __FUNCTION__);
}

unsigned int set_solar_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 13, __FUNCTION__);
}




void send_heatpump_command(char* topic, char *msg, bool (*send_command)(byte*, int), void (*log_message)(char*), bool optionalPCB) {
  unsigned char cmd[256] = { 0 };
  char log_msg[256] = { 0 };
  unsigned int len = 0;

  for (unsigned int i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    cmdStruct tmp;
    memcpy_P(&tmp, &commands[i], sizeof(tmp));
    if (strcmp(topic, tmp.name) == 0) {
      len = tmp.func(msg, cmd, log_msg);
      log_message(log_msg);
      if (len > 0) send_command(cmd, len);
    }
  }

  if (optionalPCB) {
    //run for optional pcb commands
    for (unsigned int i = 0; i < sizeof(optionalCommands) / sizeof(optionalCommands[0]); i++) {
      optCmdStruct tmp;
      memcpy_P(&tmp, &optionalCommands[i], sizeof(tmp));
      if (strcmp(topic, tmp.name) == 0) {
        len = tmp.func(msg, log_msg);
        log_message(log_msg);
#ifdef ESP32        
       xQueueOverwrite(pcbQueue, optionalPCBQuery);
#endif          
      }
    }
  }

}


bool saveOptionalPCB(byte* command, int length) {
  if (LittleFS.begin()) {
    File pcbfile = LittleFS.open("/optionalpcb.raw", "w");
    if (pcbfile) {
      pcbfile.write(command, length);
      pcbfile.close();
      return true;
    }

  }
  return false;
}
bool loadOptionalPCB(byte* command, int length) {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/optionalpcb.raw")) {
      File pcbfile = LittleFS.open("/optionalpcb.raw", "r");
      if (pcbfile) {
        pcbfile.read(command, length);
        pcbfile.close();
        return true;
      }
    }
  }
  return false;
}
