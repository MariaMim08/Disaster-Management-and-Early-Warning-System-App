#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ---------------- WIFI ----------------
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// ---------------- EMAIL ----------------
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "ismotaraprova@gmail.com"
#define AUTHOR_PASSWORD ""
#define RECIPIENT_EMAIL "provaismot@gmail.com"

// ---------------- FIREBASE ----------------
#define DATABASE_URL "https://earlywarningsystem-53511-default-rtdb.firebaseio.com/landslideData.json"

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- SENSOR PINS ----------------
#define PRESSURE_PIN 35
#define MOISTURE_PIN 34
#define BUZZER_PIN 19

// ---------------- UPDATED LANDSLIDE THRESHOLDS ----------------

#define PRESSURE_MEDIUM     400
#define PRESSURE_HIGH       410
#define PRESSURE_CRITICAL   430

#define MOISTURE_MEDIUM     1700
#define MOISTURE_HIGH       1600
#define MOISTURE_CRITICAL   1500

#define CONFIRM_COUNT       4

// ---------------- VARIABLES ----------------
bool landslideDetected = false;
bool lastSlideState = false;
bool emailSent = false;
bool safeEmailSent = false;

int confirmCounter = 0;

// ---------------- TIME ----------------
String getTimeNow() {
  configTime(6 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  if (!getLocalTime(&t)) return "Unknown";

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// ---------------- EMAIL ----------------
void sendEmail(const char* subject, const char* body) {
  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(SMTP_HOST, SMTP_PORT)) return;

  auto waitResp = [&]() {
    unsigned long t = millis() + 3000;
    while (millis() < t) {
      while (client.available()) client.readStringUntil('\n');
    }
  };

  client.println("EHLO esp32"); waitResp();
  client.println("AUTH LOGIN"); waitResp();
  client.println("aXNtb3RhcmFwcm92YUBnbWFpbC5jb20="); waitResp();
  client.println("a2NrdyBmZXd0IHF3bXAgbXR0YQ=="); waitResp();

  client.printf("MAIL FROM:<%s>\r\n", AUTHOR_EMAIL); waitResp();
  client.printf("RCPT TO:<%s>\r\n", RECIPIENT_EMAIL); waitResp();
  client.println("DATA"); waitResp();

  client.printf("Subject: %s\r\n", subject);
  client.println("Content-Type: text/plain");
  client.println();
  client.println(body);
  client.println(".");
  waitResp();

  client.println("QUIT");
  client.stop();
}

// ---------------- FIREBASE ----------------
void uploadToFirebase(int pressure, int moisture, bool slide, String risk) {

  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"timestamp\":\"" + getTimeNow() + "\",";
  json += "\"pressure\":" + String(pressure) + ",";
  json += "\"soil_moisture\":" + String(moisture) + ",";
  json += "\"landslide_detected\":" + String(slide ? 1 : 0) + ",";
  json += "\"risk_level\":\"" + risk + "\"";
  json += "}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, DATABASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.POST(json);
  http.end();
}

// ---------------- EMAIL MESSAGES ----------------
void sendSlideAlert(int pressure, int moisture) {
  char msg[250];
  snprintf(msg, sizeof(msg),
           "⛰️ LANDSLIDE ALERT!\n"
           "Pressure: %d\n"
           "Soil Moisture: %d\n"
           "Time: %s\n\n"
           "Evacuate Immediately!",
           pressure, moisture, getTimeNow().c_str());

  sendEmail("⛰️ LANDSLIDE ALERT", msg);
}

void sendSafeAlert(int pressure) {
  char msg[200];
  snprintf(msg, sizeof(msg),
           "✅ LANDSLIDE RISK REDUCED\n"
           "Pressure: %d\n"
           "Time: %s",
           pressure, getTimeNow().c_str());

  sendEmail("✅ AREA SAFE", msg);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  pinMode(BUZZER_PIN, OUTPUT);

  lcd.print("Landslide System");
  delay(1500);
  lcd.clear();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
  }
}

// ---------------- LOOP ----------------
void loop() {

  int pressureValue = analogRead(PRESSURE_PIN);
  int moistureValue = analogRead(MOISTURE_PIN);

  String riskLevel = "Low";

  // =====================================================
  // ⛰️ CALIBRATED LANDSLIDE LOGIC
  // =====================================================

 // ---------------- IMPROVED STABLE LOGIC ----------------

if (pressureValue >= PRESSURE_CRITICAL && moistureValue <= MOISTURE_CRITICAL) {

    confirmCounter++;

    if (confirmCounter >= CONFIRM_COUNT) {
        riskLevel = "Critical";
        landslideDetected = true;
    } else {
        riskLevel = "High";
    }
}

else if (pressureValue >= PRESSURE_HIGH && moistureValue <= MOISTURE_HIGH) {

    riskLevel = "High";
    landslideDetected = false;

    // DO NOT reset counter immediately
    if (confirmCounter > 0) confirmCounter--;
}

else if (pressureValue >= PRESSURE_MEDIUM && moistureValue <= MOISTURE_MEDIUM) {

    riskLevel = "Medium";
    confirmCounter = 0;
    landslideDetected = false;
}

else {

    riskLevel = "Low";
    confirmCounter = 0;
    landslideDetected = false;
}


  // ---------------- FIREBASE ----------------
  uploadToFirebase(
    pressureValue,
    moistureValue,
    landslideDetected,
    riskLevel
  );

  // ---------------- EMAIL ----------------
  if (riskLevel == "Critical") {
    if (!emailSent) {
      sendSlideAlert(pressureValue, moistureValue);
      emailSent = true;
      safeEmailSent = false;
    }
  }

  // ---------------- LCD ----------------
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Risk: ");
  lcd.print(riskLevel);

  lcd.setCursor(0, 1);
  lcd.print("Pr:");
  lcd.print(pressureValue);
  lcd.print(" Mo:");
  lcd.print(moistureValue);

  // ---------------- BUZZER ----------------
  if (riskLevel == "Critical") {
    tone(BUZZER_PIN, 3000);
  }
  else if (riskLevel == "High") {
    tone(BUZZER_PIN, 2000);
    delay(500);
    noTone(BUZZER_PIN);
    delay(500);
  }
  else if (riskLevel == "Medium") {
    tone(BUZZER_PIN, 1500);
    delay(200);
    noTone(BUZZER_PIN);
  }
  else {
    noTone(BUZZER_PIN);
  }

  // SAFE recovery
  if (!landslideDetected && lastSlideState) {
    if (!safeEmailSent) {
      sendSafeAlert(pressureValue);
      safeEmailSent = true;
      emailSent = false;
    }
  }

  lastSlideState = landslideDetected;

  Serial.printf(
    "Pressure:%d Moisture:%d Risk:%s\n",
    pressureValue, moistureValue, riskLevel.c_str()
  );

  delay(3000);
}
