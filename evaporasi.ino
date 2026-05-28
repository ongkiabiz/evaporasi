// ============================================================
//  EVAPORIMETER OTOMATIS ESP32
//  Versi Final — DMAX Dinamis + Mode Kalibrasi D0/DMAX
//
//  RUMUS EVAPORASI:
//  E_interval = tinggi_sebelumnya - tinggi_sekarang
//  E_harian  += E_interval (akumulasi, reset tiap 07:00)
//
//  DMAX DINAMIS:
//  - DMAX = max(semua pembacaan raw ADC sejak boot/reset)
//  - Disimpan ke NVS (lokal) + Firebase RTDB (cloud)
//  - Bisa direset dari app Flutter via Firebase
//
//  KALIBRASI D0 & DMAX:
//  - D0   : kirim calib_trigger = "d0"   saat panci KOSONG
//  - DMAX : kirim calib_trigger = "dmax" saat air tepat 20 cm
//  - Status kalibrasi dikembalikan via calib_status di Firebase
//
//  ALUR SESUAI FLOWCHART:
//  1. Sensor HX710B → ESP32 proses data
//  2. Hubung WiFi → Sinkronisasi NTP
//  3. Ukur tinggi air (DMAX diperbarui otomatis)
//  4. Hitung evaporasi per interval → akumulasi harian
//  5. Cek jam 06:00–07:00 → kontrol selenoid
//  6. Kirim ke Firebase + tampil di app
//  7. Reset evaporasi tiap jam 07:00
// ============================================================

#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Preferences.h>
#include "time.h"
#include "ota.h"

OtaStatus otaStatus = {
  FIRMWARE_VERSION, "-", "Belum Dicek", "", "-", -1
};

Preferences prefs;

// ============================================================
// KONFIGURASI
// ============================================================

#define WIFI_SSID      "Klimatologiot"
#define WIFI_PASSWORD  "Klimatologiotkk"

#define FIREBASE_HOST  "klimatologiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH  "AIzaSyAZrk_k4DQ_ijCa6gp67oRklFMKD2dLcbQ"

// ── Path Firebase RTDB ────────────────────────────────────────
#define PATH_REALTIME       "/Monitoring/realtime"
#define PATH_HISTORY        "/Monitoring/History"
#define PATH_OTA_TRIGGER    "/Monitoring/ota_trigger"
#define PATH_RESET_EVAP     "/Monitoring/reset_evaporasi"
#define PATH_SETTINGS       "/Monitoring/settings/evaporasi"
#define PATH_DMAX           "/Monitoring/settings/evaporasi/dmax"
#define PATH_D0             "/Monitoring/settings/evaporasi/d0"
#define PATH_RESET_DMAX     "/Monitoring/reset_dmax"
#define PATH_CALIB_TRIGGER  "/Monitoring/calib_trigger"   // "d0" / "dmax" / "idle"
#define PATH_CALIB_STATUS   "/Monitoring/calib_status"    // feedback ke app Flutter

// ============================================================
// PIN
// ============================================================

#define HX710B_OUT  19
#define HX710B_SCK  18
#define RELAY_PIN   32

// ============================================================
// KALIBRASI HX710B
// Kedua variabel sekarang bisa diperbarui dari kalibrasi lapangan
// ============================================================

long        D0              = 9626456L;       // raw saat panci KOSONG  — dikalibrasi via app
long        DMAX            = 10767088L;      // raw saat air 20 cm     — dikalibrasi via app
const float TINGGI_ACUAN_CM = 20.0;           // tinggi air referensi DMAX (cm), jangan ubah

// ============================================================
// JADWAL SELENOID — 06:00–07:00
// ============================================================

const int   JAM_POMPA_MULAI   = 6;
const int   JAM_POMPA_SELESAI = 7;
const float STANDAR_TINGGI_CM = 20.0;

// ============================================================
// INTERVAL DEFAULT
// ============================================================

const unsigned long INTERVAL_REALTIME_DEFAULT = 300000UL;   // 5 menit
const unsigned long INTERVAL_HISTORY_DEFAULT  = 600000UL;   // 10 menit
const unsigned long INTERVAL_BACA_DEFAULT     = 10000UL;    // 10 detik
const unsigned long INTERVAL_CMD_CEK          = 10000UL;    // 10 detik
const unsigned long INTERVAL_SETTINGS         = 300000UL;   // 5 menit

// ============================================================
// VARIABEL GLOBAL
// ============================================================

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

unsigned long lastRealtime    = 0;
unsigned long lastHistory     = 0;
unsigned long lastBaca        = 0;
unsigned long lastCmdCek      = 0;
unsigned long lastSettingsCek = 0;

// ── Settings yang bisa diubah dari app ───────────────────────
float         cfg_thresholdRendah    = 2.0;
float         cfg_thresholdTinggi    = 10.0;
String        cfg_rumusKalibrasi     = "selisih_max";
float         cfg_koreksiOffset      = 0.0;
unsigned long cfg_intervalRealtime   = INTERVAL_REALTIME_DEFAULT;
unsigned long cfg_intervalHistory    = INTERVAL_HISTORY_DEFAULT;
unsigned long cfg_intervalBaca       = INTERVAL_BACA_DEFAULT;

// ── Sensor & evaporasi ────────────────────────────────────────
float tinggiSebelumnya      = -1.0;
float evaporasiHarian_mm    = 0.0;
bool  sudahResetHari        = false;
int   hariSebelumnya        = -1;

// ── Status ────────────────────────────────────────────────────
String statusEvaporasi      = "Rendah";
bool   relayAktif           = false;
bool   panciTerisiDikirim   = false;

// ============================================================
// NVS + FIREBASE: SIMPAN & MUAT D0
// D0 = nilai raw saat panci kosong (titik nol sensor)
// ============================================================

/**
 * Simpan D0 ke NVS (lokal) dan Firebase (cloud).
 * Dipanggil setelah kalibrasi panci kosong berhasil.
 */
void simpanD0(long nilaiD0) {
  // 1. Simpan ke NVS agar bertahan saat restart
  prefs.begin("evap", false);
  prefs.putLong("d0", nilaiD0);
  prefs.end();

  // 2. Simpan ke Firebase sebagai backup cloud
  if (Firebase.ready()) {
    if (Firebase.setInt(fbdo, PATH_D0, (int)nilaiD0)) {
      Serial.printf("[CALIB] D0 disimpan ke NVS + Firebase: %ld\n", nilaiD0);
    } else {
      Serial.printf("[CALIB] D0 NVS OK, Firebase GAGAL: %s\n",
                    fbdo.errorReason().c_str());
    }
  } else {
    Serial.printf("[CALIB] Firebase belum siap, D0 hanya di NVS: %ld\n", nilaiD0);
  }
}

/**
 * Muat D0 saat boot.
 * Prioritas: NVS → Firebase → nilai default hardcode.
 */
void muatD0() {
  // 1. Coba NVS
  prefs.begin("evap", true);
  long d0Nvs = prefs.getLong("d0", -1);
  prefs.end();

  if (d0Nvs > 0) {
    D0 = d0Nvs;
    Serial.printf("[CALIB] D0 dimuat dari NVS: %ld\n", D0);
    return;
  }

  // 2. Fallback ke Firebase
  Serial.println("[CALIB] D0 NVS kosong, mencoba Firebase...");
  if (Firebase.ready()) {
    if (Firebase.getInt(fbdo, PATH_D0)) {
      long d0Fb = (long)fbdo.intData();
      if (d0Fb > 0) {
        D0 = d0Fb;
        prefs.begin("evap", false);
        prefs.putLong("d0", D0);
        prefs.end();
        Serial.printf("[CALIB] D0 dimuat dari Firebase + disimpan ke NVS: %ld\n", D0);
        return;
      }
    }
  }

  // 3. Pakai default
  Serial.printf("[CALIB] D0 pakai nilai default hardcode: %ld\n", D0);
}

// ============================================================
// NVS + FIREBASE: SIMPAN & MUAT DMAX
// ============================================================

/**
 * Simpan DMAX ke NVS (lokal) dan Firebase (cloud).
 */
void simpanDmax(long nilaiDmax) {
  prefs.begin("evap", false);
  prefs.putLong("dmax", nilaiDmax);
  prefs.end();

  if (Firebase.ready()) {
    if (Firebase.setInt(fbdo, PATH_DMAX, (int)nilaiDmax)) {
      Serial.printf("[DMAX] Disimpan ke NVS + Firebase: %ld\n", nilaiDmax);
    } else {
      Serial.printf("[DMAX] NVS OK, Firebase GAGAL: %s\n",
                    fbdo.errorReason().c_str());
    }
  } else {
    Serial.printf("[DMAX] Firebase belum siap, hanya disimpan ke NVS: %ld\n",
                  nilaiDmax);
  }
}

/**
 * Muat DMAX saat boot.
 * Prioritas: NVS → Firebase → nilai default hardcode.
 */
void muatDmax() {
  prefs.begin("evap", true);
  long dmaxNvs = prefs.getLong("dmax", -1);
  prefs.end();

  if (dmaxNvs > 0) {
    DMAX = dmaxNvs;
    Serial.printf("[DMAX] Dimuat dari NVS: %ld\n", DMAX);
    return;
  }

  Serial.println("[DMAX] NVS kosong, mencoba Firebase...");
  if (Firebase.ready()) {
    if (Firebase.getInt(fbdo, PATH_DMAX)) {
      long dmaxFb = (long)fbdo.intData();
      if (dmaxFb > 0) {
        DMAX = dmaxFb;
        prefs.begin("evap", false);
        prefs.putLong("dmax", DMAX);
        prefs.end();
        Serial.printf("[DMAX] Dimuat dari Firebase + disimpan ke NVS: %ld\n", DMAX);
        return;
      }
    }
  }

  Serial.printf("[DMAX] Pakai nilai default: %ld\n", DMAX);
}

// ============================================================
// BACA HX710B: SATU SAMPEL RAW ADC
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
// BACA SENSOR RAW — TRIMMED MEAN (dipakai oleh kalibrasi & normal)
// Mengembalikan nilai rata-rata ADC, atau -1 jika gagal.
// ============================================================

long bacaSensorRaw() {
  const int N = 50;
  bool sukses;

  // Buang 5 sampel pertama (warmup sensor)
  for (int i = 0; i < 5; i++) { bacaSatuSampel(sukses); delay(10); yield(); }

  // Kumpulkan sampel valid
  unsigned long sampel[N];
  int sampelValid = 0;
  for (int i = 0; i < N; i++) {
    unsigned long val = bacaSatuSampel(sukses);
    if (sukses) sampel[sampelValid++] = val;
    delay(5); yield();
  }
  if (sampelValid < 25) return -1;

  // Insertion sort
  for (int i = 1; i < sampelValid; i++) {
    unsigned long key = sampel[i]; int j = i - 1;
    while (j >= 0 && sampel[j] > key) { sampel[j+1] = sampel[j]; j--; }
    sampel[j+1] = key;
  }

  // Trimmed mean: buang 20% bawah dan 20% atas
  int   awal  = sampelValid * 20 / 100;
  int   akhir = sampelValid * 80 / 100;
  unsigned long total = 0;
  for (int i = awal; i < akhir; i++) total += sampel[i];
  return (long)(total / (akhir - awal));
}

// ============================================================
// HITUNG TINGGI AIR (mode operasional normal)
// Trimmed Mean + Anti Spike + Moving Average
// DMAX diperbarui otomatis jika ada nilai raw lebih besar
// ============================================================

float hitungTinggi() {
  long D_rata = bacaSensorRaw();
  if (D_rata < 0) return -99.0;   // sensor timeout

  // ── UPDATE DMAX OTOMATIS ─────────────────────────────────
  // Jika nilai raw rata-rata lebih besar dari DMAX tersimpan,
  // artinya air lebih tinggi → perbarui DMAX
  if (D_rata > DMAX) {
    Serial.printf("[DMAX] Update otomatis: %ld → %ld\n", DMAX, D_rata);
    DMAX = D_rata;
    simpanDmax(DMAX);
  }

  // Konversi raw ADC → tinggi air (cm)
  // Rumus: tinggi = (D_rata - D0) / (DMAX - D0) * TINGGI_ACUAN_CM
  float tinggi = (float)(D_rata - D0) * TINGGI_ACUAN_CM /
                 (float)(DMAX - D0);
  if (tinggi < 0.0)  tinggi = 0.0;
  if (tinggi > 30.0) tinggi = 30.0;

  // Anti-spike: abaikan lonjakan > 1 cm per interval
  if (tinggiSebelumnya > 0) {
    float selisih = abs(tinggi - tinggiSebelumnya);
    if (selisih > 1.0) {
      Serial.printf("[FILTER] Spike %.3fcm diabaikan, pakai %.3fcm\n",
                    tinggi, tinggiSebelumnya);
      tinggi = tinggiSebelumnya;
    }
  }

  // Moving average 5 titik untuk haluskan pembacaan
  static float buffer[5];
  static int   idx   = 0;
  static bool  penuh = false;
  buffer[idx++] = tinggi;
  if (idx >= 5) { idx = 0; penuh = true; }
  int   n   = penuh ? 5 : idx;
  float avg = 0;
  for (int i = 0; i < n; i++) avg += buffer[i];
  float tinggiFinal = avg / n;

  Serial.printf("[SENSOR] D_rata: %ld | DMAX: %ld | D0: %ld\n",
                D_rata, DMAX, D0);
  Serial.printf("[SENSOR] Tinggi: %.3f cm (%.1f mm)\n",
                tinggiFinal, tinggiFinal * 10.0);
  return tinggiFinal;
}

// ============================================================
// HITUNG EVAPORASI PER INTERVAL
// E_interval = tinggiSebelumnya - tinggiSekarang (konversi ke mm)
// Nilai negatif (air naik) atau spike diabaikan
// ============================================================

float hitungEvaporasiInterval(float tinggi_cm) {
  if (tinggiSebelumnya < 0) {
    tinggiSebelumnya = tinggi_cm;
    Serial.printf("[EVAP] Referensi awal: %.3f cm\n", tinggiSebelumnya);
    return 0.0;
  }

  float delta = tinggiSebelumnya - tinggi_cm;
  tinggiSebelumnya = tinggi_cm;

  // delta <= 0  → air naik (hujan / selenoid) → abaikan
  // delta > 0.5 → lonjakan tidak wajar → abaikan
  if (delta <= 0.0 || delta > 0.5) {
    if (delta < -0.5) {
      Serial.printf("[INFO] Air naik %.2fcm, diabaikan dari evaporasi\n",
                    -delta);
    }
    return 0.0;
  }

  float evap_mm = (delta * 10.0f) + cfg_koreksiOffset;
  if (evap_mm < 0.0f) evap_mm = 0.0f;

  Serial.printf("[EVAP] Interval: %.4f mm (delta: %.4fcm, offset: %.2fmm)\n",
                evap_mm, delta, cfg_koreksiOffset);
  return evap_mm;
}

// ============================================================
// RESET EVAPORASI HARIAN (otomatis jam 07:00)
// ============================================================

void resetEvaporasiHarian(const String &waktu) {
  Serial.printf("[RESET] Evaporasi harian kemarin: %.2f mm\n",
                evaporasiHarian_mm);
  evaporasiHarian_mm = 0.0;
  sudahResetHari     = true;

  if (Firebase.ready()) {
    FirebaseJson j;
    j.add("evaporasi_mm", 0.0);
    j.add("reset_jam",    waktu);
    Firebase.updateNode(fbdo, PATH_REALTIME, j);
  }
  Serial.printf("[RESET] Evaporasi direset jam %s\n", waktu.c_str());
}

// ============================================================
// PROSES KALIBRASI D0 / DMAX (dari app Flutter via Firebase)
//
// Alur:
//   1. App tulis calib_trigger = "d0" atau "dmax"
//   2. ESP32 baca trigger tiap 10 detik
//   3. ESP32 baca 50 sampel sensor, hitung trimmed mean
//   4. Simpan D0 atau DMAX ke NVS + Firebase
//   5. Tulis calib_status = "ok_d0" / "ok_dmax" / "error_..."
//   6. App baca calib_status untuk konfirmasi
// ============================================================

void prosesKalibrasi() {
  if (!Firebase.ready()) return;

  FirebaseData fdoK;
  if (!Firebase.getString(fdoK, PATH_CALIB_TRIGGER)) return;

  String trigger = fdoK.stringData();
  trigger.trim();
  if (trigger == "" || trigger == "idle") return;

  Serial.printf("[CALIB] Trigger diterima: '%s'\n", trigger.c_str());

  // Reset trigger lebih dulu agar tidak dieksekusi berulang
  Firebase.setString(fbdo, PATH_CALIB_TRIGGER, "idle");
  Firebase.setString(fbdo, PATH_CALIB_STATUS,  "reading");

  Serial.println("[CALIB] Membaca sensor (50 sampel trimmed mean)...");
  long rawVal = bacaSensorRaw();

  if (rawVal < 0) {
    Firebase.setString(fbdo, PATH_CALIB_STATUS, "error_sensor_timeout");
    Serial.println("[CALIB] GAGAL — sensor timeout, kurang dari 25 sampel valid!");
    return;
  }

  Serial.printf("[CALIB] Nilai raw hasil baca: %ld\n", rawVal);

  // ──────────────────────────────────────────────────────────
  //  KALIBRASI D0 — panci harus kosong
  // ──────────────────────────────────────────────────────────
  if (trigger == "d0") {
    D0 = rawVal;

    // Jika D0 baru >= DMAX, reset DMAX ke D0 + margin kecil
    // agar rumus tidak crash (pembagi = 0 atau negatif)
    if (D0 >= DMAX) {
      DMAX = D0 + 1000;
      simpanDmax(DMAX);
      Serial.println("[CALIB] DMAX ikut direset karena D0 >= DMAX lama");
    }

    simpanD0(D0);

    // Kirim hasil ke Firebase settings agar app bisa tampilkan
    FirebaseJson j;
    j.add("d0",         (int)D0);
    j.add("nilai_raw",  (int)rawVal);
    j.add("keterangan", "Kalibrasi D0 berhasil — panci kosong");
    Firebase.updateNode(fbdo, PATH_SETTINGS, j);
    Firebase.setString(fbdo, PATH_CALIB_STATUS, "ok_d0");

    // Reset referensi tinggi agar evaporasi mulai dari nol lagi
    tinggiSebelumnya = -1.0;

    Serial.printf("[CALIB] D0 BARU: %ld\n", D0);
    Serial.println("[CALIB] Sekarang isi air tepat 20 cm, lalu kirim trigger 'dmax'");

  // ──────────────────────────────────────────────────────────
  //  KALIBRASI DMAX — air harus tepat 20 cm
  // ──────────────────────────────────────────────────────────
  } else if (trigger == "dmax") {

    // Validasi: nilai raw harus lebih besar dari D0
    if (rawVal <= D0) {
      Firebase.setString(fbdo, PATH_CALIB_STATUS, "error_dmax_below_d0");
      Serial.printf("[CALIB] GAGAL — raw (%ld) <= D0 (%ld)\n", rawVal, D0);
      Serial.println("[CALIB] Pastikan air sudah mencapai 20 cm dan sensor stabil!");
      return;
    }

    DMAX = rawVal;
    simpanDmax(DMAX);

    long range = DMAX - D0;
    // Verifikasi: tinggi yang terbaca seharusnya tepat 20 cm
    float tinggiVerif = (float)(DMAX - D0) * TINGGI_ACUAN_CM / (float)(DMAX - D0);

    // Kirim hasil ke Firebase settings
    FirebaseJson j;
    j.add("dmax",       (int)DMAX);
    j.add("nilai_raw",  (int)rawVal);
    j.add("range_adc",  (int)range);
    j.add("tinggi_cm",  TINGGI_ACUAN_CM);
    j.add("keterangan", "Kalibrasi DMAX berhasil — air 20 cm");
    Firebase.updateNode(fbdo, PATH_SETTINGS, j);
    Firebase.setString(fbdo, PATH_CALIB_STATUS, "ok_dmax");

    // Reset referensi tinggi
    tinggiSebelumnya = -1.0;

    Serial.printf("[CALIB] DMAX BARU: %ld (range: %ld ADC)\n", DMAX, range);
    Serial.printf("[CALIB] Kalibrasi selesai! D0=%ld | DMAX=%ld\n", D0, DMAX);

  } else {
    // Trigger tidak dikenal
    Firebase.setString(fbdo, PATH_CALIB_STATUS, "error_unknown_trigger");
    Serial.printf("[CALIB] Trigger tidak dikenal: '%s'\n", trigger.c_str());
  }
}

// ============================================================
// CEK PERINTAH DARI FIREBASE (reset, OTA, dll.)
// ============================================================

void cekPerintahFirebase(const String &waktu) {
  if (!Firebase.ready()) return;

  // ── Reset DMAX manual dari app ────────────────────────────
  if (Firebase.getBool(fbdo, PATH_RESET_DMAX) && fbdo.boolData()) {
    Serial.println("[CMD] Reset DMAX diterima dari app!");
    Firebase.setBool(fbdo, PATH_RESET_DMAX, false);
    DMAX = D0 + 100;
    simpanDmax(DMAX);
    Serial.printf("[CMD] DMAX direset ke D0+100 = %ld, akan update otomatis\n",
                  DMAX);
  }

  // ── Reset evaporasi manual ────────────────────────────────
  if (Firebase.getBool(fbdo, PATH_RESET_EVAP) && fbdo.boolData()) {
    Serial.println("[CMD] Reset evaporasi manual diterima!");
    Firebase.setBool(fbdo, PATH_RESET_EVAP, false);
    evaporasiHarian_mm = 0.0;
    Serial.println("[CMD] Evaporasi direset ke 0");
  }

  // ── Trigger OTA update ────────────────────────────────────
  if (Firebase.getBool(fbdo, PATH_OTA_TRIGGER) && fbdo.boolData()) {
    Serial.println("[OTA] Trigger diterima!");
    Firebase.setBool(fbdo, PATH_OTA_TRIGGER, false);
    forceCheckOTA();
  }
}

// ============================================================
// KONTROL SELENOID (jam 06:00–07:00)
// ============================================================

void kontrolSelenoid(float tinggi_cm, int jam, const String &waktuStr) {
  bool jamOperasional = (jam >= JAM_POMPA_MULAI && jam < JAM_POMPA_SELESAI);

  if (!jamOperasional) {
    if (relayAktif) {
      digitalWrite(RELAY_PIN, LOW);
      relayAktif = false;
      Serial.println("[SELENOID] OFF — luar jam operasional");
    }
    if (jam >= JAM_POMPA_SELESAI) panciTerisiDikirim = false;
    return;
  }

  if (tinggi_cm < STANDAR_TINGGI_CM) {
    if (!relayAktif) {
      digitalWrite(RELAY_PIN, HIGH);
      relayAktif = true;
      Serial.printf("[SELENOID] ON — tinggi %.1fmm < standar %.0fmm\n",
                    tinggi_cm * 10.0, STANDAR_TINGGI_CM * 10.0);
    }
  } else {
    if (relayAktif) {
      digitalWrite(RELAY_PIN, LOW);
      relayAktif = false;
      Serial.printf("[SELENOID] OFF — panci penuh %.1fmm\n",
                    tinggi_cm * 10.0);
      if (!panciTerisiDikirim) {
        if (Firebase.ready()) {
          FirebaseJson j;
          j.add("terisi", true);
          j.add("waktu",  waktuStr);
          Firebase.updateNode(fbdo, "/Monitoring/panci_terisi", j);
        }
        panciTerisiDikirim = true;
        Serial.println("[NOTIF] Panci terisi terkirim ke Firebase!");
      }
    }
  }
}

// ============================================================
// BACA SETTINGS DARI FIREBASE
// Dipanggil saat boot + setiap INTERVAL_SETTINGS (5 menit)
// ============================================================

void bacaSettings() {
  if (!Firebase.ready()) return;

  Serial.println("[SETTINGS] Membaca konfigurasi dari Firebase...");

  FirebaseData fdoSet;

  if (Firebase.getJSON(fdoSet, PATH_SETTINGS)) {
    FirebaseJson   &json = fdoSet.jsonObject();
    FirebaseJsonData result;

    // ── Threshold status evaporasi ────────────────────────
    if (json.get(result, "thresholdRendah") && result.success)
      cfg_thresholdRendah = result.to<float>();

    if (json.get(result, "thresholdTinggi") && result.success)
      cfg_thresholdTinggi = result.to<float>();

    // ── Rumus kalibrasi & offset ──────────────────────────
    if (json.get(result, "rumusKalibrasi") && result.success)
      cfg_rumusKalibrasi = result.to<String>();

    if (json.get(result, "koreksiOffset") && result.success)
      cfg_koreksiOffset = result.to<float>();

    // ── Interval pengiriman data ──────────────────────────
    if (json.get(result, "interval_realtime_ms") && result.success)
      cfg_intervalRealtime = max((unsigned long)result.to<int>(), 30000UL);

    if (json.get(result, "interval_history_ms") && result.success)
      cfg_intervalHistory = max((unsigned long)result.to<int>(), 60000UL);

    if (json.get(result, "interval_baca_ms") && result.success)
      cfg_intervalBaca = max((unsigned long)result.to<int>(), 5000UL);

    // ── Sinkronisasi D0 dari Firebase ────────────────────
    // Jika Firebase punya D0 yang valid dan berbeda dari lokal, sinkronkan
    if (json.get(result, "d0") && result.success) {
      long d0Fb = (long)result.to<int>();
      if (d0Fb > 0 && d0Fb != D0) {
        Serial.printf("[SETTINGS] D0 Firebase (%ld) != lokal (%ld), sinkron\n",
                      d0Fb, D0);
        D0 = d0Fb;
        prefs.begin("evap", false);
        prefs.putLong("d0", D0);
        prefs.end();
      }
    }

    // ── Sinkronisasi DMAX dari Firebase ──────────────────
    if (json.get(result, "dmax") && result.success) {
      long dmaxFb = (long)result.to<int>();
      if (dmaxFb > DMAX && dmaxFb > D0) {
        Serial.printf("[SETTINGS] DMAX Firebase (%ld) > lokal (%ld), sinkron\n",
                      dmaxFb, DMAX);
        DMAX = dmaxFb;
        prefs.begin("evap", false);
        prefs.putLong("dmax", DMAX);
        prefs.end();
      }
    }

    Serial.printf("[SETTINGS] D0: %ld | DMAX: %ld | Range: %ld\n",
                  D0, DMAX, DMAX - D0);
    Serial.printf("[SETTINGS] Threshold: %.1f / %.1f mm\n",
                  cfg_thresholdRendah, cfg_thresholdTinggi);
    Serial.printf("[SETTINGS] Rumus: %s | Offset: %.2f mm\n",
                  cfg_rumusKalibrasi.c_str(), cfg_koreksiOffset);
    Serial.printf("[SETTINGS] Interval — baca: %lums | RT: %lums | Hist: %lums\n",
                  cfg_intervalBaca, cfg_intervalRealtime, cfg_intervalHistory);
  } else {
    Serial.println("[SETTINGS] Tidak ada settings di Firebase, pakai nilai saat ini.");
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== EVAPORIMETER STARTUP ===");
  Serial.printf("[INFO] Firmware: %s\n", FIRMWARE_VERSION);

  // ── Inisialisasi pin ──────────────────────────────────────
  pinMode(HX710B_OUT, INPUT);
  pinMode(HX710B_SCK, OUTPUT); digitalWrite(HX710B_SCK, LOW);
  pinMode(RELAY_PIN,  OUTPUT); digitalWrite(RELAY_PIN,  LOW);

  // ── Koneksi WiFi ──────────────────────────────────────────
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  unsigned long tWifi = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - tWifi > 15000) {
      Serial.println("\n[WiFi] Gagal terhubung, restart...");
      ESP.restart();
    }
    delay(500); Serial.print("."); yield();
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  // ── Sinkronisasi waktu NTP ────────────────────────────────
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sinkronisasi NTP");
  struct tm t;
  unsigned long tNtp = millis();
  while (!getLocalTime(&t)) {
    if (millis() - tNtp > 10000) { Serial.println(" GAGAL!"); break; }
    delay(500); Serial.print("."); yield();
  }
  Serial.println(" OK!");

  // ── Inisialisasi Firebase ─────────────────────────────────
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000);

  // ── Muat D0 dan DMAX (NVS → Firebase → default) ──────────
  muatD0();
  muatDmax();

  // ── Pastikan range ADC valid ──────────────────────────────
  if (DMAX <= D0) {
    Serial.println("[WARNING] DMAX <= D0! Kalibrasi diperlukan.");
    Serial.println("[WARNING] Kirim calib_trigger='d0' saat panci kosong,");
    Serial.println("[WARNING] lalu calib_trigger='dmax' saat air 20 cm.");
    DMAX = D0 + 1000;   // sementara agar tidak crash
  }

  // ── Baca semua settings dari Firebase ────────────────────
  bacaSettings();

  // ── Pastikan calib_trigger dalam kondisi idle ─────────────
  if (Firebase.ready()) {
    Firebase.setString(fbdo, PATH_CALIB_TRIGGER, "idle");
  }

  Serial.println("\n=== SETUP SELESAI ===");
  Serial.printf("[INFO] D0   aktif: %ld\n", D0);
  Serial.printf("[INFO] DMAX aktif: %ld\n", DMAX);
  Serial.printf("[INFO] Range ADC : %ld\n", DMAX - D0);
  Serial.println("[INFO] Kirim calib_trigger='d0'   untuk kalibrasi panci kosong");
  Serial.println("[INFO] Kirim calib_trigger='dmax' untuk kalibrasi air 20 cm");
  Serial.println("[INFO] Evaporasi dihitung per interval, akumulasi harian");
  Serial.println("[INFO] Reset otomatis tiap jam 07:00\n");
}

// ============================================================
// LOOP UTAMA
// ============================================================

void loop() {

  // ── Reconnect WiFi jika terputus ─────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Terputus, reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(5000);
    return;
  }

  // ── Cek & lakukan OTA update ──────────────────────────────
  checkAndUpdateOTA();

  // ── Throttle sesuai interval baca ────────────────────────
  if (millis() - lastBaca < cfg_intervalBaca) return;
  lastBaca = millis();

  // ════════════════════════════════════════════════════════
  //  CEK KALIBRASI + PERINTAH dari app Flutter
  //  (dilakukan sebelum baca sensor agar tidak bentrok)
  // ════════════════════════════════════════════════════════
  if (millis() - lastCmdCek >= INTERVAL_CMD_CEK) {
    lastCmdCek = millis();
    prosesKalibrasi();           // kalibrasi D0 / DMAX
    cekPerintahFirebase("");     // reset evaporasi, OTA, dll.
  }

  // ════════════════════════════════════════════════════════
  //  STEP 1: Ukur tinggi air
  //  DMAX diperbarui otomatis di dalam hitungTinggi()
  // ════════════════════════════════════════════════════════
  float tinggi_cm = hitungTinggi();
  if (tinggi_cm == -99.0) {
    Serial.println("[WARNING] Sensor HX710B timeout, skip iterasi ini!");
    return;
  }

  // ════════════════════════════════════════════════════════
  //  STEP 2: Hitung evaporasi interval & akumulasi harian
  // ════════════════════════════════════════════════════════
  float evapInterval = hitungEvaporasiInterval(tinggi_cm);
  evaporasiHarian_mm += evapInterval;
  if (evaporasiHarian_mm > 15.0f) evaporasiHarian_mm = 15.0f;

  // ════════════════════════════════════════════════════════
  //  STEP 3: Dapatkan waktu saat ini
  // ════════════════════════════════════════════════════════
  struct tm timeinfo;
  String datetime = "0000-00-00 00:00:00";
  String tanggal  = "0000-00-00";
  String jamMenit = "--:--";
  bool   adaWaktu = getLocalTime(&timeinfo);

  if (adaWaktu) {
    char buf[25], bufJM[6];
    strftime(buf,   sizeof(buf),   "%Y-%m-%d %H:%M:%S", &timeinfo);
    datetime = buf;
    strftime(buf,   sizeof(buf),   "%Y-%m-%d",           &timeinfo);
    tanggal  = buf;
    strftime(bufJM, sizeof(bufJM), "%H:%M",              &timeinfo);
    jamMenit = bufJM;

    // ── Reset evaporasi harian jam 07:00 ──────────────────
    if (hariSebelumnya != timeinfo.tm_mday) {
      hariSebelumnya = timeinfo.tm_mday;
      sudahResetHari = false;
    }
    if (timeinfo.tm_hour == 7 && timeinfo.tm_min == 0 && !sudahResetHari) {
      resetEvaporasiHarian(jamMenit);
    }

    // ── STEP 4: Kontrol selenoid (jam 06:00–07:00) ────────
    kontrolSelenoid(tinggi_cm, timeinfo.tm_hour, jamMenit);
  }

  // ── Perbarui settings secara berkala ─────────────────────
  if (millis() - lastSettingsCek >= INTERVAL_SETTINGS) {
    lastSettingsCek = millis();
    bacaSettings();
  }

  // ════════════════════════════════════════════════════════
  //  STEP 5: Tentukan status evaporasi
  // ════════════════════════════════════════════════════════
  if      (evaporasiHarian_mm >= cfg_thresholdTinggi) statusEvaporasi = "Tinggi";
  else if (evaporasiHarian_mm >= cfg_thresholdRendah) statusEvaporasi = "Normal";
  else                                                statusEvaporasi = "Rendah";

  // ════════════════════════════════════════════════════════
  //  STEP 6: Kirim ke Firebase — Data Realtime
  // ════════════════════════════════════════════════════════
  if (millis() - lastRealtime >= cfg_intervalRealtime) {
    lastRealtime = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonRT;
      jsonRT.add("tinggi_air_cm",     tinggi_cm);
      jsonRT.add("tinggi_air_mm",     tinggi_cm * 10.0);
      jsonRT.add("evaporasi_mm",      evaporasiHarian_mm);
      jsonRT.add("status",            statusEvaporasi);
      jsonRT.add("selenoid_aktif",    relayAktif);
      jsonRT.add("datetime",          datetime);
      jsonRT.add("d0_saat_ini",       (int)D0);           // ← D0 dinamis
      jsonRT.add("dmax_saat_ini",     (int)DMAX);         // ← DMAX dinamis
      jsonRT.add("range_adc",         (int)(DMAX - D0));
      jsonRT.add("ota_versi_device",  otaStatus.versiSekarang);
      jsonRT.add("ota_versi_terbaru", otaStatus.versiTerbaru);
      jsonRT.add("ota_status",        otaStatus.statusTerakhir);
      jsonRT.add("ota_waktu_cek",     otaStatus.waktuCekTerakhir);

      if (Firebase.updateNode(fbdo, PATH_REALTIME, jsonRT)) {
        Serial.println("[Firebase] Realtime OK");
      } else {
        Serial.println("[Firebase] Realtime GAGAL: " + fbdo.errorReason());
      }
    }
  }

  // ════════════════════════════════════════════════════════
  //  STEP 7: Kirim ke Firebase — History
  // ════════════════════════════════════════════════════════
  if (millis() - lastHistory >= cfg_intervalHistory) {
    lastHistory = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonH;
      jsonH.add("tinggi_air_cm", tinggi_cm);
      jsonH.add("tinggi_air_mm", tinggi_cm * 10.0);
      jsonH.add("evaporasi_mm",  evaporasiHarian_mm);
      jsonH.add("status",        statusEvaporasi);
      jsonH.add("datetime",      datetime);
      jsonH.add("tanggal",       tanggal);
      jsonH.add("d0_snapshot",   (int)D0);
      jsonH.add("dmax_snapshot", (int)DMAX);

      if (Firebase.pushJSON(fbdo, PATH_HISTORY, jsonH)) {
        Serial.println("[Firebase] History OK");
      } else {
        Serial.println("[Firebase] History GAGAL: " + fbdo.errorReason());
      }
    }
  }

  // ════════════════════════════════════════════════════════
  //  Serial Monitor — ringkasan setiap iterasi
  // ════════════════════════════════════════════════════════
  Serial.println("================================================");
  Serial.printf("Waktu       : %s\n",   datetime.c_str());
  Serial.printf("Tinggi Air  : %.3f cm  (%.1f mm)\n",
                tinggi_cm, tinggi_cm * 10.0);
  Serial.printf("Evaporasi   : %.2f mm  (harian akumulasi)\n",
                evaporasiHarian_mm);
  Serial.printf("Status      : %s\n",   statusEvaporasi.c_str());
  Serial.printf("Selenoid    : %s\n",   relayAktif ? "ON" : "OFF");
  Serial.printf("D0          : %ld\n",  D0);
  Serial.printf("DMAX        : %ld  (Range: %ld)\n", DMAX, DMAX - D0);
  Serial.printf("Threshold   : Rendah < %.1f | Tinggi >= %.1f mm\n",
                cfg_thresholdRendah, cfg_thresholdTinggi);
  Serial.println("================================================");
}
