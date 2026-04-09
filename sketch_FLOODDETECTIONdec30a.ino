#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_now.h>   // ✅ NEW (ESP-NOW)

// ---- Wi-Fi ----
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// ---- Gmail SMTP ----
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "ismotaraprova@gmail.com"
#define AUTHOR_PASSWORD ""
#define RECIPIENT_EMAIL_1 "provaismot@gmail.com"
#define RECIPIENT_EMAIL_2 "mariamjm13913@gmail.com"

// ---- Firebase REST ----
#define DATABASE_URL "https://earlywarningsystem-53511-default-rtdb.firebaseio.com/floodData.json"

// ---- LCD ----
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---- Sensor Pins ----
#define TRIG_PIN 5
#define ECHO_PIN 18
#define RAIN_PIN 36
#define WATER_PIN 39
#define BUZZER_PIN 19

// ---- Variables ----
long duration;
float distance;
bool floodDetected = false;
bool lastFloodState = false;
bool emailSent = false;
bool safeEmailSent = false;
unsigned long lastBlink = 0;
bool showAlert = false;

// =====================================================================
// ✅ NEW: ESP-NOW (Flood -> Storm)
// =====================================================================
typedef struct {
  uint8_t floodRiskCode;   // 0=Low,1=Medium,2=High,3=Critical
  float distanceCm;        // ultrasonic distance (cm)
  uint8_t rainP;           // 0-100
  uint8_t waterP;          // 0-100
  uint8_t floodDetected;   // 0/1
} FloodPacket;

FloodPacket floodPkt;

// Storm ESP32 MAC: 6C:C8:40:8E:89:0C
uint8_t stormMac[] = {0x6C, 0xC8, 0x40, 0x8E, 0x89, 0x0C};

uint8_t riskToCode(const String& r) {
  if (r == "Medium") return 1;
  if (r == "High") return 2;
  if (r == "Critical") return 3;
  return 0; // Low
}

void espnowInitSender() {
  // Keep STA mode so WiFi + ESP-NOW can coexist
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, stormMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Failed to add ESP-NOW peer");
  } else {
    Serial.println("✅ ESP-NOW Sender ready");
  }
}

void sendFloodStatus(const String& riskLevel, float dist, int rainPercent, int waterPercent, bool floodDet) {
  floodPkt.floodRiskCode = riskToCode(riskLevel);
  floodPkt.distanceCm = dist;
  floodPkt.rainP = (uint8_t)constrain(rainPercent, 0, 100);
  floodPkt.waterP = (uint8_t)constrain(waterPercent, 0, 100);
  floodPkt.floodDetected = floodDet ? 1 : 0;

  esp_now_send(stormMac, (uint8_t*)&floodPkt, sizeof(floodPkt));
}
// =====================================================================


// ---- Time ----
String getTimeNow() {
  configTime(6 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+6
  struct tm t;
  if (!getLocalTime(&t)) return "Unknown";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// ---- Minimal Raw SMTP with response reading ----
void sendEmail(const char* subject, const char* body) {
  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(SMTP_HOST, SMTP_PORT)) {
    Serial.println(F("❌ SMTP connection failed"));
    return;
  }

  auto readResponse = [&client](const char* msg = nullptr) {
    if (msg) Serial.println(msg);
    unsigned long timeout = millis() + 5000; // 5s timeout
    while (millis() < timeout) {
      while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) Serial.println("SMTP: " + line);
      }
    }
  };

  // 1. Handshake
  client.println("EHLO esp32");
  readResponse("Sent EHLO");

  // 2. Auth
  client.println("AUTH LOGIN");
  readResponse("Sent AUTH LOGIN");

  // 3. Email + password in base64
  client.println("aXNtb3RhcmFwcm92YUBnbWFpbC5jb20="); // base64(email)
  readResponse("Sent email base64");

  client.println("a2NrdyBmZXd0IHF3bXAgbXR0YQ=="); // base64(app password)
  readResponse("Sent password base64");

  // 4. MAIL FROM / RCPT TO
  client.printf("MAIL FROM:<%s>\r\n", AUTHOR_EMAIL);
  readResponse("Sent MAIL FROM");

  client.printf("RCPT TO:<%s>\r\n", RECIPIENT_EMAIL_1);
  readResponse("Sent RCPT TO 1");

  client.printf("RCPT TO:<%s>\r\n", RECIPIENT_EMAIL_2);
  readResponse("Sent RCPT TO 2");

  // 5. DATA
  client.println("DATA");
  readResponse("Sent DATA");

  // 6. Message content
  client.printf("Subject: %s\r\n", subject);
  client.println("Content-Type: text/plain");
  client.println();
  client.println(body);
  client.println(".");
  readResponse("Sent message body");

  // 7. Quit
  client.println("QUIT");
  readResponse("Sent QUIT");

  client.stop();
  Serial.println(F("✅ Email transaction completed"));
  delay(1000);  // small delay to ensure network finishes
}

// ---- Upload to Firebase ----
void uploadToFirebase(float d, int r, int w, bool flood, String risk) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("⚠️ WiFi not connected!"));
    return;
  }

  String json = "{";
  json += "\"timestamp\":\"" + getTimeNow() + "\",";
  json += "\"water_level_cm\":" + String(d, 1) + ",";
  json += "\"rain_intensity_percent\":" + String(r) + ",";
  json += "\"water_sensor_percent\":" + String(w) + ",";
  json += "\"flood_detected\":" + String(flood ? 1 : 0) + ",";
  json += "\"risk_level\":\"" + risk + "\"";
  json += "}";

  Serial.println(F("📤 Sending data to Firebase:"));
  Serial.println(json);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, DATABASE_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(json);
  Serial.printf("🔥 Firebase HTTP Response: %d\n", code);

  if (code > 0) {
    String response = http.getString();
    Serial.println("✅ Firebase response: " + response);
  } else {
    Serial.println("❌ Firebase upload failed!");
  }

  http.end();
}

// ---- Alerts ----
void sendFloodAlert(float d, int r, int w) {
  char msg[150];
  snprintf(msg, sizeof(msg),
           "⚠️ FLOOD WARNING!\nWL: %.1f cm\nRain: %d%%\nWater: %d%%\nTime: %s\nTake action!\nStay alert and move to higher ground...",
           d, r, w, getTimeNow().c_str());
  sendEmail("⚠️ FLOOD WARNING", msg);
}

void sendSafeAlert(float d, int r, int w) {
  char msg[150];
  snprintf(msg, sizeof(msg),
           "✅ SAFE NOW\nWL: %.1f cm\nRain: %d%%\nWater: %d%%\nTime: %s\nYou are safe now. Stay aware of future alerts.",
           d, r, w, getTimeNow().c_str());
  sendEmail("✅ SAFE NOW", msg);
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  lcd.print("Flood Detection");
  delay(1000);
  lcd.clear();

  Serial.print("Connecting WiFi...");
  WiFi.mode(WIFI_STA);            // ✅ keep STA mode (needed for ESP-NOW)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  // ✅ NEW: Start ESP-NOW after WiFi is ready
  espnowInitSender();
}

void loop() {
  // --- Water Level ---
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  duration = pulseIn(ECHO_PIN, HIGH);
  distance = (duration * 0.0343) / 2;

  // --- Sensors ---
  int rain = analogRead(RAIN_PIN);
  int water = analogRead(WATER_PIN);
  int rainP = map(rain, 4095, 0, 0, 100);
  int waterP = map(water, 0, 4095, 0, 100);

  // ---- Determine Risk Level ----
  String riskLevel = "Low";
  floodDetected = false; // default

  if (distance <= 5 || rainP >= 90 || waterP >= 80) {
    riskLevel = "Critical";
    floodDetected = true;
  } else if ((distance > 5 && distance <= 10) || rainP >= 70 || waterP >= 60) {
    riskLevel = "High";
    floodDetected = false;  // just display alert, no email
  } else if ((distance > 10 && distance <= 18) || rainP >= 40 || waterP >= 30) {
    riskLevel = "Medium";
    floodDetected = false;
  } else {
    riskLevel = "Low";
    floodDetected = false;
  }

  // ✅ Original Firebase upload (unchanged)
  uploadToFirebase(distance, rainP, waterP, floodDetected, riskLevel);

  // ✅ NEW: Send status to Storm ESP32 for fusion (does NOT affect your logic)
  sendFloodStatus(riskLevel, distance, rainP, waterP, floodDetected);

  // --- Display & Alert ---
  if (riskLevel == "Critical") {
    if (!emailSent) {
      sendFloodAlert(distance, rainP, waterP);
      emailSent = true;
      safeEmailSent = false;
    }
  }

  if (millis() - lastBlink > 1500) {
    showAlert = !showAlert;
    lastBlink = millis();
  }

  if (showAlert) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("FLOOD ALERT!");
    lcd.setCursor(0, 1);
    lcd.printf("Level:%s", riskLevel.c_str());

    // 🔔 Buzzer behavior for ACTIVE buzzer
    if (riskLevel == "Critical" || riskLevel == "High") {
      digitalWrite(BUZZER_PIN, HIGH);  // Continuous tone
    } else if (riskLevel == "Medium") {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(200);
      digitalWrite(BUZZER_PIN, LOW);
      delay(200);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }

  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.printf("WL:%.1f R:%d%%", distance, rainP);
    lcd.setCursor(0, 1);
    lcd.printf("Water:%d%%", waterP);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Safe state handling (unchanged)
  if (!floodDetected) {
    if (lastFloodState) {
      lcd.clear();
      lastFloodState = false;
      if (!safeEmailSent) {
        sendSafeAlert(distance, rainP, waterP);
        safeEmailSent = true;
        emailSent = false;
      }
    }
    lcd.setCursor(0, 0);
    lcd.printf("WL:%.1f R:%d%%", distance, rainP);
    lcd.setCursor(0, 1);
    lcd.printf("Water:%d%%", waterP);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Update lastFloodState at end (keeps your safe alert logic consistent)
  lastFloodState = floodDetected;

  Serial.printf("WL:%.1f | Rain:%d | Water:%d | Risk:%s\n",
                distance, rainP, waterP, riskLevel.c_str());

  delay(3000);
}
