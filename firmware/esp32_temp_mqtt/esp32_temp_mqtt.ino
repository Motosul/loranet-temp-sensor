/*
  ESP32 + DS18B20 → MQTT
  - DS18B20 DQ -> GPIO 4, VCC->3V3, GND->GND, 4.7k pull-up DQ->3V3
  - Publishes: c=<tempC>  to topic: loranet/up/esp32-<MAC6>
*/

/*
  ESP32 + DS18B20 → MQTT (LOW POWER, Deep Sleep)

  Wiring:
  - DS18B20 DQ -> GPIO 4
  - DS18B20 VCC -> 3V3
  - DS18B20 GND -> GND
  - 4.7k pull-up from DQ to 3V3

  Publishes retained JSON:
    {"c":22.34,"ts":"2026-01-21T01:23:45Z"}

  Topic (stable):
    loranet/up/esp32-01-mot
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include <time.h>   // NTP time for ISO timestamps
#include "esp_sleep.h"

// ---------- Wi-Fi ----------
#define WIFI_SSID     "Smart Modem-2745EJ"
#define WIFI_PASSWORD "GentleBadgerV26!"

// ---------- MQTT (pick ONE broker) ----------
// A) Online public EMQX (no auth)
#define MQTT_HOST     "broker.emqx.io"
#define MQTT_PORT     1883
#define MQTT_USER     ""
#define MQTT_PASS     ""

// // B) Your local Mosquitto (uncomment & edit; then comment EMQX block above)
 //#define MQTT_HOST     "192.168.1.72"
 //#define MQTT_PORT     1883
 //#define MQTT_USER     "Mot"
 //#define MQTT_PASS     "Buster02"

// ---------- DS18B20 ----------
#define ONE_WIRE_PIN  4         // ESP32 GPIO for DS18B20 data
#define PUB_MS        10000UL   // publish interval (ms)
#define SLEEP_MINUTES 5
#define WIFI_TIMEOUTS_MS 20000

WiFiClient net;
PubSubClient mqtt(net);
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);

DeviceAddress sensorAddr;
bool          haveSensor = false;

String deviceId;        // "ABC123" (last 3 bytes of MAC)
String topicTemp;       // "loranet/up/esp32-ABC123"
String topicStatus;     // "loranet/status/esp32-ABC123"
String mqttClientId;    // "esp32-ABC123"

unsigned long lastPub = 0;

RTC_DATA_ATTR uint32_t wakeCount = 0;
#define NTP_SYNC_EVERY_N_WAKES 12 //sync time every N wakes (saves power)

// --------- helpers ---------
String mac6()
{
  uint64_t mac = ESP.getEfuseMac();
  uint32_t last3 = (uint32_t)(mac & 0xFFFFFF);
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", last3);
  return String(buf);
}

// --- Time sync + ISO8601 UTC timestamp ---
void initTime() {
  // Use UTC (0,0). Add your local offset if you want local time.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  // wait up to ~15s for time to sync
  for (int i = 0; i < 30 && time(nullptr) < 1700000000; i++) {  // ~2023-11-14
    delay(500);
  }
}

const char* iso8601_utc(char* out, size_t len) {
  time_t now = time(nullptr);
  if (now < 1700000000) {                      // not synced yet
    snprintf(out, len, "1970-01-01T00:00:00Z");
    return out;
  }
  struct tm tmUTC;
  gmtime_r(&now, &tmUTC);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tmUTC);
  return out;
}

// --- Retained message cleanup (publish empty retained) ---
void clearRetainedOnce() {
  static bool cleared = false;
  if (cleared) return;
  // publish empty retained to delete any previous retained value
  mqtt.publish(topicTemp.c_str(), "", true);
  cleared = true;
}

void wifiConnect()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(mqttClientId.c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  uint8_t spins = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (++spins >= 60) {
      Serial.println("\nWiFi: retrying...");
      WiFi.disconnect(true, true);
      delay(400);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      spins = 0;
    }
  }
  Serial.printf("\nWiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

void mqttConnect()
{
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  // Last Will: go "offline" if we drop
  const char* willMsg = "offline";
  while (!mqtt.connected()) {
    Serial.printf("MQTT: connecting to %s:%d ... ", MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(mqttClientId.c_str(), MQTT_USER, MQTT_PASS,
                     topicStatus.c_str(), 0, true, willMsg, true)) {
      Serial.println("ok");
      mqtt.publish(topicStatus.c_str(), "online", true);
      Serial.printf("Topic: %s\n", topicTemp.c_str());
    } else {
      Serial.printf("failed, rc=%d (retry in 2s)\n", mqtt.state());
      delay(2000);
    }
  }
}

float readC()
{
  if (!haveSensor) return NAN;
  sensors.requestTemperatures();                 // blocking (we set wait=true)
  float c = sensors.getTempC(sensorAddr);        // by address (more robust)
  if (c == DEVICE_DISCONNECTED_C) return NAN;
  if (fabsf(c - 85.0f) < 0.01f) return NAN;      // not finished / default
  return c;
}

void publishTemp()
{
  float c = readC();
  if (!isnan(c)) {
    char ts[25];  // "YYYY-MM-DDThh:mm:ssZ"
    iso8601_utc(ts, sizeof(ts));

    char payload[96];
    // JSON: temperature in C and ISO8601 timestamp
    snprintf(payload, sizeof(payload), "{\"c\":%.2f,\"ts\":\"%s\"}", c, ts);

    Serial.printf("Publish %s → %s\n", payload, topicTemp.c_str());

    // retained = true so the last value is kept by the broker
    if (!mqtt.publish(topicTemp.c_str(), payload, true)) {
      Serial.println("Publish failed, forcing reconnect");
      mqtt.disconnect();
    }
  } else {
    Serial.println("Temp read invalid (disconnected/85C/pending).");
  }
}
bool wifiConnectWithTimeout()
{
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(mqttClientId.c_str());

  // More reliable on battery during connect
  WiFi.setSleep(false);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUTS_MS) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("\nWiFi: timeout (sleeping)");
  return false;
}

void initTimeSometimes()
{
  // Only do NTP occasionally to reduce wake time/power
  if ((wakeCount % NTP_SYNC_EVERY_N_WAKES) != 0) return;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  // wait up to ~10s
  for (int i = 0; i < 40 && time(nullptr) < 1700000000; i++) {
    delay(250);
  }
}

void powerDownAndSleep()
{
  mqtt.disconnect();
  delay(50);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop(); // ensure BT off too
  delay(50);

  Serial.printf("Sleeping for %d minutes...\n", SLEEP_MINUTES);
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// --------- Arduino ---------
void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32 + DS18B20 → MQTT");

  // Build IDs/topics from MAC
  //deviceId     = mac6();                 // e.g. "A1B2C3"
  //mqttClientId = "esp32-01-mot" + deviceId;
  //topicTemp    = "loranet/up/esp32-01-mot" + deviceId;
  //topicStatus  = "loranet/status/esp32-01-mot" + deviceId;

  deviceId     = mac6();                 // still fine if you want it
  mqttClientId = "esp32-01-mot";         // simple client id
  topicTemp    = "loranet/up/esp32-01-mot";
  topicStatus  = "loranet/status/esp32-01-mot";


  // DS18B20
  pinMode(ONE_WIRE_PIN, INPUT_PULLUP);
  sensors.begin();
  sensors.setResolution(11);
  sensors.setWaitForConversion(true);    // block until finished

  int n = sensors.getDeviceCount();
  Serial.printf("DS18B20 devices found: %d\n", n);
  if (n > 0 && sensors.getAddress(sensorAddr, 0)) {
    char addrHex[17] = {0};
    for (uint8_t i = 0; i < 8; i++) sprintf(addrHex + i*2, "%02X", sensorAddr[i]);
    Serial.printf("First sensor addr: %s\n", addrHex);
    haveSensor = true;
  } else {
    Serial.println("No DS18B20 detected. Check wiring and 4.7k pull-up.");
  }

wakeCount++;

// Connect (with timeout so we don't drain the battery)
if (!wifiConnectWithTimeout()) {
  powerDownAndSleep();
}

mqttConnect();

// NTP only sometimes (saves time/power)
initTimeSometimes();

clearRetainedOnce();  // optional: keep if you still want to clear retained once

}

void loop()
{
  // Ensure connections (use your existing functions)
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiConnectWithTimeout()) powerDownAndSleep();
  }
  if (!mqtt.connected()) mqttConnect();

  mqtt.loop();

  // Publish once, then sleep
  publishTemp();

  // Give the publish a moment to flush
  delay(200);

  powerDownAndSleep();
}

