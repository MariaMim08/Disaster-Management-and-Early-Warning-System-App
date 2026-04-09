#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "DHT.h"

// ---------------- WIFI ----------------
// ---- Wi-Fi ----
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// ---------------- EMAIL ----------------
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "ismotaraprova@gmail.com"
#define AUTHOR_PASSWORD ""
#define RECIPIENT_EMAIL "provaismot@gmail.com"

// ---------------- FIREBASE ----------------
#define DATABASE_URL "https://earlywarningsystem-53511-default-rtdb.firebaseio.com/wildfireData.json"


// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- SENSOR PINS ----------------
#define DHT_PIN 4
#define MQ_PIN 34
#define FLAME_PIN 26
#define BUZZER_PIN 25

#define DHTTYPE DHT22
DHT dht(DHT_PIN, DHTTYPE);

// ---------------- THRESHOLDS ----------------

// Temperature
#define TEMP_WARM 35
#define TEMP_HIGH 45
#define TEMP_EXTREME 50

// Humidity
#define HUMIDITY_MODERATE 45
#define HUMIDITY_DRY 40
#define HUMIDITY_VERY_DRY 30

#define MQ_MEDIUM 600
#define MQ_HIGH   1200


// ---------------- VARIABLES ----------------
bool wildfireDetected = false;
bool lastFireState = false;
bool emailSent = false;
bool safeEmailSent = false;

unsigned long lastBlink = 0;
bool showAlert = false;

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

// ---------------- FIREBASE UPLOAD ----------------
void uploadToFirebase(float t, float h, int gas, bool flame, bool fire, String risk) {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"timestamp\":\"" + getTimeNow() + "\",";
  json += "\"temperature\":" + String(t, 1) + ",";
  json += "\"humidity\":" + String(h, 1) + ",";
  json += "\"gas_value\":" + String(gas) + ",";
  json += "\"flame_detected\":" + String(flame ? 1 : 0) + ",";
  json += "\"wildfire_detected\":" + String(fire ? 1 : 0) + ",";
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
void sendFireAlert(float t, float h, int gas) {
  char msg[250];
  snprintf(msg, sizeof(msg),
           "🔥 WILDFIRE ALERT!\n"
           "Temperature: %.1f C\n"
           "Humidity: %.1f %%\n"
           "Gas Level: %d\n"
           "Time: %s\n\n"
           "Immediate action required!",
           t, h, gas, getTimeNow().c_str());

  sendEmail("🔥 WILDFIRE ALERT", msg);
}

void sendSafeAlert(float t, float h, int gas) {
  char msg[200];
  snprintf(msg, sizeof(msg),
           "✅ AREA SAFE AGAIN\n"
           "Temperature: %.1f C\n"
           "Humidity: %.1f %%\n"
           "Gas Level: %d\n"
           "Time: %s",
           t, h, gas, getTimeNow().c_str());

  sendEmail("✅ AREA SAFE", msg);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();

  pinMode(FLAME_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  dht.begin();
  analogSetPinAttenuation(MQ_PIN, ADC_11db); // ⭐ IMPORTANT

  lcd.print("Wildfire System");
  delay(1500);
  lcd.clear();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
  }
}

// ---------------- LOOP ----------------
void loop() {

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int mqValue = analogRead(MQ_PIN);
  bool flameDetected = (digitalRead(FLAME_PIN) == LOW); // active LOW

  String riskLevel = "Low";
  wildfireDetected = false;

  // =====================================================
  // 🔥 WILDFIRE DECISION LOGIC (FINAL & CLEAN)
  // =====================================================

  // ---- CRITICAL ----
  
  if (
    (flameDetected && mqValue >= MQ_HIGH) ||
    (temperature >= TEMP_HIGH && humidity <= HUMIDITY_DRY && mqValue >= MQ_MEDIUM)
) {
    riskLevel = "Critical";
    wildfireDetected = true;
}

// ---- HIGH ----
else if (
    (flameDetected && mqValue >= MQ_MEDIUM) ||
    (mqValue >= MQ_HIGH)
) {
    riskLevel = "High";
}

// ---- MEDIUM ----
else if (
    flameDetected ||
    mqValue >= MQ_MEDIUM ||
    (temperature >= TEMP_WARM && humidity <= HUMIDITY_MODERATE)
) {
    riskLevel = "Medium";
}

// ---- LOW ----
else {
    riskLevel = "Low";
}


  // ---------------- UPLOAD ----------------
  uploadToFirebase(
    temperature,
    humidity,
    mqValue,
    flameDetected,
    wildfireDetected,
    riskLevel
  );

  // ---------------- ALERT + BUZZER ----------------
  if (riskLevel == "Critical") {
    if (!emailSent) {
      sendFireAlert(temperature, humidity, mqValue);
      emailSent = true;
      safeEmailSent = false;
    }
  }

  if (millis() - lastBlink > 1500) {
    showAlert = !showAlert;
    lastBlink = millis();
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Risk: ");
  lcd.print(riskLevel);

  lcd.setCursor(0, 1);
  lcd.print("T:");
  lcd.print(temperature, 0);
  lcd.print(" H:");
  lcd.print(humidity, 0);

if (riskLevel == "Critical") {
    tone(BUZZER_PIN, 2500);
}
else if (riskLevel == "High") {
    tone(BUZZER_PIN, 1800);
    delay(500);
    noTone(BUZZER_PIN);
    delay(500);
}
else if (riskLevel == "Medium") {
    tone(BUZZER_PIN, 1500);
    delay(150);
    noTone(BUZZER_PIN);
}
else {
    noTone(BUZZER_PIN);
}


  // SAFE recovery mail
  if (!wildfireDetected && lastFireState) {
    if (!safeEmailSent) {
      sendSafeAlert(temperature, humidity, mqValue);
      safeEmailSent = true;
      emailSent = false;
    }
  }

  lastFireState = wildfireDetected;

  Serial.printf(
    "T:%.1f H:%.1f MQ:%d Flame:%d Risk:%s\n",
    temperature, humidity, mqValue, flameDetected, riskLevel.c_str()
  );

  delay(3000);
}
