#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include "secrets.h"

// =====================================================
// USER SETTINGS
// =====================================================

// Device name / mDNS name
const char* DEVICE_NAME   = "garage-esp32";

// MQTT topics
const char* TOPIC_RELAY1  = "garage/relay1";
const char* TOPIC_RELAY2  = "garage/relay2";
const char* TOPIC_RELAY3  = "garage/relay3";
const char* TOPIC_STATUS  = "garage/status";        // request topic
const char* TOPIC_STATE   = "garage/state";         // state/event topic
const char* TOPIC_AVAIL   = "garage/availability";  // online/offline
const char* TOPIC_INFO    = "garage/info";          // periodic diagnostic info

// Relay pins
// Active-HIGH BC337 transistor drivers
const uint8_t RELAY_ON  = HIGH;
const uint8_t RELAY_OFF = LOW;
const int RELAY1_PIN = 27;   // K1 / Up
const int RELAY2_PIN = 26;   // K2 / Stop
const int RELAY3_PIN = 25;   // K3 / Down
// Optional status LED
const int LED_PIN = 2;

// Relay pulse duration
const unsigned long RELAY_PULSE_MS = 500;  // 500 ms

// Connectivity timing
const unsigned long WIFI_RETRY_INTERVAL_MS   = 5000;
const unsigned long MQTT_RETRY_INTERVAL_MS   = 5000;
const unsigned long WIFI_REBOOT_AFTER_MS     = 180000;   // 3 minutes
const unsigned long MQTT_REBOOT_AFTER_MS     = 300000;   // 5 minutes
const unsigned long STATUS_PUBLISH_MS        = 60000;    // every 60 sec

// WiFi Restart
const unsigned long GATEWAY_CHECK_INTERVAL_MS  = 10000;   // check every 10 sec
const unsigned long GATEWAY_REBOOT_AFTER_MS    = 60000;   // reboot if unreachable for 60 sec
const uint16_t      GATEWAY_CHECK_PORT         = 53;      // router DNS port
const int32_t       GATEWAY_CONNECT_TIMEOUT_MS = 1000;    // 1 sec timeout

// =====================================================
// GLOBALS
// =====================================================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebServer webServer(80);

// Relay pulse state
bool relay1Active = false;
bool relay2Active = false;
bool relay3Active = false;
unsigned long relay1StartMs = 0;
unsigned long relay2StartMs = 0;
unsigned long relay3StartMs = 0;

// Recovery timers
unsigned long lastWifiAttemptMs = 0;
unsigned long wifiDownSinceMs   = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long mqttDownSinceMs   = 0;
unsigned long lastStatusMs      = 0;
unsigned long lastGatewayCheckMs = 0;
unsigned long gatewayDownSinceMs = 0;

// Server / mDNS state
bool webServerStarted = false;
bool mdnsStarted = false;

// =====================================================
// GATEWAY CHECK
// =====================================================

bool isGatewayReachable() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient testClient;

  bool ok = testClient.connect(
    gateway,
    GATEWAY_CHECK_PORT,
    GATEWAY_CONNECT_TIMEOUT_MS
  );

  if (ok) {
    testClient.stop();
    return true;
  }

  return false;
}

void maintainGateway() {
  const unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0,0,0,0)) {
    gatewayDownSinceMs = 0;
    lastGatewayCheckMs = 0;
    return;
  }

  if (now - lastGatewayCheckMs < GATEWAY_CHECK_INTERVAL_MS) {
    return;
  }

  lastGatewayCheckMs = now;

  if (isGatewayReachable()) {
    if (gatewayDownSinceMs != 0) {
      Serial.println("Gateway reachable again");
    }
    gatewayDownSinceMs = 0;
    return;
  }

  if (gatewayDownSinceMs == 0) {
    gatewayDownSinceMs = now;
    Serial.println("Gateway unreachable: " + gateway.toString());
  } else {
    Serial.println("Gateway still unreachable: " + gateway.toString());
  }

  if (now - gatewayDownSinceMs >= GATEWAY_REBOOT_AFTER_MS) {
    Serial.println("Gateway unreachable too long, rebooting");
    delay(200);
    ESP.restart();
  }
}

// =====================================================
// RELAY HELPERS
// =====================================================

void setRelaySafeState() {
  digitalWrite(RELAY1_PIN, RELAY_OFF);
  digitalWrite(RELAY2_PIN, RELAY_OFF);
  digitalWrite(RELAY3_PIN, RELAY_OFF);
}

void startRelay1Pulse() {
  if (relay1Active) return;
  digitalWrite(RELAY1_PIN, RELAY_ON);
  relay1Active = true;
  relay1StartMs = millis();
  Serial.println("Relay 1 pulsed (up)");
}

void startRelay2Pulse() {
  if (relay2Active) return;
  digitalWrite(RELAY2_PIN, RELAY_ON);
  relay2Active = true;
  relay2StartMs = millis();
  Serial.println("Relay 2 pulsed (stop)");
}

void startRelay3Pulse() {
  if (relay3Active) return;
  digitalWrite(RELAY3_PIN, RELAY_ON);
  relay3Active = true;
  relay3StartMs = millis();
  Serial.println("Relay 3 pulsed (down)");
}

void updateRelayPulses() {
  const unsigned long now = millis();

  if (relay1Active && (now - relay1StartMs >= RELAY_PULSE_MS)) {
    digitalWrite(RELAY1_PIN, RELAY_OFF);
    relay1Active = false;
    Serial.println("Relay 1 off");
  }

  if (relay2Active && (now - relay2StartMs >= RELAY_PULSE_MS)) {
    digitalWrite(RELAY2_PIN, RELAY_OFF);
    relay2Active = false;
    Serial.println("Relay 2 off");
  }

  if (relay3Active && (now - relay3StartMs >= RELAY_PULSE_MS)) {
    digitalWrite(RELAY3_PIN, RELAY_OFF);
    relay3Active = false;
    Serial.println("Relay 3 off");
  }
}

// =====================================================
// MQTT HELPERS
// =====================================================

String wifiSummary() {
  if (WiFi.status() == WL_CONNECTED) {
    String gwState = (gatewayDownSinceMs == 0) ? "up" : "down";

    return "wifi=connected ip=" + WiFi.localIP().toString() +
           " gw=" + gateway.toString() +
           " gw_status=" + gwState +
           " rssi=" + String(WiFi.RSSI());
  }
  return "wifi=disconnected";
}

void publishAvailability(const char* value, bool retained = true) {
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_AVAIL, value, retained);
  }
}

void publishState(const char* value, bool retained = true) {
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_STATE, value, retained);
  }
}

void publishStatus() {
  if (!mqttClient.connected()) return;

  String payload =
    "device=" + String(DEVICE_NAME) + " " +
    wifiSummary() + " " +
    "uptime_ms=" + String(millis()) + " " +
    "relay1_active=" + String(relay1Active ? "1" : "0") + " " +
    "relay2_active=" + String(relay2Active ? "1" : "0") + " " +
    "relay3_active=" + String(relay3Active ? "1" : "0");

  mqttClient.publish(TOPIC_INFO, payload.c_str(), false);
}

// =====================================================
// OTA WEB SERVER
// =====================================================

bool checkOtaAuth() {
  if (!webServer.authenticate(OTA_USER, OTA_PASSWORD)) {
    webServer.requestAuthentication();
    return false;
  }
  return true;
}

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

void handleRoot() {
  if (!checkOtaAuth()) return;

  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'><title>ESP32 Garage Gate</title></head><body>";
  html += "<h2>ESP32 Garage Gate Controller</h2>";
  html += "<p><b>Device:</b> " + htmlEscape(String(DEVICE_NAME)) + "</p>";
  html += "<p><b>IP:</b> " + htmlEscape(WiFi.localIP().toString()) + "</p>";
  html += "<p><b>RSSI:</b> " + htmlEscape(String(WiFi.RSSI())) + "</p>";
  html += "<p><b>Relay 1:</b> Clicks Up Button</p>";
  html += "<p><b>Relay 2:</b> Clicks Stop Button</p>";
  html += "<p><b>Relay 3:</b> Clicks Down Button</p>";
  html += "<hr>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin'>";
  html += "<input type='submit' value='Upload firmware'>";
  html += "</form>";
  html += "<p>Version 1.0.2</p>";
  html += "</body></html>";

  webServer.send(200, "text/html", html);
}

bool otaUploadActive = false;
bool otaUploadSucceeded = false;

void handleUpdateFinished() {
  const bool ok = otaUploadSucceeded;
  otaUploadSucceeded = false;

  if (otaUploadActive) {
    Update.abort();
    otaUploadActive = false;
  }

  if (!checkOtaAuth()) return;

  webServer.sendHeader("Connection", "close");
  webServer.send(
    ok ? 200 : 500,
    "text/plain",
    ok ? "Update successful. Rebooting..." : "Update failed."
  );

  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void handleUpdateUpload() {
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    if (otaUploadActive) {
      Update.abort();
    }

    otaUploadActive = false;
    otaUploadSucceeded = false;

    if (!webServer.authenticate(OTA_USER, OTA_PASSWORD)) {
      return;
    }

    Serial.printf("OTA upload start: %s\n",
                  upload.filename.c_str());

    setRelaySafeState();

    otaUploadActive = Update.begin(UPDATE_SIZE_UNKNOWN);

    if (!otaUploadActive) {
      Update.printError(Serial);
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaUploadActive) return;

    if (Update.write(upload.buf, upload.currentSize)
        != upload.currentSize) {
      Update.printError(Serial);
      Update.abort();
      otaUploadActive = false;
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaUploadActive) return;

    otaUploadSucceeded = Update.end(true);
    otaUploadActive = false;

    if (otaUploadSucceeded) {
      Serial.printf("OTA success: %u bytes\n",
                    upload.totalSize);
    } else {
      Update.printError(Serial);
      Update.abort();
    }

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (otaUploadActive) {
      Update.abort();
    }

    otaUploadActive = false;
    otaUploadSucceeded = false;

    Serial.println("OTA upload aborted");
  }
}

void startWebServerIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || webServerStarted) return;

  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);

  webServer.begin();
  webServerStarted = true;

  Serial.println("Web server started");
  Serial.printf("Open: http://%s/\n", WiFi.localIP().toString().c_str());
  if (mdnsStarted) {
    Serial.printf("Or:   http://%s.local/\n", DEVICE_NAME);
  }
}

void startMdnsIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || mdnsStarted) return;

  if (MDNS.begin(DEVICE_NAME)) {
    mdnsStarted = true;
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS started: http://%s.local/\n", DEVICE_NAME);
  } else {
    Serial.println("mDNS start failed");
  }
}

// =====================================================
// MQTT CALLBACK
// =====================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String msg;
  msg.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  msg.trim();
  msg.toLowerCase();

  Serial.print("Topic: ");
  Serial.print(topicStr);
  Serial.print(" | Payload: ");
  Serial.println(msg);

  if (topicStr == TOPIC_RELAY1) {
    startRelay1Pulse();
    publishState("relay1_pulsed");
    return;
  }

  if (topicStr == TOPIC_RELAY2) {
    startRelay2Pulse();
    publishState("relay2_pulsed");
    return;
  }

  if (topicStr == TOPIC_RELAY3) {
    startRelay3Pulse();
    publishState("relay3_pulsed");
    return;
  }

  if (topicStr == TOPIC_STATUS) {
    publishStatus();
    publishState("status_sent");
    return;
  }

  publishState("unknown_topic");
}

// =====================================================
// WIFI
// =====================================================

void startWifiConnect() {
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lastWifiAttemptMs = millis();
}

void maintainWifi() {
  const unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    wifiDownSinceMs = 0;
    return;
  }

  digitalWrite(LED_PIN, LOW);

  if (wifiDownSinceMs == 0) {
    wifiDownSinceMs = now;
    Serial.println("Wi-Fi down");

    webServerStarted = false;
    mdnsStarted = false;
  }

  if (now - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    Serial.println("Retrying Wi-Fi");
    WiFi.disconnect(true, false);
    delay(100);
    startWifiConnect();
  }

  if (wifiDownSinceMs != 0 && (now - wifiDownSinceMs >= WIFI_REBOOT_AFTER_MS)) {
    Serial.println("Wi-Fi down too long, rebooting");
    delay(200);
    ESP.restart();
  }
}

// =====================================================
// Home Assistant auto-discovery
// =====================================================

void publishDiscovery() {
  String configUrl = "http://" + local_IP.toString() + "/";

  String device =
    "\"dev\":{"
      "\"ids\":[\"esp32-garage\"],"
      "\"name\":\"Garage Controller\","
      "\"mf\":\"DIY\","
      "\"mdl\":\"Wireless Gate Controller v1.0\","
      "\"sw\":\"1.0.2\","
      "\"cu\":\"" + configUrl + "\""
    "}";

  String availability =
    "\"avty_t\":\"" + String(TOPIC_AVAIL) + "\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\",";

  const char* names[] = {"Up", "Stop", "Down"};
  const char* topics[] = {
    TOPIC_RELAY1,
    TOPIC_RELAY2,
    TOPIC_RELAY3
  };

  for (int i = 0; i < 3; i++) {
    String id = "garage_relay" + String(i + 1);

    String payload =
      String("{") +
      "\"name\":\"" + names[i] + "\"," +
      "\"uniq_id\":\"" + id + "\"," +
      "\"cmd_t\":\"" + topics[i] + "\"," +
      "\"pl_prs\":\"PRESS\"," +
      "\"retain\":false," +
      availability +
      device +
      "}";

    String discoveryTopic =
      "homeassistant/button/" + id + "/config";

    mqttClient.publish(
      discoveryTopic.c_str(),
      payload.c_str(),
      true
    );
  }
}

// =====================================================
// MQTT
// =====================================================

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String clientId = String(DEVICE_NAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok = mqttClient.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASS,
    TOPIC_AVAIL,
    1,
    true,
    "offline"
  );

  if (ok) {
    Serial.println("MQTT connected");
    mqttClient.subscribe(TOPIC_RELAY1);
    mqttClient.subscribe(TOPIC_RELAY2);
    mqttClient.subscribe(TOPIC_RELAY3);
    mqttClient.subscribe(TOPIC_STATUS);

    publishAvailability("online");
    publishState("booted");
    publishStatus();
    publishDiscovery();
    mqttDownSinceMs = 0;
    return true;
  }

  Serial.print("MQTT connect failed, state=");
  Serial.println(mqttClient.state());
  return false;
}

void maintainMqtt() {
  const unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (mqttClient.connected()) {
      mqttClient.disconnect();
    }
    mqttDownSinceMs = 0;
    return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    mqttDownSinceMs = 0;
    return;
  }

  if (mqttDownSinceMs == 0) {
    mqttDownSinceMs = now;
    Serial.println("MQTT down");
  }

  if (now - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS) {
    lastMqttAttemptMs = now;
    connectMqtt();
  }

  if (mqttDownSinceMs != 0 && (now - mqttDownSinceMs >= MQTT_REBOOT_AFTER_MS)) {
    Serial.println("MQTT down too long, rebooting");
    delay(200);
    ESP.restart();
  }
}

// =====================================================
// STATUS TIMER
// =====================================================

void publishPeriodicStatus() {
  const unsigned long now = millis();

  if (now - lastStatusMs >= STATUS_PUBLISH_MS) {
    lastStatusMs = now;
    publishStatus();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  setRelaySafeState();
  digitalWrite(LED_PIN, LOW);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);

  startWifiConnect();

  Serial.println("Boot complete");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  updateRelayPulses();
  maintainWifi();
  maintainGateway();

  if (WiFi.status() == WL_CONNECTED) {
    startMdnsIfNeeded();
    startWebServerIfNeeded();
    webServer.handleClient();
  }

  maintainMqtt();
  publishPeriodicStatus();

  delay(5);
}