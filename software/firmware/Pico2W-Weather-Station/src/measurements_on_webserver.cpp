//#define MEASUREMENTS_ON_WEBSERVER
#ifdef MEASUREMENTS_ON_WEBSERVER

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <MCP342x.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_SGP41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

#include <WiFi.h>
#include <WebServer.h>

#include "wificredentials.h"

// -------------------- WiFi / HTTP --------------------
WebServer server(80);

// -------------------- BME688 (Adafruit_BME680) --------------------
static const uint8_t BME_ADDR = 0x76;
Adafruit_BME680 bme;

// -------------------- MCP3426A0T (Steve Marple MCP342x) -------------------
static const uint8_t MCP3426_ADDR = 0x68;
MCP342x adc(MCP3426_ADDR);

static const MCP342x::Resolution ADC_RES = MCP342x::resolution16;
static const MCP342x::Gain       ADC_GAIN = MCP342x::gain1;

// Divider ratios: V_in = V_adc * (Rtop + Rbottom) / Rbottom
static const float VBAT_RTOP_OHMS   = 30000.0f;
static const float VBAT_RBOT_OHMS   = 20000.0f;
static const float VSOLAR_RTOP_OHMS = 110000.0f;
static const float VSOLAR_RBOT_OHMS = 10000.0f;

static const float VBAT_DIVIDER   = (VBAT_RTOP_OHMS + VBAT_RBOT_OHMS) / VBAT_RBOT_OHMS;        // 2.5
static const float VSOLAR_DIVIDER = (VSOLAR_RTOP_OHMS + VSOLAR_RBOT_OHMS) / VSOLAR_RBOT_OHMS;  // 12.0

// -------------------- VEML7700 --------------------
Adafruit_VEML7700 veml;

// -------------------- SGP41 + indices --------------------
Adafruit_SGP41 sgp;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;

// --------- Latest sample for HTTP ----------
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
static Sample g_latest;

static float mcpVoltsPerCode(const MCP342x::Resolution &res, const MCP342x::Gain &gain) {
  const int r = (int)res;   // 12, 14, 16, 18
  const int g = (int)gain;  // 1, 2, 4, 8
  const float fs = 2.048f / (float)g;
  const float counts = (float)(1UL << (r - 1));
  return fs / counts;
}

static float mcpCodeToInputVolts(long code, const MCP342x::Resolution &res, const MCP342x::Gain &gain) {
  return (float)code * mcpVoltsPerCode(res, gain);
}

static bool readMcpRaw(MCP342x::Channel ch, long &valueOut) {
  MCP342x::Config status;
  const uint8_t err = adc.convertAndRead(
      ch, MCP342x::oneShot, ADC_RES, ADC_GAIN,
      1000000, valueOut, status);
  return err == 0;
}

static void printCsvHeader() {
  Serial.println(F("ms,temp_c,hum_pct,press_hpa,gas_ohms,alt_m,vbat_v,vsolar_v,als_lux,white,sgp41_sraw_voc,sgp41_sraw_nox,sgp41_voc_index,sgp41_nox_index"));
}

static void wifiConnect() {
  WiFi.mode(WIFI_STA);

#if USE_STATIC_IP
  // Arduino-Pico WiFiClass supports up to 4 args: (local_ip, dns_server, gateway, subnet)
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

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("# wifi_ok ip="));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("# wifi_failed"));
  }
}

static void handleRoot() {
  server.send(200, "application/json", "{\"ok\":true,\"endpoints\":[\"/metrics\",\"/health\"]}");
}

static void handleHealth() {
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleMetrics() {
  const Sample s = g_latest;

  String body;
  body.reserve(700);
  body += "{";
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

  server.send(200, "application/json", body);
}

static void webserverBegin() {
  server.on("/", handleRoot);
  server.on("/health", handleHealth);
  server.on("/metrics", handleMetrics);
  server.begin();
  Serial.println(F("# http_server_started port=80"));
}

static void sgpConditioning10s() {
  Serial.println(F("# note: voc index needs ~60s, nox index ~300s learning"));
  Serial.println(F("# sgp41 conditioning (10s, compensated with BME688)"));

  for (uint8_t i = 0; i < 10; i++) {
    uint16_t sraw_voc = 0;

    if (!bme.performReading()) {
      Serial.println(F("# error: bme.performReading() failed during conditioning"));
    } else {
      const float tC = bme.temperature;
      const float rh = bme.humidity;

      if (!sgp.executeConditioning(&sraw_voc, rh, tC)) {
        Serial.println(F("# error: sgp41 conditioning failed"));
      } else {
        (void)vocAlgorithm.process(sraw_voc);
      }
    }
    delay(1000);
  }
  Serial.println(F("# sgp41 conditioning done"));
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("# boot"));

  Wire.begin();

  if (!bme.begin(BME_ADDR)) {
    Serial.println(F("# error: bme missing"));
    while (true) delay(1000);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  MCP342x::generalCallReset();
  delay(1);
  Wire.requestFrom(MCP3426_ADDR, (uint8_t)1);
  if (!Wire.available()) {
    Serial.println(F("# error: mcp3426 missing"));
    while (true) delay(1000);
  }

  if (!veml.begin()) {
    Serial.println(F("# error: veml7700 missing"));
    while (true) delay(1000);
  }
  veml.setIntegrationTime(VEML7700_IT_100MS);
  veml.setGain(VEML7700_GAIN_1);

  if (!sgp.begin()) {
    Serial.println(F("# error: sgp41 missing"));
    while (true) delay(1000);
  }
  sgpConditioning10s();

  wifiConnect();
  if (WiFi.status() == WL_CONNECTED) webserverBegin();

  printCsvHeader();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) server.handleClient();

  Sample s;
  s.ms = millis();

  if (!bme.performReading()) {
    g_latest = s;
    delay(1000);
    return;
  }

  s.tempC = bme.temperature;
  s.humPct = bme.humidity;
  s.pressHpa = bme.pressure / 100.0f;
  s.gasOhms = bme.gas_resistance;
  s.altM = bme.readAltitude(1013.25f);

  long vbatRaw = 0, vsolarRaw = 0;
  const bool ok1 = readMcpRaw(MCP342x::channel1, vbatRaw);
  const bool ok2 = readMcpRaw(MCP342x::channel2, vsolarRaw);

  const float vbat_adc   = ok1 ? mcpCodeToInputVolts(vbatRaw, ADC_RES, ADC_GAIN) : NAN;
  const float vsolar_adc = ok2 ? mcpCodeToInputVolts(vsolarRaw, ADC_RES, ADC_GAIN) : NAN;
  s.vbatV   = isnan(vbat_adc)   ? NAN : vbat_adc * VBAT_DIVIDER;
  s.vsolarV = isnan(vsolar_adc) ? NAN : vsolar_adc * VSOLAR_DIVIDER;

  s.alsLux = veml.readLux();
  s.white = veml.readWhite();

  s.sgpOk = sgp.measureRawSignals(&s.srawVoc, &s.srawNox, s.humPct, s.tempC);
  if (s.sgpOk) {
    s.vocIndex = vocAlgorithm.process(s.srawVoc);
    s.noxIndex = noxAlgorithm.process(s.srawNox);
  }

  g_latest = s;

  // Serial CSV row
  Serial.print(s.ms);            Serial.print(',');
  Serial.print(s.tempC, 2);      Serial.print(',');
  Serial.print(s.humPct, 2);     Serial.print(',');
  Serial.print(s.pressHpa, 2);   Serial.print(',');
  Serial.print(s.gasOhms);       Serial.print(',');
  Serial.print(s.altM, 2);       Serial.print(',');

  if (isnan(s.vbatV)) Serial.print(F("")); else Serial.print(s.vbatV, 6);
  Serial.print(',');

  if (isnan(s.vsolarV)) Serial.print(F("")); else Serial.print(s.vsolarV, 6);
  Serial.print(',');

  if (isnan(s.alsLux)) Serial.print(F("")); else Serial.print(s.alsLux, 2);
  Serial.print(',');

  Serial.print(s.white);
  Serial.print(',');

  if (!s.sgpOk) {
    Serial.print(F("")); Serial.print(',');
    Serial.print(F("")); Serial.print(',');
    Serial.print(F("")); Serial.print(',');
    Serial.print(F(""));
  } else {
    Serial.print(s.srawVoc);  Serial.print(',');
    Serial.print(s.srawNox);  Serial.print(',');
    Serial.print(s.vocIndex); Serial.print(',');
    Serial.print(s.noxIndex);
  }

  Serial.println();
  delay(1000);
}

#endif // MEASUREMENTS_ON_WEBSERVER