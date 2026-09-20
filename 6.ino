#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// =====================================================
// WIFI
// =====================================================

#define WIFI_SSID       "Aloor EXT"
#define WIFI_PASSWORD   "PUT_WIFI_PASSWORD_HERE"

// =====================================================
// FIREBASE - smart-d899d
// =====================================================

#define FIREBASE_API_KEY "AIzaSyDoxgNroD1snyw_QmnBS-W85XIqFMhnBfk"

#define FIREBASE_EMAIL    "PUT_FIREBASE_EMAIL_HERE"
#define FIREBASE_PASSWORD "PUT_FIREBASE_PASSWORD_HERE"

#define DATABASE_URL "https://smart-d899d-default-rtdb.firebaseio.com"

// =====================================================
// FIREBASE LOGIN
// =====================================================

String firebaseIdToken = "";

bool firebaseLogin()
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url =
      "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key="
      FIREBASE_API_KEY;

  Serial.println();
  Serial.println("Starting Firebase email/password login...");

  if (!https.begin(client, url))
  {
    Serial.println("HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  String body =
      "{"
      "\"email\":\"" FIREBASE_EMAIL "\","
      "\"password\":\"" FIREBASE_PASSWORD "\","
      "\"returnSecureToken\":true"
      "}";

  int httpCode = https.POST(body);

  Serial.print("Firebase Auth HTTP code: ");
  Serial.println(httpCode);

  String response = https.getString();

  Serial.println("Firebase Auth response:");
  Serial.println(response);

  if (httpCode != 200)
  {
    https.end();
    return false;
  }

  DynamicJsonDocument doc(4096);

  DeserializationError error = deserializeJson(doc, response);

  if (error)
  {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());
    https.end();
    return false;
  }

  if (!doc["idToken"])
  {
    Serial.println("No Firebase ID token returned.");
    https.end();
    return false;
  }

  firebaseIdToken = doc["idToken"].as<String>();

  Serial.println();
  Serial.println("================================");
  Serial.println(" FIREBASE LOGIN SUCCESS");
  Serial.println("================================");

  https.end();
  return true;
}

// =====================================================
// FIREBASE WRITE
// =====================================================

bool firebaseWriteTest()
{
  if (firebaseIdToken.length() == 0)
  {
    Serial.println("No Firebase token.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url =
      String(DATABASE_URL) +
      "/esp32_test.json?auth=" +
      firebaseIdToken;

  Serial.println();
  Serial.println("Writing test data...");

  if (!https.begin(client, url))
  {
    Serial.println("HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  String json =
      "{"
      "\"message\":\"Hello from ESP32\","
      "\"status\":\"online\""
      "}";

  int httpCode = https.PUT(json);

  Serial.print("Firebase write HTTP code: ");
  Serial.println(httpCode);

  String response = https.getString();

  Serial.println("Write response:");
  Serial.println(response);

  https.end();

  return httpCode >= 200 && httpCode < 300;
}

// =====================================================
// FIREBASE READ
// =====================================================

bool firebaseReadTest()
{
  if (firebaseIdToken.length() == 0)
  {
    Serial.println("No Firebase token.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url =
      String(DATABASE_URL) +
      "/esp32_test.json?auth=" +
      firebaseIdToken;

  Serial.println();
  Serial.println("Reading test data...");

  if (!https.begin(client, url))
  {
    Serial.println("HTTPS begin failed");
    return false;
  }

  int httpCode = https.GET();

  Serial.print("Firebase read HTTP code: ");
  Serial.println(httpCode);

  String response = https.getString();

  Serial.println("Read response:");
  Serial.println(response);

  https.end();

  return httpCode >= 200 && httpCode < 300;
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 FIREBASE TEST");
  Serial.println(" Project: smart-d899d");
  Serial.println("================================");

  Serial.print("Connecting to WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 40)
  {
    Serial.print(".");
    delay(500);
    attempts++;
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi connection FAILED.");
    return;
  }

  Serial.println("WiFi connected!");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  if (!firebaseLogin())
  {
    Serial.println();
    Serial.println("Firebase login FAILED.");
    return;
  }

  if (firebaseWriteTest())
  {
    Serial.println("Firebase WRITE SUCCESS");
  }
  else
  {
    Serial.println("Firebase WRITE FAILED");
  }

  delay(1000);

  if (firebaseReadTest())
  {
    Serial.println("Firebase READ SUCCESS");
  }
  else
  {
    Serial.println("Firebase READ FAILED");
  }

  Serial.println();
  Serial.println("================================");
  Serial.println(" TEST FINISHED");
  Serial.println("================================");
}

void loop()
{
  delay(1000);
}
