// ============================================================
//  ota.h — OTA Update GitHub Releases ESP32
//  Versi Stabil untuk Evaporimeter Otomatis
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
// KONFIGURASI OTA
// ============================================================

#define OTA_GITHUB_USER  "ongkiabiz"
#define OTA_GITHUB_REPO  "evaporasi"
// #define OTA_GITHUB_TOKEN "ghp_GANTI_TOKEN_BARU_KAMU"

#define FIRMWARE_VERSION    "v1.0.7"
#define OTA_CHECK_INTERVAL  3600000UL  // cek rutin tiap 1 jam

// ============================================================
// STATUS OTA
// ============================================================

struct OtaStatus {
  String versiSekarang;
  String versiTerbaru;
  String statusTerakhir;
  String errorMsg;
  String waktuCekTerakhir;
  int    progressPersen;
};

// Definisi ada di evaporasi.ino
extern OtaStatus otaStatus;

// ============================================================
// CALLBACK PROGRESS OTA
// ============================================================

inline void otaProgressCallback(int cur, int total) {

  static int lastPercent = -1;

  int percent = 0;

  if (total > 0) {
    percent = (cur * 100) / total;
  }

  otaStatus.progressPersen = percent;

  if (percent != lastPercent) {

    if (percent % 10 == 0) {
      Serial.printf("[OTA] Download %d%%\n", percent);
    }

    lastPercent = percent;
  }
}

// ============================================================
// FLAG FORCE OTA
// ============================================================

static bool _otaForceFlag = false;

// ============================================================
// FUNGSI CEK OTA
// ============================================================

inline void checkAndUpdateOTA() {

  static unsigned long lastOtaCheck = 0;

  // Cek interval — skip jika belum waktunya, kecuali force
  if (!_otaForceFlag &&
      lastOtaCheck != 0 &&
      millis() - lastOtaCheck < OTA_CHECK_INTERVAL) {
    return;
  }

  _otaForceFlag = false;
  lastOtaCheck  = millis();

  // Catat waktu cek
  struct tm tinfo;
  if (getLocalTime(&tinfo)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tinfo);
    otaStatus.waktuCekTerakhir = buf;
  }

  // Cek WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi tidak terhubung");
    otaStatus.statusTerakhir = "WiFi Putus";
    return;
  }

  Serial.println();
  Serial.println("[OTA] ===== CEK UPDATE =====");

  otaStatus.versiSekarang  = FIRMWARE_VERSION;
  otaStatus.progressPersen = -1;
  otaStatus.errorMsg       = "";

  // =========================================================
  // URL API GITHUB
  // =========================================================

  String url =
    "https://api.github.com/repos/" +
    String(OTA_GITHUB_USER) + "/" +
    String(OTA_GITHUB_REPO) + "/releases/latest";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  https.begin(client, url);
  https.setTimeout(10000);
  https.setConnectTimeout(8000);
  https.addHeader("User-Agent",     "ESP32");
  // https.addHeader("Authorization",  "token " + String(OTA_GITHUB_TOKEN));

  int httpCode = https.GET();

  Serial.printf("[OTA] HTTP Code: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "HTTP Error " + String(httpCode);
    https.end();
    return;
  }

  // =========================================================
  // PARSE JSON — filter hanya field yang dibutuhkan (hemat heap)
  // =========================================================

  StaticJsonDocument<128> filter;
  filter["tag_name"]                          = true;
  filter["assets"][0]["name"]                 = true;
  filter["assets"][0]["browser_download_url"] = true;

  DynamicJsonDocument doc(4096);

  DeserializationError err = deserializeJson(
    doc,
    https.getStream(),
    DeserializationOption::Filter(filter)
  );

  https.end();

  if (err) {
    otaStatus.statusTerakhir = "JSON Error";
    otaStatus.errorMsg       = err.c_str();
    Serial.printf("[OTA] JSON Error: %s\n", err.c_str());
    return;
  }

  // =========================================================
  // VERSI TERBARU
  // =========================================================

  String latestVersion = doc["tag_name"].as<String>();
  otaStatus.versiTerbaru = latestVersion;

  Serial.printf("[OTA] Versi saat ini : %s\n", FIRMWARE_VERSION);
  Serial.printf("[OTA] Versi GitHub   : %s\n", latestVersion.c_str());

  // =========================================================
  // SUDAH TERBARU
  // =========================================================

  if (latestVersion == String(FIRMWARE_VERSION)) {
    Serial.println("[OTA] Firmware sudah terbaru");
    otaStatus.statusTerakhir = "Terbaru";
    return;
  }

  Serial.printf("[OTA] Update tersedia! %s → %s\n",
    FIRMWARE_VERSION, latestVersion.c_str());

  // =========================================================
  // CARI FILE .BIN DI ASSETS
  // =========================================================

  String firmwareUrl = "";

  for (JsonObject asset : doc["assets"].as<JsonArray>()) {

    String name = asset["name"].as<String>();

    if (name.endsWith(".bin")) {
      firmwareUrl = asset["browser_download_url"].as<String>();
      break;
    }
  }

  if (firmwareUrl.isEmpty()) {
    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg       = "File BIN tidak ditemukan";
    Serial.println("[OTA] File .bin tidak ditemukan di release!");
    Serial.println("[OTA] Pastikan kamu upload file .bin saat buat release.");
    return;
  }

  Serial.println("[OTA] URL: " + firmwareUrl);

  // =========================================================
  // OTA UPDATE
  // =========================================================

  Serial.println("[OTA] Mulai download & flash...");

  otaStatus.statusTerakhir = "Updating";
  otaStatus.progressPersen = 0;

  WiFiClientSecure updateClient;
  updateClient.setInsecure();
  updateClient.setTimeout(60);  // 60 detik untuk download

  httpUpdate.onProgress(otaProgressCallback);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret =
    httpUpdate.update(updateClient, firmwareUrl);

  // Jika rebootOnUpdate(true) dan OK, baris di bawah tidak akan tercapai
  switch (ret) {

    case HTTP_UPDATE_FAILED:
      otaStatus.statusTerakhir = "Gagal";
      otaStatus.errorMsg       = httpUpdate.getLastErrorString();
      Serial.printf("[OTA] Gagal: (%d) %s\n",
        httpUpdate.getLastError(),
        httpUpdate.getLastErrorString().c_str());
      break;

    case HTTP_UPDATE_NO_UPDATES:
      otaStatus.statusTerakhir = "Tidak Ada Update";
      Serial.println("[OTA] Server bilang tidak ada update");
      break;

    case HTTP_UPDATE_OK:
      // Tidak akan sampai sini karena rebootOnUpdate(true)
      otaStatus.statusTerakhir = "Berhasil";
      otaStatus.progressPersen = 100;
      Serial.println("[OTA] Berhasil! ESP akan restart...");
      break;
  }

  Serial.println("[OTA] =====================");
}

// ============================================================
// FORCE OTA — bypass interval 1 jam, langsung cek GitHub
// Dipanggil dari bacaSettingsFirebase() saat trigger Firebase aktif
// ============================================================

inline void forceCheckOTA() {
  _otaForceFlag = true;
  checkAndUpdateOTA();
}

#endif