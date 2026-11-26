#pragma once

#include <WiFi.h>

// Copy this file to secrets.h and configure your network.

// Wi-Fi
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Static IP configuration
IPAddress local_IP(192,168,1,87);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);
IPAddress dns(192,168,1,1);

// MQTT broker
const char* MQTT_HOST     = "192.168.1.10";
const uint16_t MQTT_PORT  = 1883;
const char* MQTT_USER     = "YOUR_MQTT_USERNAME";
const char* MQTT_PASS     = "YOUR_MQTT_PASSWORD";

// OTA web page login
const char* OTA_USER      = "YOUR_OTA_USERNAME";
const char* OTA_PASSWORD  = "YOUR_OTA_PASSWORD";
