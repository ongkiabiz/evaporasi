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

#define OTA_GITHUB_USER "ongkiabiz"
#define OTA_GITHUB_REPO "evaporasi"

#define FIRMWARE_VERSION "v1.0.1"

// Interval cek OTA
#define OTA_CHECK_INTERVAL 30000UL

// ============================================================
// STATUS OTA
// ============================================================

struct OtaStatus {
  String versiSekarang;
  String versiTerbaru;
  String statusTerakhir;
  String errorMsg;
  String waktuCekTerakhir;
  int progressPersen;
};

// deklarasi extern
extern OtaStatus otaStatus;

// ============================================================
// TIMER OTA
// ============================================================

static unsigned long lastOtaCheck = 0;

// ============================================================
// CALLBACK PROGRESS OTA
// ============================================================

void otaProgressCallback(int cur, int total) {

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
// FUNGSI CEK OTA
// ============================================================

void checkAndUpdateOTA() {

  // interval
  if (millis() - lastOtaCheck < OTA_CHECK_INTERVAL && lastOtaCheck != 0) {
    return;
  }

  lastOtaCheck = millis();

  // wifi check
  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("[OTA] WiFi tidak terhubung");

    otaStatus.statusTerakhir = "WiFi Putus";

    return;
  }

  Serial.println();
  Serial.println("[OTA] ===== CEK UPDATE =====");

  otaStatus.versiSekarang = FIRMWARE_VERSION;
  otaStatus.progressPersen = -1;
  otaStatus.errorMsg = "";

  // =========================================================
  // URL API GITHUB
  // =========================================================

  String url =
    "https://api.github.com/repos/" + String(OTA_GITHUB_USER) + "/" + String(OTA_GITHUB_REPO) + "/releases/latest";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  https.begin(client, url);

  https.addHeader("User-Agent", "ESP32");

  int httpCode = https.GET();

  Serial.printf("[OTA] HTTP Code: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {

    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg =
      "HTTP Error " + String(httpCode);

    https.end();

    return;
  }

  // =========================================================
  // AMBIL JSON
  // =========================================================

  String payload = https.getString();

  https.end();

  DynamicJsonDocument doc(8192);

  DeserializationError err =
    deserializeJson(doc, payload);

  if (err) {

    otaStatus.statusTerakhir = "JSON Error";
    otaStatus.errorMsg = err.c_str();

    Serial.println("[OTA] JSON Error");

    return;
  }

  // =========================================================
  // VERSI TERBARU
  // =========================================================

  String latestVersion =
    doc["tag_name"].as<String>();

  otaStatus.versiTerbaru = latestVersion;

  Serial.printf(
    "[OTA] Versi GitHub: %s\n",
    latestVersion.c_str());

  // =========================================================
  // SUDAH TERBARU
  // =========================================================

  if (latestVersion == String(FIRMWARE_VERSION)) {

    Serial.println("[OTA] Firmware terbaru");

    otaStatus.statusTerakhir = "Terbaru";

    return;
  }

  // =========================================================
  // CARI FILE BIN
  // =========================================================

  String firmwareUrl = "";

  JsonArray assets = doc["assets"];

  for (JsonObject asset : assets) {

    String name = asset["name"].as<String>();

    if (name.endsWith(".bin")) {

      firmwareUrl =
        asset["browser_download_url"].as<String>();

      break;
    }
  }

  if (firmwareUrl == "") {

    otaStatus.statusTerakhir = "Gagal";
    otaStatus.errorMsg = "File BIN tidak ditemukan";

    Serial.println("[OTA] BIN tidak ditemukan");

    return;
  }

  // =========================================================
  // OTA UPDATE
  // =========================================================

  Serial.println("[OTA] Mulai update firmware");

  otaStatus.statusTerakhir = "Updating";
  otaStatus.progressPersen = 0;

  WiFiClientSecure updateClient;
  updateClient.setInsecure();

  httpUpdate.onProgress(otaProgressCallback);

  t_httpUpdate_return ret =
    httpUpdate.update(updateClient, firmwareUrl);

  switch (ret) {

    case HTTP_UPDATE_FAILED:

      otaStatus.statusTerakhir = "Gagal";

      otaStatus.errorMsg =
        httpUpdate.getLastErrorString();

      Serial.printf(
        "[OTA] Gagal: %s\n",
        httpUpdate.getLastErrorString().c_str());

      break;

    case HTTP_UPDATE_NO_UPDATES:

      otaStatus.statusTerakhir = "Tidak Ada Update";

      Serial.println("[OTA] Tidak ada update");

      break;

    case HTTP_UPDATE_OK:

      otaStatus.statusTerakhir = "Berhasil";

      otaStatus.progressPersen = 100;

      Serial.println("[OTA] Update berhasil");

      break;
  }

  Serial.println("[OTA] =====================");
}

// ============================================================
// FORCE OTA
// ============================================================

void forceCheckOTA() {

  lastOtaCheck = 0;

  checkAndUpdateOTA();
}

#endif