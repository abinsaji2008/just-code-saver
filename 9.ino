// ============================================================
// ESP32 Schedule Controller + Firebase Realtime Database
//
// Hardware:
//   ESP32
//   DS3231 RTC
//   16x4 I2C LCD
//   Rotary encoder
//
// LCD:
//   SDA -> GPIO 21
//   SCL -> GPIO 22
//
// Rotary encoder:
//   CLK -> GPIO 32
//   DT  -> GPIO 33
//   SW  -> GPIO 25
//   GND -> GND
//
// Libraries:
//   - hd44780 by Bill Perry
//   - RTClib by Adafruit
//   - FirebaseClient by Mobizt
//   - FirebaseJson (installed as FirebaseClient dependency / library)
//   - Preferences (ESP32 core)
//
// Firebase database structure:
//
// schedules
//   S1
//     hour: 8
//     minute: 30
//     editedAt: 1789700000123
//     editedBy: "ESP32"
//     deviceId: "schedule-controller-01"
//   S2
//     ...
//
// IMPORTANT:
//   This sketch uses the DS3231 as the local offline clock.
//   The DS3231 is treated as IST (UTC+05:30).
//   When Wi-Fi is available, NTP can update the DS3231.
// ============================================================

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <RTClib.h>
#include <Preferences.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <FirebaseJson.h>
#include <time.h>

// ------------------------------------------------------------
// USER CONFIGURATION
// ------------------------------------------------------------

// Wi-Fi
#define WIFI_SSID     "Aloor EXT"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Firebase Authentication
#define API_KEY       "AIzaSyDoxgNroD1snyw_QmnBS-W85XIqFMhnBfk"
#define USER_EMAIL    "YOUR_FIREBASE_EMAIL"
#define USER_PASSWORD "YOUR_FIREBASE_PASSWORD"

// Firebase Realtime Database
#define DATABASE_URL  "https://smart-d899d-default-rtdb.firebaseio.com"

// Unique ID for this ESP32 controller
#define DEVICE_ID "schedule-controller-01"

// ------------------------------------------------------------
// NTP / timezone
// India Standard Time = UTC + 5:30
// ------------------------------------------------------------

const long IST_OFFSET_SECONDS = 19800;

const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";

const bool SYNC_RTC_FROM_NTP = true;

// ------------------------------------------------------------
// Pins
// ------------------------------------------------------------

const int SDA_PIN = 21;
const int SCL_PIN = 22;

const int ENCODER_CLK = 32;
const int ENCODER_DT = 33;
const int ENCODER_BUTTON = 25;

// ------------------------------------------------------------
// Objects
// ------------------------------------------------------------

hd44780_I2Cexp lcd;
RTC_DS3231 rtc;
Preferences memory;

// Firebase
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

UserAuth user_auth(
  API_KEY,
  USER_EMAIL,
  USER_PASSWORD,
  3000
);

FirebaseApp app;
RealtimeDatabase Database;

// ------------------------------------------------------------
// Schedule defaults
// ------------------------------------------------------------

int scheduleHour[6] = {
  8, 10, 12, 14, 16, 18
};

int scheduleMinute[6] = {
  0, 0, 0, 0, 0, 0
};

// Last edit timestamp for each schedule.
// Stored as Unix epoch milliseconds UTC.
uint64_t scheduleEditedAt[6] = {
  0, 0, 0, 0, 0, 0
};

// True when this schedule was changed locally and has not
// yet been synchronized successfully.
bool pendingChanges[6] = {
  false, false, false, false, false, false
};

// ------------------------------------------------------------
// Schedule match pulse
// ------------------------------------------------------------

bool scheduleMatched = false;

const unsigned long TRUE_DURATION = 1000;

unsigned long matchStartedAt = 0;

uint32_t lastCheckedMinute = UINT32_MAX;

// ------------------------------------------------------------
// Screen state
// ------------------------------------------------------------

enum Screen {
  CLOCK_SCREEN,
  CHOOSE_SCHEDULE,
  CHANGE_HOUR,
  CHANGE_MINUTE
};

Screen screen = CLOCK_SCREEN;

int selectedSchedule = 0;
int firstVisibleSchedule = 0;

int newHour = 0;
int newMinute = 0;

bool saveError = false;
bool updateDisplay = true;

// ------------------------------------------------------------
// Clock
// ------------------------------------------------------------

DateTime now(2026, 1, 1, 0, 0, 0);

unsigned long lastClockRead = 0;

// ------------------------------------------------------------
// Menu timeout
// ------------------------------------------------------------

const unsigned long MENU_TIMEOUT = 10000;
unsigned long lastActivityAt = 0;

// ------------------------------------------------------------
// Encoder
// ------------------------------------------------------------

int lastEncoderCLK = HIGH;

unsigned long lastEncoderChange = 0;

const unsigned long ENCODER_DEBOUNCE_US = 2000;

// ------------------------------------------------------------
// Wi-Fi / Firebase sync
// ------------------------------------------------------------

bool firebaseStarted = false;

bool wifiWasConnected = false;

unsigned long lastWiFiAttempt = 0;

const unsigned long WIFI_RETRY_INTERVAL = 15000;

unsigned long lastFirebaseSyncRequest = 0;

const unsigned long FIREBASE_SYNC_INTERVAL = 10000;

bool firebaseSyncInFlight = false;

bool firebaseUploadInFlight = false;

int uploadScheduleIndex = -1;

// When Wi-Fi first returns, read Firebase before uploading any
// pending local change. This is important for conflict handling.
bool needFirebaseSync = true;

// ------------------------------------------------------------
// Serial value entry -> Firebase
// Type any line in Serial Monitor and press Enter.
// It will be written to /test/value.
// ------------------------------------------------------------

String serialInput = "";

bool serialWriteInFlight = false;

// ------------------------------------------------------------
// Forward declarations for Serial Firebase test
// ------------------------------------------------------------

void handleSerialInput();
void serialWriteCallback(AsyncResult &aResult);

// ------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------

void firebaseReadCallback(AsyncResult &aResult);
void firebaseWriteCallback(AsyncResult &aResult);

void requestFirebaseSchedules();
void startNextUpload();

void processFirebaseSchedules(const String &payload);

void saveLocalSchedule(int index);
void saveLocalEditMetadata(int index);

void markScheduleEditedLocally(int index);

uint64_t getCurrentUtcMs();

void syncRtcFromNtp();

void startFirebase();

void handleWiFi();

void printFirebaseResult(AsyncResult &aResult);

// ============================================================
// SERIAL VALUE -> FIREBASE
// ============================================================

void handleSerialInput() {

  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\\r') {
      continue;
    }

    if (c == '\\n') {

      serialInput.trim();

      if (serialInput.length() == 0) {
        continue;
      }

      Serial.println();
      Serial.print("Serial value: ");
      Serial.println(serialInput);

      if (!firebaseStarted ||
          !app.ready() ||
          WiFi.status() != WL_CONNECTED) {

        Serial.println(
          "Firebase is not ready. Value not written."
        );

        serialInput = "";
        continue;
      }

      if (serialWriteInFlight) {

        Serial.println(
          "A Firebase serial write is already in progress."
        );

        serialInput = "";
        continue;
      }

      serialWriteInFlight = true;

      Serial.println(
        "Writing to /test/value ..."
      );

      Database.set<String>(
        aClient,
        "/test/value",
        serialInput,
        serialWriteCallback,
        "serialValueWrite"
      );

      serialInput = "";

      continue;
    }

    // Limit accidental overlong input.
    if (serialInput.length() < 500) {
      serialInput += c;
    }
  }
}

// ============================================================
// SERIAL WRITE CALLBACK
// ============================================================

void serialWriteCallback(
  AsyncResult &aResult
) {

  if (aResult.uid() != "serialValueWrite") {
    return;
  }

  if (!aResult.isResult()) {
    return;
  }

  if (aResult.isError()) {

    Serial.printf(
      "Serial Firebase write error: %s, code: %d\\n",
      aResult.error().message().c_str(),
      aResult.error().code()
    );

    serialWriteInFlight = false;
    return;
  }

  if (aResult.available()) {

    Serial.println();
    Serial.println(
      "========================================"
    );
    Serial.println(
      " FIREBASE VALUE WRITE SUCCESS"
    );
    Serial.println(
      "========================================"
    );
    Serial.println(
      "Path: /test/value"
    );

    Serial.print(
      "Response: "
    );

    Serial.println(
      aResult.c_str()
    );

    Serial.println();
    Serial.println(
      "Enter another value:"
    );

    serialWriteInFlight = false;
  }
}

// ============================================================
// LCD
// ============================================================

void showLine(int row, const char *text) {
  char paddedText[17];

  snprintf(
    paddedText,
    sizeof(paddedText),
    "%-16.16s",
    text
  );

  lcd.setCursor(0, row);
  lcd.print(paddedText);
}

// ============================================================
// BUTTON
// ============================================================

bool buttonWasPressed() {
  static bool previousReading = HIGH;
  static bool stableReading = HIGH;
  static unsigned long changedAt = 0;

  bool reading = digitalRead(ENCODER_BUTTON);

  if (reading != previousReading) {
    changedAt = millis();
    previousReading = reading;
  }

  if (
    millis() - changedAt >= 30 &&
    reading != stableReading
  ) {
    stableReading = reading;

    if (stableReading == LOW) {
      return true;
    }
  }

  return false;
}

// ============================================================
// ROTARY ENCODER
//
// Uses falling edge of CLK.
// DT determines direction.
//
// Return:
//   +1
//   -1
//    0
// ============================================================

int readRotation() {
  int clk = digitalRead(ENCODER_CLK);

  if (clk != lastEncoderCLK) {

    unsigned long currentMicros = micros();

    if (
      currentMicros - lastEncoderChange >=
      ENCODER_DEBOUNCE_US
    ) {

      lastEncoderChange = currentMicros;
      lastEncoderCLK = clk;

      if (clk == LOW) {

        int dt = digitalRead(ENCODER_DT);

        if (dt == HIGH) {
          return 1;
        } else {
          return -1;
        }
      }
    }

    lastEncoderCLK = clk;
  }

  return 0;
}

// ============================================================
// LOCAL STORAGE
// ============================================================

void saveLocalSchedule(int index) {
  char key[16];

  uint16_t minutes =
    scheduleHour[index] * 60 +
    scheduleMinute[index];

  snprintf(
    key,
    sizeof(key),
    "s%dtime",
    index
  );

  memory.putUShort(key, minutes);
}

void saveLocalEditMetadata(int index) {
  char key[16];

  snprintf(
    key,
    sizeof(key),
    "s%dedited",
    index
  );

  memory.putULong64(
    key,
    scheduleEditedAt[index]
  );

  snprintf(
    key,
    sizeof(key),
    "s%dpending",
    index
  );

  memory.putBool(
    key,
    pendingChanges[index]
  );
}

void loadSchedules() {

  for (int i = 0; i < 6; i++) {

    char key[16];

    int defaultTime =
      scheduleHour[i] * 60 +
      scheduleMinute[i];

    snprintf(
      key,
      sizeof(key),
      "s%dtime",
      i
    );

    uint16_t savedTime =
      memory.getUShort(
        key,
        defaultTime
      );

    if (savedTime < 1440) {

      scheduleHour[i] =
        savedTime / 60;

      scheduleMinute[i] =
        savedTime % 60;
    }

    // Edit timestamp
    snprintf(
      key,
      sizeof(key),
      "s%dedited",
      i
    );

    scheduleEditedAt[i] =
      memory.getULong64(
        key,
        0
      );

    // Pending flag
    snprintf(
      key,
      sizeof(key),
      "s%dpending",
      i
    );

    pendingChanges[i] =
      memory.getBool(
        key,
        false
      );
  }
}

// ============================================================
// CURRENT UTC TIMESTAMP
//
// DS3231 contains local IST time.
// Convert it to Unix UTC milliseconds.
// ============================================================

uint64_t getCurrentUtcMs() {

  DateTime t = rtc.now();

  uint64_t localSeconds =
    (uint64_t)t.unixtime();

  const uint64_t IST_SECONDS =
    (uint64_t)IST_OFFSET_SECONDS;

  if (localSeconds < IST_SECONDS) {
    return 0;
  }

  uint64_t utcSeconds =
    localSeconds - IST_SECONDS;

  return utcSeconds * 1000ULL;
}

// ============================================================
// MARK LOCAL EDIT
// ============================================================

void markScheduleEditedLocally(int index) {

  scheduleEditedAt[index] =
    getCurrentUtcMs();

  // If RTC time is invalid, do not create a fake timestamp.
  if (scheduleEditedAt[index] == 0) {

    Serial.println(
      "WARNING: RTC timestamp invalid."
    );

    saveError = true;
    return;
  }

  pendingChanges[index] = true;

  saveLocalSchedule(index);
  saveLocalEditMetadata(index);

  Serial.printf(
    "Local edit S%d -> %02d:%02d, editAt=%llu\n",
    index + 1,
    scheduleHour[index],
    scheduleMinute[index],
    (unsigned long long)scheduleEditedAt[index]
  );
}

// ============================================================
// NTP -> DS3231
// ============================================================

void syncRtcFromNtp() {

  if (!SYNC_RTC_FROM_NTP) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.println("Synchronizing RTC from NTP...");

  configTime(
    IST_OFFSET_SECONDS,
    0,
    NTP_SERVER_1,
    NTP_SERVER_2
  );

  struct tm timeinfo;

  if (
    !getLocalTime(
      &timeinfo,
      5000
    )
  ) {

    Serial.println(
      "NTP sync failed. Keeping DS3231 time."
    );

    return;
  }

  DateTime ntpTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  );

  rtc.adjust(ntpTime);

  now = rtc.now();

  Serial.printf(
    "RTC updated: %04d-%02d-%02d %02d:%02d:%02d\n",
    now.year(),
    now.month(),
    now.day(),
    now.hour(),
    now.minute(),
    now.second()
  );
}

// ============================================================
// FIREBASE START
// ============================================================

void startFirebase() {

  if (firebaseStarted) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.println(
    "Starting Firebase..."
  );

  ssl_client.setInsecure();

  // ESP32 core 2.0.17:
  // setConnectionTimeout() is not available.
  ssl_client.setHandshakeTimeout(5);

  initializeApp(
    aClient,
    app,
    getAuth(user_auth),
    printFirebaseResult,
    "authTask"
  );

  app.getApp<RealtimeDatabase>(
    Database
  );

  Database.url(
    DATABASE_URL
  );

  firebaseStarted = true;

  Serial.println(
    "Firebase initialization started."
  );

  needFirebaseSync = true;
}

// ============================================================
// FIREBASE RESULT DEBUG
// ============================================================

void printFirebaseResult(
  AsyncResult &aResult
) {

  if (!aResult.isResult()) {
    return;
  }

  if (aResult.isError()) {

    Firebase.printf(
      "Firebase error: %s, code: %d\n",
      aResult.error().message().c_str(),
      aResult.error().code()
    );
  }

  if (aResult.isDebug()) {

    Firebase.printf(
      "Firebase debug: %s\n",
      aResult.debug().c_str()
    );
  }
}

// ============================================================
// REQUEST FIREBASE SCHEDULES
// ============================================================

void requestFirebaseSchedules() {

  if (!firebaseStarted) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!app.ready()) {
    return;
  }

  if (firebaseSyncInFlight) {
    return;
  }

  // Do not read while a local upload is in progress.
  if (firebaseUploadInFlight) {
    return;
  }

  firebaseSyncInFlight = true;

  Serial.println(
    "Firebase: reading /schedules ..."
  );

  Database.get(
    aClient,
    "/schedules",
    firebaseReadCallback,
    false,
    "scheduleRead"
  );
}

// ============================================================
// FIREBASE READ CALLBACK
// ============================================================

void firebaseReadCallback(
  AsyncResult &aResult
) {

  if (
    aResult.uid() != "scheduleRead"
  ) {
    return;
  }

  if (!aResult.isResult()) {
    return;
  }

  if (aResult.isError()) {

    firebaseSyncInFlight = false;

    Firebase.printf(
      "Firebase read error: %s, code: %d\n",
      aResult.error().message().c_str(),
      aResult.error().code()
    );

    return;
  }

  if (!aResult.available()) {
    return;
  }

  String payload =
    aResult.c_str();

  firebaseSyncInFlight = false;

  Serial.println(
    "Firebase schedule data received:"
  );

  Serial.println(payload);

  processFirebaseSchedules(
    payload
  );
}

// ============================================================
// PARSE FIREBASE SCHEDULE DATA
//
// Conflict rule:
//   Firebase editedAt > local editedAt
//      -> Firebase wins
//
//   local editedAt > Firebase editedAt
//      -> ESP32 wins
//         and uploads to Firebase
//
//   equal:
//      -> keep local value when pending,
//         otherwise keep Firebase/local same value.
// ============================================================

void processFirebaseSchedules(
  const String &payload
) {

  if (payload.length() == 0) {
    needFirebaseSync = false;
    return;
  }

  if (payload == "null") {

    Serial.println(
      "Firebase /schedules does not exist."
    );

    // Create Firebase schedules from local data.
    for (int i = 0; i < 6; i++) {
      pendingChanges[i] = true;
      saveLocalEditMetadata(i);
    }

    needFirebaseSync = false;

    return;
  }

  FirebaseJson json;

  if (!json.setJsonData(payload)) {

    Serial.println(
      "ERROR: Could not parse Firebase JSON."
    );

    return;
  }

  FirebaseJsonData data;

  for (int i = 0; i < 6; i++) {

    char path[32];

    snprintf(
      path,
      sizeof(path),
      "S%d/hour",
      i + 1
    );

    bool hasHour =
      json.get(
        data,
        path
      );

    if (!hasHour || !data.success) {

      // Remote schedule does not exist.
      // Keep/create local value.
      pendingChanges[i] = true;
      saveLocalEditMetadata(i);

      continue;
    }

    int remoteHour =
      data.to<int>();

    snprintf(
      path,
      sizeof(path),
      "S%d/minute",
      i + 1
    );

    bool hasMinute =
      json.get(
        data,
        path
      );

    if (!hasMinute || !data.success) {
      continue;
    }

    int remoteMinute =
      data.to<int>();

    // --------------------------------------------------------
    // editedAt
    // --------------------------------------------------------

    uint64_t remoteEditedAt = 0;

    snprintf(
      path,
      sizeof(path),
      "S%d/editedAt",
      i + 1
    );

    if (
      json.get(
        data,
        path
      ) &&
      data.success
    ) {

      double remoteDouble =
        data.doubleValue;

      if (remoteDouble > 0) {
        remoteEditedAt =
          (uint64_t)remoteDouble;
      }
    }

    uint64_t localEditedAt =
      scheduleEditedAt[i];

    Serial.printf(
      "S%d local=%llu remote=%llu pending=%d\n",
      i + 1,
      (unsigned long long)localEditedAt,
      (unsigned long long)remoteEditedAt,
      pendingChanges[i]
    );

    // --------------------------------------------------------
    // CONFLICT RESOLUTION
    // --------------------------------------------------------

    if (remoteEditedAt > localEditedAt) {

      // Firebase was edited later.
      // Firebase wins.
      scheduleHour[i] = constrain(
        remoteHour,
        0,
        23
      );

      scheduleMinute[i] = constrain(
        remoteMinute,
        0,
        59
      );

      scheduleEditedAt[i] =
        remoteEditedAt;

      pendingChanges[i] = false;

      saveLocalSchedule(i);
      saveLocalEditMetadata(i);

      Serial.printf(
        "S%d: Firebase wins -> %02d:%02d\n",
        i + 1,
        scheduleHour[i],
        scheduleMinute[i]
      );

      updateDisplay = true;
    }

    else if (localEditedAt > remoteEditedAt) {

      // ESP32 was edited later.
      // Keep local value and make sure it uploads.
      pendingChanges[i] = true;

      saveLocalEditMetadata(i);

      Serial.printf(
        "S%d: ESP32 wins -> will upload %02d:%02d\n",
        i + 1,
        scheduleHour[i],
        scheduleMinute[i]
      );
    }

    else {

      // Equal timestamp.
      // If local is pending, keep it and upload.
      // Otherwise they are already synchronized.
      if (pendingChanges[i]) {

        pendingChanges[i] = true;

        saveLocalEditMetadata(i);
      }
    }
  }

  needFirebaseSync = false;

  updateDisplay = true;

  // Start pending uploads after comparison.
  startNextUpload();
}

// ============================================================
// BUILD FIREBASE SCHEDULE JSON
// ============================================================

String buildScheduleJson(
  int index
) {

  FirebaseJson json;

  json.set(
    "hour",
    scheduleHour[index]
  );

  json.set(
    "minute",
    scheduleMinute[index]
  );

  // Firebase JSON number.
  json.set(
    "editedAt",
    (double)scheduleEditedAt[index]
  );

  json.set(
    "editedBy",
    "ESP32"
  );

  json.set(
    "deviceId",
    DEVICE_ID
  );

  String output;

  json.toString(
    output,
    false
  );

  return output;
}

// ============================================================
// START NEXT LOCAL UPLOAD
// ============================================================

void startNextUpload() {

  if (firebaseUploadInFlight) {
    return;
  }

  if (firebaseSyncInFlight) {
    return;
  }

  if (!firebaseStarted) {
    return;
  }

  if (!app.ready()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  for (int i = 0; i < 6; i++) {

    if (!pendingChanges[i]) {
      continue;
    }

    String path =
      "/schedules/S" +
      String(i + 1);

    String jsonPayload =
      buildScheduleJson(i);

    uploadScheduleIndex = i;

    firebaseUploadInFlight = true;

    Serial.printf(
      "Firebase: uploading S%d\n",
      i + 1
    );

    Serial.println(jsonPayload);

    String taskId =
      "uploadS" +
      String(i + 1);

    Database.set<object_t>(
      aClient,
      path,
      object_t(jsonPayload),
      firebaseWriteCallback,
      taskId
    );

    return;
  }
}

// ============================================================
// FIREBASE WRITE CALLBACK
// ============================================================

void firebaseWriteCallback(
  AsyncResult &aResult
) {

  String uid =
    aResult.uid();

  if (!uid.startsWith("uploadS")) {
    return;
  }

  if (!aResult.isResult()) {
    return;
  }

  if (aResult.isError()) {

    Serial.printf(
      "Firebase upload error: %s, code: %d\n",
      aResult.error().message().c_str(),
      aResult.error().code()
    );

    firebaseUploadInFlight = false;
    uploadScheduleIndex = -1;

    return;
  }

  // The successful write normally provides a result payload.
  if (!aResult.available()) {
    return;
  }

  int index =
    uid.substring(7).toInt() - 1;

  if (
    index >= 0 &&
    index < 6
  ) {

    pendingChanges[index] =
      false;

    saveLocalEditMetadata(index);

    Serial.printf(
      "Firebase upload success: S%d\n",
      index + 1
    );
  }

  firebaseUploadInFlight = false;
  uploadScheduleIndex = -1;

  updateDisplay = true;

  // Upload another pending schedule if any.
  startNextUpload();
}

// ============================================================
// FIND NEXT SCHEDULE
// ============================================================

int findNextSchedule() {

  long currentSeconds =
    now.hour() * 3600L +
    now.minute() * 60L +
    now.second();

  long shortestWait = 86401;

  int next = 0;

  for (int i = 0; i < 6; i++) {

    long scheduleSeconds =
      scheduleHour[i] * 3600L +
      scheduleMinute[i] * 60L;

    long wait =
      scheduleSeconds -
      currentSeconds;

    if (wait <= 0) {
      wait += 86400;
    }

    if (wait < shortestWait) {

      shortestWait = wait;
      next = i;
    }
  }

  return next;
}

// ============================================================
// CHECK SCHEDULES
// ============================================================

void checkSchedules() {

  uint32_t thisMinute =
    now.unixtime() / 60;

  if (
    thisMinute ==
    lastCheckedMinute
  ) {
    return;
  }

  lastCheckedMinute =
    thisMinute;

  for (int i = 0; i < 6; i++) {

    if (
      now.hour() ==
      scheduleHour[i] &&

      now.minute() ==
      scheduleMinute[i]
    ) {

      scheduleMatched = true;

      matchStartedAt =
        millis();

      updateDisplay =
        true;

      Serial.printf(
        "Schedule %d: TRUE\n",
        i + 1
      );

      break;
    }
  }
}

// ============================================================
// DRAW DISPLAY
// ============================================================

void drawDisplay() {

  char text[32];

  // ----------------------------------------------------------
  // CLOCK
  // ----------------------------------------------------------

  if (
    screen ==
    CLOCK_SCREEN
  ) {

    snprintf(
      text,
      sizeof(text),
      "Time %02d:%02d:%02d",
      (int)now.hour(),
      (int)now.minute(),
      (int)now.second()
    );

    showLine(
      0,
      text
    );

    snprintf(
      text,
      sizeof(text),
      "Date %02d/%02d/%04d",
      (int)now.day(),
      (int)now.month(),
      (int)now.year()
    );

    showLine(
      1,
      text
    );

    int next =
      findNextSchedule();

    snprintf(
      text,
      sizeof(text),
      "Next S%d %02d:%02d",
      next + 1,
      scheduleHour[next],
      scheduleMinute[next]
    );

    showLine(
      2,
      text
    );

    if (scheduleMatched) {
      showLine(
        3,
        "MATCH: TRUE"
      );
    }
    else if (pendingChanges[0] ||
             pendingChanges[1] ||
             pendingChanges[2] ||
             pendingChanges[3] ||
             pendingChanges[4] ||
             pendingChanges[5]) {

      // Indicate unsynchronized local change.
      showLine(
        3,
        "LOCAL: PENDING"
      );
    }
    else if (
      WiFi.status() != WL_CONNECTED
    ) {

      showLine(
        3,
        "WiFi: OFFLINE"
      );
    }
    else {
      showLine(
        3,
        "Push to edit"
      );
    }

    return;
  }

  // ----------------------------------------------------------
  // SCHEDULE SELECTION
  // ----------------------------------------------------------

  if (
    screen ==
    CHOOSE_SCHEDULE
  ) {

    if (
      selectedSchedule <
      firstVisibleSchedule
    ) {

      firstVisibleSchedule =
        selectedSchedule;
    }

    if (
      selectedSchedule >
      firstVisibleSchedule + 3
    ) {

      firstVisibleSchedule =
        selectedSchedule - 3;
    }

    for (
      int row = 0;
      row < 4;
      row++
    ) {

      int item =
        firstVisibleSchedule +
        row;

      char arrow =
        (
          item ==
          selectedSchedule
        )
        ? '>'
        : ' ';

      if (item < 6) {

        snprintf(
          text,
          sizeof(text),
          "%cSchedule %d",
          arrow,
          item + 1
        );
      }
      else {

        snprintf(
          text,
          sizeof(text),
          "%cBack to clock",
          arrow
        );
      }

      showLine(
        row,
        text
      );
    }

    return;
  }

  // ----------------------------------------------------------
  // HOUR / MINUTE EDIT
  // ----------------------------------------------------------

  snprintf(
    text,
    sizeof(text),
    "Schedule %d",
    selectedSchedule + 1
  );

  showLine(
    0,
    text
  );

  if (
    screen ==
    CHANGE_HOUR
  ) {

    snprintf(
      text,
      sizeof(text),
      "Time [%02d]:%02d",
      newHour,
      newMinute
    );

    showLine(
      1,
      text
    );

    showLine(
      2,
      "Turn: hour"
    );

    showLine(
      3,
      "Push: minute"
    );
  }

  else {

    snprintf(
      text,
      sizeof(text),
      "Time %02d:[%02d]",
      newHour,
      newMinute
    );

    showLine(
      1,
      text
    );

    if (saveError) {

      showLine(
        2,
        "Save failed!"
      );

      showLine(
        3,
        "Push: retry"
      );
    }

    else {

      showLine(
        2,
        "Turn: minute"
      );

      showLine(
        3,
        "Push: save/back"
      );
    }
  }
}

// ============================================================
// HANDLE ENCODER
// ============================================================

void handleEncoder() {

  int turn =
    readRotation();

  if (turn != 0) {

    if (
      screen ==
      CHOOSE_SCHEDULE
    ) {

      selectedSchedule =
        (
          selectedSchedule +
          turn +
          7
        ) % 7;
    }

    else if (
      screen ==
      CHANGE_HOUR
    ) {

      newHour =
        (
          newHour +
          turn +
          24
        ) % 24;
    }

    else if (
      screen ==
      CHANGE_MINUTE
    ) {

      newMinute =
        (
          newMinute +
          turn +
          60
        ) % 60;
    }

    if (
      screen !=
      CLOCK_SCREEN
    ) {

      updateDisplay =
        true;

      lastActivityAt =
        millis();
    }

    Serial.printf(
      "Encoder movement: %d\n",
      turn
    );
  }

  // ----------------------------------------------------------
  // BUTTON
  // ----------------------------------------------------------

  if (!buttonWasPressed()) {
    return;
  }

  lastActivityAt =
    millis();

  switch (screen) {

    case CLOCK_SCREEN:

      selectedSchedule =
        0;

      firstVisibleSchedule =
        0;

      screen =
        CHOOSE_SCHEDULE;

      break;

    case CHOOSE_SCHEDULE:

      if (
        selectedSchedule ==
        6
      ) {

        screen =
          CLOCK_SCREEN;
      }

      else {

        newHour =
          scheduleHour[
            selectedSchedule
          ];

        newMinute =
          scheduleMinute[
            selectedSchedule
          ];

        saveError =
          false;

        screen =
          CHANGE_HOUR;
      }

      break;

    case CHANGE_HOUR:

      screen =
        CHANGE_MINUTE;

      break;

    case CHANGE_MINUTE:

      scheduleHour[
        selectedSchedule
      ] = newHour;

      scheduleMinute[
        selectedSchedule
      ] = newMinute;

      // Mark and store the edit BEFORE trying Firebase.
      saveError =
        false;

      markScheduleEditedLocally(
        selectedSchedule
      );

      if (!saveError) {

        screen =
          CHOOSE_SCHEDULE;

        // Firebase sync happens asynchronously.
        startNextUpload();
      }

      break;
  }

  updateDisplay =
    true;
}

// ============================================================
// WIFI HANDLING
// ============================================================

void handleWiFi() {

  wl_status_t status =
    WiFi.status();

  bool connected =
    status == WL_CONNECTED;

  // ----------------------------------------------------------
  // Just connected
  // ----------------------------------------------------------

  if (
    connected &&
    !wifiWasConnected
  ) {

    Serial.println(
      "Wi-Fi connected."
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    wifiWasConnected =
      true;

    // Make RTC accurate when Internet is available.
    syncRtcFromNtp();

    if (!firebaseStarted) {
      startFirebase();
    }

    needFirebaseSync =
      true;

    lastFirebaseSyncRequest =
      0;

    updateDisplay =
      true;
  }

  // ----------------------------------------------------------
  // Just disconnected
  // ----------------------------------------------------------

  if (
    !connected &&
    wifiWasConnected
  ) {

    Serial.println(
      "Wi-Fi disconnected."
    );

    wifiWasConnected =
      false;

    updateDisplay =
      true;
  }

  // ----------------------------------------------------------
  // Initial connection
  // ----------------------------------------------------------

  if (
    !connected &&
    millis() - lastWiFiAttempt >=
      WIFI_RETRY_INTERVAL
  ) {

    lastWiFiAttempt =
      millis();

    Serial.println(
      "Trying Wi-Fi reconnect..."
    );

    WiFi.reconnect();
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(200);

  Serial.println();
  Serial.println(
    "================================"
  );
  Serial.println(
    "ESP32 Schedule Controller"
  );
  Serial.println(
    "Offline + Firebase Sync"
  );
  Serial.println(
    "================================"
  );

  // ----------------------------------------------------------
  // Encoder
  // ----------------------------------------------------------

  pinMode(
    ENCODER_CLK,
    INPUT_PULLUP
  );

  pinMode(
    ENCODER_DT,
    INPUT_PULLUP
  );

  pinMode(
    ENCODER_BUTTON,
    INPUT_PULLUP
  );

  lastEncoderCLK =
    digitalRead(ENCODER_CLK);

  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  Wire.begin(
    SDA_PIN,
    SCL_PIN
  );

  // ----------------------------------------------------------
  // LCD
  // ----------------------------------------------------------

  if (
    lcd.begin(16, 4) != 0
  ) {

    Serial.println(
      "LCD error!"
    );

    while (true) {
      delay(1000);
    }
  }

  lcd.backlight();

  showLine(
    0,
    "Starting..."
  );

  // ----------------------------------------------------------
  // RTC
  // ----------------------------------------------------------

  if (!rtc.begin()) {

    Serial.println(
      "RTC not found!"
    );

    showLine(
      0,
      "RTC not found!"
    );

    while (true) {
      delay(1000);
    }
  }

  if (rtc.lostPower()) {

    Serial.println(
      "RTC lost power."
    );

    // Temporary fallback.
    // RTC will be synchronized from NTP later if Wi-Fi works.
    rtc.adjust(
      DateTime(
        F(__DATE__),
        F(__TIME__)
      )
    );
  }

  // ----------------------------------------------------------
  // Preferences
  // ----------------------------------------------------------

  if (
    !memory.begin(
      "schedules",
      false
    )
  ) {

    Serial.println(
      "Preferences error!"
    );

    showLine(
      0,
      "Memory error!"
    );

    while (true) {
      delay(1000);
    }
  }

  // Load local schedule data FIRST.
  // This ensures offline operation is available immediately.
  loadSchedules();

  now =
    rtc.now();

  // Check schedules at startup.
  checkSchedules();

  // ----------------------------------------------------------
  // Wi-Fi
  //
  // Do not wait forever.
  // If Wi-Fi is unavailable, ESP32 continues offline.
  // ----------------------------------------------------------

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setAutoReconnect(
    true
  );

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  Serial.print(
    "Connecting to Wi-Fi"
  );

  unsigned long wifiStart =
    millis();

  while (
    WiFi.status() !=
    WL_CONNECTED &&
    millis() - wifiStart < 8000
  ) {

    Serial.print(
      "."
    );

    delay(250);
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    wifiWasConnected =
      true;

    Serial.print(
      "Wi-Fi IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    syncRtcFromNtp();

    startFirebase();
  }

  // ----------------------------------------------------------
  // Display
  // ----------------------------------------------------------

  drawDisplay();

  updateDisplay =
    false;

  lastActivityAt =
    millis();

  lastFirebaseSyncRequest =
    0;

  Serial.println(
    "System ready."
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // Serial Monitor value entry
  // ----------------------------------------------------------

  handleSerialInput();

  // ----------------------------------------------------------
  // Firebase authentication and async tasks
  // ----------------------------------------------------------

  if (firebaseStarted) {
    app.loop();
  }

  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  handleWiFi();

  // ----------------------------------------------------------
  // Encoder
  // ----------------------------------------------------------

  handleEncoder();

  // ----------------------------------------------------------
  // Menu timeout
  // ----------------------------------------------------------

  if (
    screen != CLOCK_SCREEN &&
    millis() - lastActivityAt >=
      MENU_TIMEOUT
  ) {

    screen =
      CLOCK_SCREEN;

    updateDisplay =
      true;

    Serial.println(
      "Menu timeout -> clock"
    );
  }

  // ----------------------------------------------------------
  // RTC
  // ----------------------------------------------------------

  if (
    millis() - lastClockRead >=
    200
  ) {

    lastClockRead =
      millis();

    DateTime latestTime =
      rtc.now();

    if (
      latestTime.unixtime() !=
      now.unixtime()
    ) {

      if (
        screen ==
        CLOCK_SCREEN
      ) {

        updateDisplay =
          true;
      }
    }

    now =
      latestTime;

    checkSchedules();
  }

  // ----------------------------------------------------------
  // Firebase synchronization
  //
  // Always READ first after reconnect.
  // That prevents a stale offline ESP32 change from
  // overwriting a newer Firebase change.
  // ----------------------------------------------------------

  if (
    firebaseStarted &&
    app.ready() &&
    WiFi.status() ==
      WL_CONNECTED
  ) {

    if (
      needFirebaseSync &&
      !firebaseSyncInFlight &&
      !firebaseUploadInFlight
    ) {

      requestFirebaseSchedules();

      lastFirebaseSyncRequest =
        millis();
    }

    else if (
      !firebaseSyncInFlight &&
      !firebaseUploadInFlight &&
      millis() -
        lastFirebaseSyncRequest >=
        FIREBASE_SYNC_INTERVAL
    ) {

      lastFirebaseSyncRequest =
        millis();

      requestFirebaseSchedules();
    }

    // Once there is no read in progress, uploads can proceed.
    if (
      !firebaseSyncInFlight &&
      !needFirebaseSync
    ) {

      startNextUpload();
    }
  }

  // ----------------------------------------------------------
  // MATCH pulse reset after 1 second
  // ----------------------------------------------------------

  if (
    scheduleMatched &&
    millis() - matchStartedAt >=
      TRUE_DURATION
  ) {

    scheduleMatched =
      false;

    Serial.println(
      "FALSE"
    );

    updateDisplay =
      true;
  }

  // ----------------------------------------------------------
  // Device control
  //
  // Use scheduleMatched here.
  // Do not use delay().
  // ----------------------------------------------------------

  // Example:
  // if (scheduleMatched) {
  //   digitalWrite(RELAY_PIN, HIGH);
  // }

  // ----------------------------------------------------------
  // LCD refresh
  // ----------------------------------------------------------

  if (updateDisplay) {

    drawDisplay();

    updateDisplay =
      false;
  }
}