#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "web/index_html.h"

// Define which mode to use (comment out one)
//#define USE_WIFI_MODE
#define USE_SOFTAP_MODE

// WiFi credentials
const char* ssid = "-";
const char* password = "-";

// SoftAP credentials
const char* ap_ssid = "ESP32-Weather";
const char* ap_password = "12345678";

// LED pin
#define LED_PIN 4

// Create web server on port 80
WebServer server(80);

// Forward declaration
void handleRoot();

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
#ifdef USE_WIFI_MODE
  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  digitalWrite(LED_PIN, HIGH);
  delay(500);
  digitalWrite(LED_PIN, LOW);
  
#elif defined(USE_SOFTAP_MODE)
  // Start SoftAP mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  digitalWrite(LED_PIN, HIGH);
  delay(500);
  digitalWrite(LED_PIN, LOW);
  
#endif
  
  // Configure web server routes
  server.on("/", handleRoot);
  
  // Start web server
  server.begin();
}

void loop() {
  server.handleClient();
}

void handleRoot() {
  server.send(200, "text/html", index_html);
}