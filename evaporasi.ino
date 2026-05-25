// ============================================================
//  EVAPORIMETER OTOMATIS ESP32 — NON-BLOCKING STATE MACHINE
//  Perbaikan:
//    1. Reset jam 07:00 non-blocking (state machine)
//    2. Relay pompa dengan logika ketinggian (bukan jam saja)
//    3. yield() di hitungTinggi() agar WDT tidak triggered
//    4. Log nilai evaporasi asli sebelum di-clamp
//    5. Reconnect Firebase otomatis jika terputus
// ============================================================

#include <WiFi.h>
#include <FirebaseESP32.h>
#include "time.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ota.h"

// Instance otaStatus — struct OtaStatus didefinisikan di ota.h
OtaStatus otaStatus = {
  FIRMWARE_VERSION,  // versiSekarang
  "-",               // versiTerbaru
  "Belum Dicek",     // statusTerakhir
  "",                // errorMsg
  "-",               // waktuCekTerakhir
  -1                 // progressPersen
};

// ============================================================
// KONFIGURASI
// ============================================================
#define WIFI_SSID       "Klimatologiot"
#define WIFI_PASSWORD   "Klimatologiotkk"

#define FIREBASE_HOST   "klimatologiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "AIzaSyAZrk_k4DQ_ijCa6gp67oRklFMKD2dLcbQ"

#define PATH_REALTIME   "/Monitoring/realtime"
#define PATH_HISTORY    "/Monitoring/History"
#define PATH_ACUAN      "/Monitoring/acuan_pagi_cm"
#define PATH_OTA_TRIGGER "/Monitoring/ota_trigger"  // "true" dari aplikasi = paksa cek OTA

// Pin
#define HX710B_OUT      19
#define HX710B_SCK      18
#define DS18B20_PIN     27
#define RELAY_PIN       32

// ============================================================
// KALIBRASI HX710B
// ============================================================
const long  D0              = 9626456L;
const long  DMAX            = 10767088L;
const float TINGGI_ACUAN_CM = 20.0;

// Batas relay: pompa aktif jika air di bawah level ini
const float RELAY_BATAS_BAWAH = TINGGI_ACUAN_CM - 2.0; // 18 cm
const float RELAY_BATAS_ATAS  = TINGGI_ACUAN_CM - 0.5; // 19.5 cm (pompa berhenti)

// Interval
const unsigned long INTERVAL_REALTIME = 300000UL;   // 5 menit
const unsigned long INTERVAL_HISTORY  = 600000UL;   // 10 menit
const unsigned long INTERVAL_BACA     = 10000UL;    // baca sensor tiap 10 detik
const unsigned long DURASI_RESET      = 600000UL;   // sampling reset selama 10 menit
const unsigned long INTERVAL_OTA_CEK  = 15000UL;    // cek trigger Firebase tiap 15 detik

// ============================================================
// STATE MACHINE — RESET HARIAN
// ============================================================
enum ResetState { RESET_IDLE, RESET_SAMPLING, RESET_SELESAI };
ResetState  resetState   = RESET_IDLE;
unsigned long resetMulai = 0;
float  resetTotal        = 0.0;
int    resetJumlah       = 0;

// ============================================================
// OBJEK & VARIABEL GLOBAL
// ============================================================
FirebaseData      fbdo;
FirebaseAuth      auth;
FirebaseConfig    config;
OneWire           oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

unsigned long lastRealtime   = 0;
unsigned long lastHistory    = 0;
unsigned long lastBaca       = 0;
unsigned long lastOtaCekTrigger = 0;  // Interval cek flag OTA dari Firebase

float  tinggiPagi_cm         = -1.0;
float  tinggiSebelumnya      = -1.0;
bool   sudahResetHari        = false;
int    hariSebelumnya        = -1;
String statusEvaporasi       = "Normal";
bool   relayAktif            = false;  // Tracking state relay

// ============================================================
// FUNGSI: Baca satu sampel HX710B dengan yield() agar WDT aman
// ============================================================
unsigned long bacaSatuSampel(bool &sukses) {
  sukses = true;
  unsigned long t0 = millis();

  while (digitalRead(HX710B_OUT) == HIGH) {
    if (millis() - t0 > 2000) {
      Serial.println("[HX710B] TIMEOUT - Sensor tidak merespons!");
      sukses = false;
      return 0;
    }
    yield(); // <-- beri kesempatan ke task lain, cegah WDT
  }

  unsigned long count = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(HX710B_SCK, HIGH);
    delayMicroseconds(1);
    count <<= 1;
    digitalWrite(HX710B_SCK, LOW);
    delayMicroseconds(1);
    if (digitalRead(HX710B_OUT)) count++;
  }

  // Pulse ke-25 untuk mode gain berikutnya
  digitalWrite(HX710B_SCK, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX710B_SCK, LOW);

  count ^= 0x800000;
  return count;
}

// ============================================================
// FUNGSI: Hitung tinggi air (cm) — dengan yield() di loop
// ============================================================
float hitungTinggi() {
  const int N = 50;
  bool sukses;

  // Warm-up 5 sampel
  for (int i = 0; i < 5; i++) {
    bacaSatuSampel(sukses);
    delay(10);
    yield();
  }

  unsigned long sampel[N];
  int sampelValid = 0;

  for (int i = 0; i < N; i++) {
    unsigned long val = bacaSatuSampel(sukses);
    if (sukses) {
      sampel[sampelValid] = val;
      sampelValid++;
    }
    delay(5);
    yield(); // <-- cegah WDT di loop panjang
  }

  if (sampelValid < 25) {
    Serial.println("[HX710B] Sampel valid tidak cukup (<25), return -99");
    return -99.0;
  }

  // Insertion sort
  for (int i = 1; i < sampelValid; i++) {
    unsigned long kunci = sampel[i];
    int j = i - 1;
    while (j >= 0 && sampel[j] > kunci) {
      sampel[j + 1] = sampel[j];
      j--;
    }
    sampel[j + 1] = kunci;
  }

  // Ambil 20-80 persentil (buang outlier)
  int awal  = sampelValid * 20 / 100;
  int akhir = sampelValid * 80 / 100;
  unsigned long total = 0;
  for (int i = awal; i < akhir; i++) total += sampel[i];
  long D_rata = (long)(total / (akhir - awal));

  Serial.print("[SENSOR] Raw ADC Rata-rata: ");
  Serial.println(D_rata);

  float tinggi = (float)(D_rata - D0) * TINGGI_ACUAN_CM / (float)(DMAX - D0);

  // Clamp
  if (tinggi < 0.0)  tinggi = 0.0;
  if (tinggi > 30.0) tinggi = 30.0;

  return tinggi;
}

// ============================================================
// FUNGSI: Firebase Helper
// ============================================================
void simpanAcuanPagi(float nilai) {
  if (!Firebase.ready()) return;
  if (Firebase.setFloat(fbdo, PATH_ACUAN, nilai)) {
    Serial.printf("[Firebase] Acuan pagi diperbarui: %.3f cm\n", nilai);
  } else {
    Serial.println("[Firebase] Gagal simpan acuan: " + fbdo.errorReason());
  }
}

bool bacaAcuanDariFirebase(float &hasil) {
  if (Firebase.ready() && Firebase.getFloat(fbdo, PATH_ACUAN)) {
    hasil = fbdo.floatData();
    if (hasil > 0.0 && hasil <= 30.0) return true;
  }
  return false;
}

// ============================================================
// FUNGSI: Kontrol Relay Pompa (Non-blocking, level-based)
// ============================================================
//  - Pompa ON  jika air turun di bawah RELAY_BATAS_BAWAH
//  - Pompa OFF jika air sudah mencapai RELAY_BATAS_ATAS
//  - Hanya aktif pada jam 06:00–08:00 sebagai jendela aman
// ============================================================
void kontrolRelay(float tinggi_cm, int jam) {
  bool jamOperasional = (jam >= 6 && jam <= 8);

  if (!jamOperasional) {
    // Di luar jam operasional, pastikan relay mati
    if (relayAktif) {
      digitalWrite(RELAY_PIN, LOW);
      relayAktif = false;
      Serial.println("[RELAY] OFF — di luar jam operasional");
    }
    return;
  }

  // Logika hysteresis: cegah relay nyala-mati terus-menerus
  if (!relayAktif && tinggi_cm < RELAY_BATAS_BAWAH) {
    digitalWrite(RELAY_PIN, HIGH);
    relayAktif = true;
    Serial.printf("[RELAY] ON — tinggi air %.2f cm < batas %.2f cm\n",
                  tinggi_cm, RELAY_BATAS_BAWAH);
  } else if (relayAktif && tinggi_cm >= RELAY_BATAS_ATAS) {
    digitalWrite(RELAY_PIN, LOW);
    relayAktif = false;
    Serial.printf("[RELAY] OFF — tinggi air %.2f cm sudah mencapai %.2f cm\n",
                  tinggi_cm, RELAY_BATAS_ATAS);
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== EVAPORIMETER STARTUP ===");

  pinMode(HX710B_OUT, INPUT);
  pinMode(HX710B_SCK, OUTPUT);
  digitalWrite(HX710B_SCK, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  sensors.begin();
  sensors.setResolution(12);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  unsigned long tWifi = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - tWifi > 15000) {
      Serial.println("\n[WiFi] Gagal! Restart...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
    yield();
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  // NTP
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sinkronisasi NTP");
  struct tm t;
  unsigned long tNtp = millis();
  while (!getLocalTime(&t)) {
    if (millis() - tNtp > 10000) { Serial.println(" GAGAL!"); break; }
    delay(500); Serial.print(".");
    yield();
  }
  Serial.println(" OK!");

  // Firebase
  config.host                       = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000);

  // Ambil acuan pagi
  Serial.println("Mengambil acuan pagi dari Firebase...");
  if (bacaAcuanDariFirebase(tinggiPagi_cm)) {
    Serial.printf("[OK] Acuan dari Firebase: %.3f cm\n", tinggiPagi_cm);
  } else {
    Serial.println("[INFO] Tidak ada data Firebase, ukur dari sensor...");
    delay(2000);
    float acuanAwal = hitungTinggi();
    tinggiPagi_cm = (acuanAwal != -99.0) ? acuanAwal : TINGGI_ACUAN_CM;
    simpanAcuanPagi(tinggiPagi_cm);
  }

  // Set baseline pembanding pengisian
  float tinggiAwal = hitungTinggi();
  if (tinggiAwal != -99.0) tinggiSebelumnya = tinggiAwal;

  Serial.println("=== SETUP SELESAI ===\n");
}

// ============================================================
// LOOP — Semua non-blocking
// ============================================================
void loop() {
  // --- Reconnect WiFi ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Putus, reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(5000);
    return;
  }

  // --- OTA: cek trigger dari Firebase tiap 15 detik ---
  if (millis() - lastOtaCekTrigger >= INTERVAL_OTA_CEK) {
    lastOtaCekTrigger = millis();
    if (Firebase.ready()) {
      if (Firebase.getBool(fbdo, PATH_OTA_TRIGGER)) {
        bool trigger = fbdo.boolData();
        if (trigger) {
          Serial.println("[OTA-TRIGGER] Perintah dari Firebase diterima!");
          Serial.println("[OTA-TRIGGER] Memulai cek OTA paksa...");

          // Reset flag di Firebase dulu agar tidak loop
          Firebase.setBool(fbdo, PATH_OTA_TRIGGER, false);

          // Catat waktu
          struct tm tOta;
          if (getLocalTime(&tOta)) {
            char bufOta[25];
            strftime(bufOta, sizeof(bufOta), "%Y-%m-%d %H:%M:%S", &tOta);
            otaStatus.waktuCekTerakhir = String(bufOta);
          }

          // Paksa cek OTA tanpa tunggu interval 6 jam
          forceCheckOTA();
        }
      }
    }
  }

  // --- OTA: cek rutin setiap 6 jam (otomatis, default) ---
  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL || lastOtaCheck == 0) {
    struct tm tOta;
    if (getLocalTime(&tOta)) {
      char bufOta[25];
      strftime(bufOta, sizeof(bufOta), "%Y-%m-%d %H:%M:%S", &tOta);
      otaStatus.waktuCekTerakhir = String(bufOta);
    }
  }
  checkAndUpdateOTA();

  // --- Throttle: baca sensor tiap INTERVAL_BACA ---
  if (millis() - lastBaca < INTERVAL_BACA) return;
  lastBaca = millis();

  // ── 1. BACA SENSOR TINGGI AIR ──
  float tinggi_cm = hitungTinggi();
  if (tinggi_cm == -99.0) {
    Serial.println("[WARNING] Sensor TIMEOUT, pengukuran dilewati.");
    return;
  }

  // ── 2. DETEKSI PENGISIAN AIR (AUTO-RESET CERDAS) ──
  if (tinggiSebelumnya > 0.0) {
    float delta = tinggi_cm - tinggiSebelumnya;
    if (delta > 1.0) {
      Serial.println("\n[AUTO-RESET] Terdeteksi pengisian air!");
      Serial.printf("  Sebelumnya : %.3f cm\n", tinggiSebelumnya);
      Serial.printf("  Sekarang   : %.3f cm\n", tinggi_cm);
      tinggiPagi_cm = tinggi_cm;
      simpanAcuanPagi(tinggiPagi_cm);
      Serial.println("[AUTO-RESET] Acuan diperbarui otomatis.\n");
    }
  }
  tinggiSebelumnya = tinggi_cm;

  // ── 3. SUHU AIR ──
  sensors.requestTemperatures();
  float suhuAir = sensors.getTempCByIndex(0);
  if (suhuAir == DEVICE_DISCONNECTED_C) suhuAir = -99.0;

  // ── 4. WAKTU ──
  struct tm timeinfo;
  String waktu    = "--:--:--";
  String tanggal  = "0000-00-00";
  String datetime = "0000-00-00 00:00:00";
  bool   adaWaktu = getLocalTime(&timeinfo);

  if (adaWaktu) {
    char buf[25];
    strftime(buf, sizeof(buf), "%H:%M:%S",           &timeinfo); waktu    = buf;
    strftime(buf, sizeof(buf), "%Y-%m-%d",            &timeinfo); tanggal  = buf;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",  &timeinfo); datetime = buf;

    // Reset flag jika hari berganti
    if (hariSebelumnya != timeinfo.tm_mday) {
      hariSebelumnya = timeinfo.tm_mday;
      sudahResetHari = false;
      resetState     = RESET_IDLE;
    }

    // ── 5. STATE MACHINE RESET HARIAN JAM 07:00 (NON-BLOCKING) ──
    switch (resetState) {

      case RESET_IDLE:
        // Mulai sampling saat jam tepat 07:00 dan belum reset hari ini
        if (timeinfo.tm_hour == 7 && timeinfo.tm_min == 0 && !sudahResetHari) {
          Serial.println("========================================");
          Serial.println("[RESET] Memulai sampling acuan pagi...");
          Serial.println("========================================");
          resetMulai  = millis();
          resetTotal  = 0.0;
          resetJumlah = 0;
          resetState  = RESET_SAMPLING;
        }
        break;

      case RESET_SAMPLING:
        // Kumpulkan data tinggi_cm yang sudah dibaca di atas
        if (tinggi_cm > 0.0 && tinggi_cm < 30.0) {
          resetTotal  += tinggi_cm;
          resetJumlah++;
          Serial.printf("[RESET] Sample #%d: %.3f cm\n", resetJumlah, tinggi_cm);
        }
        // Selesai setelah DURASI_RESET (10 menit)
        if (millis() - resetMulai >= DURASI_RESET) {
          resetState = RESET_SELESAI;
        }
        break;

      case RESET_SELESAI:
        if (resetJumlah > 0) {
          tinggiPagi_cm = resetTotal / resetJumlah;
          simpanAcuanPagi(tinggiPagi_cm);
          Serial.printf("[RESET] Selesai. Acuan baru: %.3f cm dari %d sampel\n",
                        tinggiPagi_cm, resetJumlah);
        } else {
          Serial.println("[RESET] Tidak ada sampel valid, acuan tidak diubah.");
        }
        sudahResetHari = true;
        resetState     = RESET_IDLE;
        break;
    }

    // ── 6. KONTROL RELAY POMPA (Non-blocking, level-based) ──
    kontrolRelay(tinggi_cm, timeinfo.tm_hour);
  }

  // ── 7. HITUNG EVAPORASI ──
  float evaporasi_cm_raw = tinggiPagi_cm - tinggi_cm; // nilai asli sebelum clamp
  float evaporasi_cm     = evaporasi_cm_raw;
  if (evaporasi_cm < 0.0) evaporasi_cm = 0.0;         // clamp negatif → 0
  float evaporasi_mm     = evaporasi_cm * 10.0;

  // Log jika evaporasi negatif (anomali / noise)
  if (evaporasi_cm_raw < -0.5) {
    Serial.printf("[ANOMALI] Evaporasi raw negatif: %.3f cm — mungkin noise sensor\n",
                  evaporasi_cm_raw);
  }

  // ── 8. STATUS EVAPORASI ──
  if      (evaporasi_mm > 10.0) statusEvaporasi = "Tinggi";
  else if (evaporasi_mm >= 2.0) statusEvaporasi = "Normal";
  else                          statusEvaporasi = "Rendah";

  // ── 9. KIRIM REALTIME KE FIREBASE (tiap 5 menit) ──
  if (millis() - lastRealtime >= INTERVAL_REALTIME) {
    lastRealtime = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonRT;
      jsonRT.add("tinggi_air_cm",  tinggi_cm);
      jsonRT.add("acuan_pagi_cm",  tinggiPagi_cm);
      jsonRT.add("evaporasi_mm",   evaporasi_mm);
      jsonRT.add("suhu_air_c",     suhuAir);
      jsonRT.add("status",         statusEvaporasi);
      jsonRT.add("relay_aktif",    relayAktif);
      jsonRT.add("reset_state",    (int)resetState);
      jsonRT.add("datetime",       datetime);
      // OTA Status
      jsonRT.add("ota_versi_device",   otaStatus.versiSekarang);
      jsonRT.add("ota_versi_terbaru",  otaStatus.versiTerbaru);
      jsonRT.add("ota_status",         otaStatus.statusTerakhir);
      jsonRT.add("ota_progress",       otaStatus.progressPersen);
      jsonRT.add("ota_error",          otaStatus.errorMsg);
      jsonRT.add("ota_waktu_cek",      otaStatus.waktuCekTerakhir);

      if (Firebase.updateNode(fbdo, PATH_REALTIME, jsonRT)) {
        Serial.println("[Firebase] Realtime OK.");
      } else {
        Serial.println("[Firebase] Realtime GAGAL: " + fbdo.errorReason());
      }
    } else {
      Serial.println("[Firebase] Tidak siap, skip realtime.");
    }
  }

  // ── 10. SIMPAN HISTORY (tiap 10 menit) ──
  if (millis() - lastHistory >= INTERVAL_HISTORY) {
    lastHistory = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonH;
      jsonH.add("tinggi_air_cm",  tinggi_cm);
      jsonH.add("evaporasi_mm",   evaporasi_mm);
      jsonH.add("suhu_air_c",     suhuAir);
      jsonH.add("status",         statusEvaporasi);
      jsonH.add("datetime",       datetime);
      jsonH.add("tanggal",        tanggal);

      if (Firebase.pushJSON(fbdo, PATH_HISTORY, jsonH)) {
        Serial.println("[Firebase] History OK.");
      } else {
        Serial.println("[Firebase] History GAGAL: " + fbdo.errorReason());
      }
    }
  }

  // ── 11. SERIAL MONITOR ──
  Serial.println("==========================================");
  Serial.printf("Waktu        : %s\n",   datetime.c_str());
  Serial.printf("Tinggi Air   : %.3f cm\n", tinggi_cm);
  Serial.printf("Acuan Pagi   : %.3f cm\n", tinggiPagi_cm);
  Serial.printf("Evaporasi    : %.2f mm\n", evaporasi_mm);
  Serial.printf("Suhu Air     : %.2f C\n",  suhuAir);
  Serial.printf("Status       : %s\n",   statusEvaporasi.c_str());
  Serial.printf("Relay Pompa  : %s\n",   relayAktif ? "ON" : "OFF");
  Serial.printf("Reset State  : %s\n",
    resetState == RESET_IDLE     ? "IDLE" :
    resetState == RESET_SAMPLING ? "SAMPLING" : "SELESAI");
  Serial.println("==========================================");
}
