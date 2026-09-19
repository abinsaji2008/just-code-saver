// ============================================================
// ESP32 + GROQ AI + 16x4 LCD
//
// Type a message in the Serial Monitor.
// ESP32 sends it to Groq AI.
// Groq's response is displayed on the 16x4 LCD.
//
// Serial Monitor:
//   Baud: 115200
//   Line ending: Newline
//
// Required libraries:
//   - hd44780 by Bill Perry
//   - ArduinoJson by Benoit Blanchon
//
// Wi-Fi and Groq settings are below.
// ============================================================

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

#include <ArduinoJson.h>

// ------------------------------------------------------------
// USER CONFIGURATION
// ------------------------------------------------------------

#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

#define GROQ_API_KEY    "YOUR_GROQ_API_KEY"

// Groq current Chat Completions endpoint.
const char* GROQ_URL =
  "https://api.groq.com/openai/v1/chat/completions";

// Groq model.
// openai/gpt-oss-20b is a current supported replacement for
// the deprecated llama-3.1-8b-instant model.
const char* GROQ_MODEL =
  "openai/gpt-oss-20b";

// ------------------------------------------------------------
// LCD
// ------------------------------------------------------------

const int SDA_PIN = 21;
const int SCL_PIN = 22;

hd44780_I2Cexp lcd;

// ------------------------------------------------------------
// DISPLAY
// ------------------------------------------------------------

const int LCD_COLUMNS = 16;
const int LCD_ROWS = 4;
const int LCD_CHARS = LCD_COLUMNS * LCD_ROWS;

String aiResponse = "";

unsigned long lastPageChange = 0;
const unsigned long PAGE_TIME = 3000;

int currentPage = 0;
int totalPages = 0;

bool showingPages = false;

// ------------------------------------------------------------
// SERIAL INPUT
// ------------------------------------------------------------

String inputText = "";

// ------------------------------------------------------------
// WIFI
// ------------------------------------------------------------

bool wifiConnected = false;

// ------------------------------------------------------------
// CLEAR LCD
// ------------------------------------------------------------

void clearDisplay() {

  for (int row = 0; row < LCD_ROWS; row++) {
    lcd.setCursor(0, row);
    lcd.print("                ");
  }
}

// ------------------------------------------------------------
// SHOW 64 CHARACTERS ON LCD
// ------------------------------------------------------------

void showPage(const String &text, int pageNumber) {

  clearDisplay();

  int start = pageNumber * LCD_CHARS;

  if (start >= text.length()) {
    return;
  }

  int index = start;

  for (int row = 0; row < LCD_ROWS; row++) {

    lcd.setCursor(0, row);

    for (int col = 0; col < LCD_COLUMNS; col++) {

      if (index < text.length()) {

        char c = text[index++];

        if (c == '\n' || c == '\r') {
          lcd.print(' ');
        } else {
          lcd.print(c);
        }

      } else {
        lcd.print(' ');
      }
    }
  }
}

// ------------------------------------------------------------
// PREPARE RESPONSE INTO LCD-SIZED PAGES
//
// This uses simple 64-character pages. Newlines are kept.
// ------------------------------------------------------------

int calculateTotalPages(const String &text) {

  if (text.length() == 0) {
    return 1;
  }

  return (text.length() + LCD_CHARS - 1) / LCD_CHARS;
}

// ------------------------------------------------------------
// DISPLAY RESPONSE
// ------------------------------------------------------------

void startDisplayingResponse(const String &response) {

  aiResponse = response;

  totalPages = calculateTotalPages(aiResponse);

  currentPage = 0;

  showingPages = true;

  lastPageChange = millis();

  showPage(aiResponse, currentPage);

  Serial.println();
  Serial.println("========== GROQ RESPONSE ==========");
  Serial.println(aiResponse);
  Serial.println("===================================");
  Serial.printf(
    "LCD pages: %d\n",
    totalPages
  );
}

// ------------------------------------------------------------
// WIFI CONNECTION
// ------------------------------------------------------------

void connectWiFi() {

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }

  Serial.println();
  Serial.println("Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 15000
  ) {

    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;

    Serial.println("Wi-Fi connected.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());

    delay(1500);

  } else {

    wifiConnected = false;

    Serial.println("Wi-Fi connection failed.");

    clearDisplay();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed");
    lcd.setCursor(0, 1);
    lcd.print("Check settings");
  }
}

// ------------------------------------------------------------
// ESCAPE JSON STRING
// ------------------------------------------------------------

String escapeJsonString(const String &input) {

  String output;
  output.reserve(input.length() + 20);

  for (int i = 0; i < input.length(); i++) {

    char c = input[i];

    switch (c) {

      case '\"':
        output += "\\\"";
        break;

      case '\\':
        output += "\\\\";
        break;

      case '\n':
        output += "\\n";
        break;

      case '\r':
        output += "\\r";
        break;

      case '\t':
        output += "\\t";
        break;

      default:
        output += c;
        break;
    }
  }

  return output;
}

// ------------------------------------------------------------
// ASK GROQ
// ------------------------------------------------------------

bool askGroq(const String &question, String &answer) {

  connectWiFi();

  if (WiFi.status() != WL_CONNECTED) {

    answer = "WiFi not connected.";

    return false;
  }

  if (
    strlen(GROQ_API_KEY) == 0 ||
    String(GROQ_API_KEY) == "YOUR_GROQ_API_KEY"
  ) {

    answer = "Add Groq API key.";

    Serial.println(
      "ERROR: GROQ_API_KEY is not configured."
    );

    return false;
  }

  WiFiClientSecure client;

  // For testing only.
  // This skips certificate verification.
  // Use a CA certificate for production.
  client.setInsecure();

  HTTPClient http;

  Serial.println();
  Serial.println("Sending to Groq...");

  if (!http.begin(client, GROQ_URL)) {

    answer = "HTTP begin failed.";

    Serial.println(
      "ERROR: HTTP begin failed."
    );

    return false;
  }

  http.setTimeout(30000);

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  http.addHeader(
    "Authorization",
    String("Bearer ") + GROQ_API_KEY
  );

  // Keep the response reasonably small for an LCD test.
  String body =
    "{"
    "\"model\":\"" + String(GROQ_MODEL) + "\","
    "\"messages\":["
      "{"
        "\"role\":\"system\","
        "\"content\":\"You are a helpful AI assistant. "
                   "Answer clearly and briefly. "
                   "Keep answers concise for a 16x4 LCD.\""
      "},"
      "{"
        "\"role\":\"user\","
        "\"content\":\"" + escapeJsonString(question) + "\""
      "}"
    "],"
    "\"max_completion_tokens\":180,"
    "\"temperature\":0.4"
    "}";

  int httpCode = http.POST(body);

  Serial.print("HTTP status: ");
  Serial.println(httpCode);

  if (httpCode <= 0) {

    answer =
      "HTTP error " +
      String(httpCode);

    Serial.println(
      "ERROR: " + http.errorToString(httpCode)
    );

    http.end();

    return false;
  }

  String payload = http.getString();

  http.end();

  Serial.println();
  Serial.println("Groq raw response:");
  Serial.println(payload);

  // ----------------------------------------------------------
  // Parse JSON response.
  // ----------------------------------------------------------

  JsonDocument doc;

  DeserializationError error =
    deserializeJson(doc, payload);

  if (error) {

    answer = "JSON parse error.";

    Serial.print(
      "JSON parse error: "
    );

    Serial.println(error.c_str());

    return false;
  }

  // Groq Chat Completions:
  // choices[0].message.content
  const char* content =
    doc["choices"][0]["message"]["content"];

  if (content == nullptr) {

    // Try to show Groq's error message when available.
    const char* apiError =
      doc["error"]["message"];

    if (apiError != nullptr) {
      answer = String("API: ") + apiError;
    } else {
      answer = "No AI response.";
    }

    return false;
  }

  answer = String(content);

  return true;
}

// ------------------------------------------------------------
// SHOW STATUS MESSAGE
// ------------------------------------------------------------

void showStatus(
  const String &line1,
  const String &line2 = "",
  const String &line3 = "",
  const String &line4 = ""
) {

  clearDisplay();

  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));

  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));

  lcd.setCursor(0, 2);
  lcd.print(line3.substring(0, 16));

  lcd.setCursor(0, 3);
  lcd.print(line4.substring(0, 16));
}

// ------------------------------------------------------------
// SETUP
// ------------------------------------------------------------

void setup() {

  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" ESP32 + GROQ AI + 16x4 LCD");
  Serial.println("====================================");
  Serial.println(
    "Type a question in Serial Monitor."
  );
  Serial.println(
    "Press Enter to send it to Groq."
  );
  Serial.println();

  // LCD
  Wire.begin(SDA_PIN, SCL_PIN);

  int status =
    lcd.begin(
      LCD_COLUMNS,
      LCD_ROWS
    );

  if (status != 0) {

    Serial.print(
      "LCD error: "
    );

    Serial.println(status);

    while (true) {
      delay(1000);
    }
  }

  lcd.backlight();

  showStatus(
    "Groq AI",
    "Starting..."
  );

  // Wi-Fi
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {

    showStatus(
      "Groq AI Ready",
      "Type in Serial",
      "and press Enter"
    );

  } else {

    showStatus(
      "WiFi required",
      "Check settings"
    );
  }
}

// ------------------------------------------------------------
// LOOP
// ------------------------------------------------------------

void loop() {

  // ----------------------------------------------------------
  // Serial input
  // ----------------------------------------------------------

  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {

      if (inputText.length() == 0) {
        continue;
      }

      String question =
        inputText;

      inputText = "";

      showingPages = false;

      Serial.println();
      Serial.print("YOU: ");
      Serial.println(question);

      showStatus(
        "Thinking...",
        "Groq AI"
      );

      String answer;

      bool success =
        askGroq(
          question,
          answer
        );

      if (success) {

        startDisplayingResponse(
          answer
        );

      } else {

        startDisplayingResponse(
          answer
        );
      }

    } else {

      // Maximum question size for this simple test.
      if (inputText.length() < 500) {
        inputText += c;
      }
    }
  }

  // ----------------------------------------------------------
  // Automatic response paging
  // ----------------------------------------------------------

  if (
    showingPages &&
    totalPages > 1 &&
    millis() - lastPageChange >= PAGE_TIME
  ) {

    lastPageChange = millis();

    currentPage++;

    if (currentPage >= totalPages) {
      currentPage = 0;
    }

    showPage(
      aiResponse,
      currentPage
    );
  }

  // ----------------------------------------------------------
  // Wi-Fi reconnect
  // ----------------------------------------------------------

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    wifiConnected = false;
  }
}