/*
  ESP32 + DS18B20 → MQTT
  - DS18B20 DQ -> GPIO 4, VCC->3V3, GND->GND, 4.7k pull-up DQ->3V3
  - Publishes: c=<tempC>  to topic: loranet/up/esp32-<MAC6>
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ---------- Wi-Fi ----------
#define WIFI_SSID     "Smart Modem-DT9L3S"
#define WIFI_PASSWORD "FunkyWallabyK62#"

// ---------- MQTT (pick ONE broker) ----------
// A) Online public EMQX (no auth)
#define MQTT_HOST     "broker.emqx.io"
#define MQTT_PORT     1883
#define MQTT_USER     ""
#define MQTT_PASS     ""

// // B) Your local Mosquitto (uncomment & edit; then comment EMQX block above)
// #define MQTT_HOST     "192.168.1.126"
// #define MQTT_PORT     1883
// #define MQTT_USER     "Mot"
// #define MQTT_PASS     "Buster02"

// ---------- DS18B20 ----------
#define ONE_WIRE_PIN  4         // ESP32 GPIO for DS18B20 data
#define PUB_MS        10000UL   // publish interval (ms)

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

// --------- helpers ---------
String mac6()
{
  uint64_t mac = ESP.getEfuseMac();
  uint32_t last3 = (uint32_t)(mac & 0xFFFFFF);
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", last3);
  return String(buf);
}

void wifiConnect()
{
  WiFi.mode(WIFI_STA);
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
    char payload[24];
    snprintf(payload, sizeof(payload), "c=%.2f", c);
    Serial.printf("Publish %s → %s\n", payload, topicTemp.c_str());
    if (!mqtt.publish(topicTemp.c_str(), payload, false)) {
      Serial.println("Publish failed, forcing reconnect");
      mqtt.disconnect();
    }
  } else {
    Serial.println("Temp read invalid (disconnected/85C/pending).");
  }
}

// --------- Arduino ---------
void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32 + DS18B20 → MQTT");

  // Build IDs/topics from MAC
  deviceId     = mac6();                 // e.g. "A1B2C3"
  mqttClientId = "esp32-01-mot" + deviceId;
  topicTemp    = "loranet/up/esp32-01-mot" + deviceId;
  topicStatus  = "loranet/status/esp32-01-mot" + deviceId;

  // DS18B20
  pinMode(ONE_WIRE_PIN, INPUT_PULLUP);
  sensors.begin();
  sensors.setResolution(12);
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

  // Connect
  wifiConnect();
  mqttConnect();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) wifiConnect();
  if (!mqtt.connected())             mqttConnect();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastPub >= PUB_MS) {
    lastPub = now;
    publishTemp();
  }
}
