// ============================================================
//  ota.h — OTA Update dari GitHub Releases (ESP32)
//  + Status OTA bisa dipantau via Firebase Realtime Database
// ============================================================

#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ============================================================
// KONFIGURASI GITHUB
// ============================================================
#define OTA_GITHUB_USER     "ongkiabiz"
#define OTA_GITHUB_REPO     "evaporasi"
#define FIRMWARE_VERSION    "v1.0.3"
#define OTA_CHECK_INTERVAL  21600000UL   // 6 jam (PRODUCTION)
// #define OTA_CHECK_INTERVAL  30000UL   // 30 detik (TESTING)

static unsigned long lastOtaCheck = 0;

// ============================================================
// STRUCT STATUS OTA
// Definisi di sini, instance (otaStatus) di evaporasi.ino
// ============================================================
struct OtaStatus {
  String versiSekarang;
  String versiTerbaru;
  String statusTerakhir;
  String errorMsg;
  String waktuCekTerakhir;
  int    progressPersen;
};

// Dideklarasikan extern — instance ada di evaporasi.ino
extern OtaStatus otaStatus;

// ============================================================
// FUNGSI: Resolve redirect URL via raw TCP
// ============================================================
String resolveRedirectUrl(String githubUrl) {
  String host = "github.com";
  String path = githubUrl;
  path.replace("https://github.com", "");
  path.replace("http://github.com", "");

  Serial.println("[OTA] Resolve URL redirect...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  if (!client.connect(host.c_str(), 443)) {
    Serial.println("[OTA] Gagal konek ke github.com");
    return githubUrl;
  }

  client.print("GET " + path + " HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("User-Agent: ESP32-OTA-Updater\r\n");
  client.print("Connection: close\r\n\r\n");

  String location = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 8000) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.startsWith("Location: ") || line.startsWith("location: ")) {
        location = line.substring(10);
        location.trim();
        break;
      }
      if (line.length() == 0) break;
    }
  }
  client.stop();
  delay(100);

  if (location.isEmpty()) {
    Serial.println("[OTA] Tidak ada redirect, pakai URL asli.");
    return githubUrl;
  }
  Serial.println("[OTA] CDN URL ditemukan!");
  return location;
}

// ============================================================
// Progress callback — update otaStatus.progressPersen live
// ============================================================
static void otaProgressCallback(int cur, int total) {
  static int lastPct = -1;
  int pct = (total > 0) ? (cur * 100 / total) : 0;
  otaStatus.progressPersen = pct;
  if (pct != lastPct && pct % 10 == 0) {
    Serial.printf("[OTA] Download: %d%%\n", pct);
    lastPct = pct;
  }
}

// ============================================================
// FUNGSI UTAMA: Cek update & jalankan OTA dari GitHub
// ============================================================
void checkAndUpdateOTA() {
  if (millis() - lastOtaCheck < OTA_CHECK_INTERVAL && lastOtaCheck != 0) return;
  lastOtaCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi tidak terhubung, skip.");
    otaStatus.statusTerakhir = "WiFi Putus";
    return;
  }

  Serial.println("\n[OTA] ======== CEK UPDATE FIRMWARE ========");
  Serial.printf("[OTA] Versi saat ini: %s\n", FIRMWARE_VERSION);

  otaStatus.progressPersen = -1;
  otaStatus.errorMsg       = "";

  String apiUrl = "https://api.github.com/repos/";
  apiUrl += OTA_GITHUB_USER;
  apiUrl += "/";
  apiUrl += OTA_GITHUB_REPO;
  apiUrl += "/releases/latest";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);

  if (!http.begin(client, apiUrl)) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "Gagal konek ke GitHub API";
    http.end();
    return;
  }

  http.addHeader("User-Agent", "ESP32-OTA-Updater");
  http.addHeader("Accept", "application/vnd.github.v3+json");

  int httpCode = http.GET();
  Serial.printf("[OTA] HTTP Code: %d\n", httpCode);

  if (httpCode != 200) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "HTTP Error: " + String(httpCode);
    http.end();
    return;
  }

  StaticJsonDocument<128> filter;
  filter["tag_name"]                          = true;
  filter["assets"][0]["name"]                 = true;
  filter["assets"][0]["browser_download_url"] = true;

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(
    doc, payload, DeserializationOption::Filter(filter)
  );
  delay(200);

  if (err) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "Parse JSON error: " + String(err.c_str());
    return;
  }

  String latestVer = doc["tag_name"].as<String>();
  otaStatus.versiTerbaru = latestVer;

  if (!doc["assets"].is<JsonArray>()) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "Assets tidak ditemukan di release";
    return;
  }

  Serial.printf("[OTA] Versi GitHub: %s\n", latestVer.c_str());

  if (latestVer == String(FIRMWARE_VERSION)) {
    Serial.println("[OTA] Firmware sudah terbaru.");
    otaStatus.statusTerakhir = "Terbaru";
    Serial.println("[OTA] =========================================\n");
    return;
  }

  String binUrl = "";
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String nama = asset["name"].as<String>();
    if (nama.endsWith(".bin")) {
      binUrl = asset["browser_download_url"].as<String>();
      break;
    }
  }

  if (binUrl.isEmpty()) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "File .bin tidak ada di release";
    return;
  }

  Serial.printf("[OTA] *** UPDATE: %s -> %s ***\n", FIRMWARE_VERSION, latestVer.c_str());
  otaStatus.statusTerakhir = "Mengupdate";
  otaStatus.progressPersen = 0;

  String finalUrl = resolveRedirectUrl(binUrl);
  Serial.println("[OTA] Memulai download & flash...");
  delay(1000);

  WiFiClientSecure clientFw;
  clientFw.setInsecure();
  clientFw.setTimeout(60000);

  httpUpdate.onProgress(otaProgressCallback);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(clientFw, finalUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      otaStatus.statusTerakhir = "Gagal";
      otaStatus.progressPersen = -1;
      otaStatus.errorMsg = "Error(" + String(httpUpdate.getLastError()) + "): "
                           + httpUpdate.getLastErrorString();
      Serial.println("[OTA] GAGAL! " + otaStatus.errorMsg);
      break;
    case HTTP_UPDATE_NO_UPDATES:
      otaStatus.statusTerakhir = "Terbaru";
      otaStatus.progressPersen = -1;
      break;
    case HTTP_UPDATE_OK:
      otaStatus.statusTerakhir = "Berhasil";
      otaStatus.progressPersen = 100;
      break;
  }

  Serial.println("[OTA] =========================================\n");
}

// ============================================================
// Paksa cek OTA sekarang (tanpa tunggu interval)
// ============================================================
void forceCheckOTA() {
  lastOtaCheck = 0;
  checkAndUpdateOTA();
}

#endif // OTA_H