// ============================================================
// ESP32 Schedule Controller + Firebase Realtime Database
// Version 2.ino - offline test, improved quadrature encoder decoder
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

#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <RTClib.h>
#include <Preferences.h>

#include <WiFi.h>
#include <time.h>

// ------------------------------------------------------------
// USER CONFIGURATION
// ------------------------------------------------------------

// Wi-Fi
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Offline-only version: Firebase is disabled.
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
 // Wi-Fi
 // ------------------------------------------------------------

 bool firebaseStarted = false; // Kept only for compatibility; always false.

 bool wifiWasConnected = false;

 unsigned long lastWiFiAttempt = 0;

 const unsigned long WIFI_RETRY_INTERVAL = 15000;

// ------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------


void saveLocalSchedule(int index);
void saveLocalEditMetadata(int index);

void markScheduleEditedLocally(int index);

uint64_t getCurrentUtcMs();

void syncRtcFromNtp();


void handleWiFi();


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
// Proper 4-state quadrature decoder.
//
// Unlike the old falling-edge decoder, this checks the complete
// CLK/DT transition sequence. This greatly reduces false
// movements caused by mechanical contact bounce or noise.
//
// One complete quadrature cycle = one logical encoder step.
// ============================================================

const int8_t ENCODER_TRANSITION_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

uint8_t encoderState = 0;
int8_t encoderAccumulator = 0;

// Sensitivity:
// 2 = faster response / less physical rotation needed.
// 4 = standard full quadrature detent.
// Start with 2 for easier scrolling and value changes.
const int8_t ENCODER_TRANSITIONS_PER_STEP = 2;

uint8_t readEncoderState() {
  uint8_t clk = digitalRead(ENCODER_CLK);
  uint8_t dt  = digitalRead(ENCODER_DT);

  return (clk << 1) | dt;
}

void initializeEncoder() {
  encoderState = readEncoderState();
  encoderAccumulator = 0;
}

int readRotation() {
  uint8_t currentState = readEncoderState();

  if (currentState == encoderState) {
    return 0;
  }

  uint8_t transition =
    (encoderState << 2) | currentState;

  int8_t movement =
    ENCODER_TRANSITION_TABLE[transition];

  encoderState = currentState;

  if (movement == 0) {
    return 0;
  }

  encoderAccumulator += movement;

  // Use fewer valid transitions per step so the encoder
  // responds to a smaller physical rotation.
  if (encoderAccumulator >= ENCODER_TRANSITIONS_PER_STEP) {
    encoderAccumulator = 0;
    return 1;
  }

  if (encoderAccumulator <= -ENCODER_TRANSITIONS_PER_STEP) {
    encoderAccumulator = 0;
    return -1;
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
// FIREBASE
//
// Disabled in this 2.ino test version.
// Local Preferences storage and schedule editing work offline.
// ============================================================

void startNextUpload() {
  // Firebase disabled.
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

        // Firebase is disabled in this test version.
        // The schedule is already saved to ESP32 Preferences.
      }

      break;
  }

  updateDisplay =
    true;
}

// ============================================================
// WIFI HANDLING
//
// Wi-Fi is retained for optional NTP time synchronization.
// Firebase is completely disabled in 2.ino.
// ============================================================

void handleWiFi() {

  wl_status_t status = WiFi.status();

  bool connected = status == WL_CONNECTED;

  if (connected && !wifiWasConnected) {

    Serial.println("Wi-Fi connected.");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    wifiWasConnected = true;

    // Update DS3231 time when Internet is available.
    syncRtcFromNtp();

    updateDisplay = true;
  }

  if (!connected && wifiWasConnected) {

    Serial.println("Wi-Fi disconnected.");

    wifiWasConnected = false;

    updateDisplay = true;
  }

  if (
    !connected &&
    millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL
  ) {

    lastWiFiAttempt = millis();

    Serial.println("Trying Wi-Fi reconnect...");

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

  initializeEncoder();

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

  }

  // ----------------------------------------------------------
  // Display
  // ----------------------------------------------------------

  drawDisplay();

  updateDisplay =
    false;

  lastActivityAt =
    millis();

  Serial.println(
    "System ready."
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

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