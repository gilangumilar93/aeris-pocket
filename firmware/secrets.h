#pragma once

// Local network only. The sketch publishes plaintext MQTT.
// For a broker off your LAN, switch the client to WiFiClientSecure
// and load the CA — do not ship passwords in a photo of this file.

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-pass"

#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""

// Full topics become aeris/pocket/<mac>/state
#define MQTT_PREFIX "aeris/pocket"
