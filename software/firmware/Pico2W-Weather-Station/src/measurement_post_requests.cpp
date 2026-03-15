#define MEASUREMENT_POST_REQUESTS
#ifdef MEASUREMENT_POST_REQUESTS

// ===================== User options =====================
// Measurement cadence (time between posts)
#define MEASUREMENT_INTERVAL_MS 10000

// Low power between measurements:
// 0 = keep WiFi on, simple delay
// 1 = turn WiFi off between measurements (saves a lot of power)
#define USE_LOW_POWER_BETWEEN_MEASUREMENTS 1
// ========================================================

#include <Arduino.h>
#include <Wire.h>

#include <Adafruit_BME680.h>
#include <MCP342x.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_SGP41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

#include <WiFi.h>
#include <HTTPClient.h>

#include "wificredentials.h"
#include "client_config.h"

// -------------------- Sensors --------------------
static const uint8_t BME_ADDR = 0x76;
Adafruit_BME680 bme;

static const uint8_t MCP3426_ADDR = 0x68;
MCP342x adc(MCP3426_ADDR);

static const MCP342x::Resolution ADC_RES = MCP342x::resolution16;
static const MCP342x::Gain       ADC_GAIN = MCP342x::gain1;

// Divider ratios: V_in = V_adc * (Rtop + Rbottom) / Rbottom
static const float VBAT_RTOP_OHMS   = 30000.0f;
static const float VBAT_RBOT_OHMS   = 20000.0f;
static const float VSOLAR_RTOP_OHMS = 110000.0f;
static const float VSOLAR_RBOT_OHMS = 10000.0f;

static const float VBAT_DIVIDER   = (VBAT_RTOP_OHMS + VBAT_RBOT_OHMS) / VBAT_RBOT_OHMS;       // 2.5
static const float VSOLAR_DIVIDER = (VSOLAR_RTOP_OHMS + VSOLAR_RBOT_OHMS) / VSOLAR_RBOT_OHMS; // 12.0

Adafruit_VEML7700 veml;

Adafruit_SGP41 sgp;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;

// Track which sensors are present; missing ones should not block loop()
static bool g_bme_ok  = false;
static bool g_mcp_ok  = false;
static bool g_veml_ok = false;
static bool g_sgp_ok  = false;

// -------------------- Sample --------------------
struct Sample {
  uint32_t ms = 0;

  float tempC = NAN;
  float humPct = NAN;
  float pressHpa = NAN;
  uint32_t gasOhms = 0;
  float altM = NAN;

  float vbatV = NAN;
  float vsolarV = NAN;

  float alsLux = NAN;
  uint16_t white = 0;

  uint16_t srawVoc = 0;
  uint16_t srawNox = 0;
  int32_t vocIndex = -1;
  int32_t noxIndex = -1;
  bool sgpOk = false;
};

static uint32_t g_lastPostMs = 0;

// -------------------- MCP helpers --------------------
static float mcpVoltsPerCode(const MCP342x::Resolution &res, const MCP342x::Gain &gain) {
  const int r = (int)res;
  const int g = (int)gain;
  const float fs = 2.048f / (float)g;
  const float counts = (float)(1UL << (r - 1));
  return fs / counts;
}

static float mcpCodeToInputVolts(long code, const MCP342x::Resolution &res, const MCP342x::Gain &gain) {
  return (float)code * mcpVoltsPerCode(res, gain);
}

static bool readMcpRaw(MCP342x::Channel ch, long &valueOut) {
  MCP342x::Config status;
  const uint8_t err = adc.convertAndRead(ch, MCP342x::oneShot, ADC_RES, ADC_GAIN, 1000000, valueOut, status);
  return err == 0;
}

// -------------------- HMAC-SHA256 (BearSSL; works on rpipico2w / Arduino-Pico) --------------------
#include <bearssl/bearssl.h>

static String hmacSha256Hex(const char *key, const uint8_t *msg, size_t msgLen) {
  unsigned char mac[br_sha256_SIZE];

  br_hmac_key_context kc;
  br_hmac_context ctx;

  br_hmac_key_init(&kc, &br_sha256_vtable, key, strlen(key));
  br_hmac_init(&ctx, &kc, 0);
  br_hmac_update(&ctx, msg, msgLen);
  br_hmac_out(&ctx, mac);

  static const char *hex = "0123456789abcdef";
  String s;
  s.reserve(64);
  for (size_t i = 0; i < sizeof(mac); i++) {
    s += hex[(mac[i] >> 4) & 0xF];
    s += hex[mac[i] & 0xF];
  }
  return s;
}

// -------------------- WiFi (use your original/known-working static-IP arg order) --------------------
static void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(250);

#if USE_STATIC_IP
  // IMPORTANT: this is the SAME order as your "works on wifi" code
  WiFi.config(WIFI_LOCAL_IP, WIFI_DNS_IP, WIFI_GATEWAY_IP, WIFI_SUBNET_IP);
  Serial.print(F("# wifi_static_ip "));
  Serial.println(WIFI_LOCAL_IP);
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print(F("# wifi_connecting ssid="));
  Serial.println(WIFI_SSID);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
    if (millis() - start > 20000) break;
  }
  Serial.println();

  Serial.print(F("# wifi_status="));
  Serial.println((int)WiFi.status());

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("# wifi_ok ip=")); Serial.println(WiFi.localIP());
    Serial.print(F("# rssi="));       Serial.println(WiFi.RSSI());
  } else {
    Serial.println(F("# wifi_failed"));
  }
}

static void wifiOff() {
#if USE_LOW_POWER_BETWEEN_MEASUREMENTS
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_OFF);
  delay(50);
#endif
}

// -------------------- Sensor power behaviors --------------------
static void vemlShutdown(bool enable) {
  if (!g_veml_ok) return;
  // Adafruit_VEML7700 provides enable/disable via power-save/shutdown in most versions.
  // `enable(false)` puts it into shutdown (very low current).
  veml.enable(!enable ? true : false); // placeholder safety if library differs
}

// More robust: use the API that exists in Adafruit_VEML7700 v2.x
static void vemlSetEnabled(bool on) {
  if (!g_veml_ok) return;
  // In Adafruit VEML7700 Library 2.1.x there is: veml.enable(bool)
  veml.enable(on);
}

// BME forced-mode only:
// Adafruit_BME680 is already forced-mode when you call performReading().
// The key is: do NOT use continuous mode loops; just call performReading() once per cycle.

static void sensorsPreSleep() {
  // Put VEML into shutdown between measurements
  vemlSetEnabled(false);
}

static void sensorsPostWake() {
  // Re-enable VEML
  vemlSetEnabled(true);
  delay(5);
}

static void waitBetweenMeasurements() {
#if USE_LOW_POWER_BETWEEN_MEASUREMENTS
  sensorsPreSleep();
  wifiOff();
#endif

  Serial.print(F("# sleep_ms="));
  Serial.println((uint32_t)MEASUREMENT_INTERVAL_MS);
  Serial.flush();

  delay(MEASUREMENT_INTERVAL_MS);

#if USE_LOW_POWER_BETWEEN_MEASUREMENTS
  // WiFi is re-enabled in loop() before reconnect
  sensorsPostWake();
#endif
}

// -------------------- Sampling --------------------
static void sgpConditioning10sNonBlocking() {
  if (!g_bme_ok || !g_sgp_ok) return;

  Serial.println(F("# sgp41 conditioning (10s)"));
  for (uint8_t i = 0; i < 10; i++) {
    uint16_t sraw_voc = 0;
    if (bme.performReading()) {
      (void)sgp.executeConditioning(&sraw_voc, bme.humidity, bme.temperature);
      (void)vocAlgorithm.process(sraw_voc);
    }
    delay(1000);
  }
  Serial.println(F("# sgp41 conditioning done"));
}

static Sample readSample() {
  Sample s;
  s.ms = millis();

  // BME forced-mode: performReading() triggers a single measurement.
  if (g_bme_ok && bme.performReading()) {
    s.tempC = bme.temperature;
    s.humPct = bme.humidity;
    s.pressHpa = bme.pressure / 100.0f;
    s.gasOhms = bme.gas_resistance;
    s.altM = bme.readAltitude(1013.25f);
  }

  // MCP one-shot conversions only when needed
  long vbatRaw = 0, vsolarRaw = 0;
  const bool ok1 = g_mcp_ok && readMcpRaw(MCP342x::channel1, vbatRaw);
  const bool ok2 = g_mcp_ok && readMcpRaw(MCP342x::channel2, vsolarRaw);

  const float vbat_adc   = ok1 ? mcpCodeToInputVolts(vbatRaw, ADC_RES, ADC_GAIN) : NAN;
  const float vsolar_adc = ok2 ? mcpCodeToInputVolts(vsolarRaw, ADC_RES, ADC_GAIN) : NAN;
  s.vbatV   = isnan(vbat_adc)   ? NAN : vbat_adc * VBAT_DIVIDER;
  s.vsolarV = isnan(vsolar_adc) ? NAN : vsolar_adc * VSOLAR_DIVIDER;

  // VEML (only read if enabled/present)
  if (g_veml_ok) {
    s.alsLux = veml.readLux();
    s.white = veml.readWhite();
  }

  // SGP (needs temp/RH)
  if (g_sgp_ok && g_bme_ok && !isnan(s.tempC) && !isnan(s.humPct)) {
    s.sgpOk = sgp.measureRawSignals(&s.srawVoc, &s.srawNox, s.humPct, s.tempC);
    if (s.sgpOk) {
      s.vocIndex = vocAlgorithm.process(s.srawVoc);
      s.noxIndex = noxAlgorithm.process(s.srawNox);
    }
  } else {
    s.sgpOk = false;
    s.srawVoc = 0;
    s.srawNox = 0;
    s.vocIndex = -1;
    s.noxIndex = -1;
  }

  return s;
}

static String buildJson(const Sample &s) {
  String body;
  body.reserve(700);
  body += "{";
  body += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  body += "\"ms\":" + String(s.ms) + ",";
  body += "\"temp_c\":" + String(s.tempC, 2) + ",";
  body += "\"hum_pct\":" + String(s.humPct, 2) + ",";
  body += "\"press_hpa\":" + String(s.pressHpa, 2) + ",";
  body += "\"gas_ohms\":" + String(s.gasOhms) + ",";
  body += "\"alt_m\":" + String(s.altM, 2) + ",";
  body += "\"vbat_v\":" + String(s.vbatV, 6) + ",";
  body += "\"vsolar_v\":" + String(s.vsolarV, 6) + ",";
  body += "\"als_lux\":" + String(s.alsLux, 2) + ",";
  body += "\"white\":" + String(s.white) + ",";
  body += "\"sgp41_sraw_voc\":" + String(s.srawVoc) + ",";
  body += "\"sgp41_sraw_nox\":" + String(s.srawNox) + ",";
  body += "\"sgp41_voc_index\":" + String(s.vocIndex) + ",";
  body += "\"sgp41_nox_index\":" + String(s.noxIndex) + ",";
  body += "\"sgp41_ok\":" + String(s.sgpOk ? "true" : "false");
  body += "}";
  return body;
}

static bool postJsonHmac(const String &jsonBody) {
  if (WiFi.status() != WL_CONNECTED) return false;

  const uint32_t ts = 0; // set REQUIRE_TIMESTAMP=False server-side unless you add NTP
  const String sig = hmacSha256Hex(AUTH_TOKEN, (const uint8_t*)jsonBody.c_str(), jsonBody.length());

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(SERVER_URL)) {
    Serial.println(F("# http_begin_failed"));
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  http.addHeader("X-Timestamp", String(ts));
  http.addHeader("X-Signature", sig);

  const int code = http.POST((uint8_t*)jsonBody.c_str(), jsonBody.length());

  Serial.print(F("# http_post code="));
  Serial.print(code);

  if (code < 0) {
    Serial.print(F(" err="));
    Serial.println(http.errorToString(code));
    http.end();
    return false;
  }

  const String resp = http.getString();
  Serial.print(F(" resp="));
  Serial.println(resp);

  http.end();
  return (code >= 200 && code < 300);
}

static void printPresence() {
  Serial.print(F("# present bme="));  Serial.print(g_bme_ok ? "1" : "0");
  Serial.print(F(" mcp="));           Serial.print(g_mcp_ok ? "1" : "0");
  Serial.print(F(" veml="));          Serial.print(g_veml_ok ? "1" : "0");
  Serial.print(F(" sgp="));           Serial.println(g_sgp_ok ? "1" : "0");
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("# boot"));

  Wire.begin();

  // Detect sensors; do not block if missing
  g_bme_ok = bme.begin(BME_ADDR);
  if (!g_bme_ok) Serial.println(F("# warn: bme missing"));
  else {
    // Forced-mode only: performReading() triggers one measurement when called.
    // Just configure oversampling + heater and DON'T run any continuous loop.
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
  }

  MCP342x::generalCallReset();
  delay(1);
  Wire.requestFrom(MCP3426_ADDR, (uint8_t)1);
  g_mcp_ok = Wire.available() > 0;
  if (!g_mcp_ok) Serial.println(F("# warn: mcp3426 missing"));

  g_veml_ok = veml.begin();
  if (!g_veml_ok) {
    Serial.println(F("# warn: veml7700 missing"));
  } else {
    veml.setIntegrationTime(VEML7700_IT_100MS);
    veml.setGain(VEML7700_GAIN_1);
    vemlSetEnabled(true);
  }

  g_sgp_ok = sgp.begin();
  if (!g_sgp_ok) Serial.println(F("# warn: sgp41 missing"));
  else {
    sgpConditioning10sNonBlocking();
  }

  printPresence();

  wifiConnect();

  Serial.print(F("# server_url="));
  Serial.println(SERVER_URL);
  Serial.print(F("# post_interval_ms="));
  Serial.println(POST_INTERVAL_MS);
}

void loop() {
#if USE_LOW_POWER_BETWEEN_MEASUREMENTS
  // If WiFi was turned off between cycles, turn it back on before reconnect.
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }
#endif

  if (WiFi.status() != WL_CONNECTED) {
    wifiConnect();
    delay(250);
    return;
  }

  const uint32_t now = millis();
  if (now - g_lastPostMs < POST_INTERVAL_MS) {
    delay(25);
    return;
  }
  g_lastPostMs = now;

  const Sample s = readSample();
  const String body = buildJson(s);
  (void)postJsonHmac(body);

  waitBetweenMeasurements();
}

#endif