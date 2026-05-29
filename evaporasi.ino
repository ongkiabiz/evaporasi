// ============================================================
//  EVAPORIMETER OTOMATIS ESP32
//  VERSI PERBAIKAN LENGKAP
//  Fix: reset harian, watchdog, settings Firebase, selenoid guard
// ============================================================

#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Preferences.h>
#include "time.h"
#include <esp_task_wdt.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include "ota.h"

// ============================================================
// WIFI
// ============================================================

#define WIFI_SSID      "Klimatologiot"
#define WIFI_PASSWORD  "Klimatologiotkk"

// ============================================================
// FIREBASE
// ============================================================

#define FIREBASE_HOST  "klimatologiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH  "AIzaSyAZrk_k4DQ_ijCa6gp67oRklFMKD2dLcbQ"

// ============================================================
// FIREBASE PATH
// ============================================================

#define PATH_REALTIME       "/Monitoring/realtime"
#define PATH_HISTORY        "/Monitoring/History"
#define PATH_SETTINGS       "/Monitoring/settings/evaporasi"
#define PATH_RESET_EVAP     "/Monitoring/reset_evaporasi"
#define PATH_OTA_TRIGGER    "/Monitoring/ota_trigger"
#define PATH_KALIBRASI      "/Monitoring/settings/kalibrasi"

// ============================================================
// PIN
// ============================================================

#define HX710B_OUT  19
#define HX710B_SCK  18
#define RELAY_PIN   32
#define SUHU_PIN    27

// ============================================================
// HX710B - Default kalibrasi
// ============================================================

long D0    = 9231122L;
long DMAX  = 10591208L;

const float TINGGI_ACUAN_CM = 20.0;

// ============================================================
// SELENOID
// ============================================================

const int JAM_POMPA_MULAI_DEFAULT   = 6;
const int JAM_POMPA_SELESAI_DEFAULT = 7;

int jamPompaMulai   = JAM_POMPA_MULAI_DEFAULT;
int jamPompaSelesai = JAM_POMPA_SELESAI_DEFAULT;

const float STANDAR_TINGGI_CM = 20.0;

// ============================================================
// WATCHDOG
// ============================================================

#define WDT_TIMEOUT_SEC 60

// ============================================================
// INTERVAL
// ============================================================

unsigned long intervalRealtime = 300000UL;
unsigned long intervalHistory  = 600000UL;
unsigned long intervalBaca     = 10000UL;
unsigned long intervalSettings = 60000UL;

// ============================================================
// GLOBAL
// ============================================================

FirebaseData fbdo;
FirebaseData fbdoRead;
FirebaseAuth auth;
FirebaseConfig config;

Preferences prefs;

OneWire oneWire(SUHU_PIN);
DallasTemperature sensors(&oneWire);

unsigned long lastRealtime = 0;
unsigned long lastHistory  = 0;
unsigned long lastBaca     = 0;
unsigned long lastSettings = 0;

float tinggiSnapshot_cm    = -1.0;
float evaporasiHarian_mm   = 0.0;

int  hariTerakhirReset    = -1;
bool snapshotDiambil      = false;

bool relayAktif  = false;
bool sensorError = false;

String statusEvaporasi = "Rendah";

// ============================================================
// OTA
// ============================================================

OtaStatus otaStatus = {
  FIRMWARE_VERSION,
  "-",
  "Belum Dicek",
  "",
  "-",
  -1
};

// ============================================================
// BACA HX710B - SATU SAMPEL
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

    if (digitalRead(HX710B_OUT)) count++;
  }

  digitalWrite(HX710B_SCK, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX710B_SCK, LOW);

  count ^= 0x800000;

  return count;
}

// ============================================================
// BACA RAW
// ============================================================

long bacaSensorRaw() {

  const int N = 30;

  bool sukses;

  unsigned long total = 0;

  int valid = 0;

  for (int i = 0; i < N; i++) {

    unsigned long val = bacaSatuSampel(sukses);

    if (sukses) {
      total += val;
      valid++;
    }

    delay(5);
  }

  if (valid < 10) return -1;

  return (long)(total / valid);
}

// ============================================================
// HITUNG TINGGI AIR
// ============================================================

float hitungTinggi() {

  long raw = bacaSensorRaw();

  if (raw < 0) return -99.0;

  if (DMAX == D0) return -99.0;

  float tinggi =
    (float)(raw - D0) *
    TINGGI_ACUAN_CM /
    (float)(DMAX - D0);

  if (tinggi < 0)  tinggi = 0;
  if (tinggi > 30) tinggi = 30;

  Serial.printf("[SENSOR] RAW: %ld\n", raw);
  Serial.printf("[SENSOR] Tinggi: %.2f cm\n", tinggi);

  return tinggi;
}

// ============================================================
// SUHU AIR
// ============================================================

float bacaSuhuAir() {

  sensors.requestTemperatures();

  float suhu = sensors.getTempCByIndex(0);

  if (suhu == DEVICE_DISCONNECTED_C) {

    Serial.println("[SUHU] Sensor gagal!");

    return -99.0;
  }

  Serial.printf("[SUHU] %.2f C\n", suhu);

  return suhu;
}

// ============================================================
// EVAPORASI HARIAN
// ============================================================

void prosesEvaporasiHarian(float tinggi_cm, int jam, int hari) {

  if (jam == 7 && hari != hariTerakhirReset) {

    if (snapshotDiambil && tinggiSnapshot_cm >= 0) {

      float delta = tinggiSnapshot_cm - tinggi_cm;

      evaporasiHarian_mm = (delta > 0) ? delta * 10.0 : 0.0;

      if (evaporasiHarian_mm > 15.0)
        evaporasiHarian_mm = 15.0;

      prefs.putFloat("evap_harian", evaporasiHarian_mm);

      Serial.printf("[EVAP] %.2f mm\n", evaporasiHarian_mm);
    }

    tinggiSnapshot_cm = tinggi_cm;
    snapshotDiambil   = true;
    hariTerakhirReset = hari;

    prefs.putFloat("snapshot", tinggiSnapshot_cm);
    prefs.putInt("snapshot_hari", hariTerakhirReset);
    prefs.putBool("snapshot_ok", true);

    Serial.printf("[EVAP] Snapshot baru %.2f cm\n", tinggiSnapshot_cm);
  }
}

// ============================================================
// STATUS EVAPORASI
// ============================================================

void updateStatusEvaporasi() {

  if (evaporasiHarian_mm >= 8.0)
    statusEvaporasi = "Tinggi";

  else if (evaporasiHarian_mm >= 3.0)
    statusEvaporasi = "Normal";

  else
    statusEvaporasi = "Rendah";
}

// ============================================================
// KONTROL SELENOID
// ============================================================

void kontrolSelenoid(float tinggi_cm, int jam) {

  if (tinggi_cm == -99.0) {

    digitalWrite(RELAY_PIN, LOW);
    relayAktif = false;

    Serial.println("[SELENOID] OFF - sensor error");

    return;
  }

  bool jamAktif =
    (jam >= jamPompaMulai &&
     jam < jamPompaSelesai);

  if (!jamAktif) {

    digitalWrite(RELAY_PIN, LOW);
    relayAktif = false;
    return;
  }

  if (tinggi_cm < STANDAR_TINGGI_CM) {

    digitalWrite(RELAY_PIN, HIGH);
    relayAktif = true;

    Serial.println("[SELENOID] ON");

  } else {

    digitalWrite(RELAY_PIN, LOW);
    relayAktif = false;

    Serial.println("[SELENOID] OFF");
  }
}

// ============================================================
// BACA SETTINGS FIREBASE
// ============================================================

void bacaSettingsFirebase() {

  if (Firebase.getJSON(fbdoRead, PATH_SETTINGS)) {

    FirebaseJson &json = fbdoRead.jsonObject();
    FirebaseJsonData result;

    if (json.get(result, "interval_realtime_ms") && result.type == "int")
      intervalRealtime = (unsigned long)result.intValue;

    if (json.get(result, "interval_history_ms") && result.type == "int")
      intervalHistory = (unsigned long)result.intValue;

    if (json.get(result, "interval_baca_ms") && result.type == "int")
      intervalBaca = (unsigned long)result.intValue;

    if (json.get(result, "jam_pompa_mulai") && result.type == "int")
      jamPompaMulai = result.intValue;

    if (json.get(result, "jam_pompa_selesai") && result.type == "int")
      jamPompaSelesai = result.intValue;

    Serial.println("[Settings] OK");
  }

  if (Firebase.getJSON(fbdoRead, PATH_KALIBRASI)) {

    FirebaseJson &json = fbdoRead.jsonObject();
    FirebaseJsonData result;

    bool updated = false;

    if (json.get(result, "d0") && result.type == "int") {
      D0 = (long)result.intValue;
      updated = true;
    }

    if (json.get(result, "dmax") && result.type == "int") {
      DMAX = (long)result.intValue;
      updated = true;
    }

    if (updated) {

      prefs.putLong("d0", D0);
      prefs.putLong("dmax", DMAX);

      Serial.printf("[Kalibrasi] D0=%ld DMAX=%ld\n", D0, DMAX);
    }
  }

  if (Firebase.getBool(fbdoRead, PATH_RESET_EVAP)) {

    if (fbdoRead.boolData() == true) {

      evaporasiHarian_mm = 0.0;
      tinggiSnapshot_cm  = -1.0;
      snapshotDiambil    = false;

      prefs.putFloat("evap_harian", 0.0);
      prefs.putBool("snapshot_ok", false);

      Firebase.setBool(fbdo, PATH_RESET_EVAP, false);

      Serial.println("[Reset] Evaporasi reset");
    }
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  // =========================================================
  // WATCHDOG TIMER (ESP32 CORE 3.x.x)
  // =========================================================

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };

  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // =========================================================
  // PIN
  // =========================================================

  pinMode(HX710B_OUT, INPUT);

  pinMode(HX710B_SCK, OUTPUT);
  digitalWrite(HX710B_SCK, LOW);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  sensors.begin();

  // =========================================================
  // PREFERENCES
  // =========================================================

  prefs.begin("evaporimeter", false);

  D0   = prefs.getLong("d0", D0);
  DMAX = prefs.getLong("dmax", DMAX);

  evaporasiHarian_mm = prefs.getFloat("evap_harian", 0.0);
  tinggiSnapshot_cm  = prefs.getFloat("snapshot", -1.0);
  hariTerakhirReset  = prefs.getInt("snapshot_hari", -1);
  snapshotDiambil    = prefs.getBool("snapshot_ok", false);

  // =========================================================
  // WIFI
  // =========================================================

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Menghubungkan WiFi");

  unsigned long t0 = millis();

  while (WiFi.status() != WL_CONNECTED) {

    if (millis() - t0 > 30000) {

      Serial.println("\n[WiFi] Timeout restart");
      ESP.restart();
    }

    delay(500);
    Serial.print(".");

    esp_task_wdt_reset();
  }

  Serial.println();
  Serial.println("WiFi Connected");

  // =========================================================
  // NTP
  // =========================================================

  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  struct tm tmCheck;

  int ntpTry = 0;

  while (!getLocalTime(&tmCheck) && ntpTry < 10) {

    delay(1000);

    ntpTry++;

    esp_task_wdt_reset();
  }

  // =========================================================
  // FIREBASE
  // =========================================================

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Firebase Connected");

  bacaSettingsFirebase();

  Serial.println("SYSTEM READY");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  esp_task_wdt_reset();

  // =========================================================
  // WIFI RECONNECT
  // =========================================================

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("[WiFi] Reconnecting...");
    WiFi.reconnect();

    delay(5000);

    return;
  }

  // =========================================================
  // OTA
  // =========================================================

  checkAndUpdateOTA();

  // =========================================================
  // SETTINGS
  // =========================================================

  if (millis() - lastSettings >= intervalSettings) {

    lastSettings = millis();

    bacaSettingsFirebase();
  }

  // =========================================================
  // INTERVAL SENSOR
  // =========================================================

  if (millis() - lastBaca < intervalBaca)
    return;

  lastBaca = millis();

  // =========================================================
  // SENSOR
  // =========================================================

  float tinggi_cm = hitungTinggi();

  sensorError = (tinggi_cm == -99.0);

  if (sensorError) {

    Serial.println("[ERROR] Sensor timeout");

    kontrolSelenoid(-99.0, 0);

    return;
  }

  float suhuAir = bacaSuhuAir();

  // =========================================================
  // WAKTU
  // =========================================================

  struct tm timeinfo;

  int jam  = 0;
  int hari = 0;

  String datetime = "-";

  if (getLocalTime(&timeinfo)) {

    char buf[25];

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    datetime = buf;

    jam  = timeinfo.tm_hour;
    hari = timeinfo.tm_yday;
  }

  // =========================================================
  // EVAPORASI
  // =========================================================

  prosesEvaporasiHarian(tinggi_cm, jam, hari);

  updateStatusEvaporasi();

  // =========================================================
  // SELENOID
  // =========================================================

  kontrolSelenoid(tinggi_cm, jam);

  // =========================================================
  // SERIAL DEBUG
  // =========================================================

  Serial.println("================================");

  Serial.printf("Waktu     : %s\n", datetime.c_str());
  Serial.printf("Tinggi    : %.2f cm\n", tinggi_cm);
  Serial.printf("Suhu Air  : %.2f C\n", suhuAir);
  Serial.printf("Evaporasi : %.2f mm\n", evaporasiHarian_mm);
  Serial.printf("Status    : %s\n", statusEvaporasi.c_str());
  Serial.printf("Selenoid  : %s\n", relayAktif ? "ON" : "OFF");

  Serial.println("================================");

  // =========================================================
  // FIREBASE REALTIME
  // =========================================================

  if (millis() - lastRealtime >= intervalRealtime) {

    lastRealtime = millis();

    FirebaseJson json;

    json.add("tinggi_air_cm", tinggi_cm);
    json.add("tinggi_air_mm", tinggi_cm * 10.0);
    json.add("suhu_air", suhuAir);
    json.add("evaporasi_mm", evaporasiHarian_mm);
    json.add("status", statusEvaporasi);
    json.add("selenoid", relayAktif);
    json.add("sensor_error", sensorError);
    json.add("datetime", datetime);
    json.add("d0", (int)D0);
    json.add("dmax", (int)DMAX);
    json.add("snapshot_cm", tinggiSnapshot_cm);
    json.add("ota_version", otaStatus.versiSekarang);

    if (Firebase.updateNode(fbdo, PATH_REALTIME, json)) {

      Serial.println("[Firebase] Realtime OK");

    } else {

      Serial.println(fbdo.errorReason());
    }
  }

  // =========================================================
  // FIREBASE HISTORY
  // =========================================================

  if (millis() - lastHistory >= intervalHistory) {

    lastHistory = millis();

    FirebaseJson jsonH;

    jsonH.add("tinggi_air_cm", tinggi_cm);
    jsonH.add("tinggi_air_mm", tinggi_cm * 10.0);
    jsonH.add("suhu_air", suhuAir);
    jsonH.add("evaporasi_mm", evaporasiHarian_mm);
    jsonH.add("status", statusEvaporasi);
    jsonH.add("datetime", datetime);

    if (Firebase.pushJSON(fbdo, PATH_HISTORY, jsonH)) {

      Serial.println("[Firebase] History OK");

    } else {

      Serial.println(fbdo.errorReason());
    }
  }
}
