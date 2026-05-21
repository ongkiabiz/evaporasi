// ============================================================
//  EVAPORIMETER OTOMATIS ESP32 — FULL AUTOMATIC RESET
//  Fitur: 
//    1. Auto-Reset Harian Jam 07:00 Pagi (Standar BMKG)
//    2. Auto-Reset Cerdas saat Mendeteksi Pengisian Air (> 1.0 cm)
// ============================================================

#include <WiFi.h>
#include <FirebaseESP32.h>
#include "time.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ota.h"    // OTA Update dari GitHub Releases

// ============================================================
// KONFIGURASI — sesuaikan di sini saja
// ============================================================
#define WIFI_SSID       "Klimatologiot"
#define WIFI_PASSWORD   "Klimatologiotkk"

#define FIREBASE_HOST   "klimatologiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "AIzaSyAZrk_k4DQ_ijCa6gp67oRklFMKD2dLcbQ"

// Path Firebase
#define PATH_REALTIME   "/Monitoring/realtime"
#define PATH_HISTORY    "/Monitoring/History"
#define PATH_ACUAN      "/Monitoring/acuan_pagi_cm"

// Pin
#define HX710B_OUT      19
#define HX710B_SCK      18
#define DS18B20_PIN     27
#define RELAY_PIN       32

// ============================================================
// KALIBRASI HX710B (HUBUNGAN NORMAL)
// ============================================================
const long  D0              = 9626456L;
const long  DMAX            = 10767088L;
const float TINGGI_ACUAN_CM = 20.0;

// Interval (milidetik)
const unsigned long INTERVAL_REALTIME = 300000UL;   // 5 menit
const unsigned long INTERVAL_HISTORY  = 600000UL;   // 10 menit
const unsigned long INTERVAL_BACA     = 10000UL;    // baca sensor tiap 10 detik

// ============================================================
// OBJEK & VARIABEL GLOBAL
// ============================================================
FirebaseData      fbdo;
FirebaseAuth      auth;
FirebaseConfig    config;
OneWire           oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

unsigned long lastRealtime = 0;
unsigned long lastHistory  = 0;
unsigned long lastBaca     = 0;

float  tinggiPagi_cm     = -1.0;
float  tinggiSebelumnya  = -1.0; // Untuk mendeteksi pengisian air
bool   sudahResetHari    = false;
int    hariSebelumnya    = -1;
String statusEvaporasi   = "Normal";

// ============================================================
// FUNGSI: Baca satu sampel HX710B dengan Deteksi Error
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

  digitalWrite(HX710B_SCK, HIGH);
  delayMicroseconds(1);
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

  for (int i = 0; i < 5; i++) {
    bacaSatuSampel(sukses);
    delay(10);
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
  }

  if (sampelValid < 25) {
    return -99.0; 
  }

  for (int i = 1; i < sampelValid; i++) {
    unsigned long kunci = sampel[i];
    int j = i - 1;
    while (j >= 0 && sampel[j] > kunci) {
      sampel[j + 1] = sampel[j];
      j--;
    }
    sampel[j + 1] = kunci;
  }

  int awal  = sampelValid * 20 / 100;
  int akhir = sampelValid * 80 / 100;
  unsigned long total = 0;
  for (int i = awal; i < akhir; i++) total += sampel[i];
  long D_rata = (long)(total / (akhir - awal));

  Serial.print("[SENSOR] Raw ADC Rata-rata: ");
  Serial.println(D_rata);

  // Konversi ADC → cm
  float tinggi = (float)(D_rata - D0) * TINGGI_ACUAN_CM / (float)(DMAX - D0);

  if (tinggi < 0.0)  tinggi = 0.0;
  if (tinggi > 30.0) tinggi = 30.0;

  return tinggi;
}

// ============================================================
// FUNGSI: Firebase Helper
// ============================================================
void simpanAcuanPagi(float nilai) {
  if (Firebase.ready()) {
    if (Firebase.setFloat(fbdo, PATH_ACUAN, nilai)) {
      Serial.print("[Firebase] Acuan pagi diperbarui: ");
      Serial.print(nilai, 3);
      Serial.println(" cm");
    } else {
      Serial.println("[Firebase] Gagal simpan acuan pagi!");
    }
  }
}

bool bacaAcuanDariFirebase(float &hasil) {
  if (Firebase.ready() && Firebase.getFloat(fbdo, PATH_ACUAN)) {
    hasil = fbdo.floatData();
    if (hasil > 0.0 && hasil <= 30.0) {
      return true;
    }
  }
  return false;
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

  // WiFi Connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  unsigned long tWifi = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - tWifi > 15000) {
      Serial.println("\n[WiFi] Gagal konek! Restart...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  // NTP Time
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sinkronisasi waktu");
  struct tm t;
  unsigned long tNtp = millis();
  while (!getLocalTime(&t)) {
    if (millis() - tNtp > 10000) { Serial.println(" GAGAL!"); break; }
    delay(500); Serial.print(".");
  }
  Serial.println(" OK!");

  // Firebase Init
  config.host                       = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000);

  // Inisialisasi Acuan Pagi
  Serial.println("Mengambil acuan pagi...");
  if (bacaAcuanDariFirebase(tinggiPagi_cm)) {
    Serial.print("[OK] Acuan dari Firebase: ");
    Serial.print(tinggiPagi_cm, 3);
    Serial.println(" cm");
  } else {
    Serial.println("[INFO] Tidak ada data di Firebase, ukur dari sensor...");
    delay(2000);
    float acuanAwal = hitungTinggi();
    if (acuanAwal != -99.0) {
      tinggiPagi_cm = acuanAwal;
      simpanAcuanPagi(tinggiPagi_cm);
    } else {
      tinggiPagi_cm = TINGGI_ACUAN_CM;
    }
  }

  // Set tinggiSebelumnya untuk pertama kali
  float tinggiAwal = hitungTinggi();
  if (tinggiAwal != -99.0) {
    tinggiSebelumnya = tinggiAwal;
  }

  Serial.println("=== SETUP SELESAI, MULAI MONITORING ===\n");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Putus, reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(5000);
    return;
  }

  // OTA — cek update firmware dari GitHub setiap 6 jam
  checkAndUpdateOTA();

  if (millis() - lastBaca < INTERVAL_BACA) return;
  lastBaca = millis();

  // 1. Tinggi Air Sekarang
  float tinggi_cm = hitungTinggi();
  if (tinggi_cm == -99.0) {
    Serial.println("[WARNING] Pengukuran dilewati karena sensor TIMEOUT!");
    return;
  }

  // ── DETEKSI OTOMATIS PENGISIAN AIR (AUTO-RESET) ──
  if (tinggiSebelumnya > 0.0) {
    // Jika air tiba-tiba naik lebih dari 1.0 cm dibanding data sebelumnya
    if ((tinggi_cm - tinggiSebelumnya) > 1.0) {
      Serial.println("\n[SENSING] TERDETEKSI PENGISIAN AIR PANCI!");
      Serial.print("[SENSING] Tinggi Sebelumnya: "); Serial.print(tinggiSebelumnya); Serial.println(" cm");
      Serial.print("[SENSING] Tinggi Baru: "); Serial.print(tinggi_cm); Serial.println(" cm");
      
      // Jadikan tinggi baru sebagai acuan pagi secara otomatis
      tinggiPagi_cm = tinggi_cm;
      simpanAcuanPagi(tinggiPagi_cm);
      Serial.println("[AUTO-RESET] Acuan air berhasil disinkronkan secara otomatis!\n");
    }
  }
  tinggiSebelumnya = tinggi_cm; // Perbarui data pembanding

  // 2. Suhu Air
  sensors.requestTemperatures();
  float suhuAir = sensors.getTempCByIndex(0);
  if (suhuAir == DEVICE_DISCONNECTED_C) {
    suhuAir = -99.0;
  }

  // 3. Waktu & Auto Reset Jam 07:00 Pagi (Standar BMKG)
  struct tm timeinfo;
  String waktu    = "--:--:--";
  String tanggal  = "0000-00-00";
  String datetime = "0000-00-00 00:00:00";
  bool   adaWaktu = getLocalTime(&timeinfo);

  if (adaWaktu) {
    char buf[25];
    strftime(buf, sizeof(buf), "%H:%M:%S",          &timeinfo); waktu    = buf;
    strftime(buf, sizeof(buf), "%Y-%m-%d",           &timeinfo); tanggal  = buf;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo); datetime = buf;

    if (hariSebelumnya != timeinfo.tm_mday) {
      hariSebelumnya = timeinfo.tm_mday;
      sudahResetHari = false;
    }

    // Auto Reset harian jam 07:00 pagi
    if (timeinfo.tm_hour == 7 && timeinfo.tm_min == 0 && !sudahResetHari) {
      Serial.println("======================================");
      Serial.println("MEMULAI AUTO-RESET ACUAN PAGI (07:00)");
      Serial.println("======================================");
      float totalTinggi = 0.0;
      int jumlahData = 0;
      unsigned long mulaiSampling = millis();

      while (millis() - mulaiSampling < 600000UL) {
        float tinggiSample = hitungTinggi();
        if (tinggiSample > 0.0 && tinggiSample < 30.0) {
          totalTinggi += tinggiSample;
          jumlahData++;
        }
        delay(10000);
      }

      if (jumlahData > 0) {
        tinggiPagi_cm = totalTinggi / jumlahData;
        simpanAcuanPagi(tinggiPagi_cm);
      }
      sudahResetHari = true;
    }

    // Relay Pompa
    if (timeinfo.tm_hour == 7 && tinggi_cm < (TINGGI_ACUAN_CM - 2.0)) {
      digitalWrite(RELAY_PIN, HIGH);
    } else {
      digitalWrite(RELAY_PIN, LOW);
    }
  }

  // 4. Hitung Evaporasi
  float evaporasi_cm = tinggiPagi_cm - tinggi_cm;
  if (evaporasi_cm < 0.0) evaporasi_cm = 0.0; 
  float evaporasi_mm = evaporasi_cm * 10.0;

  // 5. Status Evaporasi
  if      (evaporasi_mm > 10.0) statusEvaporasi = "Tinggi";
  else if (evaporasi_mm >= 2.0) statusEvaporasi = "Normal";
  else                          statusEvaporasi = "Rendah";

  // 6. Kirim ke Firebase — Realtime (tiap 5 menit)
  if (millis() - lastRealtime >= INTERVAL_REALTIME) {
    lastRealtime = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonRT;
      jsonRT.add("tinggi_air_cm",  tinggi_cm);
      jsonRT.add("acuan_pagi_cm",  tinggiPagi_cm);
      jsonRT.add("evaporasi_mm",   evaporasi_mm);
      jsonRT.add("suhu_air_c",     suhuAir);
      jsonRT.add("status",         statusEvaporasi);
      jsonRT.add("datetime",       datetime);

      if (Firebase.updateNode(fbdo, PATH_REALTIME, jsonRT)) {
        Serial.println("[Firebase] Realtime OK.");
      } else {
        Serial.println("[Firebase] Realtime GAGAL: " + fbdo.errorReason());
      }
    }
  }

  // 7. Simpan History (tiap 10 menit)
  if (millis() - lastHistory >= INTERVAL_HISTORY) {
    lastHistory = millis();
    if (Firebase.ready()) {
      FirebaseJson jsonH;
      jsonH.add("tinggi_air_cm", tinggi_cm);
      jsonH.add("evaporasi_mm",  evaporasi_mm);
      jsonH.add("suhu_air_c",    suhuAir);
      jsonH.add("status",        statusEvaporasi);
      jsonH.add("datetime",      datetime);
      jsonH.add("tanggal",       tanggal);

      if (Firebase.pushJSON(fbdo, PATH_HISTORY, jsonH)) {
        Serial.println("[Firebase] History OK.");
      } else {
        Serial.println("[Firebase] History GAGAL: " + fbdo.errorReason());
      }
    }
  }

  // 8. Serial Monitor
  Serial.println("==========================================");
  Serial.print  ("Waktu      : "); Serial.println(datetime);
  Serial.print  ("Tinggi Air : "); Serial.print(tinggi_cm, 3);    Serial.println(" cm");
  Serial.print  ("Acuan Pagi : "); Serial.print(tinggiPagi_cm, 3); Serial.println(" cm");
  Serial.print  ("Evaporasi  : "); Serial.print(evaporasi_mm, 2);  Serial.println(" mm");
  Serial.print  ("Suhu Air   : "); Serial.print(suhuAir, 2);       Serial.println(" °C");
  Serial.print  ("Status     : "); Serial.println(statusEvaporasi);
  Serial.println("==========================================");
}