#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <time.h>
#include <stdint.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef ENABLE_OLED
#define ENABLE_OLED 1
#endif
#if ENABLE_OLED
#include <Wire.h>
#include <U8g2lib.h>
#endif

#define WDT_TIMEOUT_S 30  // watchdog: reinicia si loop() se cuelga más de 30s

// --------------------------- Configuracion base ---------------------------
static const char *WIFI_SSID = "MYM";
static const char *WIFI_PASS = "Mati-4062636263";
static const char *SUPABASE_URL = "https://ontyquiklrmebtzcvjyn.supabase.co";
static const char *SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im9udHlxdWlrbHJtZWJ0emN2anluIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzU1NzMzMjIsImV4cCI6MjA5MTE0OTMyMn0.rmbcm5s7HdAnljRnVHueq-qU6zgITizaBDVpqyI9-vs";

static const char *SUPABASE_COMMANDS_ENDPOINT = "/rest/v1/remote_commands?select=id,pump_id,target_state,is_processed,created_at&is_processed=eq.false&order=created_at.asc&limit=1";
static const char *SUPABASE_SYSTEM_STATE_ENDPOINT = "/rest/v1/system_state?id=eq.1";
static const char *SUPABASE_SENSOR_DATA_ENDPOINT = "/rest/v1/sensor_data";

static const uint8_t ZONE_COUNT = 5;
static const uint8_t SENSORS_PER_STATION = 3;  // sondas por satélite (ESP-NOW sensorID 1..3)
static const uint8_t ZONE_PINS[ZONE_COUNT] = {25, 26, 27, 32, 33};
#if ENABLE_OLED
static const uint8_t OLED_SDA = 21;
static const uint8_t OLED_SCL = 22;
#endif
static const uint32_t MAX_WATERING_MS = 15UL * 60UL * 1000UL;
// Si `WiFi.channel()` no es válido aún, se usa este canal al registrar ESP-NOW.
static const uint8_t ESPNOW_FALLBACK_CHANNEL = 11;
static const uint32_t STARTUP_STAGGER_MS = 9000UL;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 15000UL;
static const uint32_t CLOUD_START_DELAY_MS = 15000UL;

// Ventana de evaluación de riego: minutos antes y después del HH:MM programado (default ±2).
#ifndef SCHED_EVAL_WINDOW_MIN
#define SCHED_EVAL_WINDOW_MIN 2
#endif
// OLED: refresco pantalla normal y reloj en reposo (1 s = segundo visible continuo).
#ifndef OLED_NORMAL_REFRESH_MS
#define OLED_NORMAL_REFRESH_MS 1000UL
#endif
#ifndef OLED_SLEEP_CLOCK_REFRESH_MS
#define OLED_SLEEP_CLOCK_REFRESH_MS 1000UL
#endif
// Historial/gráfico en Supabase: antes 30 min → no reflejaba ESP-NOW. ~30 s encaja con pushStatus (~30 s).
static const uint32_t SENSOR_PUSH_INTERVAL_MS = 30UL * 1000UL;
// Solo entran lecturas con antigüedad <= esto en el promedio; debe ser > ciclo típico satélite (p. ej. deep sleep 30 min).
static const uint32_t SENSOR_SAMPLE_MAX_AGE_MS = 40UL * 60UL * 1000UL;
static const bool USE_BROWNOUT_WORKAROUND = true;

static const uint8_t PROGRAM_COUNT = 3;
static const uint32_t AUDIT_DELAY_MS = 45UL * 60UL * 1000UL;
// MAX_DECISION_MIN y RAIN_CANCEL_MM ahora son por zona (campo en PumpSchedule)

// --- Clima: RedMeteo 3.0 (observaciones) + Open-Meteo (ET₀ y T max/min diarios) ---
// RedMeteo: https://redmeteo.cl/last-data.json (mismo feed que el mapa v3.0). Cita: redmeteo.cl/api.html
// `wx_rain24h`: mm para cancelar/reducir riego = max(lluvia ya medida EMA/RedMeteo si aplica, suma horaria Open-Meteo próximas N h).
#ifndef WEATHER_RAIN_FORECAST_HOURS
#define WEATHER_RAIN_FORECAST_HOURS 6  // ventana de pronóstico (no usar acumulado diario “mañana” para cancelar)
#endif
#ifndef WEATHER_USE_REDMETEO
#define WEATHER_USE_REDMETEO 1
#endif
static const float   SITE_LAT            = -33.04f;    // Villa Alemana, Valparaíso (solo si REDMETEO_STATION_ID vacío)
static const float   SITE_LON            = -71.37f;
static const char    REDMETEO_LASTDATA_URL[] = "https://redmeteo.cl/last-data.json";
static const char    REDMETEO_STATION_ID[]   = "RMCL0022";  // Villa Alemana - Pob. Guillermo Marconi; "" = más cercana a SITE_LAT/LON
static const uint32_t WEATHER_INTERVAL_MS = 6UL * 3600UL * 1000UL; // refrescar cada 6h

// --- DMC Meteochile (climatologia.meteochile.gob.cl) — opcional: lluvia desde EMA oficial ---
// Documentación: https://climatologia.meteochile.gob.cl/application/documentacion/getDocumento/0
// Si rellenas usuario + token y codigo > 0, se intenta ANTES que RedMeteo. Vacío = omitir.
// Opcional NVS namespace "meteoch": claves "user", "token", "cod" (sobrescriben lo de abajo sin recompilar).
static const char     METEOCHILE_USER[]           = "matiasbricenoaguilera@gmail.com";       // mismo correo que el registro en el portal
static const char     METEOCHILE_TOKEN[]          = "7a864137a35a4db44700e503";       // token API del portal (no la contraseña de login, salvo que DMC use la misma)
static const uint32_t METEOCHILE_CODIGO_NACIONAL  = 320041;    // EMA DMC Torquemada Viña del Mar
static const char     METEOCHILE_BASE[] =
    "https://climatologia.meteochile.gob.cl/application/servicios/getDatosRecientesEma/";

// Caché en Supabase (`weather_cache`): la Edge Function `sync-weather` descarga Meteochile + Open-Meteo; la ESP32 solo lee esta fila.
#ifndef WEATHER_USE_SUPABASE_CACHE
#define WEATHER_USE_SUPABASE_CACHE 1
#endif
#ifndef WEATHER_CACHE_MAX_AGE_SEC
#define WEATHER_CACHE_MAX_AGE_SEC (4UL * 3600UL)  // fila más vieja → se ignoran y se usan APIs directas
#endif
static const char SUPABASE_WEATHER_CACHE_SELECT[] =
    "/rest/v1/weather_cache?id=eq.default&select=rain_mm,et0_mm,t_max,t_min,description,updated_unix,source";

static const char *LOCAL_USER = "matias";
static const char *LOCAL_PASS = "406263";

// --------------------------- Modelos ---------------------------
enum ZoneRunSource : uint8_t { ZSRC_AUTO = 0, ZSRC_LOCAL = 1, ZSRC_CLOUD = 2 };
struct ZoneState { bool on; uint32_t startedAt; uint32_t requestedDurationMs; uint8_t source; };
struct PumpSchedule {
  uint8_t startHour;
  uint8_t startMinute;
  float   flowRate;       // L/min del aspersor/gotero
  float   area;           // m² cubiertos por esta zona
  uint8_t threshold;      // % humedad umbral de activación
  uint8_t stationID;      // satélite asignado
  float   efficiency;     // eficiencia sistema: goteo≈0.90, aspersor≈0.75
  float   soilMaxMm;      // capacidad máx de agua disponible en suelo (mm)
  float   Kc;             // coeficiente de cultivo: suculenta≈0.3, tomate≈1.1, césped≈0.8
  float   rainCancelMm;   // mm lluvia prevista que cancela el riego (default 5.0)
  uint8_t maxDecisionMin; // máx minutos de riego calculado (default 10)
  bool    enabled;        // false = zona deshabilitada, nunca riega
};
struct ZoneDecision {
  bool shouldWater;
  uint8_t humidityNow;
  uint8_t minutes;
  String reason;
  String result;
  uint32_t irrigationTs;
  bool auditPending;
  int lastEvalDay = -1;
  int lastRunDay = -1;
  int lastEvalSched = -1;
  int lastRunSched = -1;
  bool forceEval = false;
  uint16_t nextEvalMins = 0;
  String nextEvalHHMM = "--:--";
  uint32_t runDueMs = 0;
  bool runDueArmed = false;
  uint16_t minsToSched = 0;
  char schedHHMM[6] = "--:--";
  // Auditoría post-riego
  uint8_t  humAtStart    = 0;
  uint8_t  auditMins     = 0;
  float    auditLiters   = 0.0f;
  int8_t   auditDeltaHum = 0;
  String   auditResult   = "";
  // Ventana de evaluación T±SCHED_EVAL_WINDOW_MIN (min) y reintentos por SIN_LECTURA
  bool     wasInEvalWindow   = false;
  bool     sinLecturaPending = false;
};

// legado: 12 bytes
struct LegacySensorPacket { int32_t stationID; int32_t sensorID; int32_t rawValue; };
// recomendado
struct __attribute__((packed)) SensorPacketV2 { uint8_t stationID; uint8_t sensorID; uint16_t rawValue; uint16_t batteryMv; uint32_t nonce; };

WebServer server(80);
Preferences prefs;
ZoneState zones[ZONE_COUNT];
PumpSchedule schedules[PROGRAM_COUNT][ZONE_COUNT];
ZoneDecision decisions[ZONE_COUNT];
uint8_t activeProgram = 0;

uint8_t moisturePct[ZONE_COUNT] = {0, 0, 0, 0, 0};
// calDry = raw seco (máximo), calWet = raw mojado (mínimo); por sensor dentro de cada estación
uint16_t calDry[ZONE_COUNT][SENSORS_PER_STATION] = {
  {3200, 3200, 3200}, {3200, 3200, 3200}, {3200, 3200, 3200}, {3200, 3200, 3200}, {3200, 3200, 3200}};
uint16_t calWet[ZONE_COUNT][SENSORS_PER_STATION] = {
  {1500, 1500, 1500}, {1500, 1500, 1500}, {1500, 1500, 1500}, {1500, 1500, 1500}, {1500, 1500, 1500}};
uint16_t sensorRaw[ZONE_COUNT][SENSORS_PER_STATION] = {{0}};
bool sensorSeen[ZONE_COUNT][SENSORS_PER_STATION] = {{false}};
uint16_t stationBatteryMv[ZONE_COUNT] = {0};

// --- Estado climático (OpenWeatherMap) ---
float    wx_rain24h     = 0.0f;   // mm para decisión riego (estación + pronóstico corto; ver WEATHER_RAIN_FORECAST_HOURS)
float    wx_tMax        = 25.0f;  // °C máxima del día
float    wx_tMin        = 15.0f;  // °C mínima del día
float    wx_et0         = 0.0f;   // mm/día evapotranspiración de referencia (Hargreaves)
bool     wx_fetched     = false;
uint32_t wx_lastFetchAt = 0;
uint32_t lastWxAttemptAt = 0;  // reintentos clima si falla RedMeteo/Open-Meteo
static const uint32_t WX_RETRY_MS = 60000UL;
String   wx_desc        = "Sin datos";

uint32_t bootAt = 0, lastEspNowAt = 0, lastStatusPushAt = 0, lastWiFiAttemptAt = 0, lastScheduleTickAt = 0, lastSensorDataPushAt = 0;
uint32_t lastStationSeenAt[ZONE_COUNT] = {0};  // millis() de última recepción por estación (índice = stationID-1)
uint32_t lastSensorPacketAt[ZONE_COUNT][SENSORS_PER_STATION] = {{0}};  // último paquete por sonda (para descartar obsoletos)
bool moisturePctValid[ZONE_COUNT] = {false, false, false, false, false};
bool espNowReady = false, wifiBeginDone = false, ntpConfigured = false, pumpPinsArmed = false;
// Solo estaciones con paquete ESP-NOW nuevo desde el último POST a sensor_data (evita duplicar puntos en el gráfico)
bool sensorDataPushPending[ZONE_COUNT] = {false, false, false, false, false};
bool statusPushUrgent = false;  // push inmediato cuando cambia estado de bomba
uint32_t localPriorityUntil = 0;
String sessionToken;
uint32_t lastSchedulePrintAt = 0;

// Hora local = UTC + utc_offset_sec (mismo valor que envía el navegador: -getTimezoneOffset()*60).
static const long kBrowserUtcOffsetUnset = 999999L;
static long       appliedBrowserUtcOffsetSec = kBrowserUtcOffsetUnset;

static int32_t loadPrefsUtcOffsetSec() {
  prefs.begin("tm", true);
  int32_t v = prefs.getInt("utc_os", INT32_MAX);
  prefs.end();
  return v;
}

static void savePrefsUtcOffsetSec(int32_t sec) {
  prefs.begin("tm", false);
  prefs.putInt("utc_os", sec);
  prefs.end();
}

// Aplica desfase fijo (incluye horario de verano actual del navegador cuando se guardó).
static void applyBrowserUtcOffsetSeconds(long sec, bool persistPrefs) {
  if (sec < -46800L || sec > 46800L) return;
  if (appliedBrowserUtcOffsetSec == sec) return;
  appliedBrowserUtcOffsetSec = sec;
  configTime(sec, 0, "pool.ntp.org", "time.nist.gov", "time.cloudflare.com");
  if (persistPrefs) savePrefsUtcOffsetSec((int32_t)sec);
  Serial.printf("[NTP] Zona = UTC%+ld s (alineada con tu sistema / navegador)\n", sec);
}

// --------------------------- Utils ---------------------------
bool isSessionValid() {
  if (!server.hasHeader("Cookie")) return false;
  return server.header("Cookie").indexOf("rs_session=" + sessionToken) >= 0;
}
bool isAuthenticated() { return sessionToken.length() && isSessionValid(); }
void requireAuth() { server.sendHeader("Location", "/login"); server.send(302, "text/plain", "Redirect"); }

uint8_t rawToPercent(uint16_t raw, uint16_t dry, uint16_t wet) {
  if (dry <= wet) return 0;
  if (raw >= dry) return 0;
  if (raw <= wet) return 100;
  float p = 100.0f * ((float)(dry - raw) / (float)(dry - wet));
  if (p < 0) p = 0; if (p > 100) p = 100;
  return (uint8_t)p;
}

String htmlHeader(const char *title) {
  String h = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>";
  h += title;
  h += "</title><style>body{font-family:sans-serif;margin:16px;}button{padding:8px 12px;margin:4px;}input{padding:8px;margin:4px;}table{border-collapse:collapse;}td,th{border:1px solid #ccc;padding:6px;} .on{color:green;} .off{color:#555;}</style></head><body>";
  return h;
}

void saveCalibration() {
  prefs.begin("riego", false);
  for (uint8_t st = 0; st < ZONE_COUNT; st++) {
    for (uint8_t sid = 0; sid < SENSORS_PER_STATION; sid++) {
      String kd = "d" + String(st) + "_" + String(sid);
      String kw = "w" + String(st) + "_" + String(sid);
      prefs.putUShort(kd.c_str(), calDry[st][sid]);
      prefs.putUShort(kw.c_str(), calWet[st][sid]);
    }
  }
  prefs.end();
}
void loadCalibration() {
  prefs.begin("riego", true);
  for (uint8_t st = 0; st < ZONE_COUNT; st++) {
    uint16_t legDry = prefs.getUShort(("dry" + String(st)).c_str(), 3200);
    uint16_t legWet = prefs.getUShort(("wet" + String(st)).c_str(), 1500);
    for (uint8_t sid = 0; sid < SENSORS_PER_STATION; sid++) {
      String kd = "d" + String(st) + "_" + String(sid);
      String kw = "w" + String(st) + "_" + String(sid);
      if (prefs.isKey(kd.c_str()))
        calDry[st][sid] = prefs.getUShort(kd.c_str(), 3200);
      else
        calDry[st][sid] = legDry;
      if (prefs.isKey(kw.c_str()))
        calWet[st][sid] = prefs.getUShort(kw.c_str(), 1500);
      else
        calWet[st][sid] = legWet;
    }
  }
  prefs.end();
}

static bool sensorSampleFresh(uint8_t st, uint8_t sid) {
  uint32_t t = lastSensorPacketAt[st][sid];
  if (t == 0) return false;
  return (millis() - t) <= SENSOR_SAMPLE_MAX_AGE_MS;
}

void recalcStationMoisture(uint8_t st) {
  uint32_t sumPct = 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < SENSORS_PER_STATION; i++) {
    if (!sensorSampleFresh(st, i)) continue;
    sumPct += rawToPercent(sensorRaw[st][i], calDry[st][i], calWet[st][i]);
    count++;
  }
  if (count == 0) {
    moisturePct[st] = 0;
    moisturePctValid[st] = false;
    return;
  }
  moisturePct[st] = (uint8_t)(sumPct / count);
  moisturePctValid[st] = true;
}

void recalcAllStationMoisture(void) {
  for (uint8_t st = 0; st < ZONE_COUNT; st++) recalcStationMoisture(st);
}

/** Índice 0..ZONE_COUNT-1 para arrays por estación. Si stationID en Flash es 0 o >5, el cálculo
 *  cfg.stationID - 1 desborda (uint8_t) y lee fuera de lastStationSeenAt[] → "nunca recibido" en dashboard. */
static uint8_t stationIdxFromSchedule(const PumpSchedule &cfg, uint8_t zoneIdx) {
  uint8_t sid = cfg.stationID;
  if (sid >= 1 && sid <= ZONE_COUNT) return sid - 1;
  if (zoneIdx < ZONE_COUNT) return zoneIdx;
  return 0;
}

/** Última recepción útil por estación: máximo entre lastStationSeenAt y cualquier sonda. */
static uint32_t effectiveLastRxMs(uint8_t stIdx) {
  if (stIdx >= ZONE_COUNT) return 0;
  uint32_t t = lastStationSeenAt[stIdx];
  for (uint8_t sid = 0; sid < SENSORS_PER_STATION; sid++) {
    uint32_t lp = lastSensorPacketAt[stIdx][sid];
    if (lp > t) t = lp;
  }
  return t;
}

void initDefaults() {
  for (uint8_t p = 0; p < PROGRAM_COUNT; p++) {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
      schedules[p][i] = {
        (uint8_t)7, (uint8_t)(i * 5),
        2.0f,   // flowRate L/min
        1.0f,   // area m²
        30,     // threshold %
        (uint8_t)(i + 1),
        0.85f,  // efficiency
        50.0f,  // soilMaxMm
        0.8f,   // Kc
        5.0f,   // rainCancelMm
        10,     // maxDecisionMin
        true    // enabled
      };
    }
  }
}
void loadSchedules() {
  prefs.begin("riego", true);
  activeProgram = prefs.getUChar("active_prog", 0);
  if (activeProgram >= PROGRAM_COUNT) activeProgram = 0;
  bool stationIdSanitized = false;
  for (uint8_t p = 0; p < PROGRAM_COUNT; p++) {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
      String b = "p" + String(p) + "z" + String(i);
      schedules[p][i].startHour     = prefs.getUChar((b + "h").c_str(), schedules[p][i].startHour);
      schedules[p][i].startMinute   = prefs.getUChar((b + "m").c_str(), schedules[p][i].startMinute);
      schedules[p][i].flowRate      = prefs.getFloat((b + "f").c_str(), schedules[p][i].flowRate);
      schedules[p][i].area          = prefs.getFloat((b + "a").c_str(), schedules[p][i].area);
      schedules[p][i].threshold     = prefs.getUChar((b + "t").c_str(), schedules[p][i].threshold);
      schedules[p][i].stationID     = prefs.getUChar((b + "s").c_str(), schedules[p][i].stationID);
      schedules[p][i].efficiency    = prefs.getFloat((b + "e").c_str(), schedules[p][i].efficiency);
      schedules[p][i].soilMaxMm     = prefs.getFloat((b + "x").c_str(), schedules[p][i].soilMaxMm);
      schedules[p][i].Kc            = prefs.getFloat((b + "k").c_str(), schedules[p][i].Kc);
      schedules[p][i].rainCancelMm  = prefs.getFloat((b + "r").c_str(), schedules[p][i].rainCancelMm);
      schedules[p][i].maxDecisionMin= prefs.getUChar((b + "d").c_str(), schedules[p][i].maxDecisionMin);
      schedules[p][i].enabled       = prefs.getBool( (b + "n").c_str(), schedules[p][i].enabled);
      if (schedules[p][i].stationID < 1 || schedules[p][i].stationID > ZONE_COUNT) {
        schedules[p][i].stationID = (uint8_t)(i + 1);
        stationIdSanitized = true;
        Serial.printf("[SCHED] WARN: stationID invalido p%u z%u -> corregido a %u\n",
                      (unsigned)p, (unsigned)i, (unsigned)schedules[p][i].stationID);
      }
    }
  }
  prefs.end();
  if (stationIdSanitized) saveSchedules();
}
void saveSchedules() {
  prefs.begin("riego", false);
  prefs.putUChar("active_prog", activeProgram);
  for (uint8_t p = 0; p < PROGRAM_COUNT; p++) {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
      String b = "p" + String(p) + "z" + String(i);
      prefs.putUChar((b + "h").c_str(), schedules[p][i].startHour);
      prefs.putUChar((b + "m").c_str(), schedules[p][i].startMinute);
      prefs.putFloat((b + "f").c_str(), schedules[p][i].flowRate);
      prefs.putFloat((b + "a").c_str(), schedules[p][i].area);
      prefs.putUChar((b + "t").c_str(), schedules[p][i].threshold);
      prefs.putUChar((b + "s").c_str(), schedules[p][i].stationID);
      prefs.putFloat((b + "e").c_str(), schedules[p][i].efficiency);
      prefs.putFloat((b + "x").c_str(), schedules[p][i].soilMaxMm);
      prefs.putFloat((b + "k").c_str(), schedules[p][i].Kc);
      prefs.putFloat((b + "r").c_str(), schedules[p][i].rainCancelMm);
      prefs.putUChar((b + "d").c_str(), schedules[p][i].maxDecisionMin);
      prefs.putBool( (b + "n").c_str(), schedules[p][i].enabled);
    }
  }
  prefs.end();
}

// --------------------------- OLED 128x64 I2C (U8g2, 1 página = poca RAM) ---------------------------
#if ENABLE_OLED
static U8G2_SSD1306_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
static bool     oledOk = false;
static uint32_t oledLastUserMs = 0;
static bool     oledSleep = false;
static uint32_t oledLastNormDraw = 0;
static uint32_t oledLastSleepDraw = 0;
static int16_t  oledBx = 4, oledBy = 6, oledBvx = 8, oledBvy = 6;

void oledMarkActivity(void) {
  oledLastUserMs = millis();
  oledSleep = false;
}

static bool oledAnyPumpOn(void) {
  for (uint8_t i = 0; i < ZONE_COUNT; i++)
    if (zones[i].on) return true;
  return false;
}

// Avisos: red, WiFi, NTP, hora; sensores solo tras 2 min de arranque
static bool oledCriticalAlert(char *outMsg, size_t outN) {
  if (outN == 0) return false;
  outMsg[0] = '\0';
  if (!WiFi.isConnected()) {
    strncpy(outMsg, "! Sin WiFi", outN - 1);
    outMsg[outN - 1] = '\0';
    return true;
  }
  if (!ntpConfigured) {
    strncpy(outMsg, "! NTP", outN - 1);
    outMsg[outN - 1] = '\0';
    return true;
  }
  struct tm t;
  if (!getLocalTime(&t, 10)) {
    strncpy(outMsg, "! Hora", outN - 1);
    outMsg[outN - 1] = '\0';
    return true;
  }
  const uint32_t staleMs = 15UL * 60UL * 1000UL;
  const uint32_t graceMs = 120000UL;
  if (millis() < graceMs) return false;
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    PumpSchedule &cfg = schedules[activeProgram][z];
    if (!cfg.enabled) continue;
    uint8_t st = cfg.stationID;
    if (st < 1 || st > ZONE_COUNT) continue;
    uint32_t last = lastStationSeenAt[st - 1];
    if (last == 0) {
      snprintf(outMsg, outN, "! Sin S%u", st);
      return true;
    }
    if (millis() - last > staleMs) {
      snprintf(outMsg, outN, "! S%u >15m", st);
      return true;
    }
  }
  // Alguna sonda con dato fresco en la estación pero otra sin dato reciente → posible fallo
  for (uint8_t st = 0; st < ZONE_COUNT; st++) {
    bool anyFresh = false;
    for (uint8_t j = 0; j < SENSORS_PER_STATION; j++)
      if (sensorSampleFresh(st, j)) {
        anyFresh = true;
        break;
      }
    if (!anyFresh) continue;
    for (uint8_t sid = 0; sid < SENSORS_PER_STATION; sid++) {
      if (sensorSampleFresh(st, sid)) continue;
      if (lastSensorPacketAt[st][sid] == 0)
        snprintf(outMsg, outN, "! E%u S%u sin dato", (unsigned)(st + 1), (unsigned)(sid + 1));
      else
        snprintf(outMsg, outN, "! E%u S%u obsoleto", (unsigned)(st + 1), (unsigned)(sid + 1));
      outMsg[outN - 1] = '\0';
      return true;
    }
  }
  return false;
}

static void oledDrawNormal(void) {
  char l0[24], l1[22], l2[24], l3[30], l4[22], alert[22];
  struct tm tmv;
  if (getLocalTime(&tmv, 80)) {
    snprintf(l0, sizeof(l0), "%02d:%02d:%02d %02d/%02d/%02d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             tmv.tm_mday, tmv.tm_mon + 1, (tmv.tm_year + 1900) % 100);
  } else
    snprintf(l0, sizeof(l0), "--:--:--");

  snprintf(l1, sizeof(l1), "%s", WiFi.isConnected() ? "WiFi OK" : "Sin WiFi");

  uint8_t onZ = 255;
  for (uint8_t i = 0; i < ZONE_COUNT; i++)
    if (zones[i].on) {
      onZ = i;
      break;
    }
  if (onZ == 255)
    snprintf(l2, sizeof(l2), "Riego: todas OFF");
  else {
    const char *how = zones[onZ].source == ZSRC_AUTO ? "Auto" : (zones[onZ].source == ZSRC_LOCAL ? "Local" : "Nube");
    snprintf(l2, sizeof(l2), "Z%u ON (%s)", onZ + 1, how);
  }

  uint16_t bestM = 9999;
  uint8_t bestZ = 255;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (!schedules[activeProgram][i].enabled) continue;
    if (decisions[i].minsToSched < bestM) {
      bestM = decisions[i].minsToSched;
      bestZ = i;
    }
  }
  if (bestZ == 255)
    snprintf(l3, sizeof(l3), "Prox: --");
  else if (bestM == 0)
    snprintf(l3, sizeof(l3), "Prox: Z%u %s ya", bestZ + 1, decisions[bestZ].schedHHMM);
  else
    snprintf(l3, sizeof(l3), "Prox: Z%u %s %um", bestZ + 1, decisions[bestZ].schedHHMM, (unsigned)bestM);

  uint8_t minH = 255, minS = 0;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (!schedules[activeProgram][i].enabled) continue;
    uint8_t st = schedules[activeProgram][i].stationID;
    if (st < 1 || st > ZONE_COUNT) continue;
    if (!moisturePctValid[st - 1]) continue;
    uint8_t h = moisturePct[st - 1];
    if (h < minH) {
      minH = h;
      minS = st;
    }
  }
  if (minS == 0)
    snprintf(l4, sizeof(l4), "Hum: --");
  else
    snprintf(l4, sizeof(l4), "Hum S%u:%u%%", minS, (unsigned)minH);

  bool hasAl = oledCriticalAlert(alert, sizeof(alert));

  // 5x7 + salto 8 px: 6 líneas caben en 64 px (6x10 dejaba la última fuera de pantalla)
  oled.firstPage();
  do {
    oled.setFont(u8g2_font_5x7_tf);
    uint8_t y = 7;
    const uint8_t lineH = 8;
    oled.drawStr(0, y, l0);
    y += lineH;
    oled.drawStr(0, y, l1);
    y += lineH;
    oled.drawStr(0, y, l2);
    y += lineH;
    oled.drawStr(0, y, l3);
    y += lineH;
    oled.drawStr(0, y, l4);
    y += lineH;
    if (hasAl) oled.drawStr(0, y, alert);
  } while (oled.nextPage());
}

static void oledSleepBounceStep(void) {
  const int16_t maxX = 128 - 72;
  const int16_t maxY = 64 - 12 - 26;
  oledBx += oledBvx;
  oledBy += oledBvy;
  if (oledBx <= 0) {
    oledBx = 0;
    oledBvx = (int16_t)abs((int)oledBvx);
  } else if (oledBx >= maxX) {
    oledBx = maxX;
    oledBvx = -(int16_t)abs((int)oledBvx);
  }
  if (oledBy <= 0) {
    oledBy = 0;
    oledBvy = (int16_t)abs((int)oledBvy);
  } else if (oledBy >= maxY) {
    oledBy = maxY;
    oledBvy = -(int16_t)abs((int)oledBvy);
  }
}

static void oledDrawSleep(void) {
  char tm[8], dt[16];
  struct tm tmv;
  if (getLocalTime(&tmv, 80)) {
    snprintf(tm, sizeof(tm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    snprintf(dt, sizeof(dt), "%02d/%02d/%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
  } else {
    snprintf(tm, sizeof(tm), "--:--");
    dt[0] = '\0';
  }

  oled.firstPage();
  do {
    oled.setFont(u8g2_font_logisoso20_tn);
    oled.drawStr(oledBx, oledBy + 20, tm);
    oled.setFont(u8g2_font_5x7_tf);
    if (dt[0]) oled.drawStr(0, 62, dt);
  } while (oled.nextPage());
}

void oledSetup(void) {
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  oled.setBusClock(400000);
  oledOk = true;
  oledMarkActivity();
  oledLastNormDraw = 0;
  oledLastSleepDraw = 0;
  Serial.println("[OLED] SSD1306 128x64 I2C (SDA=21 SCL=22)");
}

void oledTick(uint32_t nowMs) {
  if (!oledOk) return;

  char alertBuf[22];
  bool alert = oledCriticalAlert(alertBuf, sizeof(alertBuf));
  bool pump = oledAnyPumpOn();
  if (pump || alert) oledMarkActivity();

  if (!pump && !alert && !oledSleep && (nowMs - oledLastUserMs >= 120000UL)) {
    oledSleep = true;
    oledLastSleepDraw = 0;
  }

  if (oledSleep) {
    if (oledLastSleepDraw == 0 || (nowMs - oledLastSleepDraw >= OLED_SLEEP_CLOCK_REFRESH_MS)) {
      oledDrawSleep();
      oledSleepBounceStep();
      oledLastSleepDraw = nowMs;
    }
  } else {
    if (nowMs - oledLastNormDraw >= OLED_NORMAL_REFRESH_MS) {
      oledDrawNormal();
      oledLastNormDraw = nowMs;
    }
  }
}
#endif  // ENABLE_OLED

// --------------------------- Salidas bombas ---------------------------
void armPumpPinsNow() {
  if (pumpPinsArmed) return;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) { pinMode(ZONE_PINS[i], OUTPUT); digitalWrite(ZONE_PINS[i], LOW); delay(15); }
  pumpPinsArmed = true;
}
void enforceSinglePump(uint8_t keepOn) {
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (i == keepOn) continue;
    if (!zones[i].on) continue;
    zones[i] = {false, 0, 0, 0};
    digitalWrite(ZONE_PINS[i], LOW);
  }
}
void stopZone(uint8_t i, const char *reason) {
  if (i >= ZONE_COUNT) return;
  if (!pumpPinsArmed) armPumpPinsNow();
  zones[i] = {false, 0, 0, 0};
  pinMode(ZONE_PINS[i], OUTPUT);
  digitalWrite(ZONE_PINS[i], LOW);
  statusPushUrgent = true;  // forzar push inmediato para actualizar dashboard
#if ENABLE_OLED
  oledMarkActivity();
#endif
  Serial.printf("[ZONE %u] OFF (%s)\n", i + 1, reason);
}
void startZone(uint8_t i, uint32_t durationMs, ZoneRunSource src) {
  if (i >= ZONE_COUNT) return;
  if (!pumpPinsArmed) armPumpPinsNow();
  enforceSinglePump(i);
  zones[i] = {true, millis(), durationMs, (uint8_t)src};
  pinMode(ZONE_PINS[i], OUTPUT);
  digitalWrite(ZONE_PINS[i], HIGH);
  if (src == ZSRC_LOCAL) localPriorityUntil = millis() + 2UL * 60UL * 1000UL;
#if ENABLE_OLED
  oledMarkActivity();
#endif
  Serial.printf("[ZONE %u] ON dur=%lu src=%u\n", i + 1, durationMs, (unsigned)src);
}

// --------------------------- API local y UI ---------------------------
void handleLoginGet() {
  String html = htmlHeader("Login");
  html += "<h2>RiegoSmart Central</h2><form method='POST' action='/login'><input name='u' autocomplete='username' placeholder='usuario'><br><input name='p' type='password' autocomplete='current-password' placeholder='clave'><br><button type='submit'>Entrar</button></form></body></html>";
  server.send(200, "text/html", html);
}
void handleLoginPost() {
  String u = server.arg("u"), p = server.arg("p"); u.trim(); p.trim(); u.toLowerCase();
  if (u == String(LOCAL_USER) && p == String(LOCAL_PASS)) {
    sessionToken = String((uint32_t)esp_random(), HEX);
    server.sendHeader("Set-Cookie", "rs_session=" + sessionToken + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "OK");
    return;
  }
  server.send(401, "text/plain", "Credenciales invalidas");
}

void handleApiState() {
  if (!isAuthenticated()) return requireAuth();
  DynamicJsonDocument doc(1024);
  doc["wifi"] = WiFi.isConnected();
  doc["espnow_age_s"] = (millis() - lastEspNowAt) / 1000UL;
  JsonArray zonesJson = doc.createNestedArray("zones");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    JsonObject z = zonesJson.createNestedObject();
    z["id"] = i + 1;
    z["on"] = zones[i].on;
    z["hum"] = moisturePct[i];
    z["hum_valid"] = moisturePctValid[i];
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}
void handleOnOff(bool turnOn) {
  if (!isAuthenticated()) return requireAuth();
  uint8_t z = (uint8_t)server.arg("z").toInt();
  uint32_t m = (uint32_t)server.arg("m").toInt();
  if (m == 0) m = 5; if (m > 15) m = 15;
  if (turnOn) startZone(z, m * 60000UL, ZSRC_LOCAL); else stopZone(z, "manual");
  DynamicJsonDocument doc(128); doc["ok"] = true; JsonObject zone = doc.createNestedObject("zone"); zone["id"] = z + 1; zone["on"] = zones[z].on; zone["hum"] = moisturePct[z]; zone["hum_valid"] = moisturePctValid[z];
  String out; serializeJson(doc, out);
  if (server.uri().startsWith("/api/")) server.send(200, "application/json", out);
  else { server.sendHeader("Location", "/"); server.send(302, "text/plain", "OK"); }
}
void handleOn() { handleOnOff(true); }
void handleOff() { handleOnOff(false); }

void handleApiScheduleGet() {
  if (!isAuthenticated()) return requireAuth();
  int prog = server.arg("prog").toInt(); if (prog < 0 || prog >= PROGRAM_COUNT) prog = activeProgram;
  DynamicJsonDocument doc(2048); doc["active"] = activeProgram; JsonArray pumps = doc.createNestedArray("pumps");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    JsonObject p = pumps.createNestedObject();
    p["id"] = i; p["startHour"] = schedules[prog][i].startHour; p["startMinute"] = schedules[prog][i].startMinute; p["flowRate"] = schedules[prog][i].flowRate;
    p["area"] = schedules[prog][i].area; p["vwcThreshold"] = schedules[prog][i].threshold; p["assignedStationID"] = schedules[prog][i].stationID;
  }
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}
void handleApiScheduleSave() {
  if (!isAuthenticated()) return requireAuth();
  int prog = server.arg("prog").toInt();
  if (prog < 0 || prog >= PROGRAM_COUNT) return server.send(400, "application/json", "{\"ok\":false}");
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok || !doc.is<JsonArray>()) return server.send(400, "application/json", "{\"ok\":false}");
  for (JsonObject p : doc.as<JsonArray>()) {
    int id = p["id"] | -1; if (id < 0 || id >= ZONE_COUNT) continue;
    schedules[prog][id].startHour = constrain((int)(p["startHour"] | 7), 0, 23);
    schedules[prog][id].startMinute = constrain((int)(p["startMinute"] | 0), 0, 59);
    schedules[prog][id].flowRate = (float)(p["flowRate"] | 2.0f);
    schedules[prog][id].area = (float)(p["area"] | 1.0f);
    schedules[prog][id].threshold = constrain((int)(p["vwcThreshold"] | 30), 0, 100);
    schedules[prog][id].stationID = constrain((int)(p["assignedStationID"] | (id + 1)), 1, ZONE_COUNT);
  }
  saveSchedules();
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleApiProgramSelect() {
  if (!isAuthenticated()) return requireAuth();
  int id = server.arg("id").toInt();
  if (id < 0 || id >= PROGRAM_COUNT) return server.send(400, "application/json", "{\"ok\":false}");
  activeProgram = id; saveSchedules(); server.send(200, "application/json", "{\"ok\":true}");
}
void handleApiTime() {
  if (!isAuthenticated()) return requireAuth();
  DynamicJsonDocument doc(128); struct tm t;
  if (getLocalTime(&t, 30)) { char ntp[16]; strftime(ntp, sizeof(ntp), "%H:%M:%S", &t); doc["ntp"] = ntp; } else doc["ntp"] = "--:--:--";
  uint32_t s = millis() / 1000UL; char rtc[16]; snprintf(rtc, sizeof(rtc), "%02lu:%02lu:%02lu", (s / 3600UL) % 24UL, (s / 60UL) % 60UL, s % 60UL); doc["rtc"] = rtc;
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}
void handleApiSensorsList() {
  if (!isAuthenticated()) return requireAuth();
  DynamicJsonDocument doc(4096); JsonArray arr = doc.to<JsonArray>();
  for (uint8_t st = 0; st < ZONE_COUNT; st++) {
    JsonObject s = arr.createNestedObject();
    s["name"] = "Estacion " + String(st + 1);
    s["stationID"] = st + 1;
    JsonArray sens = s.createNestedArray("sensors");
    for (uint8_t sid = 0; sid < SENSORS_PER_STATION; sid++) {
      JsonObject o = sens.createNestedObject();
      o["sensorID"] = sid + 1;
      o["min"] = calWet[st][sid];
      o["max"] = calDry[st][sid];
      o["raw"] = sensorRaw[st][sid];
      o["fresh"] = sensorSampleFresh(st, sid);
      o["age_s"] = (lastSensorPacketAt[st][sid] == 0) ? 999999u : (unsigned)((millis() - lastSensorPacketAt[st][sid]) / 1000UL);
    }
    s["fail"] = (millis() - lastEspNowAt > (SENSOR_PUSH_INTERVAL_MS + 600000UL));
  }
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}
void handleApiSensorsAdd() { if (!isAuthenticated()) return requireAuth(); server.send(200, "application/json", "{\"ok\":true}"); }
void handleApiSensorsRemove() { if (!isAuthenticated()) return requireAuth(); server.send(200, "application/json", "{\"ok\":true}"); }

void handleCalGet() {
  if (!isAuthenticated()) return requireAuth();
  String html = htmlHeader("Calibracion");
  html += "<h2>Calibracion raw por sensor (seco=máx, mojado=mín)</h2><form method='POST' action='/cal'>";
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    html += "<p><b>Zona " + String(i + 1) + "</b> (3 sensores)</p>";
    for (uint8_t j = 0; j < SENSORS_PER_STATION; j++) {
      html += "S" + String(j + 1) + " seco:<input type='number' name='d" + String(i) + "_" + String(j) + "' value='" + String(calDry[i][j]) + "'> ";
      html += "mojado:<input type='number' name='w" + String(i) + "_" + String(j) + "' value='" + String(calWet[i][j]) + "'><br>";
    }
  }
  html += "<button type='submit'>Guardar</button></form><p><a href='/'><button>Volver</button></a></p></body></html>";
  server.send(200, "text/html", html);
}
void handleCalPost() {
  if (!isAuthenticated()) return requireAuth();
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    for (uint8_t j = 0; j < SENSORS_PER_STATION; j++) {
      uint16_t d = (uint16_t)server.arg("d" + String(i) + "_" + String(j)).toInt();
      uint16_t w = (uint16_t)server.arg("w" + String(i) + "_" + String(j)).toInt();
      if (d > 0 && w > 0 && d > w) {
        calDry[i][j] = d;
        calWet[i][j] = w;
      }
    }
  }
  saveCalibration(); server.sendHeader("Location", "/cal"); server.send(302, "text/plain", "OK");
}
void handleRoot() {
  if (!isAuthenticated()) return requireAuth();
  String html = htmlHeader("Panel local");
  html += "<h2>Panel local</h2><p id='wifiState'>WiFi: " + String(WiFi.isConnected() ? "conectado" : "sin internet") + "</p><p id='espnowAge'>Ultimo paquete ESP-NOW: " + String((millis() - lastEspNowAt) / 1000) + "s</p>";
  html += "<table><thead><tr><th>Zona</th><th>Estado</th><th>Humedad %</th><th>Acciones</th></tr></thead><tbody>";
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    html += "<tr><td>" + String(i + 1) + "</td><td id='st_" + String(i) + "' class='" + String(zones[i].on ? "on" : "off") + "'>" + String(zones[i].on ? "ON" : "OFF") + "</td><td id='hm_" + String(i) + "'>" + String(moisturePctValid[i] ? String(moisturePct[i]) : String("--")) + "</td>";
    html += "<td><button onclick='setZone(" + String(i) + ",1)'>ON 5m</button><button onclick='setZone(" + String(i) + ",0)'>OFF</button></td></tr>";
  }
  html += "</tbody></table><p><a href='/cal'><button>Calibracion</button></a></p>";
  html += "<script>async function setZone(z,on){const u=on?'/api/on':'/api/off'; await fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'z='+z+'&m=5',credentials:'same-origin'});} setInterval(async()=>{const r=await fetch('/api/state',{credentials:'same-origin'});const d=await r.json();document.getElementById('wifiState').innerText='WiFi: '+(d.wifi?'conectado':'sin internet');document.getElementById('espnowAge').innerText='Ultimo paquete ESP-NOW: '+d.espnow_age_s+'s';for(let i=0;i<d.zones.length;i++){let s=document.getElementById('st_'+i),h=document.getElementById('hm_'+i);s.innerText=d.zones[i].on?'ON':'OFF';s.className=d.zones[i].on?'on':'off';h.innerText=d.zones[i].hum_valid?String(d.zones[i].hum):'--';}},2000);</script></body></html>";
  server.send(200, "text/html", html);
}

void startWebServer() {
  const char *headerKeys[] = {"Cookie"}; server.collectHeaders(headerKeys, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLoginGet); server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/on", HTTP_GET, handleOn); server.on("/off", HTTP_GET, handleOff);
  server.on("/api/on", HTTP_GET, handleOn); server.on("/api/off", HTTP_GET, handleOff);
  server.on("/api/on", HTTP_POST, handleOn); server.on("/api/off", HTTP_POST, handleOff);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/schedule", HTTP_GET, handleApiScheduleGet);
  server.on("/api/schedule/save", HTTP_POST, handleApiScheduleSave);
  server.on("/api/program/select", HTTP_GET, handleApiProgramSelect);
  server.on("/api/time", HTTP_GET, handleApiTime);
  server.on("/api/sensors/list", HTTP_GET, handleApiSensorsList);
  server.on("/api/sensors/add", HTTP_GET, handleApiSensorsAdd);
  server.on("/api/sensors/remove", HTTP_GET, handleApiSensorsRemove);
  server.on("/cal", HTTP_GET, handleCalGet); server.on("/cal", HTTP_POST, handleCalPost);
  server.begin();
}

// --------------------------- ESP-NOW ---------------------------
void applySensorSample(uint8_t stationID, uint8_t sensorID, uint16_t raw, uint16_t batteryMv) {
  if (stationID < 1 || stationID > ZONE_COUNT || sensorID < 1 || sensorID > SENSORS_PER_STATION) return;
  uint8_t st = stationID - 1, sid = sensorID - 1;
  sensorRaw[st][sid] = raw;
  sensorSeen[st][sid] = true;
  if (batteryMv > 0) stationBatteryMv[st] = batteryMv;
  lastSensorPacketAt[st][sid] = millis();
  lastStationSeenAt[st] = millis();
  recalcStationMoisture(st);
  sensorDataPushPending[st] = true;
  lastEspNowAt = millis();
  Serial.printf("ESP-NOW st=%u s=%u raw=%u hum=%u%% valid=%s batt=%umV\n", stationID, sensorID, raw, moisturePct[st],
                moisturePctValid[st] ? "si" : "no", stationBatteryMv[st]);
}
void processEspNowPayload(const uint8_t *incomingData, int len) {
  if (len == (int)sizeof(LegacySensorPacket)) {
    LegacySensorPacket p; memcpy(&p, incomingData, sizeof(p));
    applySensorSample((uint8_t)p.stationID, (uint8_t)p.sensorID, (uint16_t)p.rawValue, 0);
    return;
  }
  if (len == (int)sizeof(SensorPacketV2)) {
    SensorPacketV2 p2; memcpy(&p2, incomingData, sizeof(p2));
    applySensorSample(p2.stationID, p2.sensorID, p2.rawValue, p2.batteryMv);
    return;
  }
  Serial.printf("ESP-NOW paquete invalido len=%d\n", len);
}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) { (void)info; processEspNowPayload(incomingData, len); }
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *incomingData, int len) { (void)mac; processEspNowPayload(incomingData, len); }
#endif
void setupEspNow() {
  if (espNowReady) return;
  uint8_t ch = (uint8_t)WiFi.channel();
  if (ch < 1 || ch > 13) ch = ESPNOW_FALLBACK_CHANNEL;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(onEspNowRecv);
  espNowReady = true;
  Serial.printf("ESP-NOW RX canal %u = satélite en mismo canal\n", (unsigned)ch);
}

// --------------------------- Supabase ---------------------------
// HTTPS sobre ESP32 necesita tiempo para TLS handshake; 2500ms es insuficiente.
// setConnectTimeout: cuánto esperar para establecer TCP.
// setTimeout: cuánto esperar para recibir respuesta una vez conectado.
static const int HTTP_CONNECT_TIMEOUT_MS = 6000;
static const int HTTP_RESPONSE_TIMEOUT_MS = 8000;

String supabaseGet(const char *endpoint) {
  if (!WiFi.isConnected()) return "";
  HTTPClient http;
  http.begin(String(SUPABASE_URL) + endpoint);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  int code = http.GET();
  String out = (code > 0) ? http.getString() : "";
  http.end();
  return out;
}
bool supabasePost(const char *endpoint, const String &json) {
  if (!WiFi.isConnected()) return false;
  HTTPClient http;
  http.begin(String(SUPABASE_URL) + endpoint);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  http.addHeader("Prefer", "return=minimal");
  int code = http.POST(json);
  http.end();
  return code >= 200 && code < 300;
}
bool supabasePatch(const String &endpoint, const String &json) {
  if (!WiFi.isConnected()) return false;
  HTTPClient http;
  http.begin(String(SUPABASE_URL) + endpoint);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  http.addHeader("Prefer", "return=minimal");
  int code = http.sendRequest("PATCH", (uint8_t *)json.c_str(), json.length());
  http.end();
  return code >= 200 && code < 300;
}

void processCloudCommands() {
  if (millis() < localPriorityUntil) return;
  String payload = supabaseGet(SUPABASE_COMMANDS_ENDPOINT);
  if (!payload.length()) return;
  static DynamicJsonDocument doc(512);  // static: se asigna una sola vez, evita fragmentación
  doc.clear();
  if (deserializeJson(doc, payload) != DeserializationError::Ok || !doc.is<JsonArray>() || doc.size() == 0) return;
  JsonObject c = doc[0];
  int cmdId = c["id"] | -1, pumpId = c["pump_id"] | -1; bool target = c["target_state"] | false;
  if (cmdId < 0) return;
  int idx = (pumpId >= 0 && pumpId < ZONE_COUNT) ? pumpId : ((pumpId >= 1 && pumpId <= ZONE_COUNT) ? pumpId - 1 : -1);
  if (idx < 0) return;
  if (target) startZone((uint8_t)idx, 5UL * 60000UL, ZSRC_CLOUD); else stopZone((uint8_t)idx, "cloud");
  supabasePatch(String("/rest/v1/remote_commands?id=eq.") + cmdId, "{\"is_processed\":true}");
}

void pullScheduleFromCloud() {
  String payload = supabaseGet("/rest/v1/system_state?select=active_program,pumps,utc_offset_sec&id=eq.1");
  if (!payload.length()) {
    Serial.println("[SCHED] Sin datos desde nube.");
    return;
  }
  // 5 zonas × 3 programas × ~15 campos → ~6–10 KB; 4096 provocaba NoMemory y nunca aplicaba horarios
  static DynamicJsonDocument doc(14336);
  doc.clear();
  DeserializationError jerr = deserializeJson(doc, payload);
  if (jerr) {
    Serial.printf("[SCHED] JSON error: %s (payload %u bytes)\n", jerr.c_str(), (unsigned)payload.length());
    return;
  }
  if (!doc.is<JsonArray>() || doc.size() == 0) {
    Serial.println("[SCHED] Respuesta system_state vacia.");
    return;
  }
  JsonObject row = doc[0];
  if (!row["utc_offset_sec"].isNull()) {
    long u = row["utc_offset_sec"].as<long>();
    applyBrowserUtcOffsetSeconds(u, true);
  }
  int oldActive = activeProgram;
  int ap = row["active_program"] | activeProgram;
  if (ap >= 0 && ap < PROGRAM_COUNT) activeProgram = ap;
  JsonArray pumps = row["pumps"].as<JsonArray>();
  if (pumps.isNull()) {
    Serial.println("[SCHED] WARN: columna pumps null en Supabase.");
    return;
  }

  bool anyChanged = (oldActive != activeProgram);  // guardar solo si algo cambia

  for (uint8_t i = 0; i < ZONE_COUNT && i < pumps.size(); i++) {
    JsonArray progs = pumps[i]["programs"].as<JsonArray>();
    if (progs.isNull()) {
      Serial.printf("[SCHED] WARN Z%u sin array programs (no se actualiza desde nube)\n", i + 1);
      continue;
    }
    PumpSchedule oldActiveCfg = schedules[activeProgram][i];
    for (uint8_t p = 0; p < PROGRAM_COUNT && p < progs.size(); p++) {
      JsonObject cfg = progs[p];
      schedules[p][i].startHour     = constrain((int)(cfg["startHour"]         | schedules[p][i].startHour),     0, 23);
      schedules[p][i].startMinute   = constrain((int)(cfg["startMinute"]       | schedules[p][i].startMinute),   0, 59);
      schedules[p][i].flowRate      = (float)(cfg["flowRate"]    | schedules[p][i].flowRate);
      schedules[p][i].area          = (float)(cfg["area"]        | schedules[p][i].area);
      schedules[p][i].threshold     = constrain((int)(cfg["vwcThreshold"]      | schedules[p][i].threshold),     0, 100);
      schedules[p][i].stationID     = constrain((int)(cfg["assignedStationID"] | schedules[p][i].stationID),     1, ZONE_COUNT);
      schedules[p][i].efficiency    = constrain((float)(cfg["efficiency"]      | schedules[p][i].efficiency),    0.1f, 1.0f);
      schedules[p][i].soilMaxMm     = constrain((float)(cfg["soilMaxMm"]       | schedules[p][i].soilMaxMm),     5.0f, 300.0f);
      schedules[p][i].Kc            = constrain((float)(cfg["Kc"]              | schedules[p][i].Kc),            0.1f, 2.0f);
      schedules[p][i].rainCancelMm  = constrain((float)(cfg["rainCancelMm"]    | schedules[p][i].rainCancelMm),  0.0f, 100.0f);
      schedules[p][i].maxDecisionMin= constrain((int)(cfg["maxDecisionMin"]    | schedules[p][i].maxDecisionMin),1, 60);
      if (cfg.containsKey("enabled")) schedules[p][i].enabled = (bool)cfg["enabled"];
    }
    PumpSchedule newActiveCfg = schedules[activeProgram][i];
    bool zoneChanged =
      oldActiveCfg.startHour   != newActiveCfg.startHour   ||
      oldActiveCfg.startMinute != newActiveCfg.startMinute ||
      oldActiveCfg.threshold   != newActiveCfg.threshold   ||
      oldActiveCfg.stationID   != newActiveCfg.stationID   ||
      oldActiveCfg.flowRate    != newActiveCfg.flowRate     ||
      oldActiveCfg.area        != newActiveCfg.area         ||
      oldActiveCfg.enabled     != newActiveCfg.enabled;
    if (zoneChanged) {
      anyChanged = true;
      decisions[i].lastRunDay   = -1;
      decisions[i].lastRunSched = -1;
      decisions[i].result = "Horario actualizado";
      Serial.printf("[SCHED] Cambio Z%u (%02u:%02u->%02u:%02u thr=%u->%u ena=%d) reseteado\n",
                    i + 1,
                    oldActiveCfg.startHour, oldActiveCfg.startMinute,
                    newActiveCfg.startHour, newActiveCfg.startMinute,
                    oldActiveCfg.threshold, newActiveCfg.threshold,
                    (int)newActiveCfg.enabled);
    }
  }

  // Solo escribe en Flash si algo cambió (ahorra ciclos de escritura NOR)
  if (anyChanged) {
    saveSchedules();
    if (oldActive != activeProgram)
      Serial.printf("[SCHED] active_program: %d -> %d\n", oldActive, activeProgram);
  }
}

// Registra un riego completado (o cancelado) en la tabla irrigation_log de Supabase.
// Se llama desde predictiveCheck: una vez al cancelar por lluvia y una al completar auditoría.
void pushIrrigationLog(uint8_t zoneIdx, uint8_t durationMins, float liters,
                       uint8_t humStart, uint8_t humEnd, int8_t deltaHum,
                       const char* reason, bool cancelled) {
  if (!WiFi.isConnected()) return;
  DynamicJsonDocument doc(256);
  doc["zone_id"]       = zoneIdx + 1;
  doc["duration_mins"] = durationMins;
  doc["liters"]        = liters;
  doc["hum_start"]     = humStart;
  doc["hum_end"]       = humEnd;
  doc["delta_hum"]     = deltaHum;
  doc["reason"]        = reason;
  doc["cancelled"]     = cancelled;
  doc["rain_mm"]       = wx_rain24h;
  doc["et0_mm"]        = wx_et0;
  String out; serializeJson(doc, out);
  bool ok = supabasePost("/rest/v1/irrigation_log", out);
  Serial.printf("[LOG] Z%u irrigation_log %s\n", zoneIdx + 1, ok ? "OK" : "FAIL");
}

void pushStatusToCloud() {
  // REGLA DE ORO:
  //   "decisions" → SOLO escribe el ESP32  (estado operativo: active, hum, timers, etc.)
  //   "pumps"     → SOLO escribe el dashboard (programas, horarios, enabled, etc.)
  // El ESP32 NUNCA incluye "pumps" en este PATCH. Si lo hiciera, Supabase reemplazaría la
  // columna entera borrando los programas guardados por el usuario.
  static DynamicJsonDocument doc(3072);
  doc.clear();
  doc["active_program"] = activeProgram;
  JsonArray decisionsJson = doc.createNestedArray("decisions");
  uint32_t now = millis();
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    PumpSchedule &cfg = schedules[activeProgram][i];
    uint8_t stIdx = stationIdxFromSchedule(cfg, i);
    uint8_t liveHum = moisturePct[stIdx];
    bool humValid = moisturePctValid[stIdx];
    uint32_t lastRx = effectiveLastRxMs(stIdx);
    uint32_t sensorAge = (lastRx == 0)
                         ? 0xFFFFu
                         : (now - lastRx) / 1000UL;

    JsonObject d = decisionsJson.createNestedObject();
    // Estado operativo de la bomba
    d["active"]      = zones[i].on;
    d["manual"]      = (now < localPriorityUntil);
    if (zones[i].on && zones[i].requestedDurationMs > 0) {
      uint32_t elapsed = now - zones[i].startedAt;
      int32_t  rem     = (int32_t)zones[i].requestedDurationMs - (int32_t)elapsed;
      d["remaining_s"] = (rem > 0) ? (uint32_t)(rem / 1000UL) : 0;
      d["total_s"]     = zones[i].requestedDurationMs / 1000UL;
    } else {
      d["remaining_s"] = 0;
      d["total_s"]     = 0;
    }
    // Estado agronómico
    d["hum"]            = liveHum;
    d["hum_valid"]      = humValid;
    JsonArray psa = d.createNestedArray("sensor_age_s");
    for (uint8_t sid = 0; sid < SENSORS_PER_STATION; sid++) {
      uint32_t lp = lastSensorPacketAt[stIdx][sid];
      psa.add((lp == 0) ? 0xFFFFu : (uint32_t)((now - lp) / 1000UL));
    }
    d["thr"]            = cfg.threshold;
    d["min"]            = decisions[i].minutes;
    d["reason"]         = decisions[i].reason.length() ? decisions[i].reason : "Sincronizando";
    d["result"]         = decisions[i].result.length()  ? decisions[i].result  : "Sin datos";
    d["sched_hhmm"]     = decisions[i].schedHHMM;
    d["mins_to_sched"]  = decisions[i].minsToSched;
    d["next_eval_min"]  = decisions[i].nextEvalMins;
    d["next_eval_hhmm"] = decisions[i].nextEvalHHMM;
    d["sensor_age_s"]   = sensorAge;
    d["enabled"]        = cfg.enabled;
    // Auditoría del último riego
    if (decisions[i].auditResult.length()) {
      JsonObject audit = d.createNestedObject("audit");
      audit["liters"]    = decisions[i].auditLiters;
      audit["hum_start"] = decisions[i].humAtStart;
      audit["delta_hum"] = decisions[i].auditDeltaHum;
      audit["mins"]      = decisions[i].auditMins;
      audit["summary"]   = decisions[i].auditResult;
    }
  }
  // Datos climáticos para el dashboard
  JsonObject wx = doc.createNestedObject("weather");
  wx["rain_24h"] = wx_rain24h;
  wx["et0_mm"]   = wx_et0;
  wx["tmax"]     = wx_tMax;
  wx["tmin"]     = wx_tMin;
  wx["desc"]     = wx_desc;
  wx["fetched"]  = wx_fetched;
  if (wx_fetched) {
    struct tm t; char ts[8];
    if (getLocalTime(&t, 50)) { strftime(ts, sizeof(ts), "%H:%M", &t); wx["at"] = ts; }
  }

  String out; serializeJson(doc, out);
  bool ok = supabasePatch(SUPABASE_SYSTEM_STATE_ENDPOINT, out);
  Serial.printf("[CLOUD] pushStatus %s (%u bytes) wx_fetched=%s\n",
                ok ? "OK" : "FAIL", out.length(), wx_fetched ? "SI" : "NO");

  bool anySensorPush = false;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (sensorDataPushPending[i]) {
      anySensorPush = true;
      break;
    }
  }
  if (!anySensorPush) return;
  now = millis();
  if (lastSensorDataPushAt != 0 && now - lastSensorDataPushAt < SENSOR_PUSH_INTERVAL_MS) return;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (!sensorDataPushPending[i]) continue;
    if (!moisturePctValid[i]) {
      sensorDataPushPending[i] = false;
      continue;
    }
    DynamicJsonDocument hum(128);
    hum["estacion"] = i + 1;
    hum["humedad"] = moisturePct[i];
    String hs;
    serializeJson(hum, hs);
    bool posted = supabasePost(SUPABASE_SENSOR_DATA_ENDPOINT, hs);
    Serial.printf("[CLOUD] sensor_data E%u hum=%u%% %s\n", i + 1, moisturePct[i], posted ? "OK" : "FAIL");
    sensorDataPushPending[i] = false;
  }
  lastSensorDataPushAt = now;
}

// --------------------------- Clima y ET₀ ---------------------------
// Open-Meteo docs: https://open-meteo.com/en/docs

static float haversineKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  const float toRad = 3.14159265f / 180.0f;
  float dlat = (lat2 - lat1) * toRad;
  float dlon = (lon2 - lon1) * toRad;
  float a = sinf(dlat * 0.5f) * sinf(dlat * 0.5f)
            + cosf(lat1 * toRad) * cosf(lat2 * toRad) * sinf(dlon * 0.5f) * sinf(dlon * 0.5f);
  float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
  return R * c;
}

/** Timeouts largos y sin gzip. No usar HTTP/1.0 ni Connection:close en HTTPS: algunos hosts (p. ej. LiteSpeed/redmeteo) devuelven cuerpo vacío. */
static void httpWeatherConfigure(HTTPClient &http) {
  http.setTimeout(60000);
  http.setConnectTimeout(25000);
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", "RiegoSmart-ESP32/1.0");
}

/**
 * getString() a veces no lee todo el cuerpo en respuestas HTTPS grandes → JSON truncado (IncompleteInput).
 * Si hay Content-Length, lee del stream hasta completar bytes.
 */
static bool httpReadFullBody(HTTPClient &http, String &payload, const char *tag) {
  payload = "";
  int contentLen = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  if (contentLen > 0 && stream) {
    if (!payload.reserve((size_t)contentLen + 32)) {
      Serial.printf("[WX] %s: reserve(%d) fallo heap=%u\n", tag, contentLen, (unsigned)ESP.getFreeHeap());
      payload = http.getString();
      return payload.length() > 0;
    }
    int total = 0;
    uint32_t t0 = millis();
    while (total < contentLen) {
      if (millis() - t0 > 120000UL) break;
      int avail = stream->available();
      if (avail <= 0) {
        if (!http.connected()) break;
        delay(1);
        continue;
      }
      uint8_t buf[1536];
      int need = contentLen - total;
      int n = stream->readBytes((char *)buf, need < (int)sizeof(buf) ? need : (int)sizeof(buf));
      if (n <= 0) {
        delay(1);
        continue;
      }
      for (int i = 0; i < n; i++) payload += (char)buf[i];
      total += n;
    }
    if (total != contentLen)
      Serial.printf("[WX] %s: leidos %d de %d bytes (esperado Content-Length)\n", tag, total, contentLen);
    return total == contentLen && total > 0;
  }
  payload = http.getString();
  if (payload.length() == 0 && contentLen == 0)
    Serial.printf("[WX] %s: sin Content-Length, getString len=%u\n", tag, (unsigned)payload.length());
  return payload.length() > 0;
}

/** Suma `precipitation` horaria de Open-Meteo en las próximas N horas (N = WEATHER_RAIN_FORECAST_HOURS). */
static bool fetchOpenMeteoHourlyPrecipNextHours(float *outSum) {
  if (!outSum) return false;
  *outSum = 0.0f;
  unsigned nh = (unsigned)WEATHER_RAIN_FORECAST_HOURS;
  if (nh < 1u) nh = 1u;
  if (nh > 48u) nh = 48u;
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + String(SITE_LAT, 4)
               + "&longitude=" + String(SITE_LON, 4)
               + "&hourly=precipitation&forecast_hours=" + String(nh)
               + "&timezone=America%2FSantiago";
  HTTPClient http;
  http.begin(url);
  httpWeatherConfigure(http);
  int hc = http.GET();
  if (hc != 200) {
    Serial.printf("[WX] Open-Meteo (hourly precip) HTTP %d\n", hc);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  DynamicJsonDocument doc(8192);
  DeserializationError jerr = deserializeJson(doc, payload);
  if (jerr) {
    Serial.printf("[WX] Open-Meteo (hourly precip) JSON: %s (len=%u)\n", jerr.c_str(), (unsigned)payload.length());
    return false;
  }
  JsonObject hourly = doc["hourly"];
  if (hourly.isNull()) return false;
  JsonArray p = hourly["precipitation"];
  if (p.isNull()) return false;
  for (size_t i = 0; i < p.size() && i < nh; i++) *outSum += (p[i] | 0.0f);
  return true;
}

/** Mezcla lluvia observada en estación (mm) con pronóstico horario corto Open-Meteo. */
static void mergeStationRainWithOpenMeteoHourly(float stationMm) {
  float fh = 0.0f;
  if (fetchOpenMeteoHourlyPrecipNextHours(&fh))
    wx_rain24h = (stationMm > fh) ? stationMm : fh;
  else
    wx_rain24h = stationMm;
}

/** Solo ET₀ y temperaturas diarias (pronóstico Open-Meteo). */
static bool fetchOpenMeteoDailyEt0Temps() {
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + String(SITE_LAT, 4)
               + "&longitude=" + String(SITE_LON, 4)
               + "&daily=temperature_2m_max,temperature_2m_min,et0_fao_evapotranspiration"
               + "&forecast_days=1&timezone=America%2FSantiago";
  HTTPClient http;
  http.begin(url);
  httpWeatherConfigure(http);
  int hc = http.GET();
  if (hc != 200) {
    Serial.printf("[WX] Open-Meteo (ET0/T) HTTP %d\n", hc);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  DynamicJsonDocument doc(3072);
  DeserializationError jerr = deserializeJson(doc, payload);
  if (jerr) {
    Serial.printf("[WX] Open-Meteo (ET0/T) JSON: %s (len=%u)\n", jerr.c_str(), (unsigned)payload.length());
    return false;
  }
  JsonObject daily = doc["daily"];
  if (daily.isNull()) return false;
  wx_et0  = daily["et0_fao_evapotranspiration"][0] | wx_et0;
  wx_tMax = daily["temperature_2m_max"][0] | wx_tMax;
  wx_tMin = daily["temperature_2m_min"][0] | wx_tMin;
  return true;
}

/** Pronóstico lluvia (ventana horaria corta) + ET0 todo Open-Meteo (modo sin RedMeteo). */
static bool fetchWeatherOpenMeteoOnly() {
  unsigned nh = (unsigned)WEATHER_RAIN_FORECAST_HOURS;
  if (nh < 1u) nh = 1u;
  if (nh > 48u) nh = 48u;
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + String(SITE_LAT, 4)
               + "&longitude=" + String(SITE_LON, 4)
               + "&hourly=precipitation&forecast_hours=" + String(nh)
               + "&daily=temperature_2m_max,temperature_2m_min,et0_fao_evapotranspiration"
               + "&forecast_days=1&timezone=America%2FSantiago";
  HTTPClient http;
  http.begin(url);
  httpWeatherConfigure(http);
  int hc = http.GET();
  if (hc != 200) {
    Serial.printf("[WX] Open-Meteo error HTTP %d\n", hc);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  DynamicJsonDocument doc(10240);
  DeserializationError jerr = deserializeJson(doc, payload);
  if (jerr) {
    Serial.printf("[WX] Error JSON Open-Meteo: %s (len=%u)\n", jerr.c_str(), (unsigned)payload.length());
    return false;
  }
  float sumh = 0.0f;
  JsonObject hourly = doc["hourly"];
  if (!hourly.isNull()) {
    JsonArray p = hourly["precipitation"];
    if (!p.isNull()) {
      for (size_t i = 0; i < p.size() && i < nh; i++) sumh += (p[i] | 0.0f);
    }
  }
  wx_rain24h = sumh;
  JsonObject daily = doc["daily"];
  if (daily.isNull()) {
    Serial.println("[WX] Sin bloque daily");
    return false;
  }
  wx_et0  = daily["et0_fao_evapotranspiration"][0] | 0.0f;
  wx_tMax = daily["temperature_2m_max"][0] | 25.0f;
  wx_tMin = daily["temperature_2m_min"][0] | 15.0f;
  if      (wx_rain24h >= 15.0f) wx_desc = "Lluvia intensa (prono.)";
  else if (wx_rain24h >=  5.0f) wx_desc = "Lluvia moderada (prono.)";
  else if (wx_rain24h >=  1.0f) wx_desc = "Lluvia leve (prono.)";
  else                          wx_desc = "Sin lluvia prevista";
  Serial.printf("[WX] Open-Meteo | lluvia prox %uh=%.1fmm | ET0=%.2f | T=%.0f/%.0fC | %s\n",
                nh, wx_rain24h, wx_et0, wx_tMax, wx_tMin, wx_desc.c_str());
  return true;
}

static String meteochileUrlEncode(const char *s) {
  String out;
  if (!s) return out;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      char buf[5];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

static void meteochileCredentials(String &user, String &tok) {
  user = METEOCHILE_USER;
  tok  = METEOCHILE_TOKEN;
  prefs.begin("meteoch", true);
  if (prefs.isKey("user")) {
    String u = prefs.getString("user", "");
    if (u.length()) user = u;
  }
  if (prefs.isKey("token")) {
    String t = prefs.getString("token", "");
    if (t.length()) tok = t;
  }
  prefs.end();
}

static uint32_t meteochileCodigoResolved() {
  prefs.begin("meteoch", true);
  uint32_t c = 0;
  if (prefs.isKey("cod")) c = (uint32_t)prefs.getULong("cod", 0);
  prefs.end();
  if (c > 0) return c;
  return METEOCHILE_CODIGO_NACIONAL;
}

/** Busca un número en el JSON bajo claves que sugieran precipitación (estructura variable según DMC). */
static bool jsonFindPrecipMm(JsonVariant v, float *out) {
  if (out == nullptr || v.isNull()) return false;
  if (v.is<JsonObject>()) {
    JsonObject o = v.as<JsonObject>();
    for (JsonPair p : o) {
      String k = p.key().c_str();
      k.toLowerCase();
      bool relevant = (k.indexOf("precip") >= 0 || k.indexOf("aguacai") >= 0 || k.indexOf("lluvia") >= 0);
      if (relevant) {
        JsonVariant w = p.value();
        if (w.is<float>() || w.is<double>() || w.is<int>()) {
          *out = w.as<float>();
          return true;
        }
        if (w.is<JsonObject>() || w.is<JsonArray>()) {
          if (jsonFindPrecipMm(w, out)) return true;
        }
      }
    }
    for (JsonPair p : o) {
      String k = p.key().c_str();
      k.toLowerCase();
      if (k == "datos" || k == "registros" || k == "observaciones" || k == "data" || k == "valores") {
        if (jsonFindPrecipMm(p.value(), out)) return true;
      }
    }
  } else if (v.is<JsonArray>()) {
    for (JsonVariant it : v.as<JsonArray>()) {
      if (jsonFindPrecipMm(it, out)) return true;
    }
  }
  return false;
}

/** Lluvia reciente (mm) desde EMA DMC vía getDatosRecientesEma/{codigoNacional}. */
static bool fetchMeteochileDatosRecientesEma(const String &user, const String &tok, uint32_t codigo) {
  if (user.length() == 0 || tok.length() == 0 || codigo == 0) return false;
  String url = String(METEOCHILE_BASE) + String(codigo)
               + "?usuario=" + meteochileUrlEncode(user.c_str()) + "&token=" + meteochileUrlEncode(tok.c_str());
  HTTPClient http;
  http.begin(url);
  httpWeatherConfigure(http);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[WX] Meteochile HTTP %d\n", code);
    http.end();
    return false;
  }
  String payload;
  if (!httpReadFullBody(http, payload, "Meteochile")) {
    http.end();
    Serial.println("[WX] Meteochile: no se pudo leer cuerpo");
    return false;
  }
  http.end();
  if (payload.length() < 4) {
    Serial.println("[WX] Meteochile: cuerpo vacio");
    return false;
  }
  if (payload.indexOf("bloqueda") >= 0 || payload.indexOf("bloqueada") >= 0) {
    Serial.println("[WX] Meteochile: servicio bloqueado para este usuario/token (revisa credenciales en portal DMC)");
    return false;
  }
  size_t dc = (size_t)payload.length() * 2u + 8192u;
  if (dc < 65536u) dc = 65536u;
  if (dc > 131072u) dc = 131072u;
  DynamicJsonDocument doc(dc);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[WX] Meteochile JSON: %s | len=%u heap=%u\n", err.c_str(), (unsigned)payload.length(),
                  (unsigned)ESP.getFreeHeap());
    if (err == DeserializationError::IncompleteInput) {
      Serial.println("[WX] Meteochile: JSON aun incompleto (servidor corto o doc RAM insuficiente).");
    }
    return false;
  }
  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) return false;
  float mm = 0.0f;
  if (root["precipitacion"].is<float>() || root["precipitacion"].is<int>()) {
    mm = root["precipitacion"].as<float>();
  } else if (root["precipitacionDiaria"].is<float>() || root["precipitacionDiaria"].is<int>()) {
    mm = root["precipitacionDiaria"].as<float>();
  } else if (root["totalPrecipitacion"].is<float>()) {
    mm = root["totalPrecipitacion"].as<float>();
  } else if (!jsonFindPrecipMm(doc.as<JsonVariant>(), &mm)) {
    Serial.printf("[WX] Meteochile: no se encontro precipitacion en JSON (len=%u). Primeros 160 chars: %.160s\n",
                  (unsigned)payload.length(), payload.c_str());
    return false;
  }
  if (mm < 0.0f) mm = 0.0f;
  wx_rain24h = mm;
  if      (wx_rain24h >= 15.0f) wx_desc = "Lluvia intensa (Meteochile EMA)";
  else if (wx_rain24h >=  5.0f) wx_desc = "Lluvia moderada (Meteochile EMA)";
  else if (wx_rain24h >=  1.0f) wx_desc = "Lluvia leve (Meteochile EMA)";
  else                          wx_desc = "Sin lluvia acum. relevante (Meteochile EMA)";
  Serial.printf("[WX] Meteochile EMA cod=%lu | lluvia (extraida)=%.1fmm\n", (unsigned long)codigo, wx_rain24h);
  return true;
}

/** Fallback si ArduinoJson+Filter deja el array vacío: busca el objeto de la estación en el texto (~70 KB). */
static bool redMeteoScanLluviaForId(const String &p, const char *stationId, float *mm) {
  if (!mm || !stationId || !stationId[0]) return false;
  char head[72];
  snprintf(head, sizeof(head), "\"id_estacion\":\"%s\"", stationId);
  int pos = p.indexOf(head);
  if (pos < 0) return false;
  int li = p.indexOf("\"lluviadiaria\"", pos);
  if (li < 0 || li > pos + 2200) return false;
  int colon = p.indexOf(':', li);
  if (colon < 0) return false;
  int s = colon + 1;
  while (s < (int)p.length() && (p.charAt(s) == ' ' || p.charAt(s) == '\t')) s++;
  if (p.substring(s, s + 4) == "null") {
    *mm = 0.0f;
    return true;
  }
  int e = s;
  while (e < (int)p.length() && e < s + 24) {
    char c = (char)p.charAt(e);
    if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
      e++;
      continue;
    }
    break;
  }
  if (e == s) return false;
  *mm = p.substring(s, e).toFloat();
  return true;
}

// Lluvia observada hoy (mm) desde RedMeteo; ET0/T después con Open-Meteo.
static bool fetchRedMeteoObservation() {
  String payload;
  int      code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) {
      delay(500);
      Serial.printf("[WX] RedMeteo: reintento tras cuerpo vacio (heap=%u)\n", (unsigned)ESP.getFreeHeap());
    }
    HTTPClient http;
    http.begin(String(REDMETEO_LASTDATA_URL));
    httpWeatherConfigure(http);
    code = http.GET();
    if (code != 200) {
      Serial.printf("[WX] RedMeteo HTTP %d\n", code);
      http.end();
      return false;
    }
    if (!httpReadFullBody(http, payload, "RedMeteo")) {
      http.end();
      Serial.printf("[WX] RedMeteo: lectura cuerpo incompleta o vacia (intento %d)\n", attempt + 1);
      continue;
    }
    http.end();
    if (payload.length() >= 4) break;
    Serial.printf("[WX] RedMeteo: cuerpo corto (%u bytes) HTTP=%d heap=%u\n", (unsigned)payload.length(), code,
                  (unsigned)ESP.getFreeHeap());
  }
  if (payload.length() < 4) {
    Serial.println("[WX] RedMeteo: sin datos tras reintento (TLS/memoria o servidor vacio)");
    return false;
  }
  StaticJsonDocument<768> filter;
  filter["*"]["id_estacion"] = true;
  filter["*"]["latitud"]     = true;
  filter["*"]["longitud"]    = true;
  filter["*"]["lluviadiaria"] = true;
  filter["*"]["nombre"]     = true;
  DynamicJsonDocument doc(28672);
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[WX] RedMeteo JSON: %s (len=%u)\n", err.c_str(), (unsigned)payload.length());
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) {
    float mmScan = 0.0f;
    if (payload.length() > 5000 && REDMETEO_STATION_ID[0] != '\0'
        && redMeteoScanLluviaForId(payload, REDMETEO_STATION_ID, &mmScan)) {
      wx_rain24h = mmScan;
      if      (wx_rain24h >= 15.0f) wx_desc = "Lluvia intensa (RedMeteo hoy)";
      else if (wx_rain24h >=  5.0f) wx_desc = "Lluvia moderada (RedMeteo hoy)";
      else if (wx_rain24h >=  1.0f) wx_desc = "Lluvia leve (RedMeteo hoy)";
      else                          wx_desc = "Sin lluvia acum. hoy (RedMeteo)";
      Serial.printf("[WX] RedMeteo %s (scan) | lluvia hoy=%.1fmm\n", REDMETEO_STATION_ID, wx_rain24h);
      return true;
    }
    Serial.printf("[WX] RedMeteo: lista vacia (payload %u bytes)\n", (unsigned)payload.length());
    return false;
  }
  JsonObject st;
  bool picked = false;
  if (REDMETEO_STATION_ID[0] != '\0') {
    for (JsonObject o : arr) {
      const char *id = o["id_estacion"] | "";
      if (strcmp(id, REDMETEO_STATION_ID) == 0) {
        st = o;
        picked = true;
        break;
      }
    }
    if (!picked) {
      Serial.printf("[WX] RedMeteo: no se encontro estacion %s\n", REDMETEO_STATION_ID);
      return false;
    }
  } else {
    float bestKm = 1e9f;
    for (JsonObject o : arr) {
      float la = o["latitud"] | 0.0f;
      float lo = o["longitud"] | 0.0f;
      float d = haversineKm(SITE_LAT, SITE_LON, la, lo);
      if (d < bestKm) {
        bestKm = d;
        st = o;
        picked = true;
      }
    }
    if (!picked) return false;
  }
  float mm = 0.0f;
  if (!st["lluviadiaria"].isNull()) mm = st["lluviadiaria"].as<float>();
  wx_rain24h = mm;
  const char *nom = st["nombre"] | "?";
  const char *sid = st["id_estacion"] | "?";
  if      (wx_rain24h >= 15.0f) wx_desc = "Lluvia intensa (RedMeteo hoy)";
  else if (wx_rain24h >=  5.0f) wx_desc = "Lluvia moderada (RedMeteo hoy)";
  else if (wx_rain24h >=  1.0f) wx_desc = "Lluvia leve (RedMeteo hoy)";
  else                          wx_desc = "Sin lluvia acum. hoy (RedMeteo)";
  Serial.printf("[WX] RedMeteo %s | %s | lluvia hoy=%.1fmm\n", sid, nom, wx_rain24h);
  return true;
}

#if WEATHER_USE_SUPABASE_CACHE
/** Clima desde tabla `weather_cache` (actualizada por Edge Function en Supabase). */
static bool fetchWeatherFromSupabaseCache() {
  if (!WiFi.isConnected()) return false;
  if (!ntpConfigured) {
    Serial.println("[WX] Supabase cache: NTP no listo");
    return false;
  }
  String payload = supabaseGet(SUPABASE_WEATHER_CACHE_SELECT);
  if (payload.length() < 8) return false;
  DynamicJsonDocument doc(1536);
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return false;
  JsonObject row = arr[0];
  uint32_t u = row["updated_unix"].as<uint32_t>();
  if (u == 0) return false;
  uint32_t now32 = (uint32_t)time(nullptr);
  if (now32 < 1700000000UL) return false;
  if (now32 > u + WEATHER_CACHE_MAX_AGE_SEC) {
    Serial.printf("[WX] Supabase cache caducada (>%lu s)\n", (unsigned long)WEATHER_CACHE_MAX_AGE_SEC);
    return false;
  }
  wx_rain24h = row["rain_mm"] | 0.0f;
  wx_et0     = row["et0_mm"] | 0.0f;
  wx_tMax    = row["t_max"] | 25.0f;
  wx_tMin    = row["t_min"] | 15.0f;
  if (row["description"].is<const char*>()) wx_desc = String(row["description"].as<const char*>());
  else wx_desc = "Cache Supabase";
  Serial.printf("[WX] Supabase cache | lluvia=%.1fmm ET0=%.2f T=%.0f/%.0fC | %s\n",
                wx_rain24h, wx_et0, wx_tMax, wx_tMin, wx_desc.c_str());
  return true;
}
#endif

void fetchWeather() {
  if (!WiFi.isConnected()) return;

  bool ok = false;

#if WEATHER_USE_SUPABASE_CACHE
  if (fetchWeatherFromSupabaseCache()) ok = true;
  else
    Serial.println("[WX] Sin cache Supabase util; APIs directas (Meteochile/RedMeteo/Open-Meteo)");
#endif

  String mcUser, mcTok;
  meteochileCredentials(mcUser, mcTok);
  uint32_t mcCod = meteochileCodigoResolved();
  if (!ok && mcUser.length() > 0 && mcTok.length() > 0 && mcCod > 0) {
    if (fetchMeteochileDatosRecientesEma(mcUser, mcTok, mcCod)) {
      float obs = wx_rain24h;
      (void)fetchOpenMeteoDailyEt0Temps();
      mergeStationRainWithOpenMeteoHourly(obs);
      ok = true;
      Serial.printf("[WX] Mix | lluvia decision=%.1fmm (EMA + prono %uh) | ET0=%.2fmm/d T=%.0f/%.0fC | %s\n",
                    wx_rain24h, (unsigned)WEATHER_RAIN_FORECAST_HOURS, wx_et0, wx_tMax, wx_tMin, wx_desc.c_str());
    } else {
      Serial.println("[WX] Meteochile fallo; se intenta RedMeteo u Open-Meteo");
    }
  }

  if (!ok) {
    // Meteochile deja buffers grandes; pausa breve mejora hueco contiguo para ~70KB de RedMeteo.
    if (mcUser.length() > 0 && mcTok.length() > 0 && mcCod > 0) delay(300);
#if WEATHER_USE_REDMETEO
    if (fetchRedMeteoObservation()) {
      float obs = wx_rain24h;
      (void)fetchOpenMeteoDailyEt0Temps();  // ET₀/T opcional si falla HTTP
      mergeStationRainWithOpenMeteoHourly(obs);
      ok = true;
      Serial.printf("[WX] Mix | lluvia decision=%.1fmm (RedMeteo + prono %uh) | ET0=%.2fmm/d T=%.0f/%.0fC (Open-Meteo) | %s\n",
                    wx_rain24h, (unsigned)WEATHER_RAIN_FORECAST_HOURS, wx_et0, wx_tMax, wx_tMin, wx_desc.c_str());
    } else {
      Serial.println("[WX] RedMeteo fallo; fallback Open-Meteo completo");
      ok = fetchWeatherOpenMeteoOnly();
    }
#else
    ok = fetchWeatherOpenMeteoOnly();
#endif
  }

  if (ok) {
    wx_fetched     = true;
    wx_lastFetchAt = millis();
  } else {
    Serial.println("[WX] Sin datos validos (reintentara)");
    // Si ya habia clima y falla un refresco 6h, no reintentar cada loop
    if (wx_fetched) wx_lastFetchAt = millis();
  }
}

// --------------------------- Programacion ---------------------------
//
// Logica de ejecucion (ventana T±SCHED_EVAL_WINDOW_MIN min respecto a HH:MM de la zona):
//  - Cada ~1 s se evalua si el reloj esta en [horario-N, horario+N] (mismo dia, modulo 24h).
//  - La decision de humedad se toma con datos *antes y alrededor* del instante programado;
//    +N min despues da margen si el satelite manda tarde.
//  - SIN_LECTURA no consume el disparo diario: reintenta hasta haber humedad fresca o
//    cerrar la ventana (entonces se marca el dia como evaluado sin riego por datos).
//  - HUM_OK, riego efectivo o cancelacion por lluvia si consumen el disparo (no dos veces el mismo evento).
//
static uint32_t lastSchedDebugAt = 0;

void predictiveCheck() {
  if (!ntpConfigured) return;
  if (millis() - lastScheduleTickAt < 1000UL) return;
  lastScheduleTickAt = millis();

  struct tm nowTm;
  if (!getLocalTime(&nowTm, 200)) {
    Serial.println("[SCHED] WARN: getLocalTime fallo (NTP no listo aun)");
    return;
  }

  int nowMin  = nowTm.tm_hour * 60 + nowTm.tm_min;
  int dayStamp = nowTm.tm_year * 400 + nowTm.tm_yday;
  bool printDebug = (millis() - lastSchedDebugAt >= 30000UL);
  if (printDebug) lastSchedDebugAt = millis();

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    PumpSchedule &cfg = schedules[activeProgram][i];

    int sched         = cfg.startHour * 60 + cfg.startMinute;
    int minsFromSched = (nowMin - sched + 1440) % 1440;  // 0 = en punto, 1..N = N mins despues
    int minsToSched   = (sched - nowMin + 1440) % 1440;  // 0 = en punto, 1..N = N mins antes
    const bool inEvalWindow =
        (minsToSched <= SCHED_EVAL_WINDOW_MIN) || (minsFromSched <= SCHED_EVAL_WINDOW_MIN);

    // Zona deshabilitada: apagar si estaba encendida y marcar estado
    if (!cfg.enabled) {
      if (zones[i].on) stopZone(i, "disabled");
      decisions[i].result = "⛔ Zona deshabilitada";
      decisions[i].reason = "DISABLED";
      decisions[i].sinLecturaPending = false;
      decisions[i].wasInEvalWindow   = inEvalWindow;
      continue;
    }

    uint8_t stIdx     = stationIdxFromSchedule(cfg, i);
    uint8_t humNow    = moisturePct[stIdx];
    bool humFresh     = moisturePctValid[stIdx];

    // --- Estado para dashboard ---
    decisions[i].humidityNow = humNow;
    // "SIN_LECTURA" solo se escribe en la ventana de ejecución; si ya hay humedad fresca, no dejar el motivo pegado
    if (humFresh && decisions[i].reason == "SIN_LECTURA") decisions[i].reason = "";

    decisions[i].minsToSched  = (uint16_t)minsToSched;
    snprintf(decisions[i].schedHHMM, sizeof(decisions[i].schedHHMM),
             "%02u:%02u", cfg.startHour, cfg.startMinute);
    {
      int evalMin = (sched + 1440 - SCHED_EVAL_WINDOW_MIN) % 1440;
      int minsToEval = (evalMin - nowMin + 1440) % 1440;
      char hhmm[8];
      snprintf(hhmm, sizeof(hhmm), "%02u:%02u", (uint8_t)(evalMin / 60), (uint8_t)(evalMin % 60));
      decisions[i].nextEvalHHMM = hhmm;
      decisions[i].nextEvalMins = (uint16_t)minsToEval;
    }

    // Salida de ventana de evaluación con reintentos SIN_LECTURA pendientes → consumir disparo del dia
    bool wasInWindow = decisions[i].wasInEvalWindow;
    if (wasInWindow && !inEvalWindow && decisions[i].sinLecturaPending) {
      decisions[i].sinLecturaPending = false;
      decisions[i].shouldWater       = false;
      decisions[i].minutes           = 0;
      decisions[i].reason            = "SIN_LECTURA";
      {
        char _sl[72];
        snprintf(_sl, sizeof(_sl), "Sin lectura fresca al cierre (T-%d..T+%d)",
                 (int)SCHED_EVAL_WINDOW_MIN, (int)SCHED_EVAL_WINDOW_MIN);
        decisions[i].result = _sl;
      }
      decisions[i].lastRunDay        = dayStamp;
      decisions[i].lastRunSched      = sched;
      Serial.printf("[SCHED-RUN] Z%u FIN VENTANA | sin datos frescos, disparo consumido\n", i + 1);
    }
    decisions[i].wasInEvalWindow = inEvalWindow;

    bool alreadyRanToday = (decisions[i].lastRunDay == dayStamp && decisions[i].lastRunSched == sched);

    // --- Log de diagnostico cada 30s ---
    if (printDebug) {
      Serial.printf("[SCHED-DBG] Z%u now=%02d:%02d sched=%02u:%02u minsFrom=%d minsTo=%d win=%s hum=%u thr=%u ran=%s on=%s\n",
                    i + 1, nowTm.tm_hour, nowTm.tm_min,
                    cfg.startHour, cfg.startMinute,
                    minsFromSched, minsToSched,
                    inEvalWindow ? "SI" : "NO",
                    humNow, cfg.threshold,
                    alreadyRanToday ? "SI" : "NO",
                    zones[i].on ? "SI" : "NO");
    }

    // --- EJECUCION: ventana [horario-N, horario+N] ---
    if (inEvalWindow && !alreadyRanToday && !zones[i].on) {
      if (!humFresh) {
        decisions[i].shouldWater = false;
        decisions[i].minutes = 0;
        decisions[i].reason = "SIN_LECTURA";
        {
          char _r[80];
          snprintf(_r, sizeof(_r),
                   "No riega: sin humedad fresca (reintento ventana T-%d..T+%d)",
                   (int)SCHED_EVAL_WINDOW_MIN, (int)SCHED_EVAL_WINDOW_MIN);
          decisions[i].result = _r;
        }
        if (!decisions[i].sinLecturaPending) {
          Serial.printf("[SCHED-RUN] Z%u SIN_LECTURA | estacion %u (reintento hasta fin ventana)\n",
                        i + 1, cfg.stationID);
        }
        decisions[i].sinLecturaPending = true;
      } else if (humNow < cfg.threshold) {

        // ---- Nivel 1: Física real (caudal + área + eficiencia) ----
        float eff       = (cfg.efficiency >= 0.1f && cfg.efficiency <= 1.0f) ? cfg.efficiency : 0.85f;
        float soilMx    = (cfg.soilMaxMm  > 1.0f)  ? cfg.soilMaxMm  : 50.0f;
        float kc        = (cfg.Kc         > 0.0f)  ? cfg.Kc         : 0.8f;
        // Tasa de precipitación del sistema: (L/min × 60) / m² = mm/h
        float precipR   = (cfg.flowRate > 0.0f && cfg.area > 0.0f)
                          ? (cfg.flowRate * 60.0f / cfg.area) : 60.0f;
        // Déficit hídrico del suelo en milímetros
        float defPct    = (float)(cfg.threshold - humNow);
        float mm_needed = (defPct / 100.0f) * soilMx;

        // ---- Nivel 2a: Ajuste por evapotranspiración ETc ----
        char et0Tag[24] = "";
        if (wx_fetched && wx_et0 > 0.0f) {
          float etcDay   = wx_et0 * kc;
          mm_needed      = min(mm_needed + etcDay * 0.5f, soilMx * 0.9f);
          snprintf(et0Tag, sizeof(et0Tag), " ET0=%.1fmm", wx_et0);
        }

        // ---- Nivel 2b: Cancelar si hay lluvia significativa prevista ----
        // rainCancelMm<=0 = no cancelar por lluvia (evita wx_rain24h>=0 siempre cierto)
        if (wx_fetched && cfg.rainCancelMm > 0.0f && wx_rain24h >= cfg.rainCancelMm) {
          decisions[i].sinLecturaPending = false;
          decisions[i].shouldWater  = false;
          decisions[i].minutes      = 0;
          decisions[i].reason       = "LLUVIA_PREVISTA";
          char buf[64];
          snprintf(buf, sizeof(buf), "Cancelado: %.1fmm lluvia prox. %u h", wx_rain24h,
                   (unsigned)WEATHER_RAIN_FORECAST_HOURS);
          decisions[i].result       = buf;
          decisions[i].lastRunDay   = dayStamp;
          decisions[i].lastRunSched = sched;
          Serial.printf("[SCHED-RUN] Z%u CANCELADO | lluvia_pron=%.1fmm (limite=%.1fmm)\n",
                        i + 1, wx_rain24h, cfg.rainCancelMm);
          pushIrrigationLog(i, 0, 0.0f, humNow, humNow, 0, "LLUVIA_PREVISTA", true);
        } else {
          // ---- Nivel 2c: Reducir mm por lluvia parcial esperada ----
          if (wx_fetched && wx_rain24h > 0.0f)
            mm_needed = max(0.0f, mm_needed - wx_rain24h * 0.8f);

          // Tiempo de riego = mm / (mm/h) × 60 / eficiencia  →  minutos
          float mins_f = (precipR > 0.0f && mm_needed > 0.0f)
                         ? ceilf((mm_needed / precipR) * 60.0f / eff)
                         : 1.0f;
          uint8_t mins = (uint8_t)constrain((int)mins_f, 1, (int)cfg.maxDecisionMin);

          decisions[i].sinLecturaPending = false;
          startZone(i, (uint32_t)mins * 60000UL, ZSRC_AUTO);
          decisions[i].shouldWater    = true;
          decisions[i].minutes        = mins;
          decisions[i].reason         = "HUM_BAJA";
          decisions[i].result         = "Regando";
          decisions[i].irrigationTs   = millis();
          decisions[i].auditPending   = true;
          decisions[i].lastRunDay     = dayStamp;
          decisions[i].lastRunSched   = sched;
          decisions[i].humAtStart     = humNow;
          decisions[i].auditMins      = mins;
          Serial.printf("[SCHED-RUN] Z%u ENCENDIDO %umin | hum=%u thr=%u mm=%.1f precipR=%.0fmm/h eff=%.2f%s rain=%.1f\n",
                        i + 1, mins, humNow, cfg.threshold,
                        mm_needed, precipR, eff, et0Tag, wx_rain24h);
        }

      } else {
        decisions[i].sinLecturaPending = false;
        decisions[i].shouldWater  = false;
        decisions[i].minutes      = 0;
        decisions[i].reason       = "HUM_OK";
        decisions[i].result       = "No riega (hum=" + String(humNow) + "% >= thr=" + String(cfg.threshold) + "%)";
        decisions[i].lastRunDay   = dayStamp;
        decisions[i].lastRunSched = sched;
        Serial.printf("[SCHED-RUN] Z%u NO RIEGA | hum=%u >= thr=%u\n", i + 1, humNow, cfg.threshold);
      }
    }

    // --- Actualizar estado mientras espera auditoría (bomba ya apagada) ---
    // Sin este bloque, result quedaba en "Regando" los 45min hasta la auditoría.
    if (decisions[i].auditPending && !zones[i].on &&
        millis() - decisions[i].irrigationTs < AUDIT_DELAY_MS) {
      uint32_t minsPassed = (millis() - decisions[i].irrigationTs) / 60000UL;
      uint32_t auditDelayMin = AUDIT_DELAY_MS / 60000UL;
      uint32_t minsLeft = (auditDelayMin > minsPassed) ? (auditDelayMin - minsPassed) : 0;
      char buf[56];
      snprintf(buf, sizeof(buf), "Riego completado · audit en %umin", (unsigned)minsLeft);
      decisions[i].result = buf;
    }

    // --- Estado de display (solo cuando no hay riego activo ni auditoria) ---
    if (!alreadyRanToday && !zones[i].on && !decisions[i].auditPending) {
      decisions[i].humidityNow = humNow;
      char schedStr[8];
      snprintf(schedStr, sizeof(schedStr), "%02u:%02u", cfg.startHour, cfg.startMinute);

      // Nota clima
      String wxNote = "";
      if (wx_fetched) {
        if (cfg.rainCancelMm > 0.0f && wx_rain24h >= cfg.rainCancelMm)
          wxNote = " ⛔" + String(wx_rain24h, 1) + "mm";
        else if (wx_rain24h > 0.5f)
          wxNote = " 🌧" + String(wx_rain24h, 1) + "mm";
        if (wx_et0 > 0.0f)
          wxNote += " ET₀=" + String(wx_et0, 1);
      }

      if (inEvalWindow) {
        decisions[i].result = String("Evaluando umbral...") + wxNote;
      } else if (minsToSched <= 60) {
        decisions[i].result = "Riego en " + String(minsToSched) + "m (" + String(schedStr) + ")" + wxNote;
      } else {
        decisions[i].result = String("Prog. ") + schedStr + wxNote;
      }
    }

    // --- Auditoria post-riego (+45 min) ---
    if (decisions[i].auditPending && millis() - decisions[i].irrigationTs >= AUDIT_DELAY_MS) {
      uint8_t humAfter  = moisturePct[stIdx];
      int8_t  deltaHum  = (int8_t)humAfter - (int8_t)decisions[i].humAtStart;
      float liters = cfg.flowRate * decisions[i].auditMins / 60.0f;
      decisions[i].auditLiters   = liters;
      decisions[i].auditDeltaHum = deltaHum;
      char buf[80];
      snprintf(buf, sizeof(buf), "Audit+45m: %.1fL | hum %u%%→%u%% (%+d%%)",
               liters, decisions[i].humAtStart, humAfter, (int)deltaHum);
      decisions[i].auditResult  = buf;
      decisions[i].result       = buf;
      decisions[i].auditPending = false;
      Serial.printf("[AUDIT] Z%u %s\n", i + 1, buf);
      // Registrar en historial de Supabase
      pushIrrigationLog(i, decisions[i].auditMins, liters,
                        decisions[i].humAtStart, humAfter, deltaHum,
                        "HUM_BAJA", false);
    }
  }
}

void enforceSafety() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (!zones[i].on) continue;
    uint32_t elapsed = now - zones[i].startedAt;
    if (elapsed >= MAX_WATERING_MS || (zones[i].requestedDurationMs > 0 && elapsed >= zones[i].requestedDurationMs)) stopZone(i, "safety");
  }
}

// --------------------------- Boot/network ---------------------------
void setupPins() { for (uint8_t i = 0; i < ZONE_COUNT; i++) { pinMode(ZONE_PINS[i], INPUT); zones[i] = {false, 0, 0, 0}; } }
void beginWiFi() {
  WiFi.mode(WIFI_AP_STA); WiFi.setSleep(true); WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.print("MAC STA Central: "); Serial.println(WiFi.macAddress());
  if (!wifiBeginDone) { WiFi.begin(WIFI_SSID, WIFI_PASS); wifiBeginDone = true; lastWiFiAttemptAt = millis(); Serial.println("WiFi begin solicitado."); }
}
void maintainConnectivity() {
  uint32_t now = millis(); wl_status_t st = WiFi.status();
  if (st != WL_CONNECTED && now - lastWiFiAttemptAt >= WIFI_RETRY_INTERVAL_MS) { WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); lastWiFiAttemptAt = now; }
  // ESP-NOW en cuanto hay STA (no esperar STARTUP_STAGGER): el satélite envía al arranque y luego deep sleep 30 min.
  if (st == WL_CONNECTED && !espNowReady) setupEspNow();
  if (st == WL_CONNECTED && !ntpConfigured) {
    int32_t savedOff = loadPrefsUtcOffsetSec();
    if (savedOff != INT32_MAX && savedOff >= -46800 && savedOff <= 46800) {
      applyBrowserUtcOffsetSeconds((long)savedOff, false);
    } else {
      // Sin offset guardado (primera vez): Chile por compatibilidad hasta el primer pull / dashboard.
      configTzTime("CLT4CLST3,M9.1.6/24,M4.1.6/24",
                   "pool.ntp.org", "time.nist.gov", "time.cloudflare.com");
      appliedBrowserUtcOffsetSec = kBrowserUtcOffsetUnset;
    }
    ntpConfigured = true;
    Serial.print("WiFi conectado. IP local: ");
    Serial.println(WiFi.localIP());
    // Verificar que NTP y timezone queden correctos (espera hasta 5s).
    struct tm verifyTm;
    if (getLocalTime(&verifyTm, 5000)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &verifyTm);
      Serial.printf("[NTP] Hora local sincronizada: %s\n", buf);
    } else {
      Serial.println("[NTP] WARN: No se pudo verificar hora inmediatamente (esperando sync)");
    }
  }
}

void printResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *desc = "DESCONOCIDO";
  switch (r) {
    case ESP_RST_POWERON:   desc = "ENCENDIDO_NORMAL";  break;
    case ESP_RST_SW:        desc = "RESET_SOFTWARE";    break;
    case ESP_RST_PANIC:     desc = "PANIC_FIRMWARE";    break;
    case ESP_RST_INT_WDT:   desc = "WDT_INTERRUPCION";  break;
    case ESP_RST_TASK_WDT:  desc = "WDT_TAREA";         break;
    case ESP_RST_WDT:       desc = "WDT_OTRO";          break;
    case ESP_RST_DEEPSLEEP: desc = "DEEP_SLEEP";        break;
    case ESP_RST_BROWNOUT:  desc = "BROWNOUT";          break;
    case ESP_RST_SDIO:      desc = "SDIO";              break;
    default: break;
  }
  Serial.printf("[BOOT] Motivo reinicio: %s (%d)\n", desc, (int)r);
}

void setup() {
  if (USE_BROWNOUT_WORKAROUND) WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200); delay(250); setCpuFrequencyMhz(80); bootAt = millis();
  printResetReason();
  // Watchdog: reinicia automáticamente si loop() se bloquea más de WDT_TIMEOUT_S
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  setupPins(); initDefaults(); loadSchedules(); loadCalibration(); beginWiFi(); delay(120); startWebServer();
#if ENABLE_OLED
  oledSetup();
#endif
  Serial.printf("[BOOT] Heap libre: %u bytes\n", esp_get_free_heap_size());
  Serial.println("Central lista.");
}

// -----------------------------------------------------------------------
// Loop cloud scheduler — diseño por capas de prioridad:
//
//  CAPA 0 (antes del resto): fetchWeather() si falta clima o cada 6h — si iba al final, cmd/push/sched impedían wx_fetched
//  CAPA 1 (cada 10s): processCloudCommands()  ← urgente, usuario espera respuesta
//  CAPA 2 (cada 30s): pushStatusToCloud()     ← o inmediato si statusPushUrgent
//  CAPA 3 (cada 5min): pullScheduleFromCloud() ← no urgente
//
// Regla: solo UNA operación HTTP por iteración de loop() para no bloquear el WDT.
// -----------------------------------------------------------------------
static uint32_t lastCmdPollAt    = 0;
static uint32_t lastSchedPullAt  = 0;
static uint32_t wifiDownSince    = 0;
static uint32_t lastHeapLogAt    = 0;
static uint32_t lastMoistRecalcAt = 0;

void loop() {
  esp_task_wdt_reset();  // alimentar watchdog al inicio de cada ciclo

  uint32_t nowLoop = millis();
  if (nowLoop - lastMoistRecalcAt >= 5000UL) {
    lastMoistRecalcAt = nowLoop;
    recalcAllStationMoisture();
  }

  server.handleClient();
  maintainConnectivity();
  enforceSafety();
  predictiveCheck();

  if (!pumpPinsArmed && millis() - bootAt > STARTUP_STAGGER_MS) armPumpPinsNow();

  uint32_t now = millis();
#if ENABLE_OLED
  oledTick(now);
#endif

  // --- Log periódico de heap libre ---
  if (now - lastHeapLogAt >= 60000UL) {
    lastHeapLogAt = now;
    Serial.printf("[SYS] Heap libre: %u bytes | WiFi: %s\n",
                  esp_get_free_heap_size(),
                  WiFi.isConnected() ? "OK" : "DESCONECTADO");
  }

  // --- WiFi hard-reset si lleva más de 3 min desconectado ---
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) wifiDownSince = now;
    if (now - wifiDownSince > 3UL * 60UL * 1000UL) {
      Serial.println("[WIFI] Desconectado >3min, forzando reinicio WiFi...");
      WiFi.disconnect(true); delay(500); WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifiDownSince = now;
    }
    return;
  }
  wifiDownSince = 0;

  if (now - bootAt < CLOUD_START_DELAY_MS) return;

  // --- Print periódico de horarios ---
  if (now - lastSchedulePrintAt >= 60000UL) {
    lastSchedulePrintAt = now;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
      PumpSchedule &cfg = schedules[activeProgram][i];
      Serial.printf("[SCHED-MIN] Z%u %02u:%02u thr=%u station=%u\n",
                    i + 1, cfg.startHour, cfg.startMinute, cfg.threshold, cfg.stationID);
    }
  }

  // CAPA 0 — Clima (antes de cmd/push/sched): si no, fetchWeather nunca se alcanzaba y wx_fetched quedaba false ("Sin datos").
  {
    bool needFirst = !wx_fetched && (now - bootAt > CLOUD_START_DELAY_MS + 15000UL);
    bool needRefresh = wx_fetched && (now - wx_lastFetchAt >= WEATHER_INTERVAL_MS);
    if (needFirst) {
      if (lastWxAttemptAt == 0 || now - lastWxAttemptAt >= WX_RETRY_MS) {
        lastWxAttemptAt = now;
        fetchWeather();
        return;
      }
    } else if (needRefresh) {
      fetchWeather();
      return;
    }
  }

  // CAPA 1 — Comandos remotos: cada 10s (usuario espera respuesta rápida)
  if (now - lastCmdPollAt >= 10000UL) {
    lastCmdPollAt = now;
    processCloudCommands();
    return;  // una sola HTTP call por iteración
  }

  // CAPA 2 — Status push: urgente (bomba apagada) o cada 30s
  if (statusPushUrgent) {
    statusPushUrgent = false;
    lastStatusPushAt = now;
    pushStatusToCloud();
    return;
  }
  if (now - lastStatusPushAt >= 30000UL) {
    lastStatusPushAt = now;
    pushStatusToCloud();
    return;
  }

  // CAPA 3 — Sincronizar programación: primer pull en cuanto pasa el arranque cloud,
  // luego cada 5 min (antes: solo tras 5 min de uptime → horarios de Supabase ignorados al inicio)
  {
    bool schedDue = (lastSchedPullAt == 0)
                    || (now - lastSchedPullAt >= 5UL * 60UL * 1000UL);
    if (schedDue) {
      lastSchedPullAt = now;
      pullScheduleFromCloud();
      return;
    }
  }
}
