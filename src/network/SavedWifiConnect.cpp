#include "SavedWifiConnect.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "WifiCredentialStore.h"

namespace {
// WifiSelectionActivity's own figures: its auto-connect timeout, and the heap below
// which bringing the Wi-Fi driver up panics inside the SDK.
constexpr unsigned long JOIN_TIMEOUT_MS = 7000;
constexpr uint32_t MIN_FREE_HEAP_TO_START_WIFI = 60U * 1024U;
constexpr uint32_t MIN_MAX_ALLOC_TO_START_WIFI = 24U * 1024U;
constexpr unsigned long SCAN_TIMEOUT_MS = 10000;
constexpr unsigned long POLL_MS = 50;

bool isJoinFailure(const wl_status_t status) {
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) return true;
#ifndef SIMULATOR
  return status == WL_CONNECTION_LOST;
#else
  return false;
#endif
}

bool join(const WifiCredential& credential, const SavedWifiConnect::StopCheck shouldStop) {
  LOG_INF("WIFI", "Joining saved network %s in the background", credential.ssid.c_str());
  if (!WiFi.disconnect(false, false, 1000)) {
    LOG_DBG("WIFI", "Disconnect before begin timed out; continuing with begin");
  }
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (credential.password.empty()) {
    WiFi.begin(credential.ssid.c_str());
  } else {
    WiFi.begin(credential.ssid.c_str(), credential.password.c_str());
  }

  const unsigned long start = millis();
  wl_status_t status = WiFi.status();
  while (millis() - start < JOIN_TIMEOUT_MS && !shouldStop()) {
    status = WiFi.status();
    if (status == WL_CONNECTED) {
      LOG_INF("WIFI", "Joined %s (rssi=%d)", credential.ssid.c_str(), WiFi.RSSI());
      // Next time, the network that answered here is the first one tried.
      WIFI_STORE.setLastConnectedSsid(credential.ssid);
      return true;
    }
    if (isJoinFailure(status)) break;
    delay(POLL_MS);
  }
  if (shouldStop()) {
    LOG_INF("WIFI", "Stopped joining %s after %lums", credential.ssid.c_str(), millis() - start);
  } else {
    LOG_INF("WIFI", "Could not join %s (status=%d, %lums)", credential.ssid.c_str(), static_cast<int>(status),
            millis() - start);
  }
  WiFi.disconnect();
  return false;
}

struct VisibleNetwork {
  std::string ssid;
  int32_t rssi;
};

// Saved networks the scan can see, strongest first, leaving out `skipSsid`.
bool scanSavedNetworks(const std::string& skipSsid, const SavedWifiConnect::StopCheck shouldStop,
                       std::vector<VisibleNetwork>& out) {
  WiFi.disconnect();
  delay(100);
  if (WiFi.scanNetworks(true) != WIFI_SCAN_RUNNING) {
    LOG_ERR("WIFI", "Background scan did not start");
    return false;
  }
  const unsigned long start = millis();
  int found = WiFi.scanComplete();
  while (found == WIFI_SCAN_RUNNING) {
    if (shouldStop() || millis() - start >= SCAN_TIMEOUT_MS) {
      WiFi.scanDelete();
      return false;
    }
    delay(POLL_MS);
    found = WiFi.scanComplete();
  }
  if (found < 0) {
    LOG_INF("WIFI", "Background scan failed (%d)", found);
    return false;
  }

  out.reserve(WIFI_STORE.getCredentialCount());
  for (int i = 0; i < found; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    if (ssid[0] == '\0' || skipSsid == ssid) continue;
    const int32_t rssi = WiFi.RSSI(i);
    const auto seen = std::find_if(out.begin(), out.end(), [&ssid](const VisibleNetwork& n) { return n.ssid == ssid; });
    if (seen != out.end()) {
      seen->rssi = std::max(seen->rssi, rssi);
    } else if (WIFI_STORE.hasSavedCredential(ssid)) {
      out.push_back({ssid, rssi});
    }
  }
  WiFi.scanDelete();
  std::sort(out.begin(), out.end(), [](const VisibleNetwork& a, const VisibleNetwork& b) { return a.rssi > b.rssi; });
  return true;
}
}  // namespace

bool SavedWifiConnect::connect(const StopCheck shouldStop) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (WIFI_STORE.getCredentialCount() == 0) {
    LOG_INF("WIFI", "No saved network to join in the background");
    return false;
  }
  if (WiFi.getMode() == WIFI_OFF &&
      (ESP.getFreeHeap() < MIN_FREE_HEAP_TO_START_WIFI || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_TO_START_WIFI)) {
    LOG_ERR("WIFI", "Refusing to start WiFi on low memory: free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  WiFi.persistent(false);  // WifiCredentialStore owns the credentials; keep the SDK's NVS copy out of it
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("WIFI", "Failed to set station mode for a background join");
    return false;
  }
  // The name routers show, as WifiSelectionActivity sets it.
  uint8_t mac[6] = {};
  WiFi.macAddress(mac);
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  WiFi.setHostname(hostname);

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    const auto credential = WIFI_STORE.findCredential(lastSsid);
    if (credential && join(*credential, shouldStop)) return true;
  }
  if (shouldStop()) return false;

  std::vector<VisibleNetwork> visible;
  if (!scanSavedNetworks(lastSsid, shouldStop, visible)) return false;
  for (const VisibleNetwork& network : visible) {
    if (shouldStop()) return false;
    const auto credential = WIFI_STORE.findCredential(network.ssid);
    if (credential && join(*credential, shouldStop)) return true;
  }
  LOG_INF("WIFI", "No saved network joined in the background (%u visible)", static_cast<unsigned>(visible.size()));
  return false;
}
