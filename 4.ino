// ============================================================
// ESP32 16x4 LCD SERIAL TEXT TEST
//
// Type any text in the Serial Monitor.
// The ESP32 displays it on the 16x4 LCD.
//
// Serial Monitor:
//   Baud: 115200
//   Line ending: Newline
//
// LCD:
//   SDA -> GPIO 21
//   SCL -> GPIO 22
// ============================================================

#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

hd44780_I2Cexp lcd;

const int SDA_PIN = 21;
const int SCL_PIN = 22;

String inputText = "";

void clearDisplay() {
  for (int row = 0; row < 4; row++) {
    lcd.setCursor(0, row);
    lcd.print("                ");
  }
}

void printWrappedText(const String &text) {

  clearDisplay();

  int row = 0;
  int col = 0;

  for (int i = 0; i < text.length(); i++) {

    char c = text[i];

    // Ignore carriage return.
    if (c == '\r') {
      continue;
    }

    // New line from Serial input.
    if (c == '\n') {
      row++;
      col = 0;

      if (row >= 4) {
        break;
      }

      continue;
    }

    lcd.setCursor(col, row);
    lcd.write(c);

    col++;

    // Move automatically to the next LCD line.
    if (col >= 16) {
      col = 0;
      row++;

      if (row >= 4) {
        break;
      }
    }
  }
}

void setup() {

  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32 16x4 LCD Serial Text Test");
  Serial.println("================================");
  Serial.println("Type text and press Enter.");
  Serial.println("Maximum display area: 64 characters.");
  Serial.println();

  Wire.begin(SDA_PIN, SCL_PIN);

  int status = lcd.begin(16, 4);

  if (status != 0) {
    Serial.print("LCD error: ");
    Serial.println(status);

    while (true) {
      delay(1000);
    }
  }

  lcd.backlight();

  clearDisplay();

  lcd.setCursor(0, 0);
  lcd.print("Serial Text Test");

  lcd.setCursor(0, 1);
  lcd.print("Waiting...");

  Serial.println("LCD ready.");
}

void loop() {

  while (Serial.available()) {

    char c = Serial.read();

    // Enter / newline means display the complete message.
    if (c == '\n' || c == '\r') {

      if (inputText.length() > 0) {

        Serial.print("Displaying: ");
        Serial.println(inputText);

        printWrappedText(inputText);

        inputText = "";
      }

      continue;
    }

    // Keep only the first 64 characters because
    // a 16x4 LCD can show 64 characters at once.
    if (inputText.length() < 64) {
      inputText += c;
    }
  }
}