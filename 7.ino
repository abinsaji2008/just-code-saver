#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

// =====================================================
// Network and Firebase credentials
// =====================================================

// Wi-Fi
#define WIFI_SSID "Aloor EXT"
#define WIFI_PASSWORD "PUT_WIFI_PASSWORD_HERE"

// Smart Firebase project: smart-d899d
#define Web_API_KEY "AIzaSyDoxgNroD1snyw_QmnBS-W85XIqFMhnBfk"
#define DATABASE_URL "https://smart-d899d-default-rtdb.firebaseio.com"

// Firebase Authentication user
#define USER_EMAIL "PUT_FIREBASE_EMAIL_HERE"
#define USER_PASS "PUT_FIREBASE_PASSWORD_HERE"

// User function
void processData(AsyncResult &aResult);

// Authentication
UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);

// Firebase components
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

// Timer variables for reading data every 10 seconds
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 10000;

// Variables to save values from the database
int intValue = 0;
float floatValue = 0.0;
String stringValue = "";

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" ESP32 Firebase Test");
  Serial.println(" Firebase project: smart-d899d");
  Serial.println("======================================");

  // Connect to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(300);
  }

  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  // Configure SSL client
  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  // Initialize Firebase
  initializeApp(
    aClient,
    app,
    getAuth(user_auth),
    processData,
    "authTask"
  );

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  Serial.println("Firebase initialization started.");
}

void loop()
{
  // Maintain authentication and async tasks
  app.loop();

  // Check if authentication is ready
  if (app.ready())
  {
    unsigned long currentTime = millis();

    if (currentTime - lastSendTime >= sendInterval)
    {
      lastSendTime = currentTime;

      // Read test values from Firebase
      Database.get(
        aClient,
        "/test/int",
        processData,
        false,
        "RTDB_GetInt"
      );

      Database.get(
        aClient,
        "/test/float",
        processData,
        false,
        "RTDB_GetFloat"
      );

      Database.get(
        aClient,
        "/test/string",
        processData,
        false,
        "RTDB_GetString"
      );

      Serial.println(
        "Requested /test/int, /test/float and /test/string"
      );
    }
  }
}

void processData(AsyncResult &aResult)
{
  if (!aResult.isResult())
    return;

  if (aResult.isEvent())
  {
    Firebase.printf(
      "Event task: %s, msg: %s, code: %d\n",
      aResult.uid().c_str(),
      aResult.eventLog().message().c_str(),
      aResult.eventLog().code()
    );
  }

  if (aResult.isDebug())
  {
    Firebase.printf(
      "Debug task: %s, msg: %s\n",
      aResult.uid().c_str(),
      aResult.debug().c_str()
    );
  }

  if (aResult.isError())
  {
    Firebase.printf(
      "Error task: %s, msg: %s, code: %d\n",
      aResult.uid().c_str(),
      aResult.error().message().c_str(),
      aResult.error().code()
    );
  }

  if (aResult.available())
  {
    Firebase.printf(
      "Task: %s, payload: %s\n",
      aResult.uid().c_str(),
      aResult.c_str()
    );

    String payload = aResult.c_str();

    // Handle int
    if (aResult.uid() == "RTDB_GetInt")
    {
      intValue = payload.toInt();

      Firebase.printf(
        "Stored intValue: %d\n",
        intValue
      );
    }

    // Handle float
    else if (aResult.uid() == "RTDB_GetFloat")
    {
      floatValue = payload.toFloat();

      Firebase.printf(
        "Stored floatValue: %.2f\n",
        floatValue
      );
    }

    // Handle string
    else if (aResult.uid() == "RTDB_GetString")
    {
      stringValue = payload;

      Firebase.printf(
        "Stored stringValue: %s\n",
        stringValue.c_str()
      );
    }
  }
}
