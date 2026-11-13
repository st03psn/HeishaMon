# Main topic reference for integrations (TOP0–TOP196)

This page lists every main data topic (`main/...`, TOP0–TOP196) with the command that writes it, the setting it depends on, its values and the series whose service manual has the matching menu item. It is meant to help an integration (Home Assistant or similar) decide which topics apply to a unit and which command writes them. Topic names, bytes and value tables are those of [MQTT-Topics.md](MQTT-Topics.md) and `HeishaMon/decode.h`; extra data (`extra/...`, XTOP) and optional PCB topics (`optional/...`, OPT) are not covered here.

## Sources

- Panasonic service manuals (order number as printed on page 1):
  - M series: PCZ2411004CE, indoor unit WH-SDC0316M9E8, outdoor units WH-WXG09ME8, WH-WXG12ME8, WH-WXG16ME8.
  - L series: PAPAMY2303060CE, all-in-one WH-ADC0509L3E5 / L6E5 (and variants), outdoor units WH-WDG05LE5, WH-WDG07LE5, WH-WDG09LE5.
  - K series: PAPAMY2306062CE, indoor units WH-SDC0309K3E5, WH-SDC0309K6E5, outdoor units WH-UDZ03KE5 to WH-UDZ09KE5.
  - J series: PAPAMY2003034CE, monoblock WH-MDC05J3E5, WH-MDC07J3E5, WH-MDC09J3E5.
- The operating manual of the M series, for a few dependencies (marked in the notes).
- Tests on one M-series unit, WH-SDC0316M9E8 with WH-WXG09ME8 (one heating circuit, no DHW tank, no buffer tank, no solar). See [AquareaParity.md](AquareaParity.md).

Availability on series other than M is taken from the manuals only. Nothing was tested on L, K or J.

## How to read the tables

- **Kind:** *measurement* = sensor or value computed by the heat pump; *setting* = value stored in the unit that a user or installer changes; *state* = current operating state (some can be triggered by a command); *counter* = running hours or starts; *model info* = model identification.
- **Write command:** command topic `commands/<Name>`, with the SET number of MQTT-Topics.md. "–" = no command. A command sends only its own field; all other bytes and fields mean "no change".
- **Parent / only meaningful when:** the remote controller and the Aquarea Service Cloud hide or grey out the setting unless the parent has this value ("takes effect only when" is marked as such). HeishaMon does not check these conditions, and the heat pump accepts child values while the parent is off (tested on M). Mainly from the M service manual; other series may differ.
- **Range / values:** the published values (numbers as in the value tables). "cmd" = what the command accepts (encoding range, not the menu limit). "menu" = installer menu range in the manuals, with the series; "M:" marks an M-only value. HeishaMon does not enforce menu ranges. The older commands (SET1–SET48) mostly do not check the range at all: numbers are sent as given, and on/off or choice values outside the allowed range are sent as 0.
- **Series in manuals (M L K J):** only for items of the installer, service or custom menu. ✓ = in that series' manual, – = not in that manual, ? = unclear or not covered by the comparison. Blank = measurement, state, user setting, or not compared.
- **Tested on M:** ✓ = the command was written with a changed value on the M-series unit, read back on this topic and restored. Blank = not tested this way (many older commands are long in use but were not part of these tests).

When a model does not provide the byte of a topic (byte = 0), the topic shows a value outside the range of the setting (for example -128 °C, -0.1 kW, -5 minutes or -4.0 K); two-bit status fields show -1. A topic group that is switched off is still decoded but not published (see "Topic groups" in MQTT-Topics.md).

## Basic (always on)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 0 | Heatpump_State | state | SET1 SetHeatpump | – | 0=off, 1=on | | |
| 1 | Pump_Flow | measurement | – | – | l/min | | |
| 2 | Force_DHW_State | state | SET10 SetForceDHW | TOP100 = 1; operating mode with DHW (TOP4 = 3–6) | 0=off, 1=on | | |
| 3 | Quiet_Mode_Schedule | setting | SET55 SetQuietModeSchedule | – | 0=inactive, 1=active [1] | | ✓ |
| 4 | Operating_Mode_State | setting | SET9 SetOperationMode | cool modes: TOP168 = 1; DHW modes: TOP100 = 1; auto not usable with TOP120 = 1 | 0=heat, 1=cool, 2=auto(heat), 3=DHW, 4=heat+DHW, 5=cool+DHW, 6=auto(heat)+DHW, 7=auto(cool), 8=auto(cool)+DHW; SET9 writes 0–6 | | |
| 5 | Main_Inlet_Temp | measurement | – | – | °C | | |
| 6 | Main_Outlet_Temp | measurement | – | – | °C | | |
| 7 | Main_Target_Temp | measurement | – | – | °C, computed by the heat pump [2] | | |
| 8 | Compressor_Freq | measurement | – | – | Hz | | |
| 9 | DHW_Target_Temp | setting | SET11 SetDHWTemp | TOP100 = 1 | cmd 40–75 °C (not checked); menu K/J: 40–75 external, 40–65 internal heater [3] | | |
| 10 | DHW_Temp | measurement | – | TOP100 = 1 [4] | °C | | |
| 11 | Operations_Hours | counter | – | – | hours | | |
| 12 | Operations_Counter | counter | – | – | starts | | |
| 13 | Main_Schedule_State | setting | SET24 SetMainSchedule | not usable with TOP120 = 1 | 0=inactive, 1=active | | |
| 14 | Outside_Temp | measurement | – | – | °C | | |
| 15 | Heat_Power_Production | measurement | – | – | W [5] | | |
| 16 | Heat_Power_Consumption | measurement | – | – | W [5] | | |
| 17 | Powerful_Mode_Time | setting | SET4 SetPowerfulMode | heating only; not effective with TOP76 = 1 [6] | 0=off, 1=30 min, 2=60 min, 3=90 min | | |
| 18 | Quiet_Mode_Level | setting | SET3 SetQuietMode | – | 0=off, 1–3 = level 1–3 | | |
| 19 | Holiday_Mode_State | state | SET2 SetHolidayMode | – | 0=off, 1=scheduled, 2=active; SET2 writes 0/1 | | |
| 20 | ThreeWay_Valve_State | state | – | – | 0=room, 1=DHW | | |
| 26 | Defrosting_State | state | SET12 SetForceDefrost (trigger) | – | 0=off, 1=on | | |
| 38 | Cool_Power_Production | measurement | – | – | W [5] | | |
| 39 | Cool_Power_Consumption | measurement | – | – | W [5] | | |
| 40 | DHW_Power_Production | measurement | – | – | W [5] | | |
| 41 | DHW_Power_Consumption | measurement | – | – | W [5] | | |
| 44 | Error | state | – (SET48 SetReset clears a latched fault) | – | last active error code, e.g. H72 | | |
| 92 | Heat_Pump_Model | model info | – | – | see HeatPumpType.md | | |
| 160 | Quiet_Mode_Actual_State | state | – | – | 0=off, 1=on [7] | | |
| 161 | Powerful_Mode_Actual_State | state | – | – | 0=off, 1=on [7] | | |

1. SetQuietModeSchedule switches the existing schedule on or off; it does not set the times or the level.
2. With a buffer tank the target follows the buffer target, otherwise the active zone (M service manual pp. 157, 161).
3. The tank target range is given only in the K and J control chapters; none was found in the M and L manuals.
4. On the M test unit (no tank) the byte read 0 (-128 °C) with TOP100 = 0 and -20 °C with TOP100 = 1.
5. On heat pumps with the extra data block (K/L series and newer) these values are invalid; use XTOP0–XTOP5.
6. M service manual p. 177; with an optional PCB the zones are raised instead of the main outlet.
7. Actual state, distinct from the configured level (TOP18) and time (TOP17).

## Heating & cooling (`topics_heatcool`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 22 | DHW_Heat_Delta | setting | SET20 SetDHWHeatDelta | TOP100 = 1 | -12 to -2 K (menu, all series) | ✓ ✓ ✓ ✓ | |
| 23 | Heat_Delta | setting | SET18 SetFloorHeatDelta | TOP106 = 0 | 1–15 K (menu, all series) | ✓ ✓ ✓ ✓ | |
| 24 | Cool_Delta | setting | SET19 SetFloorCoolDelta | TOP106 = 0; TOP168 = 1 | 1–15 K (menu, all series) | ✓ ✓ ✓ ✓ | |
| 27 | Z1_Heat_Request_Temp | setting | SET5 SetZ1HeatRequestTemperature | TOP76: 0 = shift, 1 = direct [1] | shift -5 to 5 K, or direct 20 °C to max; menu direct L 20–75, K/J 20–60 °C | | |
| 28 | Z1_Cool_Request_Temp | setting | SET6 SetZ1CoolRequestTemperature | TOP168 = 1; TOP81: 0 = shift, 1 = direct [1] | shift -5 to 5 K, or direct °C [2] | | |
| 29 | Z1_Heat_Curve_Target_High_Temp | setting | SET16 SetCurves | TOP76 = 0 | °C, water temperature at the lowest outdoor point; menu L 20–75 (WH-WDG), K/J 20–60 °C [3] | ✓ ✓ ✓ ✓ | |
| 30 | Z1_Heat_Curve_Target_Low_Temp | setting | SET16 SetCurves | TOP76 = 0 | °C, as TOP29 [3] | ✓ ✓ ✓ ✓ | |
| 31 | Z1_Heat_Curve_Outside_High_Temp | setting | SET16 SetCurves | TOP76 = 0 | °C, lowest outdoor point; menu L/K/J -20 to 15 °C [3] | ✓ ✓ ✓ ✓ | |
| 32 | Z1_Heat_Curve_Outside_Low_Temp | setting | SET16 SetCurves | TOP76 = 0 | °C, highest outdoor point; menu as TOP31 [3] | ✓ ✓ ✓ ✓ | |
| 33 | Room_Thermostat_Temp | measurement | – | – | °C | | |
| 36 | Z1_Water_Temp | measurement | – | – | °C [4] | | |
| 42 | Z1_Water_Target_Temp | measurement | – | – | °C, computed by the heat pump [5] | | |
| 45 | Room_Holiday_Shift_Temp | setting | SET82 SetRoomHolidayShiftTemp | takes effect only when TOP19 = 2 | K; cmd -127 to 127; topic description -15 to 15 | | ✓ |
| 56 | Z1_Temp | measurement | – | – | °C | | |
| 72 | Z1_Cool_Curve_Target_High_Temp | setting | SET16 SetCurves | TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 73 | Z1_Cool_Curve_Target_Low_Temp | setting | SET16 SetCurves | TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 74 | Z1_Cool_Curve_Outside_High_Temp | setting | SET16 SetCurves | TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 75 | Z1_Cool_Curve_Outside_Low_Temp | setting | SET16 SetCurves | TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 76 | Heating_Mode | setting | SET71 SetHeatingWaterMode | – | 0=compensation curve, 1=direct [6] | ✓ ✓ ✓ ✓ | ✓ |
| 77 | Heating_Off_Outdoor_Temp | setting | SET29 SetHeatingOffOutdoorTemp | – | cmd 5–35 °C (not checked); menu M: 6–35, L/K/J 5–35 °C [7] | ✓ ✓ ✓ ✓ | |
| 79 | Heat_To_Cool_Temp | setting | SET51 SetHeatToCoolTemp | TOP168 = 1; at least TOP80 + 1 | cmd -127 to 127 °C; menu M/L 11–25, K/J 5–25 °C | ✓ ✓ ✓ ✓ | ✓ |
| 80 | Cool_To_Heat_Temp | setting | SET52 SetCoolToHeatTemp | TOP168 = 1 | cmd -127 to 127 °C; menu M/L 5–14, K/J 5–25 °C [8] | ✓ ✓ ✓ ✓ | ✓ |
| 81 | Cooling_Mode | setting | SET72 SetCoolingWaterMode | TOP168 = 1 | 0=compensation curve, 1=direct [6] | ✓ ✓ ✓ ✓ | ✓ |
| 94 | Zones_State | setting | SET17 SetZones | – | 0=zone 1 active, 1=zone 2 active, 2=both | | |
| 124 | Z1_Pump_State | state | – | – | 0=off, 1=on | | |
| 127 | Z1_Valve_PID | measurement | – | – | % | | |
| 166 | Z1_Active | state | – | – | 0=inactive, 1=active (activity bit, not TOP94) | | |
| 169 | Heat_Request_Temp_With_Buffer | measurement | – | – | °C, shift/direct target including the buffer delta | | |

1. With an internal or external thermostat or a thermistor (TOP111 = 1–3) the user sets a room temperature instead (M service manual p. 156). See also the note on direct temperatures in MQTT-Topics.md.
2. The topic description gives 5–20 °C for direct cooling, the SET6 text "20 to max". No manual range found.
3. Curve ranges are given in the L, K and J menus; the M manual gives only the default points (-5/15 °C → 55/35 °C for heating, 20/30 °C → 15/10 °C for cooling).
4. Without a connected sensor the M test unit read -78 °C.
5. Read-only. TOP43 (zone 2) changed by itself during the tests on a one-zone unit; see the zone 2 table.
6. Coupling on the M test unit: changing the mode moved heating or cooling curve points; some did not return when the mode was set back.
7. The M manual gives 6–35 °C in the menu (p. 109) and 5–35 °C in the control chapter (p. 155).
8. The M test unit reported 20 °C, which is outside the M menu range, and accepted 19 °C.

## Zone 2 (`topics_zone2`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 34 | Z2_Heat_Request_Temp | setting | SET7 SetZ2HeatRequestTemperature | two zones (TOP146 = 22 or 38); TOP76 as for TOP27 | as TOP27 | | |
| 35 | Z2_Cool_Request_Temp | setting | SET8 SetZ2CoolRequestTemperature | two zones (TOP146 = 22 or 38); TOP168 = 1; TOP81 | as TOP28 | | |
| 37 | Z2_Water_Temp | measurement | – | – | °C [1] | | |
| 43 | Z2_Water_Target_Temp | measurement | – | – | °C, computed by the heat pump [2] | | |
| 57 | Z2_Temp | measurement | – | – | °C | | |
| 82 | Z2_Heat_Curve_Target_High_Temp | setting | SET16 SetCurves | two zones; TOP76 = 0 | as TOP29 | ✓ ✓ ✓ ✓ | |
| 83 | Z2_Heat_Curve_Target_Low_Temp | setting | SET16 SetCurves | two zones; TOP76 = 0 | as TOP30 | ✓ ✓ ✓ ✓ | |
| 84 | Z2_Heat_Curve_Outside_High_Temp | setting | SET16 SetCurves | two zones; TOP76 = 0 | as TOP31 | ✓ ✓ ✓ ✓ | |
| 85 | Z2_Heat_Curve_Outside_Low_Temp | setting | SET16 SetCurves | two zones; TOP76 = 0 | as TOP32 | ✓ ✓ ✓ ✓ | |
| 86 | Z2_Cool_Curve_Target_High_Temp | setting | SET16 SetCurves | two zones; TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 87 | Z2_Cool_Curve_Target_Low_Temp | setting | SET16 SetCurves | two zones; TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 88 | Z2_Cool_Curve_Outside_High_Temp | setting | SET16 SetCurves | two zones; TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 89 | Z2_Cool_Curve_Outside_Low_Temp | setting | SET16 SetCurves | two zones; TOP81 = 0; TOP168 = 1 | °C; no menu range in the manuals | ✓ ✓ ✓ ✓ | |
| 118 | Second_Room_Thermostat_Temp | measurement | – | – | °C | | |
| 123 | Z2_Pump_State | state | – | – | 0=off, 1=on | | |
| 128 | Z2_Valve_PID | measurement | – | – | % | | |
| 165 | Z2_Active | state | – | – | 0=inactive, 1=active (activity bit, not TOP94) | | |

1. Without a connected sensor the M test unit read -78 °C.
2. Read-only. On the M test unit (one zone) the value changed by itself between reads (for example 39 → 37 °C, 44 → 43 °C) without any zone 2 setting being written.

## DHW & heaters (`topics_dhw_heaters`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 25 | DHW_Holiday_Shift_Temp | setting | SET83 SetDHWHolidayShiftTemp | takes effect only when TOP19 = 2 | K; cmd -127 to 127; topic description -15 to 15 | | ✓ |
| 58 | DHW_Heater_State | setting | SET44 SetDHWHeaterState | TOP100 = 1 | 0=blocked, 1=free | | |
| 59 | Room_Heater_State | setting | SET45 SetRoomHeaterState | not in cooling | 0=blocked, 1=free | | |
| 60 | Internal_Heater_State | state | – | – | 0=inactive, 1=active | | |
| 61 | External_Heater_State | state | – | – | 0=inactive, 1=active | | |
| 68 | Force_Heater_State | state | SET47 SetForceHeater | takes effect only when TOP167 = 1 | 0=inactive, 1=active | | |
| 69 | Sterilization_State | state | SET13 SetForceSterilization (trigger) | TOP100 = 1 | 0=inactive, 1=active [1] | | |
| 70 | Sterilization_Temp | setting | SET49 SetSterilizationTemp | TOP100 = 1 | cmd 55–75 °C; menu M/L 55–65, K/J 55–75 °C [2] | ✓ ✓ ✓ ✓ | ✓ |
| 71 | Sterilization_Max_Time | setting | SET50 SetSterilizationMaxTime | TOP100 = 1 | 5–60 min in steps of 5 (cmd and menu, all series) | ✓ ✓ ✓ ✓ | ✓ |
| 78 | Heater_On_Outdoor_Temp | setting | SET46 SetHeaterOnOutdoorTemp | takes effect only when TOP59 = 1 | cmd text -15 to 20 °C (not checked); menu -20 to 15 °C (all series) | ✓ ✓ ✓ ✓ | |
| 90 | Room_Heater_Operations_Hours | counter | – | – | hours | | |
| 91 | DHW_Heater_Operations_Hours | counter | – | – | hours | | |
| 96 | Heater_Delay_Time | setting | SET21 SetHeaterDelayTime | takes effect only when TOP59 = 1 | min; menu 10–60 min (all series) [3] | ✓ ✓ ✓ ✓ | ✓ |
| 97 | Heater_Start_Delta | setting | SET22 SetHeaterStartDelta | takes effect only when TOP59 = 1 | K; menu -10 to -2 K (all series) [3] | ✓ ✓ ✓ ✓ | ✓ |
| 98 | Heater_Stop_Delta | setting | SET23 SetHeaterStopDelta | takes effect only when TOP59 = 1 | K; menu M/L -3 to 0, K/J -8 to 0 K [3] | ✓ ✓ ✓ ✓ | ✓ |
| 140 | Smart_DHW | setting | SET40 SetSmartDHW | TOP100 = 1 | 0=variable, 1=standard (menu item "DHW capacity") [4] | ✓ ✓ ✓ ✓ | ✓ |
| 151 | DHW_Defrost | setting | SET79 SetDHWDefrost | TOP100 = 1 | 0=no, 1=yes | ✓ ✓ ✓ – | ✓ |
| 158 | Room_Operation_Max_Time | setting | SET53 SetRoomOperationMaxTime | TOP100 = 1 | cmd 30–7620 min in steps of 30; menu 30 min to 10 h (all series) | ✓ ✓ ✓ ✓ | ✓ |
| 159 | DHW_Heatup_Max_Time | setting | SET54 SetDHWHeatupMaxTime | TOP100 = 1 | cmd 1–254 min; menu 5 min to 4 h (all series) | ✓ ✓ ✓ ✓ | ✓ |

1. The manuals do not make clear whether this is the user switch for sterilisation or the running program.
2. K and J add "if use back-up heater, it is 65 °C". The M operating manual allows up to 75 °C with an external heater.
3. MQTT-Topics.md labels TOP96–TOP98 "J-series only", but the M manual has these menu items and the M test unit took over changed values. Values outside the menu range (90 min, +1 K) were not taken over by the M unit.
4. Despite the name, on M this is the menu item "DHW capacity" (M service manual p. 101; byte 24 bits 7-6, confirmed on the test unit). The "Smart DHW" function of the custom menu has no topic. See also TOP145.

## Diagnostics (`topics_diagnostics`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 21 | Outside_Pipe_Temp | measurement | – | – | °C | | |
| 49 | Main_Hex_Outlet_Temp | measurement | – | – | °C | | |
| 50 | Discharge_Temp | measurement | – | – | °C | | |
| 51 | Inside_Pipe_Temp | measurement | – | – | °C | | |
| 52 | Defrost_Temp | measurement | – | – | °C | | |
| 53 | Eva_Outlet_Temp | measurement | – | – | °C | | |
| 54 | Bypass_Outlet_Temp | measurement | – | – | °C | | |
| 55 | Ipm_Temp | measurement | – | – | °C | | |
| 62 | Fan1_Motor_Speed | measurement | – | – | r/min | | |
| 63 | Fan2_Motor_Speed | measurement | – | – | r/min | | |
| 64 | High_Pressure | measurement | – | – | kgf/cm² | | |
| 65 | Pump_Speed | measurement | – | – | r/min | | |
| 66 | Low_Pressure | measurement | – | – | kgf/cm² | | |
| 67 | Compressor_Current | measurement | – | – | A | | |
| 93 | Pump_Duty | measurement | – | – | duty | | |
| 115 | Water_Pressure | measurement | – | – | bar [1] | | |
| 116 | Second_Inlet_Temp | measurement | – | – | °C [1] | | |
| 117 | Economizer_Outlet_Temp | measurement | – | – | °C [1] | | |
| 125 | TwoWay_Valve_State | state | – | – | 0=cool, 1=heat | | |
| 126 | ThreeWay_Valve_State2 | state | – | – | 0=room, 1=DHW | | |
| 142 | Expansion_Valve | measurement | – | – | steps | | |

1. The topic description says K/L series; this is not taken from the manuals.

## Buffer, solar & pool (`topics_buffer_solar_pool`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 46 | Buffer_Temp | measurement | – | – | °C [1] | | |
| 47 | Solar_Temp | measurement | – | – | °C [1] | | |
| 48 | Pool_Temp | measurement | – | – | °C [1] | | |
| 101 | Solar_Mode | setting | SET84 SetSolarMode | TOP110 = 1 | 0=disabled, 1=buffer, 2=DHW; L: buffer only [2] | ✓ ✓ ✓ ✓ | ✓ |
| 102 | Solar_On_Delta | setting | SET85 SetSolarOnDelta | TOP101 = 1 or 2 | K; cmd -127 to 127; menu 5–15 K (all series) [3] | ✓ ✓ ✓ ✓ | ✓ |
| 103 | Solar_Off_Delta | setting | SET86 SetSolarOffDelta | TOP101 = 1 or 2 | K; cmd -127 to 127; menu 2–10 K (all series) [3] | ✓ ✓ ✓ ✓ | ✓ |
| 104 | Solar_Frost_Protection | setting | SET87 SetSolarFrostProtection | TOP101 = 1 or 2 | °C; cmd -127 to 127; menu -20 to 10 °C (all series) [3] | ✓ ✓ ✓ ✓ | ✓ |
| 105 | Solar_High_Limit | setting | SET88 SetSolarHighLimit | TOP101 = 1 or 2 | °C; cmd -127 to 127; menu 70–90 °C (all series) [3][4] | ✓ ✓ ✓ ✓ | ✓ |
| 113 | Buffer_Tank_Delta | setting | SET27 SetBufferDelta | TOP99 = 1 | K; cmd text 0–10 (not checked); menu L/K 0–10 K, M/J no range | ✓ ✓ ✓ ✓ | |
| 114 | External_Pad_Heater | setting | SET26 SetExternalPadHeater | – | 0=disabled, 1=type A, 2=type B | ✓ ✓ ✓ ✓ | |
| 144 | Dry_Concrete_State | state | SET96 SetDryConcrete | – | 0=off, 1=on [5] | ✓ ✓ ✓ ✓ | ✓ |
| 152 | Dry_Concrete_Target_Temp | state | – [6] | – | °C, target of the current stage; menu 25–55 °C (all series) | ✓ ✓ ✓ ✓ | |
| 153 | Pool_Delta | setting | SET89 SetPoolDelta | TOP146 = 25 or 38 | K; cmd -127 to 127; menu 0–10 K [7] | ✓ ✓ ✓ ✓ | ✓ |

1. Without a connected sensor the M test unit read -78 °C (also with the solar mode on).
2. Coupling on the M test unit: solar on without a solar sensor raised error H28 within 5 s while the heat pump was running and switched it off; the fault stays until reset. With the heat pump off no fault was seen.
3. For M the ranges come from the solar control chapter (p. 182), not from the menu text.
4. On the M test unit (no solar) a changed value was reverted after about 5 s.
5. Switching it on starts the dry concrete program and switches the heat pump on (TOP0 = 1, circulation pump on, first stage target); switching it off returned to the previous state (M test unit, heat pump off before).
6. A write to the byte was not taken over on the M test unit.
7. The comparison of the manuals gives 0–10 K for L, K and J and no range for M; the M dependency notes give 0–10 K (p. 100 and operating manual p. 32).

## Bivalent (`topics_bivalent`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 129 | Bivalent_Control | setting | SET34 SetBivalentControl | – [1] | 0=disabled, 1=enabled | ✓ ✓ ✓ ✓ | |
| 130 | Bivalent_Mode | setting | SET35 SetBivalentMode | TOP129 = 1; TOP180 = 0 | 0=alternative, 1=parallel, 2=advanced parallel | ✓ ✓ ✓ ✓ | |
| 131 | Bivalent_Start_Temp | setting | SET36 SetBivalentStartTemp | TOP129 = 1 | cmd text -15 to 35 °C (not checked); M/L -15 to 15, K/J -15 to 35 °C [2] | ✓ ✓ ✓ ✓ | |
| 132 | Bivalent_Advanced_Heat | setting | SET90 SetBivalentAPHeat | TOP130 = 2; takes effect only when TOP99 = 1 | 0=no, 1=yes | ✓ ✓ ✓ ✓ | ✓ |
| 133 | Bivalent_Advanced_DHW | setting | SET91 SetBivalentAPDHW | TOP130 = 2 | 0=no, 1=yes | ✓ ✓ ✓ ✓ | ✓ |
| 134 | Bivalent_Advanced_Start_Temp | setting | SET37 SetBivalentAPStartTemp | TOP132 = 1 | cmd text -15 to 35 °C (not checked); no range in the manuals | ✓ ✓ ✓ ✓ | |
| 135 | Bivalent_Advanced_Stop_Temp | setting | SET38 SetBivalentAPStopTemp | TOP132 = 1 | cmd text -15 to 35 °C (not checked); no range in the manuals | ✓ ✓ ✓ ✓ | |
| 136 | Bivalent_Advanced_Start_Delay | setting | SET94 SetBivalentAPStartDelay | TOP132 = 1 | cmd 1–254 min; no range in the manuals; 5–250 min in steps of 5 seen on the M unit | ✓ ✓ ✓ ✓ | ✓ |
| 137 | Bivalent_Advanced_Stop_Delay | setting | SET95 SetBivalentAPStopDelay | TOP132 = 1 | as TOP136 | ✓ ✓ ✓ ✓ | ✓ |
| 138 | Bivalent_Advanced_DHW_Delay | setting | – | TOP133 = 1 | min; no range in the manuals | ✓ ✓ ✓ ✓ | |
| 164 | Bivalent_Boiler_Contact_State | state | – | – | 0=off, 1=on | | |
| 180 | Bivalent_Operating_Mode | setting | SET92 SetBivalentOperatingMode | TOP129 = 1; value 1 needs TOP110 = 1 | 0=auto, 1=SG ready, 2=intelligent; J: auto and SG ready only [3] | ✓ ✓ ✓ ✓ | ✓ |
| 181 | Bivalent_External_Pump | setting | SET93 SetBivalentExternalPump | TOP130 = 0, or TOP180 = 1 or 2 | 0=off, 1=on | ✓ ✓ ✓ ✓ [4] | ✓ |

1. Coupling: bivalent control and the defrost signal (TOP194) share one terminal. Switching one on switches the other off (M manual p. 106; seen on the test unit). Switching bivalent off does not restore the defrost signal.
2. The menu gives only the default -5 °C; the ranges come from the control chapters (M p. 183, L p. 326, K p. 186, J p. 131).
3. Coupling: SG ready (TOP149) and the bivalent pattern "SG ready" share one terminal; setting one resets the other, and SG ready on sets the bivalent operating mode to auto (M manual pp. 103, 105). TOP180 is not the switching behaviour; that is TOP130.
4. M: not in the menu text of the service manual, but in the M operating manual (pp. 34-35) and in the Aquarea Service Cloud.

## External control & SG ready (`topics_external_sgready`)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 110 | Optional_PCB | setting | SET56 SetOptionalPCB | – | 0=no, 1=yes [1] | ✓ ✓ ✓ ✓ | ✓ |
| 119 | External_Control | setting | SET30 SetExternalControl | – | 0=disabled, 1=enabled | ✓ ✓ ✓ ✓ | |
| 120 | External_Heat_Cool_Control | setting | SET33 SetExternalHeatCoolControl | TOP110 = 1; TOP168 = 1 | 0=disabled, 1=enabled [2] | ✓ ✓ ✓ ✓ | |
| 121 | External_Error_Signal | setting | SET31 SetExternalError | TOP110 = 1 | 0=disabled, 1=enabled | ✓ ✓ ✓ ✓ | |
| 122 | External_Compressor_Control | setting | SET32 SetExternalCompressorControl | TOP110 = 1 | 0=disabled, 1=enabled | ✓ ✓ ✓ ✓ | |
| 149 | SG_Ready_Control | setting | SET97 SetSGReadyMode | TOP110 = 1 | 0=disabled, 1=enabled (capacity), 2=enabled (power consumption, M only) [3] | ✓ ✓ ✓ ✓ | ✓ |
| 150 | Demand_Control | setting | SET112 SetDemandControlEnable | TOP110 = 1 | 0=disabled, 1=enabled [4] | ✓ ✓ ✓ ✓ | ✓ |
| 154 | SG_Ready_1_Heat_Capacity | setting | SET98 SetSGReadyHeatCapacity1 | TOP149 = 1 | %; cmd 0–254 [5] | ✓ ✓ ✓ ✓ | ✓ |
| 155 | SG_Ready_1_DHW_Capacity | setting | SET99 SetSGReadyDHWCapacity1 | TOP149 = 1 | %; cmd 0–254 [5] | ✓ ✓ ✓ ✓ | ✓ |
| 156 | SG_Ready_2_Heat_Capacity | setting | SET102 SetSGReadyHeatCapacity2 | TOP149 = 1 | %; cmd 0–254 [5] | ✓ ✓ ✓ ✓ | ✓ |
| 157 | SG_Ready_2_DHW_Capacity | setting | SET103 SetSGReadyDHWCapacity2 | TOP149 = 1 | %; cmd 0–254 [5] | ✓ ✓ ✓ ✓ | ✓ |
| 162 | Heat_Cool_Switch_State | state | – | – | 0=heat, 1=cool | | |
| 163 | External_Switch_State | state | – | – | 0=closed, 1=open | | |
| 184 | SG_Ready_1_DHW_Power | setting | SET106 SetSGReadyDHWPower1 | TOP149 = 2 | kW; cmd 0.1–25.4 in steps of 0.1 [5] | ✓ – – – | ✓ |
| 185 | SG_Ready_1_Heat_Power | setting | SET105 SetSGReadyHeatPower1 | TOP149 = 2 | as TOP184 | ✓ – – – | ✓ |
| 186 | SG_Ready_1_Cool_Power | setting | SET107 SetSGReadyCoolPower1 | TOP149 = 2 | as TOP184 | ✓ – – – | ✓ |
| 187 | SG_Ready_2_DHW_Power | setting | SET109 SetSGReadyDHWPower2 | TOP149 = 2 | as TOP184 | ✓ – – – | ✓ |
| 188 | SG_Ready_2_Heat_Power | setting | SET108 SetSGReadyHeatPower2 | TOP149 = 2 | as TOP184 | ✓ – – – | ✓ |
| 189 | SG_Ready_2_Cool_Power | setting | SET110 SetSGReadyCoolPower2 | TOP149 = 2 | as TOP184 [6] | ✓ – – – | ✓ |
| 190 | SG_Ready_Stop_Power | setting | SET111 SetSGReadyStopPower | TOP149 = 2 | kW, limit while the heat pump is stopped; as TOP184 | ✓ – – – | ✓ |
| 191 | SG_Ready_1_Cool_Capacity | setting | SET100 SetSGReadyCoolCapacity1 | TOP149 = 1 | °C offset; cmd -127 to 127 [5] | ✓ ✓ ✓ ✓ | ✓ |
| 192 | SG_Ready_2_Cool_Capacity | setting | SET104 SetSGReadyCoolCapacity2 | TOP149 = 1 | °C offset; cmd -127 to 127 [5] | ✓ ✓ ✓ ✓ | ✓ |

1. The installer menu setting, not the optional PCB emulation of HeishaMon. With TOP110 = 0 the Service Cloud greys out zones and sensors, solar, error output, demand control, SG ready, external compressor switch, heat/cool switch and extra pump. Child values with TOP110 = 0 were not tested.
2. With TOP120 = 1 the weekly timer (TOP13) and the operation mode auto cannot be used (M manual pp. 106, 190).
3. See the bivalent table, note 3, for the coupling with TOP180.
4. The installer menu option, not the optional PCB value of SetDemandControl.
5. The service manuals give no menu range for the SG ready values (L/K/J give the default 120 %). MQTT-Topics.md lists installer menu ranges (50–150 %, -15 to 0 °C, 0.5–10.0 kW) whose source is not in the manuals.
6. Label assigned by elimination.

## Installer settings (`topics_installer`, off by default)

| TOP | Name | Kind | Write command | Parent / only meaningful when | Range / values | Series in manuals (M L K J) | Tested on M |
|---|---|---|---|---|---|---|---|
| 95 | Max_Pump_Duty | setting | SET15 SetMaxPumpDuty | used as fixed pump output when TOP106 = 1 | cmd text 64–254 (not checked); menu 0x40–0xFE (L/K/J table), 0x40–0xFF (pump chapters) | ✓ ✓ ✓ ✓ | |
| 99 | Buffer_Installed | setting | SET28 SetBuffer | K/J: TOP110 = 1 | 0=no, 1=yes [1] | ✓ ✓ ✓ ✓ | |
| 100 | DHW_Installed | setting | SET76 SetDHWInstalled | – | 0=no, 1=yes | ✓ – ✓ ✓ | ✓ |
| 106 | Pump_Flowrate_Mode | setting | SET42 SetPumpFlowrateMode | – | 0=ΔT, 1=max duty [2] | ✓ ✓ ✓ ✓ | ✓ |
| 107 | Liquid_Type | setting | SET58 SetLiquidType | – | 0=water, 1=glycol | ✓ ✓ ✓ ✓ | ✓ |
| 108 | Alt_External_Sensor | setting | SET25 SetAltExternalSensor | – | 0=disabled, 1=enabled | ✓ ✓ ✓ ✓ | |
| 109 | Anti_Freeze_Mode | setting | SET57 SetAntiFreezeMode | – | 0=off, 1=on | ✓ ✓ ✓ ✓ | ✓ |
| 111 | Z1_Sensor_Settings | setting | SET63 SetZ1SensorSettings | – | 0=water temperature, 1=external thermostat, 2=internal thermostat, 3=thermistor | ✓ ✓ ✓ ✓ | ✓ |
| 112 | Z2_Sensor_Settings | setting | – | two zones (TOP146 = 22 or 38) | as TOP111 | ✓ ✓ ✓ ✓ | |
| 139 | Heating_Control | setting | SET39 SetHeatingControl | – | 0=comfort, 1=efficiency | ✓ ✓ ✓ – | |
| 141 | Quiet_Mode_Priority | setting | SET41 SetQuietModePriority | – | 0=sound, 1=capacity | | |
| 143 | DHW_Sensor_Selection | setting | SET43 SetDHWSensorSelection | – | 0=top, 1=center [3] | ? ? ? ? | |
| 145 | DHW_Capacity_Mode | setting | – | – | 0=standard, 1=variable [4] | ? ? ? ? | |
| 146 | Zone_Configuration | setting | SET62 SetZoneType (zone 1 type only) | TOP110 = 1 | raw code: 21=one zone, room; 25=one zone, pool; 22=two zones, zone 2 room; 38=two zones, zone 2 pool [5] | ✓ ✓ ✓ ✓ | ✓ |
| 147 | Internal_Heater_Capacity | setting | SET61 SetInternalHeaterCapacity | – | 3, 6 or 9 kW depending on the model; -1=unavailable [6] | ✓ ✓ ✓ ✓ | ✓ |
| 148 | DHW_Heater_Type | setting | SET77 SetDHWHeaterType | TOP100 = 1 | 0=internal, 1=external [7] | ✓ – ✓ ✓ | ✓ |
| 167 | Backup_Heater_Enable | setting | – (custom menu) | – | 0=inactive, 1=active | ✓ ✓ ✓ ✓ | |
| 168 | Cooling_Enable | setting | – (custom menu) | – | 0=inactive, 1=active [8] | ✓ ✓ ✓ ✓ | |
| 170 | Static_Pressure | setting | SET59 SetStaticPressure | – | 0=disabled, 1=enabled | ✓ – – – | ✓ |
| 171 | Heating_On_Outdoor_Temp | setting | SET69 SetHeatingOnOutdoorTemp | range 5 °C to TOP77 − 1 | °C; cmd -127 to 127; menu M: 5 to (TOP77 − 1) | ✓ – – – | ✓ |
| 172 | Heating_On_Delay | setting | SET70 SetHeatingOnDelay | – | cmd 30–7620 min in steps of 30; no range in the manual [9] | ✓ – – – | ✓ |
| 173 | DHW_Booster_Delay | setting | SET78 SetDHWBoosterDelay | TOP100 = 1; TOP148 = 1 | cmd 1–254 min; K/J 20 min to 3 h (control chapter); M: no range [9] | ✓ – ✓ ✓ | ✓ |
| 174 | Efficiency_Stage1_Time | setting | SET73 SetEfficiencyStage1Time | TOP139 = 1 | cmd 0–70 min in steps of 5 [9] | ✓ – – – | ✓ |
| 175 | Efficiency_Stage2_Time | setting | SET74 SetEfficiencyStage2Time | TOP139 = 1 | cmd 0–70 min in steps of 5; 0–60 seen on the M unit | ✓ – – – | ✓ |
| 176 | Efficiency_Stage3_Time | setting | SET75 SetEfficiencyStage3Time | TOP139 = 1 | cmd 0–70 min in steps of 5; 0–30 seen on the M unit | ✓ – – – | ✓ |
| 177 | Sensor_Offset_Remote_Th1 | setting | SET64 SetSensorOffsetRemoteTh1 | – | K; cmd -3.5 to 3.5 in steps of 0.5; menu M: -3 to 3, L/K/J -2 to 2 [10] | ✓ ✓ ✓ ✓ | ✓ |
| 178 | Sensor_Offset_Outdoor | setting | SET65 SetSensorOffsetOutdoor | – | as TOP177 [10] | ✓ ✓ ✓ ✓ | ✓ |
| 179 | Anti_Stick_Mode | setting | – [11] | – | 0=disabled, 1=enabled | ✓ – – – | |
| 182 | Extra_Pump_Use | setting | SET80 SetExtraPump | TOP100 = 1 [12] | 0=not used, 1=heating, 2=DHW | ✓ – – – | ✓ |
| 183 | Extra_Pump_Mode | setting | SET81 SetExtraPumpMode | TOP182 = 2 | 0=comfort, 1=efficiency; also 0 while the pump is not used | ✓ – – – | ✓ |
| 193 | Sensor_Offset_Return | setting | SET66 SetSensorOffsetReturn | – | as TOP177 [10] | ✓ ✓ ✓ ✓ | ✓ |
| 194 | Defrost_Signal | setting | SET60 SetDefrostSignal | – | 0=no, 1=yes [13] | ✓ ✓ ✓ ✓ | ✓ |
| 195 | Sensor_Offset_DHW_Tank | setting | SET68 SetSensorOffsetDhwTank | – | as TOP177 [10] | ✓ ✓ ✓ ✓ | ✓ |
| 196 | Sensor_Offset_Flow | setting | SET67 SetSensorOffsetFlow | – | as TOP177 [10] | ✓ ✓ ✓ ✓ | ✓ |

1. On the M test unit (no buffer sensor) TOP99 = 1 raised error H43 within 5 s; it stays until reset, also after the buffer is set back. The buffer depends on the optional PCB only in the K and J manuals.
2. MQTT-Topics.md labels this topic "J-series only", but the menu item is in all four manuals and the M test unit took over the change.
3. The topic description says K/L series all-in-one only. The menu item is not part of the manual comparison; not tested.
4. The topic description says J series. On M the menu item "DHW capacity" is TOP140 (byte 24); what TOP145 (byte 9) shows on M is not known. Note the opposite value order.
5. SET62 sets only the zone 1 type (0=room, 1=pool); the number of zones and the zone 2 type have no command. Coupling on the M test unit: changing the zone 1 type changed the zone 1 request temperatures and curve points. Pool is only possible on zone 2 in a two-zone system (M manual pp. 100, 158).
6. Only 6 and 9 kW were tested, on a unit with a 9 kW heater. What a value above the installed heater does is not known.
7. With TOP148 = 0 the internal heater heats the tank only when TOP167 = 1 (M manual pp. 163, 174).
8. No command: the byte has no position in the command datagram. Switching cooling on the remote controller reset the zone 1 cooling curve and TOP79 / TOP80 to factory values on the M test unit. The M manual requires the heat pump to be off for this (p. 202).
9. The manuals give no range for this item on M. MQTT-Topics.md lists installer menu ranges (TOP172 30–1440 min, TOP173 20–180 min in steps of 5, TOP174 0–60 min) that are not found in the manual comparison.
10. Writing any sensor offset stopped the compressor of the M test unit for about 4 minutes, also with an unchanged value (7 of 7 writes, no error code). The L, K and J ranges are from the maintenance menu pages (L p. 199, K p. 140, J p. 85); which individual sensors each series offers was not compared. The 0.5 K step comes from the encoding, not from the manuals.
11. A write to byte 34 bits 7-6 is ignored by the M unit.
12. The M menu tree places the condition "only when tank connection is yes" next to the extra pump; the assignment is not certain.
13. Coupling with bivalent control (TOP129), see the bivalent table, note 1.

## Commands without a readback topic

SET14 SetPump (pump service mode) and SET48 SetReset (fault reset) have no topic of their own; SET12 SetForceDefrost and SET13 SetForceSterilization are triggers whose effect shows in TOP26 and TOP69. SET101 does not exist.

## Legend

| Mark | Meaning |
|---|---|
| ✓ (series) | The menu item is in the service manual of that series (pages in the comparison of the manuals). |
| – (series) | The menu item is not in that manual. |
| ? (series) | Unclear, or the item was not part of the comparison. |
| blank (series) | Measurement, state, user setting or counter; no menu comparison. |
| ✓ (tested) | Written with a changed value on the M-series unit, read back on this topic, restored. |
| cmd | Range the HeishaMon command accepts or encodes. |
| menu | Installer menu range in the manuals, with the series it applies to. |

## Not covered

- H series and other variants (for example other indoor units or all-in-one variants of a series) were not checked.
- Values outside the documented menu ranges were not tested in general; where the M unit refused or reverted a value, this is noted.
- The behaviour of child settings while the parent is off was not tested in general. The M unit accepted the child values that were tried; child values with the optional PCB setting off were not tested.
- Operating-state effects (errors, compressor stops) were seen on one unit and may differ on other units and series.
