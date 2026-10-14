/*
  MASALA PACKER - ESP32 IoT gateway
  Bridges the Arduino Mega (Serial2) <-> MQTT.

  Wiring: ESP32 GPIO16 (RX2) <- Mega TX2 (use 5V->3.3V divider, e.g. 1k + 2k)
          ESP32 GPIO17 (TX2) -> Mega RX2   |  GND common

  MQTT topics (device id = mpm01):
    masala/mpm01/event    <- JSON lines from the machine (pack, fault, status ...)
    masala/mpm01/online   <- "online" / "offline" (retained, last-will)
    masala/mpm01/cmd      -> send: START | STOP | TARE | CAL 100 | SET 2 1.5 | STATUS

  Library: PubSubClient (Nick O'Leary)
  broker.hivemq.com is PUBLIC - anyone can read/command if they guess your
  topic. Use a unique device id for testing, then move to your own broker
  with a username/password (Mosquitto, HiveMQ Cloud, EMQX Cloud) before real use.
  Dashboard: Node-RED, MQTT Dashboard app, Home Assistant, or ThingsBoard.
*/

#include <WiFi.h>
#include <PubSubClient.h>

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* DEVICE_ID = "mpm01";            // make this unique

String tEvent  = String("masala/") + DEVICE_ID + "/event";
String tOnline = String("masala/") + DEVICE_ID + "/online";
String tCmd    = String("masala/") + DEVICE_ID + "/cmd";

WiFiClient net;
PubSubClient mqtt(net);
String line;
uint32_t lastTry = 0;

void onMsg(char* topic, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len && i < 40; i++) cmd += (char)payload[i];
  Serial2.println(cmd);                      // forward to the Mega
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
}

void connectMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  String cid = String("masala-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(cid.c_str(), NULL, NULL, tOnline.c_str(), 1, true, "offline")) {
    mqtt.publish(tOnline.c_str(), "online", true);
    mqtt.subscribe(tCmd.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMsg);
  connectWifi();
}

void loop() {
  connectWifi();
  connectMqtt();
  mqtt.loop();

  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      line.trim();
      if (line.length() && mqtt.connected()) mqtt.publish(tEvent.c_str(), line.c_str());
      Serial.println(line);
      line = "";
    } else if (c != '\r' && line.length() < 200) line += c;
  }
}
