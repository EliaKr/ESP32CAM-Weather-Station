#pragma once

// Create this file next to main.cpp (or in include/) and keep it out of git.
// Example:
//   #define WIFI_SSID "MyWifi"
//   #define WIFI_PASS "supersecret"

#define WIFI_SSID "MyWifi"
#define WIFI_PASS "supersecret"

// Static IP config (set USE_STATIC_IP to 1 to enable)
#define USE_STATIC_IP 0

// Example values (edit to match your LAN)
#define WIFI_LOCAL_IP   IPAddress(192,168,1,2)
#define WIFI_DNS_IP     IPAddress(1,1,1,1)
#define WIFI_GATEWAY_IP IPAddress(192, 168, 1, 1)
#define WIFI_SUBNET_IP  IPAddress(255, 255, 255, 0)
