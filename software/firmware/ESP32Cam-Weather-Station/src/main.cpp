#include <WiFi.h>
#include <WebServer.h>
#include "web/index_html.h"

// WiFi credentials
const char* ssid = "SSID";
const char* password = "PWD";

// Create web server on port 80
WebServer server(80);

// Forward declaration - ADD THIS LINE
void handleRoot();

void setup() {
  // Connect to WiFi
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  
  // Configure web server routes
  server.on("/", handleRoot);
  
  // Start web server
  server.begin();
}

void loop() {
  // Handle incoming client requests
  server.handleClient();
}

void handleRoot() {
  // Serve the HTML page from the separate file
  server.send(200, "text/html", index_html);
}