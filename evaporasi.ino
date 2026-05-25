// ============================================================
//  EVAPORIMETER OTOMATIS ESP32
//  Versi: Acuan Manual + Pompa 06-07 + Notifikasi Panci Terisi
//
//  Alur harian:
//    06:00 → Pompa mulai isi air jika tinggi < 20 cm
//    07:00 → Pompa berhenti (apapun kondisinya)
//    Saat air capai 20 cm → tulis panci_terisi:true ke Firebase
//    Aplikasi tampilkan banner → Teknisi tap "Konfirmasi & Set Acuan"
//    Teknisi input nilai acuan → tersimpan di Firebase
//    ESP32 baca acuan saat startup → kunci 24 jam
// ============================================================

#include <WiFi.h>
#include <FirebaseESP32.h>
#include "time.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ota.h"

OtaStatus otaStatus = {
  FIRMWARE_VERSION, "-", "Belum Dicek", "", "-", -1
};

// ============================================================
// KONFIGURASI
// ============================================================
#define WIFI_SSID       "Klimatologiot"
#define WIFI_PASSWORD   "Klimatologiotkk"
#define FIREBASE_HOST   "klimatologiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "AIzaSyAZrk_k4DQ_ijCa6gp67oRklFMKD2dLcbQ"

#define PATH_REALTIME     "/Monitoring/realtime"
#define PATH_HISTORY      "/Monitoring/History"
#define PATH_ACUAN        "/Monitoring/acuan_pagi_cm"
#define PATH_PANCI_TERISI "/Monitoring/panci_terisi"   // { terisi: bool, waktu: string }
#define PATH_OTA_TRIGGER  "/Monitoring/ota_trigger"

// Pin
#define HX710B_OUT   19
#define HX710B_SCK   18
#define DS18B20_PIN  27
#define RELAY_PIN    32

// ============================================================
// KALIBRASI HX710B
// ============================================================
const long  D0              = 9626456L;
const long  DMAX            = 10767088L;
const float TINGGI_ACUAN_CM = 20.0;

// Pompa aktif jam 06:00–07:00, berhenti saat air sudah 20 cm
const int   JAM_POMPA_MULAI   = 6;
const int   JAM_POMPA_SELESAI = 7;

// Hysteresis: pompa ON jika < 19.0 cm, OFF jika >= 20.0 cm
const float RELAY_BATAS_BAWAH = 19.0;
const float RELAY_BATAS_ATAS  = TINGGI_ACUAN_CM;   // 20.0 cm

// Interval
const unsigned long INTERVAL_REALTIME = 300000UL;   // 5 menit
const unsigned long INTERVAL_HISTORY  = 600000UL;   // 10 menit
const unsigned long INTERVAL_BACA     = 10000UL;    // baca sensor tiap 10 detik
const unsigned long INTERVAL_OTA_CEK  = 15000UL;

// ============================================================
// VARIABEL GLOBAL
// ============================================================
FirebaseData      fbdo;
FirebaseAuth      auth;
FirebaseConfig    config;
OneWire           oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

unsigned long lastRealtime      = 0;
unsigned long lastHistory       = 0;
unsigned long lastBaca          = 0;
unsigned long lastOtaCekTrigger = 0;

// Acuan pagi — hanya dibaca SEKALI saat startup, tidak pernah ditulis oleh ESP32
float  tinggiPagi_cm    = -1.0;
bool   acuanSudahDiSet  = false;

String statusEvaporasi  = "Normal";
bool   relayAktif       = false;

// Flag panci terisi: cegah kirim notifikasi berulang dalam satu sesi pompa
bool   panciTerisiDikirim = false;

// ============================================================
// FUNGSI: Baca satu sampel HX710B
// ============================================================
unsigned long bacaSatuSampel(bool &sukses) {
  sukses = true;
  unsigned long t0 = millis();
  while (digitalRead(HX710B_OUT) == HIGH) {
    if (millis() - t0 > 2000) { sukses = false; return 0; }
    yield();
  }
  unsigned long count = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(HX710B_SCK, HIGH); delayMicroseconds(1);
    count <<= 1;
    digitalWrite(HX710B_SCK, LOW);  delayMicroseconds(1);
    if (digitalRead(HX710B_OUT)) count++;
  }
  digitalWrite(HX710B_SCK, HIGH); delayMicroseconds(1);
  digitalWrite(HX710B_SCK, LOW);
  count ^= 0x800000;
  return count;
}

// ============================================================
// FUNGSI: Hitung tinggi air (cm)
// ============================================================
float hitungTinggi() {
  const int N = 50;
  bool sukses;
  for (int i = 0; i < 5; i++) { bacaSatuSampel(sukses); delay(10); yield(); }

  unsigned long sampel[N];
  int sampelValid = 0;
  for (int i = 0; i < N; i++) {
    unsigned long val = bacaSatuSampel(sukses);
    if (sukses) sampel[sampelValid++] = val;
    delay(5); yield();
  }
  if (sampelValid < 25) return -99.0;

  for (int i = 1; i < sampelValid; i++) {
    unsigned long k = sampel[i]; int j = i - 1;
    while (j >= 0 && sampel[j] > k) { sampel[j+1] = sampel[j]; j--; }
    sampel[j+1] = k;
  }
  int awal = sampelValid * 20 / 100, akhir = sampelValid * 80 / 100;
  unsigned long total = 0;
  for (int i = awal; i < akhir; i++) total += sampel[i];
  long D_rata = (long)(total / (akhir - awal));

  float tinggi = (float)(D_rata - D0) * TINGGI_ACUAN_CM / (float)(DMAX - D0);
  if (tinggi < 0.0)  tinggi = 0.0;
  if (tinggi > 30.0) tinggi = 30.0;
  return tinggi;
}

// ============================================================
// FUNGSI: Baca acuan pagi dari Firebase (hanya baca, tidak tulis)
// ============================================================
bool bacaAcuanDariFirebase(float &hasil) {
  if (!Firebase.ready()) return false;
  if (!Firebase.getFloat(fbdo, PATH_ACUAN)) return false;
  float val = fbdo.floatData();
  if (val > 0.0 && val <= 30.0) { hasil = val; return true; }
  return false;
}

// ============================================================
// FUNGSI: Kirim notifikasi panci terisi ke Firebase
//   Format node: { terisi: true, waktu: "HH:MM" }
//   Aplikasi membaca node ini dan menampilkan banner.
// ============================================================
void kirimNotifikasiPanciTerisi(const String &waktu) {
  if (!Firebase.ready()) return;
  FirebaseJson jsonNotif;
  jsonNotif.add("terisi", true);
  jsonNotif.add("waktu", waktu);
  if (Firebase.updateNode(fbdo, PATH_PANCI_TERISI, jsonNotif)) {
    Serial.println("[NOTIF] Panci terisi dikirim ke Firebase: " + waktu);
  } else {
    Serial.println("[NOTIF] Gagal kirim: " + fbdo.errorReason());
  }
}

// ============================================================
// FUNGSI: Kontrol Relay Pompa
//
//  Aturan:
//  1. Hanya aktif jam JAM_POMPA_MULAI (06) s.d. JAM_POMPA_SELESAI (07)
//  2. Di dalam jendela jam itu:
//     - Pompa ON  jika tinggi < RELAY_BATAS_BAWAH (19 cm)
//     - Pompa OFF jika tinggi >= RELAY_BATAS_ATAS  (20 cm)
//  3. Saat pompa baru mati karena air SUDAH 20 cm → kirim notifikasi 1x
//  4. Di luar jam operasional → relay pasti OFF
//
//  Parameter waktuStr digunakan untuk isi field "waktu" di notifikasi.
// ============================================================
void kontrolRelay(float tinggi_cm, int jam, int menit, const String &waktuStr) {
  bool jamOperasional = (jam >= JAM_POMPA_MULAI && jam < JAM_POMPA_SELESAI);

  if (!jamOperasional) {
    // Luar jam → matikan relay, reset flag agar besok bisa notif lagi
    if (relayAktif) {
      digitalWrite(RELAY_PIN, LOW);
      relayAktif = false;
      Serial.println("[RELAY] OFF — jam pompa selesai (07:00)");
    }
    // Reset flag setelah jam 07 agar besok bisa kirim notif lagi
    if (jam >= JAM_POMPA_SELESAI) panciTerisiDikirim = false;
    return;
  }

  // ── Di dalam jam operasional (06:00–06:59) ──
  if (!relayAktif && tinggi_cm < RELAY_BATAS_BAWAH) {
    // Air kurang → hidupkan pompa
    digitalWrite(RELAY_PIN, HIGH);
    relayAktif = true;
    Serial.printf("[RELAY] ON  — tinggi %.2f cm < %.2f cm\n", tinggi_cm, RELAY_BATAS_BAWAH);

  } else if (relayAktif && tinggi_cm >= RELAY_BATAS_ATAS) {
    // Air sudah 20 cm → matikan pompa
    digitalWrite(RELAY_PIN, LOW);
    relayAktif = false;
    Serial.printf("[RELAY] OFF — air mencapai %.2f cm (target %.2f cm)\n",
                  tinggi_cm, RELAY_BATAS_ATAS);

    // Kirim notifikasi ke Firebase (hanya 1 kali per sesi)
    if (!panciTerisiDikirim) {
      kirimNotifikasiPanciTerisi(waktuStr);
      panciTerisiDikirim = true;
    }
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
    if (millis() - tWifi > 15000) { Serial.println("\n[WiFi] Gagal! Restart..."); ESP.restart(); }
    delay(500); Serial.print("."); yield();
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  // NTP (WIB = UTC+7)
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sinkronisasi NTP");
  struct tm t;
  unsigned long tNtp = millis();
  while (!getLocalTime(&t)) {
    if (millis() - tNtp > 10000) { Serial.println(" GAGAL!"); break; }
    delay(500); Serial.print("."); yield();
  }
  Serial.println(" OK!");

  // Firebase
  config.host                       = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000);

  // ── Baca acuan pagi SEKALI saat startup ──
  // Teknisi sudah set nilai ini dari aplikasi sebelum ESP32 menyala/restart
  Serial.println("Membaca acuan pagi dari Firebase...");
  if (bacaAcuanDariFirebase(tinggiPagi_cm)) {
    acuanSudahDiSet = true;
    Serial.printf("[OK] Acuan pagi: %.3f cm\n", tinggiPagi_cm);
  } else {
    acuanSudahDiSet = false;
    Serial.println("============================================");
    Serial.println("[PERINGATAN] Acuan belum di-set teknisi!");
    Serial.println("  Set nilai di aplikasi → path Firebase:");
    Serial.println("  " PATH_ACUAN);
    Serial.println("  Evaporasi tidak dihitung sampai nilai diisi.");
    Serial.println("============================================");
  }

  Serial.println("=== SETUP SELESAI ===\n");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  // Reconnect WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Putus, reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(5000); return;
  }

  // OTA: cek trigger dari Firebase tiap 15 detik
  if (millis() - lastOtaCekTrigger >= INTERVAL_OTA_CEK) {
    lastOtaCekTrigger = millis();
    if (Firebase.ready() && Firebase.getBool(fbdo, PATH_OTA_TRIGGER)) {
      if (fbdo.boolData()) {
        Serial.println("[OTA] Trigger dari Firebase!");
        Firebase.setBool(fbdo, PATH_OTA_TRIGGER, false);
        struct tm tOta;
        if (getLocalTime(&tOta)) {
          char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tOta);
          otaStatus.waktuCekTerakhir = String(buf);
        }
        forceCheckOTA();
      }
    }
  }
  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL || lastOtaCheck == 0) {
    struct tm tOta;
    if (getLocalTime(&tOta)) {
      char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tOta);
      otaStatus.waktuCekTerakhir = String(buf);
    }
  }
  checkAndUpdateOTA();

  // Throttle baca sensor
  if (millis() - lastBaca < INTERVAL_BACA) return;
  lastBaca = millis();

  // ── 1. BACA SENSOR TINGGI AIR ──
  float tinggi_cm = hitungTinggi();
  if (tinggi_cm == -99.0) {
    Serial.println("[WARNING] Sensor TIMEOUT, dilewati.");
    return;
  }

  // ── 2. SUHU AIR ──
  sensors.requestTemperatures();
  float suhuAir = sensors.getTempCByIndex(0);
  if (suhuAir == DEVICE_DISCONNECTED_C) suhuAir = -99.0;

  // ── 3. WAKTU ──
  struct tm timeinfo;
  String waktu    = "--:--:--";
  String tanggal  = "0000-00-00";
  String datetime = "0000-00-00 00:00:00";
  bool   adaWaktu = getLocalTime(&timeinfo);
  String waktuJamMenit = "--:--";

  if (adaWaktu) {
    char buf[25];
    strftime(buf, sizeof(buf), "%H:%M:%S",          &timeinfo); waktu    = buf;
    strftime(buf, sizeof(buf), "%Y-%m-%d",           &timeinfo); tanggal  = buf;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo); datetime = buf;
    char bufJM[6];
    strftime(bufJM, sizeof(bufJM), "%H:%M",          &timeinfo); waktuJamMenit = bufJM;

    // ── 4. KONTROL RELAY POMPA (06:00–07:00, berbasis level air) ──
    kontrolRelay(tinggi_cm, timeinfo.tm_hour, timeinfo.tm_min, waktuJamMenit);
  }

  // ── 5. HITUNG EVAPORASI ──
  float evaporasi_cm  = 0.0;
  float evaporasi_mm  = 0.0;
  if (acuanSudahDiSet) {
    float raw = tinggiPagi_cm - tinggi_cm;
    evaporasi_cm = (raw < 0.0) ? 0.0 : raw;
    evaporasi_mm = evaporasi_cm * 10.0;
    if (raw < -0.5) {
      Serial.printf("[ANOMALI] Evaporasi raw negatif: %.3f cm\n", raw);
    }
  }

  // ── 6. STATUS EVAPORASI ──
  if (!acuanSudahDiSet)       statusEvaporasi = "Acuan Belum Diset";
  else if (evaporasi_mm > 10) statusEvaporasi = "Tinggi";
  else if (evaporasi_mm >= 2) statusEvaporasi = "Normal";
  else                        statusEvaporasi = "Rendah";

  // ── 7. KIRIM REALTIME KE FIREBASE (tiap 5 menit) ──
  if (millis() - lastRealtime >= INTERVAL_REALTIME) {
    lastRealtime = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonRT;
      jsonRT.add("tinggi_air_cm",     tinggi_cm);
      jsonRT.add("acuan_pagi_cm",     acuanSudahDiSet ? tinggiPagi_cm : -1.0);
      jsonRT.add("acuan_sudah_diset", acuanSudahDiSet);
      jsonRT.add("evaporasi_mm",      evaporasi_mm);
      jsonRT.add("suhu_air_c",        suhuAir);
      jsonRT.add("status",            statusEvaporasi);
      jsonRT.add("relay_aktif",       relayAktif);
      jsonRT.add("panci_terisi",      panciTerisiDikirim);
      jsonRT.add("datetime",          datetime);
      jsonRT.add("ota_versi_device",  otaStatus.versiSekarang);
      jsonRT.add("ota_versi_terbaru", otaStatus.versiTerbaru);
      jsonRT.add("ota_status",        otaStatus.statusTerakhir);
      jsonRT.add("ota_progress",      otaStatus.progressPersen);
      jsonRT.add("ota_error",         otaStatus.errorMsg);
      jsonRT.add("ota_waktu_cek",     otaStatus.waktuCekTerakhir);

      if (Firebase.updateNode(fbdo, PATH_REALTIME, jsonRT)) {
        Serial.println("[Firebase] Realtime OK.");
      } else {
        Serial.println("[Firebase] Realtime GAGAL: " + fbdo.errorReason());
      }
    }
  }

  // ── 8. SIMPAN HISTORY (tiap 10 menit, hanya jika acuan sudah di-set) ──
  if (millis() - lastHistory >= INTERVAL_HISTORY) {
    lastHistory = millis();
    if (Firebase.ready()) {
      if (!acuanSudahDiSet) {
        Serial.println("[History] Dilewati — acuan belum di-set.");
      } else {
        FirebaseJson jsonH;
        jsonH.add("tinggi_air_cm",  tinggi_cm);
        jsonH.add("acuan_pagi_cm",  tinggiPagi_cm);
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
  }

  // ── 9. SERIAL MONITOR ──
  Serial.println("==========================================");
  Serial.printf("Waktu        : %s\n",   datetime.c_str());
  Serial.printf("Tinggi Air   : %.3f cm\n", tinggi_cm);
  if (acuanSudahDiSet) {
    Serial.printf("Acuan Pagi   : %.3f cm\n", tinggiPagi_cm);
    Serial.printf("Evaporasi    : %.2f mm\n", evaporasi_mm);
  } else {
    Serial.println("Acuan Pagi   : *** BELUM DISET ***");
    Serial.println("Evaporasi    : ---");
  }
  Serial.printf("Suhu Air     : %.2f C\n",  suhuAir);
  Serial.printf("Status       : %s\n",   statusEvaporasi.c_str());
  Serial.printf("Relay Pompa  : %s\n",   relayAktif ? "ON" : "OFF");
  Serial.printf("Panci Terisi : %s\n",   panciTerisiDikirim ? "Sudah dikirim" : "-");
  Serial.println("==========================================");
}
