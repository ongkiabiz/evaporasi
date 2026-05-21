// ============================================================
//  ota.h — OTA Update dari GitHub Releases (ESP32)
//
//  Alur:
//  1. Cek versi via GitHub API (ArduinoJson)
//  2. Resolve redirect URL secara manual (hemat RAM)
//  3. Download langsung dari CDN tanpa redirect
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
#define OTA_GITHUB_USER    "ongkiabiz"
#define OTA_GITHUB_REPO    "evaporasi"

// Versi firmware yang sedang berjalan di ESP32
#define FIRMWARE_VERSION   "1.0.1"

// Interval cek OTA
#define OTA_CHECK_INTERVAL  30000UL       // 30 detik (TESTING)
// #define OTA_CHECK_INTERVAL  21600000UL // 6 jam (PRODUCTION)

static unsigned long lastOtaCheck = 0;

// ============================================================
// FUNGSI: Resolve redirect URL via raw TCP (hemat RAM)
// Menghubungi github.com, ambil header Location, lalu tutup
// ============================================================
String resolveRedirectUrl(String githubUrl) {
  // Ekstrak host dan path dari URL
  // URL format: https://github.com/user/repo/releases/download/v1.0.2/file.bin
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

  // Kirim GET request, hanya butuh header-nya saja
  client.print("GET " + path + " HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("User-Agent: ESP32-OTA-Updater\r\n");
  client.print("Connection: close\r\n\r\n");

  // Baca header respons, cari "Location:"
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
      if (line.length() == 0) break; // Akhir header
    }
  }
  client.stop();
  delay(100); // Beri waktu socket benar-benar tertutup

  if (location.isEmpty()) {
    Serial.println("[OTA] Tidak ada redirect, pakai URL asli.");
    return githubUrl;
  }

  Serial.println("[OTA] CDN URL ditemukan!");
  return location;
}

// ============================================================
// Progress download sdadsa
// ============================================================
static void otaProgressCallback(int cur, int total) {
  static int lastPct = -1;
  int pct = (total > 0) ? (cur * 100 / total) : 0;
  if (pct != lastPct && pct % 10 == 0) {
    Serial.print("[OTA] Download: ");
    Serial.print(pct);
    Serial.println("%");
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
    return;
  }

  Serial.println("\n[OTA] ======== CEK UPDATE FIRMWARE ========");
  Serial.print("[OTA] Versi saat ini: ");
  Serial.println(FIRMWARE_VERSION);

  // ── Cek versi via GitHub API ────────────────────────────────
  String apiUrl = "https://api.github.com/repos/";
  apiUrl += OTA_GITHUB_USER;
  apiUrl += "/";
  apiUrl += OTA_GITHUB_REPO;
  apiUrl += "/releases/latest";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient http;
  http.setTimeout(10000);

  if (!http.begin(client, apiUrl)) {
    Serial.println("[OTA] Gagal begin HTTP.");
    Serial.println("[OTA] =========================================\n");
    return;
  }

  http.addHeader("User-Agent", "ESP32-OTA-Updater");
  http.addHeader("Accept", "application/vnd.github.v3+json");

  int httpCode = http.GET();
  Serial.print("[OTA] HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode != 200) {
    Serial.printf("[OTA] Gagal cek GitHub: HTTP %d\n", httpCode);
    http.end();
    Serial.println("[OTA] =========================================\n");
    return;
  }

  // Parse JSON dengan filter hemat memori
  StaticJsonDocument<128> filter;
  filter["tag_name"]                          = true;
  filter["assets"][0]["name"]                 = true;
  filter["assets"][0]["browser_download_url"] = true;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(
    doc, http.getStream(),
    DeserializationOption::Filter(filter)
  );
  http.end(); // Tutup segera setelah dapat data
  delay(200);

  if (err) {
    Serial.print("[OTA] Gagal parse JSON: ");
    Serial.println(err.c_str());
    Serial.println("[OTA] =========================================\n");
    return;
  }

  // Cek versi
  String latestTag = doc["tag_name"].as<String>();
  String latestVer = latestTag;
  if (latestVer.startsWith("v") || latestVer.startsWith("V")) {
    latestVer = latestVer.substring(1);
  }
  Serial.print("[OTA] Versi GitHub: ");
  Serial.println(latestVer);

  if (latestVer == String(FIRMWARE_VERSION)) {
    Serial.println("[OTA] Firmware sudah terbaru.");
    Serial.println("[OTA] =========================================\n");
    return;
  }

  // Cari file .bin di assets
  String binUrl = "";
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String nama = asset["name"].as<String>();
    if (nama.endsWith(".bin")) {
      binUrl = asset["browser_download_url"].as<String>();
      break;
    }
  }

  if (binUrl.isEmpty()) {
    Serial.println("[OTA] File .bin tidak ditemukan di release assets!");
    Serial.println("[OTA] =========================================\n");
    return;
  }

  Serial.printf("[OTA] *** UPDATE TERSEDIA! %s -> %s ***\n",
                FIRMWARE_VERSION, latestVer.c_str());
  Serial.println("[OTA] Download URL: " + binUrl);

  // ── Resolve redirect URL dulu (SEBELUM buka koneksi download) ──
  String finalUrl = resolveRedirectUrl(binUrl);
  Serial.println("[OTA] Final URL: " + finalUrl);

  // Beri jeda agar semua koneksi lama benar-benar tutup dan heap bebas
  Serial.println("[OTA] Memulai download & flash...");
  delay(1000);

  // ── Download & Flash menggunakan URL final (tanpa redirect) ────
  WiFiClientSecure clientFw;
  clientFw.setInsecure();
  clientFw.setTimeout(60000);

  httpUpdate.onProgress(otaProgressCallback);
  httpUpdate.rebootOnUpdate(true);
  // DISABLE redirect karena URL sudah CDN langsung
  httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(clientFw, finalUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] GAGAL! Error (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] Tidak ada update dari server.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] BERHASIL! ESP32 akan restart...");
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