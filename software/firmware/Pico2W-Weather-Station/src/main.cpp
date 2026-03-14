#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <MCP342x.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_SGP41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

// -------------------- BME688 (Adafruit_BME680) --------------------
static const uint8_t BME_ADDR = 0x76;
Adafruit_BME680 bme; // I2C

// -------------------- MCP3426A0T (Steve Marple MCP342x) -------------------
static const uint8_t MCP3426_ADDR = 0x68;
MCP342x adc(MCP3426_ADDR);

// Use the library's types (these are small wrapper classes, not enums)
static const MCP342x::Resolution ADC_RES = MCP342x::resolution16;
static const MCP342x::Gain       ADC_GAIN = MCP342x::gain1;

// Divider ratios: V_in = V_adc * (Rtop + Rbottom) / Rbottom
static const float VBAT_RTOP_OHMS   = 30000.0f;
static const float VBAT_RBOT_OHMS   = 20000.0f;
static const float VSOLAR_RTOP_OHMS = 110000.0f;
static const float VSOLAR_RBOT_OHMS = 10000.0f;

static const float VBAT_DIVIDER  = (VBAT_RTOP_OHMS + VBAT_RBOT_OHMS) / VBAT_RBOT_OHMS;        // 2.5
static const float VSOLAR_DIVIDER = (VSOLAR_RTOP_OHMS + VSOLAR_RBOT_OHMS) / VSOLAR_RBOT_OHMS; // 12.0

// -------------------- VEML7700 (Adafruit_VEML7700) --------------------
static const uint8_t VEML7700_ADDR = 0x10; // default
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// -------------------- SGP41 (Adafruit_SGP41) + Gas Index Algorithm --------------------
static const uint8_t SGP41_ADDR = 0x59; // default
Adafruit_SGP41 sgp;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;

// Volts per code for MCP342x using its internal full-scale of +/-2.048V / gain.
static float mcpVoltsPerCode(const MCP342x::Resolution &res, const MCP342x::Gain &gain) {
  const int r = (int)res;   // 12, 14, 16, 18
  const int g = (int)gain;  // 1, 2, 4, 8

  const float fs = 2.048f / (float)g;            // full-scale magnitude (V)
  const float counts = (float)(1UL << (r - 1));  // positive full-scale counts
  return fs / counts;
}

static float mcpCodeToInputVolts(long code, const MCP342x::Resolution &res, const MCP342x::Gain &gain) {
  return (float)code * mcpVoltsPerCode(res, gain);
}

static bool readMcpRaw(MCP342x::Channel ch, long &valueOut) {
  MCP342x::Config status;
  const uint8_t err = adc.convertAndRead(
      ch,
      MCP342x::oneShot,
      ADC_RES,
      ADC_GAIN,
      1000000, // timeout in microseconds
      valueOut,
      status);
  return err == 0;
}

static void printCsvHeader() {
  Serial.println(F("ms,temp_c,hum_pct,press_hpa,gas_ohms,alt_m,vbat_v,vsolar_v,als_lux,white,sgp41_sraw_voc,sgp41_sraw_nox,sgp41_voc_index,sgp41_nox_index"));
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Serial.println();
  Serial.println(F("# boot"));

  Wire.begin();

  MCP342x::generalCallReset();
  delay(1);

  // Probe MCP342x presence and print address
  Wire.requestFrom(MCP3426_ADDR, (uint8_t)1);
  if (!Wire.available()) {
    Serial.print(F("# error: no MCP342x device found at address 0x"));
    Serial.println(MCP3426_ADDR, HEX);
    while (true) { delay(1000); }
  }
  Serial.print(F("# mcp3426_addr=0x"));
  Serial.println(MCP3426_ADDR, HEX);

  // Init BME688 at fixed address and print it
  if (!bme.begin(BME_ADDR)) {
    Serial.print(F("# error: could not find BME688/BME680 at 0x"));
    Serial.println(BME_ADDR, HEX);
    while (true) { delay(1000); }
  }
  Serial.print(F("# bme68x_addr=0x"));
  Serial.println(BME_ADDR, HEX);

  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  // Init VEML7700 and print address
  if (!veml.begin()) {
    Serial.print(F("# error: could not find VEML7700 at 0x"));
    Serial.println(VEML7700_ADDR, HEX);
    while (true) { delay(1000); }
  }
  Serial.print(F("# veml7700_addr=0x"));
  Serial.println(VEML7700_ADDR, HEX);

  veml.setIntegrationTime(VEML7700_IT_100MS);
  veml.setGain(VEML7700_GAIN_1);

  // Init SGP41 and print address
  if (!sgp.begin()) {
    Serial.print(F("# error: could not find SGP41 at 0x"));
    Serial.println(SGP41_ADDR, HEX);
    while (true) { delay(1000); }
  }
  Serial.print(F("# sgp41_addr=0x"));
  Serial.println(SGP41_ADDR, HEX);

  Serial.println(F("# note: voc index needs ~60s, nox index ~300s learning"));

  // Conditioning loop (10 seconds) WITH COMPENSATION using BME temp+RH.
  Serial.println(F("# sgp41 conditioning (10s, compensated with BME688)"));
  for (uint8_t i = 0; i < 10; i++) {
    uint16_t sraw_voc = 0;

    if (!bme.performReading()) {
      Serial.println(F("# error: bme.performReading() failed during conditioning"));
    } else {
      const float tC = bme.temperature;
      const float rh = bme.humidity;

      // Pass compensation: (RH %, Temp C)
      if (!sgp.executeConditioning(&sraw_voc, rh, tC)) {
        Serial.println(F("# error: sgp41 conditioning failed"));
      } else {
        // Feed voc algorithm so it can start learning immediately.
        (void)vocAlgorithm.process(sraw_voc);
      }
    }

    delay(1000);
  }
  Serial.println(F("# sgp41 conditioning done"));

  Serial.print(F("# mcp_res_bits="));
  Serial.print((int)ADC_RES);
  Serial.print(F(",mcp_gain="));
  Serial.println((int)ADC_GAIN);

  Serial.print(F("# vbat_divider="));
  Serial.println(VBAT_DIVIDER, 6);
  Serial.print(F("# vsolar_divider="));
  Serial.println(VSOLAR_DIVIDER, 6);

  printCsvHeader();
}

void loop() {
  const uint32_t ms = millis();

  // --- BME688 reading ---
  if (!bme.performReading()) {
    Serial.print(ms);
    Serial.println(F(",,,,,,,,,,,,,,"));
    delay(1000);
    return;
  }

  const float tempC = bme.temperature;
  const float humPct = bme.humidity;
  const float pressHpa = bme.pressure / 100.0f;
  const uint32_t gasOhms = bme.gas_resistance;
  const float seaLevelHpa = 1013.25f;
  const float altM = bme.readAltitude(seaLevelHpa);

  // --- MCP3426 readings ---
  long vbatRaw = 0, vsolarRaw = 0;
  const bool ok1 = readMcpRaw(MCP342x::channel1, vbatRaw);   // VBAT (divided)
  const bool ok2 = readMcpRaw(MCP342x::channel2, vsolarRaw); // VSOLAR (divided)

  const float vbat_adc   = ok1 ? mcpCodeToInputVolts(vbatRaw, ADC_RES, ADC_GAIN) : NAN;
  const float vsolar_adc = ok2 ? mcpCodeToInputVolts(vsolarRaw, ADC_RES, ADC_GAIN) : NAN;

  const float vbatV   = isnan(vbat_adc)   ? NAN : (vbat_adc * VBAT_DIVIDER);
  const float vsolarV = isnan(vsolar_adc) ? NAN : (vsolar_adc * VSOLAR_DIVIDER);

  // --- VEML7700 readings ---
  const float alsLux = veml.readLux();
  const uint16_t white = veml.readWhite();

  // --- SGP41 readings + indices (COMPENSATED with BME temp+RH) ---
  uint16_t srawVoc = 0, srawNox = 0;
  int32_t vocIndex = -1;
  int32_t noxIndex = -1;

  const bool sgpOk = sgp.measureRawSignals(&srawVoc, &srawNox, humPct, tempC);
  if (sgpOk) {
    vocIndex = vocAlgorithm.process(srawVoc);
    noxIndex = noxAlgorithm.process(srawNox);
  }

  // CSV row
  Serial.print(ms);            Serial.print(',');
  Serial.print(tempC, 2);      Serial.print(',');
  Serial.print(humPct, 2);     Serial.print(',');
  Serial.print(pressHpa, 2);   Serial.print(',');
  Serial.print(gasOhms);       Serial.print(',');
  Serial.print(altM, 2);       Serial.print(',');

  if (isnan(vbatV)) Serial.print(F("")); else Serial.print(vbatV, 6);
  Serial.print(',');

  if (isnan(vsolarV)) Serial.print(F("")); else Serial.print(vsolarV, 6);
  Serial.print(',');

  if (isnan(alsLux)) Serial.print(F("")); else Serial.print(alsLux, 2);
  Serial.print(',');

  Serial.print(white);
  Serial.print(',');

  if (!sgpOk) {
    Serial.print(F("")); Serial.print(','); // sraw voc
    Serial.print(F("")); Serial.print(','); // sraw nox
    Serial.print(F("")); Serial.print(','); // voc index
    Serial.print(F(""));                   // nox index
  } else {
    Serial.print(srawVoc);  Serial.print(',');
    Serial.print(srawNox);  Serial.print(',');
    Serial.print(vocIndex); Serial.print(',');
    Serial.print(noxIndex);
  }

  Serial.println();
  delay(1000);
}