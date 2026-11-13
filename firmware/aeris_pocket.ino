/*
  AERIS Pocket — ESP32-S3-WROOM-1-N8
  SGP41 (0x59) + VEML6075 (0x10) + SH1106 1.3" OLED (0x3C)

  Board: ESP32S3 Dev Module, USB CDC On Boot = Enabled,
         Flash 8 MB, PSRAM Disabled, Arduino-ESP32 core 3.x

  Libraries (Library Manager):
    Sensirion I2C SGP41
    Sensirion Gas Index Algorithm
    SparkFun VEML6075 Arduino Library
    U8g2
    PubSubClient
    ArduinoJson  (7.x)

  If the picture is shifted 2 px, the glass is SSD1306 — swap the
  constructor noted in beginDisplay(). If the scan shows 0x3D, change
  OLED_ADDR.

  AQI* is a comfort figure mapped from the Sensirion VOC index.
  It is not an EPA AQI. The SGP41 does not see particles or ozone.
  The VOC algorithm wants one sample per second. Do not "optimize"
  that down to once a minute or the index becomes nonsense.

  Fixed compensation is 25 °C / 50 %RH. An SHT4x later is a straight
  swap of the two tick values in compensate().
*/

#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>
#include <SparkFun_VEML6075_Arduino_Library.h>
#include "secrets.h"

static constexpr int PIN_SDA = 8;
static constexpr int PIN_SCL = 9;
static constexpr int PIN_BAT = 4;
static constexpr int PIN_CHRG = 6;
static constexpr int PIN_STDBY = 7;
static constexpr int PIN_BTN = 0;

static constexpr uint8_t OLED_ADDR = 0x3C;
static constexpr uint32_t SAMPLE_MS = 1000;
static constexpr uint32_t DRAW_MS = 200;
static constexpr uint32_t PUBLISH_MS = 10000;
static constexpr uint32_t SCREEN_IDLE_MS = 30000;
static constexpr uint32_t NET_RETRY_MS = 5000;

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

SensirionI2CSgp41 sgp41;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;
VEML6075 uv;

WiFiClient wifi;
PubSubClient mqtt(wifi);

enum Page : uint8_t { PAGE_AIR = 0, PAGE_NOX, PAGE_UV, PAGE_SYS, PAGE_COUNT };

struct Sample {
  int32_t voc = 0;
  int32_t nox = 0;
  int aqi = 0;
  const char* band = "----";
  float uvi = 0;
  float uva = 0;
  float uvb = 0;
  float vbat = 0;
  int batPct = 0;
  bool charging = false;
  bool charged = false;
  bool vocValid = false;
  bool noxValid = false;
  bool uvValid = false;
} sample;

uint32_t nextSampleAt = 0;
uint32_t nextDrawAt = 0;
uint32_t nextPublishAt = 0;
uint32_t nextNetAt = 0;
uint32_t lastButtonAt = 0;
uint32_t lastInteraction = 0;
bool screenOff = false;
bool mqttReady = false;
Page page = PAGE_AIR;
String deviceId;
String stateTopic;
String availTopic;
char ipText[16] = "—";

// Same piecewise map as the build sheet. 100 VOC index → AQI* 50, Clear.
void comfortFromVoc(int32_t voc, int& aqi, const char*& band) {
  int v = voc;
  if (v < 1) v = 1;
  if (v > 500) v = 500;
  if (v <= 100) {
    aqi = (v * 50) / 100;
    band = "Clear";
  } else if (v <= 200) {
    aqi = 50 + ((v - 100) * 50) / 100;
    band = "Fair";
  } else if (v <= 300) {
    aqi = 100 + ((v - 200) * 50) / 100;
    band = "Elevated";
  } else if (v <= 400) {
    aqi = 150 + ((v - 300) * 50) / 100;
    band = "Poor";
  } else {
    aqi = 200 + ((v - 400) * 100) / 100;
    band = "Severe";
  }
}

// Sensirion ticks. Default 25 °C, 50 %RH until a humidity sensor exists.
void compensate(uint16_t& rhTicks, uint16_t& tTicks) {
  const float rh = 50.0f;
  const float tC = 25.0f;
  rhTicks = (uint16_t)(rh * 65535.0f / 100.0f);
  tTicks = (uint16_t)((tC + 45.0f) * 65535.0f / 175.0f);
}

float readBattery() {
  // 200k over 100k. analogReadMilliVolts is the core 3.x calibrated read.
  int mv = analogReadMilliVolts(PIN_BAT);
  return (mv / 1000.0f) * 3.0f;
}

int batteryPercent(float v) {
  // Linear stand-in. A real Li-Po is flatter in the middle.
  float pct = (v - 3.30f) / (4.20f - 3.30f) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (int)(pct + 0.5f);
}

void beginDisplay() {
  u8g2.setI2CAddress(OLED_ADDR << 1);
  u8g2.begin();
  u8g2.setContrast(180);
  u8g2.setPowerSave(0);
}

void splash(const char* line) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 14, "AERIS");
  u8g2.drawStr(0, 32, line);
  u8g2.drawStr(0, 52, "conditioning");
  u8g2.sendBuffer();
}

bool beginSensors() {
  sgp41.begin(Wire);
  uint16_t rh, t;
  compensate(rh, t);
  uint16_t sraw = 0;
  uint16_t err = 0;
  // Ten 1-second conditioning calls. NOx raw is not valid during this.
  for (int i = 0; i < 10; i++) {
    splash(i < 5 ? "SGP41 warm" : "SGP41 settle");
    err = sgp41.executeConditioning(rh, t, sraw);
    delay(1000);
  }
  sample.vocValid = (err == 0);
  bool uvOk = uv.begin();
  sample.uvValid = uvOk;
  return err == 0;
}

void sampleSensors() {
  uint16_t rh, t;
  compensate(rh, t);
  uint16_t srawVoc = 0, srawNox = 0;
  uint16_t err = sgp41.measureRawSignals(rh, t, srawVoc, srawNox);
  if (err == 0) {
    sample.voc = vocAlgorithm.process(srawVoc);
    sample.nox = noxAlgorithm.process(srawNox);
    comfortFromVoc(sample.voc, sample.aqi, sample.band);
    sample.vocValid = true;
    // Library returns a placeholder near the start of NOx learning.
    sample.noxValid = sample.nox > 0;
  }
  if (sample.uvValid) {
    sample.uvi = uv.index();
    sample.uva = uv.uva();
    sample.uvb = uv.uvb();
  }
  sample.vbat = readBattery();
  sample.batPct = batteryPercent(sample.vbat);
  sample.charging = digitalRead(PIN_CHRG) == LOW;
  sample.charged = digitalRead(PIN_STDBY) == LOW;
}

void drawHeader() {
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 10, "AERIS");
  char bat[12];
  snprintf(bat, sizeof(bat), "%d%%", sample.batPct);
  u8g2.drawStr(92, 10, bat);
  u8g2.drawHLine(0, 13, 128);
}

void drawAir() {
  drawHeader();
  u8g2.setFont(u8g2_font_logisoso16_tr);
  char num[8];
  snprintf(num, sizeof(num), "%ld", (long)sample.voc);
  u8g2.drawStr(0, 34, num);
  // Right-align the band. "Elevated" is 48 px and used to start at x=88,
  // which walked off the 128 px glass and into the number.
  u8g2.setFont(u8g2_font_6x12_tr);
  int bandW = u8g2.getStrWidth(sample.band);
  if (bandW > 128) bandW = 128;
  u8g2.drawStr(128 - bandW, 28, sample.band);
  char aqi[24];
  snprintf(aqi, sizeof(aqi), "AQI* %d  not EPA", sample.aqi);
  u8g2.drawStr(0, 50, aqi);
  int w = sample.voc;
  if (w < 0) w = 0;
  if (w > 500) w = 500;
  u8g2.drawFrame(0, 55, 128, 8);
  int fill = (w * 126) / 500;
  if (fill > 0) u8g2.drawBox(1, 56, fill, 6);
}

void drawNox() {
  drawHeader();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 28, "NOx");
  u8g2.setFont(u8g2_font_logisoso16_tr);
  char num[8];
  if (!sample.noxValid) {
    u8g2.drawStr(36, 36, "---");
  } else {
    snprintf(num, sizeof(num), "%ld", (long)sample.nox);
    u8g2.drawStr(36, 36, num);
  }
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 58, "1 = clean baseline");
}

void drawUv() {
  drawHeader();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 28, "UV");
  u8g2.setFont(u8g2_font_logisoso16_tr);
  char num[8];
  snprintf(num, sizeof(num), "%1.1f", sample.uvi);
  u8g2.drawStr(28, 34, num);
  u8g2.setFont(u8g2_font_6x12_tr);
  char sub[28];
  snprintf(sub, sizeof(sub), "A %1.0f  B %1.0f", sample.uva, sample.uvb);
  u8g2.drawStr(0, 58, sub);
}

void drawSys() {
  drawHeader();
  u8g2.setFont(u8g2_font_6x12_tr);
  char line[24];
  snprintf(line, sizeof(line), "BAT %1.2fV", sample.vbat);
  u8g2.drawStr(0, 28, line);
  snprintf(line, sizeof(line), "RSSI %d", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  u8g2.drawStr(0, 40, line);
  u8g2.drawStr(0, 52, mqtt.connected() ? "MQTT up" : "MQTT down");
  u8g2.drawStr(0, 63, ipText);
}

void draw() {
  u8g2.clearBuffer();
  switch (page) {
    case PAGE_NOX: drawNox(); break;
    case PAGE_UV: drawUv(); break;
    case PAGE_SYS: drawSys(); break;
    default: drawAir(); break;
  }
  u8g2.sendBuffer();
}

void wakeScreen() {
  if (screenOff) {
    screenOff = false;
    u8g2.setPowerSave(0);
  }
  lastInteraction = millis();
}

void serviceButton() {
  static bool last = true;
  static uint32_t changed = 0;
  bool down = digitalRead(PIN_BTN) == LOW;
  uint32_t now = millis();
  if (down != last) {
    changed = now;
    last = down;
  }
  if (down && (now - changed) > 30 && (now - lastButtonAt) > 250) {
    lastButtonAt = now;
    wakeScreen();
    page = (Page)((page + 1) % PAGE_COUNT);
    draw();
  }
}

void serviceScreen() {
  if (!screenOff && millis() - lastInteraction > SCREEN_IDLE_MS) {
    screenOff = true;
    u8g2.setPowerSave(1);
  }
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    strncpy(ipText, WiFi.localIP().toString().c_str(), sizeof(ipText) - 1);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void publishDiscovery() {
  struct Item {
    const char* id;
    const char* name;
    const char* unit;
    const char* tmpl;
    const char* klass;
  };
  const Item items[] = {
      {"voc", "VOC index", "index", "{{ value_json.voc }}", nullptr},
      {"nox", "NOx index", "index", "{{ value_json.nox }}", nullptr},
      {"aqi", "AQI star", nullptr, "{{ value_json.aqi_star }}", nullptr},
      {"uv", "UV index", "UVI", "{{ value_json.uv }}", nullptr},
      {"vbat", "Battery voltage", "V", "{{ value_json.vbat }}", "voltage"},
      {"bat", "Battery", "%", "{{ value_json.bat_pct }}", "battery"},
      {"rssi", "RSSI", "dBm", "{{ value_json.rssi }}", "signal_strength"},
  };

  for (auto& item : items) {
    JsonDocument doc;
    char uid[64];
    snprintf(uid, sizeof(uid), "%s_%s", deviceId.c_str(), item.id);
    doc["name"] = item.name;
    doc["unique_id"] = uid;
    doc["state_topic"] = stateTopic;
    doc["availability_topic"] = availTopic;
    doc["value_template"] = item.tmpl;
    if (item.unit) doc["unit_of_measurement"] = item.unit;
    if (item.klass) doc["device_class"] = item.klass;
    if (strcmp(item.id, "aqi") == 0) doc["icon"] = "mdi:air-filter";
    doc["device"]["identifiers"][0] = deviceId;
    doc["device"]["name"] = "AERIS Pocket";
    doc["device"]["mf"] = "AERIS";
    doc["device"]["mdl"] = "Pocket AQ+UV";
    doc["device"]["sw"] = "1.0.0";
    char topic[96];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", uid);
    char payload[1024];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0 || n >= sizeof(payload)) continue;
    mqtt.publish(topic, payload, true);
  }
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  String client = "aeris-" + deviceId;
  bool ok;
  if (MQTT_USER[0] == '\0') {
    ok = mqtt.connect(client.c_str(), availTopic.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(client.c_str(), MQTT_USER, MQTT_PASS, availTopic.c_str(), 1, true, "offline");
  }
  if (!ok) return;
  mqtt.publish(availTopic.c_str(), "online", true);
  publishDiscovery();
  mqttReady = true;
}

void publishState() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["voc"] = sample.voc;
  doc["nox"] = sample.nox;
  doc["aqi_star"] = sample.aqi;
  doc["band"] = sample.band;
  doc["uv"] = sample.uvi;
  doc["uva"] = sample.uva;
  doc["uvb"] = sample.uvb;
  doc["vbat"] = sample.vbat;
  doc["bat_pct"] = sample.batPct;
  doc["charging"] = sample.charging;
  doc["rssi"] = WiFi.RSSI();
  doc["ip"] = ipText;
  char payload[384];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(stateTopic.c_str(), payload, true);
}

void serviceNetwork(uint32_t now) {
  if ((int32_t)(now - nextNetAt) < 0 && WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if ((int32_t)(now - nextNetAt) >= 0) {
    nextNetAt = now + NET_RETRY_MS;
    ensureWifi();
    ensureMqtt();
  }
  mqtt.loop();
}

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_CHRG, INPUT_PULLUP);
  pinMode(PIN_STDBY, INPUT_PULLUP);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  uint64_t mac = ESP.getEfuseMac();
  char id[16];
  snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  deviceId = id;
  stateTopic = String(MQTT_PREFIX) + "/" + deviceId + "/state";
  availTopic = String(MQTT_PREFIX) + "/" + deviceId + "/availability";

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(1);
  beginDisplay();
  splash("boot");
  beginSensors();

  lastInteraction = millis();
  nextSampleAt = millis();
  draw();
}

void loop() {
  uint32_t now = millis();
  serviceButton();
  serviceScreen();

  if ((int32_t)(now - nextSampleAt) >= 0) {
    // One sample. If we were late, do not burst — the index assumes 1 Hz.
    nextSampleAt = now + SAMPLE_MS;
    sampleSensors();
  }

  if (!screenOff && (int32_t)(now - nextDrawAt) >= 0) {
    nextDrawAt = now + DRAW_MS;
    draw();
  }

  serviceNetwork(now);

  if (mqtt.connected() && (int32_t)(now - nextPublishAt) >= 0) {
    nextPublishAt = now + PUBLISH_MS;
    publishState();
  }
}
