// ============================================================
//  EVAPORIMETER OTOMATIS ESP32
//  Versi Final — Tanpa Acuan Pagi
//
//  RUMUS EVAPORASI:
//  E_interval = tinggi_sebelumnya - tinggi_sekarang
//  E_harian  += E_interval (akumulasi, reset tiap 07:00)
//
//  ALUR SESUAI FLOWCHART:
//  1. Sensor HX710B → ESP32 proses data
//  2. Hubung WiFi → Sinkronisasi NTP
//  3. Ukur tinggi air
//  4. Hitung evaporasi per interval → akumulasi harian
//  5. Cek jam 06:00–07:00 → kontrol selenoid
//  6. Kirim ke Firebase + tampil di app
//  7. Reset evaporasi tiap jam 07:00
// ============================================================

#include <WiFi.h>
#include <FirebaseESP32.h>
#include "time.h"
#include "ota.h"

OtaStatus otaStatus = {
  FIRMWARE_VERSION, "-", "Belum Dicek", "", "-", -1
};

// ============================================================
// KONFIGURASI
// ============================================================

#define WIFI_SSID      "Klimatologiot"
#define WIFI_PASSWORD  "Klimatologiotkk"

#define FIREBASE_HOST  "klimatologiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH  "AIzaSyAZrk_k4DQ_ijCa6gp67oRklFMKD2dLcbQ"

#define PATH_REALTIME    "/Monitoring/realtime"
#define PATH_HISTORY     "/Monitoring/History"
#define PATH_OTA_TRIGGER "/Monitoring/ota_trigger"
#define PATH_RESET_EVAP  "/Monitoring/reset_evaporasi"  // "true" = reset akumulasi manual

// ============================================================
// PIN
// ============================================================

#define HX710B_OUT  19
#define HX710B_SCK  18
#define RELAY_PIN   32

// ============================================================
// KALIBRASI HX710B
// ============================================================

const long  D0              = 9626456L;
const long  DMAX            = 10767088L;
const float TINGGI_ACUAN_CM = 20.0;

// ============================================================
// JADWAL SELENOID — 06:00–07:00
// ============================================================

const int JAM_POMPA_MULAI   = 6;
const int JAM_POMPA_SELESAI = 7;

// Standar ketinggian air panci: 200mm = 20cm
const float STANDAR_TINGGI_CM = 20.0;

// ============================================================
// INTERVAL
// ============================================================

const unsigned long INTERVAL_REALTIME = 300000UL;  // 5 menit
const unsigned long INTERVAL_HISTORY  = 600000UL;  // 10 menit
const unsigned long INTERVAL_BACA     = 10000UL;   // 10 detik
const unsigned long INTERVAL_CMD_CEK  = 10000UL;   // 10 detik

// ============================================================
// VARIABEL GLOBAL
// ============================================================

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

unsigned long lastRealtime = 0;
unsigned long lastHistory  = 0;
unsigned long lastBaca     = 0;
unsigned long lastCmdCek   = 0;

// Sensor
float tinggiSebelumnya = -1.0;  // untuk hitung selisih

// Evaporasi akumulasi harian
float evaporasiHarian_mm = 0.0;
bool  sudahResetHari     = false;
int   hariSebelumnya     = -1;

// Status
String statusEvaporasi    = "Rendah";
bool   relayAktif         = false;
bool   panciTerisiDikirim = false;

// ============================================================
// BACA HX710B
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
// HITUNG TINGGI AIR
// Trimmed Mean + Anti Spike + Moving Average
// ============================================================

float hitungTinggi() {
  const int N = 50;
  bool sukses;

  // Warm-up
  for (int i = 0; i < 5; i++) { bacaSatuSampel(sukses); delay(10); yield(); }

  unsigned long sampel[N];
  int sampelValid = 0;
  for (int i = 0; i < N; i++) {
    unsigned long val = bacaSatuSampel(sukses);
    if (sukses) sampel[sampelValid++] = val;
    delay(5); yield();
  }
  if (sampelValid < 25) return -99.0;

  // Insertion sort
  for (int i = 1; i < sampelValid; i++) {
    unsigned long key = sampel[i]; int j = i - 1;
    while (j >= 0 && sampel[j] > key) { sampel[j+1] = sampel[j]; j--; }
    sampel[j+1] = key;
  }

  // Trimmed mean 20–80%
  int awal  = sampelValid * 20 / 100;
  int akhir = sampelValid * 80 / 100;
  unsigned long total = 0;
  for (int i = awal; i < akhir; i++) total += sampel[i];
  long D_rata = total / (akhir - awal);

  // Konversi ke cm
  float tinggi = (float)(D_rata - D0) * TINGGI_ACUAN_CM / (float)(DMAX - D0);
  if (tinggi < 0.0)  tinggi = 0.0;
  if (tinggi > 30.0) tinggi = 30.0;

  // Anti Spike: abaikan lonjakan > 1cm
  if (tinggiSebelumnya > 0) {
    float selisih = abs(tinggi - tinggiSebelumnya);
    if (selisih > 1.0) {
      Serial.printf("[FILTER] Spike %.3fcm diabaikan, pakai %.3fcm\n",
                    tinggi, tinggiSebelumnya);
      tinggi = tinggiSebelumnya;
    }
  }

  // Moving Average 5 sampel
  static float buffer[5];
  static int   idx   = 0;
  static bool  penuh = false;
  buffer[idx++] = tinggi;
  if (idx >= 5) { idx = 0; penuh = true; }
  int   n   = penuh ? 5 : idx;
  float avg = 0;
  for (int i = 0; i < n; i++) avg += buffer[i];
  float tinggiFinal = avg / n;

  Serial.printf("[SENSOR] Tinggi: %.3f cm (%.1f mm)\n",
                tinggiFinal, tinggiFinal * 10.0);
  return tinggiFinal;
}

// ============================================================
// HITUNG EVAPORASI INTERVAL
// Dipanggil setiap pembacaan sensor
// ============================================================

float hitungEvaporasiInterval(float tinggi_cm) {
  // Pembacaan pertama — belum ada data sebelumnya
  if (tinggiSebelumnya < 0) {
    tinggiSebelumnya = tinggi_cm;
    return 0.0;
  }

  float delta = tinggiSebelumnya - tinggi_cm;
  tinggiSebelumnya = tinggi_cm;  // update untuk interval berikutnya

  // Hanya akumulasi jika air benar-benar turun
  // delta negatif = air naik (isi/hujan) → abaikan
  // delta > 0.5cm per 10 detik = tidak wajar (noise) → abaikan
  if (delta <= 0.0 || delta > 0.5) {
    if (delta < -0.5) {
      Serial.printf("[INFO] Air naik %.2fcm, diabaikan dari evaporasi\n", -delta);
    }
    return 0.0;
  }

  float evap_mm = delta * 10.0;
  Serial.printf("[EVAP] Interval: %.4f mm\n", evap_mm);
  return evap_mm;
}

// ============================================================
// RESET EVAPORASI HARIAN
// ============================================================

void resetEvaporasiHarian(const String &waktu) {
  Serial.printf("[RESET] Evaporasi harian kemarin: %.2f mm\n", evaporasiHarian_mm);
  evaporasiHarian_mm = 0.0;
  sudahResetHari     = true;

  // Simpan info reset ke Firebase
  if (Firebase.ready()) {
    FirebaseJson j;
    j.add("evaporasi_mm",    0.0);
    j.add("reset_jam",       waktu);
    Firebase.updateNode(fbdo, PATH_REALTIME, j);
  }
  Serial.printf("[RESET] Evaporasi direset jam %s\n", waktu.c_str());
}

// ============================================================
// CEK PERINTAH FIREBASE
// ============================================================

void cekPerintahFirebase(const String &waktu) {
  if (!Firebase.ready()) return;

  // Reset evaporasi manual dari aplikasi
  if (Firebase.getBool(fbdo, PATH_RESET_EVAP) && fbdo.boolData()) {
    Serial.println("[CMD] Reset evaporasi manual diterima!");
    Firebase.setBool(fbdo, PATH_RESET_EVAP, false);
    evaporasiHarian_mm = 0.0;
    Serial.println("[CMD] Evaporasi direset ke 0");
  }

  // OTA trigger
  if (Firebase.getBool(fbdo, PATH_OTA_TRIGGER) && fbdo.boolData()) {
    Serial.println("[OTA] Trigger diterima!");
    Firebase.setBool(fbdo, PATH_OTA_TRIGGER, false);
    forceCheckOTA();
  }
}

// ============================================================
// KONTROL SELENOID — sesuai flowchart
// ============================================================

void kontrolSelenoid(float tinggi_cm, int jam, const String &waktuStr) {
  bool jamOperasional = (jam >= JAM_POMPA_MULAI && jam < JAM_POMPA_SELESAI);

  if (!jamOperasional) {
    if (relayAktif) {
      digitalWrite(RELAY_PIN, LOW);
      relayAktif = false;
      Serial.println("[SELENOID] OFF — luar jam 06:00-07:00");
    }
    if (jam >= JAM_POMPA_SELESAI) panciTerisiDikirim = false;
    return;
  }

  // Jam operasional: cek ketinggian
  if (tinggi_cm < STANDAR_TINGGI_CM) {
    if (!relayAktif) {
      digitalWrite(RELAY_PIN, HIGH);
      relayAktif = true;
      Serial.printf("[SELENOID] ON — %.1fmm < 200mm\n", tinggi_cm * 10.0);
    }
  } else {
    if (relayAktif) {
      digitalWrite(RELAY_PIN, LOW);
      relayAktif = false;
      Serial.printf("[SELENOID] OFF — %.1fmm >= 200mm\n", tinggi_cm * 10.0);
      if (!panciTerisiDikirim) {
        if (Firebase.ready()) {
          FirebaseJson j;
          j.add("terisi", true);
          j.add("waktu",  waktuStr);
          Firebase.updateNode(fbdo, "/Monitoring/panci_terisi", j);
        }
        panciTerisiDikirim = true;
        Serial.println("[NOTIF] Panci terisi terkirim!");
      }
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
  pinMode(HX710B_SCK, OUTPUT); digitalWrite(HX710B_SCK, LOW);
  pinMode(RELAY_PIN,  OUTPUT); digitalWrite(RELAY_PIN,  LOW);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  unsigned long tWifi = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - tWifi > 15000) { Serial.println("\n[WiFi] Gagal!"); ESP.restart(); }
    delay(500); Serial.print("."); yield();
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  // NTP
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
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000);

  Serial.println("=== SETUP SELESAI ===");
  Serial.println("[INFO] Evaporasi dihitung per interval, akumulasi harian");
  Serial.println("[INFO] Reset otomatis tiap jam 07:00\n");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(5000); return;
  }

  // OTA rutin 6 jam
  checkAndUpdateOTA();

  // Throttle 10 detik
  if (millis() - lastBaca < INTERVAL_BACA) return;
  lastBaca = millis();

  // ── STEP 1: Ukur tinggi air ──
  float tinggi_cm = hitungTinggi();
  if (tinggi_cm == -99.0) {
    Serial.println("[WARNING] Sensor timeout!");
    return;
  }

  // ── STEP 2: Hitung evaporasi interval & akumulasi ──
  float evap_interval = hitungEvaporasiInterval(tinggi_cm);
  evaporasiHarian_mm += evap_interval;

  // Batas wajar harian max 15mm
  if (evaporasiHarian_mm > 15.0) evaporasiHarian_mm = 15.0;

  // ── STEP 3: Waktu ──
  struct tm timeinfo;
  String datetime = "0000-00-00 00:00:00";
  String tanggal  = "0000-00-00";
  String jamMenit = "--:--";
  bool   adaWaktu = getLocalTime(&timeinfo);

  if (adaWaktu) {
    char buf[25], bufJM[6];
    strftime(buf,   sizeof(buf),   "%Y-%m-%d %H:%M:%S", &timeinfo); datetime = buf;
    strftime(buf,   sizeof(buf),   "%Y-%m-%d",           &timeinfo); tanggal  = buf;
    strftime(bufJM, sizeof(bufJM), "%H:%M",              &timeinfo); jamMenit = bufJM;

    // Reset harian jam 07:00
    if (hariSebelumnya != timeinfo.tm_mday) {
      hariSebelumnya = timeinfo.tm_mday;
      sudahResetHari = false;
    }
    if (timeinfo.tm_hour == 7 && timeinfo.tm_min == 0 && !sudahResetHari) {
      resetEvaporasiHarian(jamMenit);
    }

    // Cek perintah Firebase
    if (millis() - lastCmdCek >= INTERVAL_CMD_CEK) {
      lastCmdCek = millis();
      cekPerintahFirebase(jamMenit);
    }

    // ── STEP 4: Kontrol selenoid jam 06:00–07:00 ──
    kontrolSelenoid(tinggi_cm, timeinfo.tm_hour, jamMenit);
  }

  // ── STEP 5: Status evaporasi ──
  if      (evaporasiHarian_mm > 10.0) statusEvaporasi = "Tinggi";
  else if (evaporasiHarian_mm >= 2.0) statusEvaporasi = "Normal";
  else                                statusEvaporasi = "Rendah";

  // ── STEP 6: Kirim ke Firebase ──
  if (millis() - lastRealtime >= INTERVAL_REALTIME) {
    lastRealtime = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonRT;
      jsonRT.add("tinggi_air_cm",     tinggi_cm);
      jsonRT.add("tinggi_air_mm",     tinggi_cm * 10.0);
      jsonRT.add("evaporasi_mm",      evaporasiHarian_mm);
      jsonRT.add("status",            statusEvaporasi);
      jsonRT.add("selenoid_aktif",    relayAktif);
      jsonRT.add("datetime",          datetime);
      jsonRT.add("ota_versi_device",  otaStatus.versiSekarang);
      jsonRT.add("ota_versi_terbaru", otaStatus.versiTerbaru);
      jsonRT.add("ota_status",        otaStatus.statusTerakhir);
      jsonRT.add("ota_waktu_cek",     otaStatus.waktuCekTerakhir);

      if (Firebase.updateNode(fbdo, PATH_REALTIME, jsonRT))
        Serial.println("[Firebase] Realtime OK");
      else
        Serial.println("[Firebase] GAGAL: " + fbdo.errorReason());
    }
  }

  if (millis() - lastHistory >= INTERVAL_HISTORY) {
    lastHistory = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonH;
      jsonH.add("tinggi_air_cm", tinggi_cm);
      jsonH.add("tinggi_air_mm", tinggi_cm * 10.0);
      jsonH.add("evaporasi_mm",  evaporasiHarian_mm);
      jsonH.add("status",        statusEvaporasi);
      jsonH.add("datetime",      datetime);
      jsonH.add("tanggal",       tanggal);
      Firebase.pushJSON(fbdo, PATH_HISTORY, jsonH);
      Serial.println("[Firebase] History OK");
    }
  }

  // ── Serial Monitor ──
  Serial.println("================================");
  Serial.printf("Waktu      : %s\n",   datetime.c_str());
  Serial.printf("Tinggi Air : %.3f cm (%.1f mm)\n", tinggi_cm, tinggi_cm * 10.0);
  Serial.printf("Evaporasi  : %.2f mm (harian)\n",  evaporasiHarian_mm);
  Serial.printf("Status     : %s\n",   statusEvaporasi.c_str());
  Serial.printf("Selenoid   : %s\n",   relayAktif ? "ON" : "OFF");
  Serial.println("================================");
}
