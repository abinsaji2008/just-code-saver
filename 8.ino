#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

// =====================================================
// Wi-Fi
// =====================================================

#define WIFI_SSID       "Aloor EXT"
#define WIFI_PASSWORD   "PUT_WIFI_PASSWORD_HERE"

// =====================================================
// Firebase: smart-d899d
// =====================================================

#define Web_API_KEY     "AIzaSyDoxgNroD1snyw_QmnBS-W85XIqFMhnBfk"
#define DATABASE_URL    "https://smart-d899d-default-rtdb.firebaseio.com"

#define USER_EMAIL      "PUT_FIREBASE_EMAIL_HERE"
#define USER_PASS       "PUT_FIREBASE_PASSWORD_HERE"

// =====================================================
// Firebase
// =====================================================

void processData(AsyncResult &aResult);

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);

FirebaseApp app;

WiFiClientSecure ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

RealtimeDatabase Database;

// =====================================================
// Serial input
// =====================================================

String inputValue = "";

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ESP32 Firebase Value Entry Test");
  Serial.println(" Project: smart-d899d");
  Serial.println("========================================");

  // Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(300);
  }

  Serial.println();
  Serial.println("Wi-Fi connected!");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // SSL
  ssl_client.setInsecure();
  ssl_client.setHandshakeTimeout(5);

  // Firebase
  initializeApp(
    aClient,
    app,
    getAuth(user_auth),
    processData,
    "authTask"
  );

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  Serial.println();
  Serial.println("Firebase initialization started.");
  Serial.println("Waiting for Firebase authentication...");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  app.loop();

  if (!app.ready())
  {
    return;
  }

  // Read Serial Monitor
  while (Serial.available())
  {
    char c = Serial.read();

    if (c == '\n' || c == '\r')
    {
      if (inputValue.length() > 0)
      {
        inputValue.trim();

        Serial.println();
        Serial.print("Entered value: ");
        Serial.println(inputValue);

        // Write value to Firebase
        Database.set<String>(
          aClient,
          "/test/value",
          inputValue,
          processData,
          "RTDB_SetValue"
        );

        Serial.println("Write request sent.");

        inputValue = "";
      }
    }
    else
    {
      inputValue += c;
    }
  }

  delay(5);
}

// =====================================================
// FIREBASE RESULT CALLBACK
// =====================================================

void processData(AsyncResult &aResult)
{
  if (!aResult.isResult())
    return;

  if (aResult.isEvent())
  {
    Firebase.printf(
      "Event: %s | %s | code: %d\n",
      aResult.uid().c_str(),
      aResult.eventLog().message().c_str(),
      aResult.eventLog().code()
    );
  }

  if (aResult.isDebug())
  {
    Firebase.printf(
      "Debug: %s | %s\n",
      aResult.uid().c_str(),
      aResult.debug().c_str()
    );
  }

  if (aResult.isError())
  {
    Firebase.printf(
      "Error: %s | %s | code: %d\n",
      aResult.uid().c_str(),
      aResult.error().message().c_str(),
      aResult.error().code()
    );
  }

  if (aResult.available())
  {
    Firebase.printf(
      "Task: %s | Payload: %s\n",
      aResult.uid().c_str(),
      aResult.c_str()
    );

    if (aResult.uid() == "RTDB_SetValue")
    {
      Serial.println();
      Serial.println("========================================");
      Serial.println(" FIREBASE WRITE SUCCESS");
      Serial.println("========================================");
      Serial.println("Path: /test/value");
      Serial.print("Value: ");
      Serial.println(aResult.c_str());
      Serial.println();
      Serial.println("Enter another value:");
    }
  }
}
