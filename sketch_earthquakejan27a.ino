#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <MPU6050.h>

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
#define DATABASE_URL "https://earlywarningsystem-53511-default-rtdb.firebaseio.com/earthquakeData.json"

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- SENSOR PINS ----------------
#define VIBRATION_PIN 27   // SW-18010P DO
#define BUZZER_PIN 26

// ---------------- MPU6050 ----------------
MPU6050 mpu;

// ---------------- EARTHQUAKE THRESHOLDS ----------------
// (Designed so you can create FALSE earthquakes for dataset)

#define MOTION_LOW        0.05   // Noise / idle
#define MOTION_MEDIUM     0.15   // Human / table shake
#define MOTION_HIGH       0.30   // Earthquake-like motion

#define CONFIRM_COUNT     8      // consecutive samples (~2–3 sec)

// ---------------- VARIABLES ----------------
bool earthquakeDetected = false;
bool lastQuakeState = false;
bool emailSent = false;
bool safeEmailSent = false;

unsigned long lastBlink = 0;
bool showAlert = false;

int motionCounter = 0;

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
void uploadToFirebase(float motion, bool vibration, bool quake, String risk) {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"timestamp\":\"" + getTimeNow() + "\",";  
  json += "\"motion\":" + String(motion, 3) + ",";
  json += "\"vibration_detected\":" + String(vibration ? 1 : 0) + ",";
  json += "\"earthquake_detected\":" + String(quake ? 1 : 0) + ",";
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
void sendQuakeAlert(float motion) {
  char msg[220];
  snprintf(msg, sizeof(msg),
           "🌍 EARTHQUAKE ALERT!\n"
           "Ground Motion: %.3f g\n"
           "Time: %s\n\n"
           "Take immediate precautions!",
           motion, getTimeNow().c_str());

  sendEmail("🌍 EARTHQUAKE ALERT", msg);
}

void sendSafeAlert(float motion) {
  char msg[200];
  snprintf(msg, sizeof(msg),
           "✅ AREA STABLE AGAIN\n"
           "Ground Motion: %.3f g\n"
           "Time: %s",
           motion, getTimeNow().c_str());

  sendEmail("✅ AREA SAFE", msg);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();

  pinMode(VIBRATION_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  Wire.begin();
  mpu.initialize();

  lcd.print("Earthquake Syst.");
  delay(1500);
  lcd.clear();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
  }
}

// ---------------- LOOP ----------------
void loop() {

  // ---- SW-18010P ----
  bool vibrationDetected = (digitalRead(VIBRATION_PIN) == LOW);

  // ---- MPU6050 ----
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float Ax = ax / 16384.0;
  float Ay = ay / 16384.0;
  float Az = az / 16384.0;

  float totalAcc = sqrt(Ax * Ax + Ay * Ay + Az * Az);
  float motion = abs(totalAcc - 1.0); // remove gravity

  String riskLevel = "Low";
  

  // =====================================================
  // 🌍 EARTHQUAKE DECISION LOGIC (FINAL & CLEAN)
  // =====================================================

  // ---- CRITICAL ----

  // ================= CLEAN EARTHQUAKE LOGIC =================

static int confirmCounter = 0;

if (motion >= MOTION_HIGH) {
   earthquakeDetected = false;   // <-- ADD THIS


  // Strong motion detected
  if (vibrationDetected) {
    confirmCounter++;
  } else {
    confirmCounter = max(confirmCounter - 1, 0);
  }

  if (confirmCounter >= 3) {   // ~8 seconds total
    riskLevel = "Critical";
    earthquakeDetected = true;
  } else {
    riskLevel = "High";
  }
}

else if (motion >= MOTION_MEDIUM) {
  riskLevel = "Medium";
  confirmCounter = 0;
}

else if (vibrationDetected) {
  riskLevel = "Medium";   // vibration alone = suspicious
  confirmCounter = 0;
}

else {
  riskLevel = "Low";
  confirmCounter = 0;
  earthquakeDetected = false;
}


  // ---------------- UPLOAD ----------------
  uploadToFirebase(
    motion,
    vibrationDetected,
    earthquakeDetected,
    riskLevel
  );

  // ---------------- ALERT + BUZZER ----------------
  if (riskLevel == "Critical") {
    if (!emailSent) {
      sendQuakeAlert(motion);
      emailSent = true;
      safeEmailSent = false;
    }
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Risk: ");
  lcd.print(riskLevel);

  lcd.setCursor(0, 1);
  lcd.print("Mo:");
  lcd.print(motion, 2);
  lcd.print(" V:");
  lcd.print(vibrationDetected ? "ON" : "OFF");


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
  if (!earthquakeDetected && lastQuakeState) {
    if (!safeEmailSent) {
      sendSafeAlert(motion);
      safeEmailSent = true;
      emailSent = false;
    }
  }

  lastQuakeState = earthquakeDetected;

  Serial.printf(
    "Motion:%.3f Vib:%d Risk:%s\n",
    motion, vibrationDetected, riskLevel.c_str()
  );

  delay(3000);
}
