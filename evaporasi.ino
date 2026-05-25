// ============================================================
//  EVAPORIMETER OTOMATIS ESP32
//  Versi Outdoor Stabil + Filter Noise
//  HX710B + Firebase + OTA + Relay Otomatis
//
//  FITUR:
//  ✅ Filter Trimmed Mean
//  ✅ Moving Average
//  ✅ Anti Spike Filter
//  ✅ Validasi Evaporasi
//  ✅ Pompa Otomatis 06:00–07:00
//  ✅ Firebase Realtime + History
//  ✅ OTA Update
//  ✅ Stabil untuk Lapangan Terbuka
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

// Firebase Path
#define PATH_REALTIME     "/Monitoring/realtime"
#define PATH_HISTORY      "/Monitoring/History"
#define PATH_ACUAN        "/Monitoring/acuan_pagi_cm"
#define PATH_PANCI_TERISI "/Monitoring/panci_terisi"
#define PATH_OTA_TRIGGER  "/Monitoring/ota_trigger"

// ============================================================
// PIN
// ============================================================

#define HX710B_OUT   19
#define HX710B_SCK   18

#define DS18B20_PIN  27
#define RELAY_PIN    32

// ============================================================
// KALIBRASI
// ============================================================

const long  D0              = 9626456L;
const long  DMAX            = 10767088L;

const float TINGGI_ACUAN_CM = 20.0;

// ============================================================
// JADWAL POMPA
// ============================================================

const int JAM_POMPA_MULAI   = 6;
const int JAM_POMPA_SELESAI = 7;

// ============================================================
// HYSTERESIS RELAY
// ============================================================

const float RELAY_BATAS_BAWAH = 19.0;
const float RELAY_BATAS_ATAS  = 20.0;

// ============================================================
// INTERVAL
// ============================================================

const unsigned long INTERVAL_REALTIME = 300000UL;
const unsigned long INTERVAL_HISTORY  = 600000UL;
const unsigned long INTERVAL_BACA     = 10000UL;
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

// ============================================================
// FILTER DATA
// ============================================================

float tinggiSebelumnya = -1.0;

// ============================================================
// ACUAN PAGI
// ============================================================

float tinggiPagi_cm   = -1.0;
bool  acuanSudahDiSet = false;

// ============================================================
// STATUS
// ============================================================

String statusEvaporasi = "Normal";

bool relayAktif = false;
bool panciTerisiDikirim = false;

// ============================================================
// FUNGSI BACA HX710B
// ============================================================

unsigned long bacaSatuSampel(bool &sukses) {

  sukses = true;

  unsigned long t0 = millis();

  while (digitalRead(HX710B_OUT) == HIGH) {

    if (millis() - t0 > 2000) {
      sukses = false;
      return 0;
    }

    yield();
  }

  unsigned long count = 0;

  for (int i = 0; i < 24; i++) {

    digitalWrite(HX710B_SCK, HIGH);
    delayMicroseconds(1);

    count <<= 1;

    digitalWrite(HX710B_SCK, LOW);
    delayMicroseconds(1);

    if (digitalRead(HX710B_OUT)) {
      count++;
    }
  }

  digitalWrite(HX710B_SCK, HIGH);
  delayMicroseconds(1);

  digitalWrite(HX710B_SCK, LOW);

  count ^= 0x800000;

  return count;
}

// ============================================================
// HITUNG TINGGI AIR STABIL
// ============================================================

float hitungTinggi() {

  const int N = 50;

  bool sukses;

  // Buang pembacaan awal
  for (int i = 0; i < 5; i++) {

    bacaSatuSampel(sukses);

    delay(10);
    yield();
  }

  unsigned long sampel[N];

  int sampelValid = 0;

  // Ambil sampel
  for (int i = 0; i < N; i++) {

    unsigned long val = bacaSatuSampel(sukses);

    if (sukses) {
      sampel[sampelValid++] = val;
    }

    delay(5);
    yield();
  }

  // Minimal data valid
  if (sampelValid < 25) {
    return -99.0;
  }

  // =========================================================
  // SORTING
  // =========================================================

  for (int i = 1; i < sampelValid; i++) {

    unsigned long key = sampel[i];

    int j = i - 1;

    while (j >= 0 && sampel[j] > key) {

      sampel[j + 1] = sampel[j];

      j--;
    }

    sampel[j + 1] = key;
  }

  // =========================================================
  // TRIMMED MEAN
  // =========================================================

  int awal  = sampelValid * 20 / 100;
  int akhir = sampelValid * 80 / 100;

  unsigned long total = 0;

  for (int i = awal; i < akhir; i++) {
    total += sampel[i];
  }

  long D_rata = total / (akhir - awal);

  // =========================================================
  // KONVERSI KE CM
  // =========================================================

  float tinggi =
      (float)(D_rata - D0) *
      TINGGI_ACUAN_CM /
      (float)(DMAX - D0);

  // Limit
  if (tinggi < 0.0)  tinggi = 0.0;
  if (tinggi > 30.0) tinggi = 30.0;

  // =========================================================
  // ANTI SPIKE FILTER
  // =========================================================

  if (tinggiSebelumnya > 0) {

    float selisih = abs(tinggi - tinggiSebelumnya);

    if (selisih > 1.0) {

      Serial.println("[FILTER] Spike/noise terdeteksi!");

      tinggi = tinggiSebelumnya;
    }
  }

  // =========================================================
  // MOVING AVERAGE
  // =========================================================

  static float buffer[5];

  static int index = 0;
  static bool penuh = false;

  buffer[index] = tinggi;

  index++;

  if (index >= 5) {

    index = 0;

    penuh = true;
  }

  int jumlahData = penuh ? 5 : index;

  float totalAvg = 0;

  for (int i = 0; i < jumlahData; i++) {
    totalAvg += buffer[i];
  }

  float tinggiFinal = totalAvg / jumlahData;

  tinggiSebelumnya = tinggiFinal;

  Serial.printf("[FILTER] Tinggi Stabil: %.3f cm\n", tinggiFinal);

  return tinggiFinal;
}

// ============================================================
// BACA ACUAN FIREBASE
// ============================================================

bool bacaAcuanDariFirebase(float &hasil) {

  if (!Firebase.ready()) return false;

  if (!Firebase.getFloat(fbdo, PATH_ACUAN)) return false;

  float val = fbdo.floatData();

  if (val > 0.0 && val <= 30.0) {

    hasil = val;

    return true;
  }

  return false;
}

// ============================================================
// NOTIFIKASI PANCI TERISI
// ============================================================

void kirimNotifikasiPanciTerisi(const String &waktu) {

  if (!Firebase.ready()) return;

  FirebaseJson jsonNotif;

  jsonNotif.add("terisi", true);
  jsonNotif.add("waktu", waktu);

  if (Firebase.updateNode(fbdo, PATH_PANCI_TERISI, jsonNotif)) {

    Serial.println("[NOTIF] Panci terisi terkirim!");

  } else {

    Serial.println("[NOTIF] Gagal: " + fbdo.errorReason());
  }
}

// ============================================================
// KONTROL RELAY
// ============================================================

void kontrolRelay(
  float tinggi_cm,
  int jam,
  int menit,
  const String &waktuStr
) {

  bool jamOperasional =
      (jam >= JAM_POMPA_MULAI &&
       jam < JAM_POMPA_SELESAI);

  // =========================================================
  // LUAR JAM
  // =========================================================

  if (!jamOperasional) {

    if (relayAktif) {

      digitalWrite(RELAY_PIN, LOW);

      relayAktif = false;

      Serial.println("[RELAY] OFF — luar jam");
    }

    if (jam >= JAM_POMPA_SELESAI) {
      panciTerisiDikirim = false;
    }

    return;
  }

  // =========================================================
  // RELAY ON
  // =========================================================

  if (!relayAktif &&
      tinggi_cm < RELAY_BATAS_BAWAH) {

    digitalWrite(RELAY_PIN, HIGH);

    relayAktif = true;

    Serial.println("[RELAY] ON");
  }

  // =========================================================
  // RELAY OFF
  // =========================================================

  else if (relayAktif &&
           tinggi_cm >= RELAY_BATAS_ATAS) {

    digitalWrite(RELAY_PIN, LOW);

    relayAktif = false;

    Serial.println("[RELAY] OFF — target tercapai");

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

  // =========================================================
  // WIFI
  // =========================================================

  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Menghubungkan WiFi");

  unsigned long tWifi = millis();

  while (WiFi.status() != WL_CONNECTED) {

    if (millis() - tWifi > 15000) {

      Serial.println("\n[WiFi] Gagal!");

      ESP.restart();
    }

    delay(500);

    Serial.print(".");

    yield();
  }

  Serial.println("\nWiFi OK!");

  // =========================================================
  // NTP
  // =========================================================

  configTime(
    7 * 3600,
    0,
    "pool.ntp.org",
    "time.nist.gov"
  );

  Serial.println("Sinkronisasi NTP...");

  // =========================================================
  // FIREBASE
  // =========================================================

  config.host = FIREBASE_HOST;

  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&config, &auth);

  Firebase.reconnectWiFi(true);

  delay(1000);

  // =========================================================
  // BACA ACUAN
  // =========================================================

  Serial.println("Membaca acuan pagi...");

  if (bacaAcuanDariFirebase(tinggiPagi_cm)) {

    acuanSudahDiSet = true;

    Serial.printf(
      "[OK] Acuan: %.2f cm\n",
      tinggiPagi_cm
    );

  } else {

    acuanSudahDiSet = false;

    Serial.println("[WARNING] Acuan belum diset!");
  }

  Serial.println("=== SETUP SELESAI ===");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // =========================================================
  // WIFI RECONNECT
  // =========================================================

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("[WiFi] Reconnecting...");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    delay(5000);

    return;
  }

  // =========================================================
  // OTA CHECK
  // =========================================================

  if (millis() - lastOtaCekTrigger >= INTERVAL_OTA_CEK) {

    lastOtaCekTrigger = millis();

    if (Firebase.ready() &&
        Firebase.getBool(fbdo, PATH_OTA_TRIGGER)) {

      if (fbdo.boolData()) {

        Serial.println("[OTA] Trigger diterima!");

        Firebase.setBool(fbdo, PATH_OTA_TRIGGER, false);

        forceCheckOTA();
      }
    }
  }

  checkAndUpdateOTA();

  // =========================================================
  // INTERVAL BACA SENSOR
  // =========================================================

  if (millis() - lastBaca < INTERVAL_BACA) {
    return;
  }

  lastBaca = millis();

  // =========================================================
  // TINGGI AIR
  // =========================================================

  float tinggi_cm = hitungTinggi();

  if (tinggi_cm == -99.0) {

    Serial.println("[WARNING] Sensor timeout!");

    return;
  }

  // =========================================================
  // SUHU
  // =========================================================

  sensors.requestTemperatures();

  float suhuAir = sensors.getTempCByIndex(0);

  if (suhuAir == DEVICE_DISCONNECTED_C) {
    suhuAir = -99.0;
  }

  // =========================================================
  // WAKTU
  // =========================================================

  struct tm timeinfo;

  String datetime = "0000-00-00 00:00:00";
  String tanggal  = "0000-00-00";
  String jamMenit = "--:--";

  bool adaWaktu = getLocalTime(&timeinfo);

  if (adaWaktu) {

    char buf[25];

    strftime(
      buf,
      sizeof(buf),
      "%Y-%m-%d %H:%M:%S",
      &timeinfo
    );

    datetime = buf;

    strftime(
      buf,
      sizeof(buf),
      "%Y-%m-%d",
      &timeinfo
    );

    tanggal = buf;

    char bufJM[6];

    strftime(
      bufJM,
      sizeof(bufJM),
      "%H:%M",
      &timeinfo
    );

    jamMenit = bufJM;

    // =======================================================
    // RELAY
    // =======================================================

    if (tinggi_cm > 0 &&
        tinggi_cm <= 30.0) {

      kontrolRelay(
        tinggi_cm,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        jamMenit
      );

    } else {

      digitalWrite(RELAY_PIN, LOW);

      relayAktif = false;

      Serial.println("[RELAY] Sensor invalid!");
    }
  }

  // =========================================================
  // EVAPORASI
  // =========================================================

  float evaporasi_cm = 0.0;
  float evaporasi_mm = 0.0;

  if (acuanSudahDiSet) {

    float raw =
        tinggiPagi_cm - tinggi_cm;

    evaporasi_cm =
        (raw < 0.0) ? 0.0 : raw;

    evaporasi_mm =
        evaporasi_cm * 10.0;

    // =======================================================
    // VALIDASI
    // =======================================================

    if (evaporasi_mm > 15.0) {

      Serial.println(
        "[WARNING] Evaporasi tidak wajar!"
      );

      evaporasi_mm = 15.0;
    }
  }

  // =========================================================
  // STATUS
  // =========================================================

  if (!acuanSudahDiSet) {

    statusEvaporasi = "Acuan Belum Diset";

  } else if (evaporasi_mm > 10) {

    statusEvaporasi = "Tinggi";

  } else if (evaporasi_mm >= 2) {

    statusEvaporasi = "Normal";

  } else {

    statusEvaporasi = "Rendah";
  }

  // =========================================================
  // REALTIME FIREBASE
  // =========================================================

  if (millis() - lastRealtime >= INTERVAL_REALTIME) {

    lastRealtime = millis();

    if (Firebase.ready()) {

      FirebaseJson jsonRT;

      jsonRT.add("tinggi_air_cm", tinggi_cm);
      jsonRT.add("evaporasi_mm", evaporasi_mm);
      jsonRT.add("suhu_air_c", suhuAir);
      jsonRT.add("status", statusEvaporasi);
      jsonRT.add("relay_aktif", relayAktif);
      jsonRT.add("datetime", datetime);

      Firebase.updateNode(
        fbdo,
        PATH_REALTIME,
        jsonRT
      );

      Serial.println("[Firebase] Realtime OK");
    }
  }

  // =========================================================
  // HISTORY
  // =========================================================

  if (millis() - lastHistory >= INTERVAL_HISTORY) {

    lastHistory = millis();

    if (Firebase.ready() &&
        acuanSudahDiSet) {

      FirebaseJson jsonH;

      jsonH.add("tinggi_air_cm", tinggi_cm);
      jsonH.add("evaporasi_mm", evaporasi_mm);
      jsonH.add("suhu_air_c", suhuAir);
      jsonH.add("status", statusEvaporasi);
      jsonH.add("datetime", datetime);
      jsonH.add("tanggal", tanggal);

      Firebase.pushJSON(
        fbdo,
        PATH_HISTORY,
        jsonH
      );

      Serial.println("[Firebase] History OK");
    }
  }

  // =========================================================
  // SERIAL MONITOR
  // =========================================================

  Serial.println("================================");

  Serial.printf(
    "Tinggi Air : %.3f cm\n",
    tinggi_cm
  );

  Serial.printf(
    "Evaporasi  : %.2f mm\n",
    evaporasi_mm
  );

  Serial.printf(
    "Suhu Air   : %.2f C\n",
    suhuAir
  );

  Serial.printf(
    "Status     : %s\n",
    statusEvaporasi.c_str()
  );

  Serial.printf(
    "Relay      : %s\n",
    relayAktif ? "ON" : "OFF"
  );

  Serial.println("================================");
}