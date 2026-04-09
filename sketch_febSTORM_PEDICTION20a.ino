#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_now.h>   // ✅ ESP-NOW

// ---- Wi-Fi ----
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// ---- Gmail SMTP ----
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "ismotaraprova@gmail.com"
#define AUTHOR_PASSWORD ""
#define RECIPIENT_EMAIL "provaismot@gmail.com"

// ---- Firebase REST (Storm) ----
#define DATABASE_URL "https://earlywarningsystem-53511-default-rtdb.firebaseio.com/stormData.json"

// ---- Firebase REST (Fusion) ----
#define FUSION_URL "https://earlywarningsystem-53511-default-rtdb.firebaseio.com/fusionData.json"

// ---- LCD ----
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---- Sensor Pin ----
#define WIND_PIN 34
#define BUZZER_PIN 25   // ✅ GPIO25

// ---- Variables ----
float windSpeed;
bool cycloneDetected = false;
bool lastCycloneState = false;
bool emailSent = false;
bool safeEmailSent = false;
unsigned long lastBlink = 0;
bool showAlert = false;

// =====================================================================
// ✅ ESP-NOW Receiver (Flood -> Storm)  [Fixed for ESP32 core 3.x callback]
// =====================================================================
typedef struct {
  uint8_t floodRiskCode;   // 0=Low,1=Medium,2=High,3=Critical
  float distanceCm;        // ultrasonic distance (cm)
  uint8_t rainP;           // 0-100
  uint8_t waterP;          // 0-100
  uint8_t floodDetected;   // 0/1
} FloodPacket;

volatile bool gotFlood = false;
FloodPacket lastFlood = {0, 0.0, 0, 0, 0};

// ✅ FIX: New callback signature for ESP32 Arduino core 3.x
void onFloodRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (len == sizeof(FloodPacket)) {
    memcpy(&lastFlood, incomingData, sizeof(FloodPacket));
    gotFlood = true;
  }
}

void espnowInitReceiver() {
  // WiFi must be STA for ESP-NOW
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onFloodRecv);
  Serial.println("✅ ESP-NOW Receiver ready");
}

String floodRiskText(uint8_t code) {
  if (code == 1) return "Medium";
  if (code == 2) return "High";
  if (code == 3) return "Critical";
  return "Low";
}
// =====================================================================


// ---- Time ----
String getTimeNow() {
  configTime(6 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  if (!getLocalTime(&t)) return "Unknown";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// ---- Email ----
void sendEmail(const char* subject, const char* body) {
  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(SMTP_HOST, SMTP_PORT)) return;

  client.println("EHLO esp32"); delay(500);
  client.println("AUTH LOGIN"); delay(500);
  client.println("aXNtb3RhcmFwcm92YUBnbWFpbC5jb20="); delay(500);
  client.println("a2Nrd2Zld3Rxd21wbXR0YQ=="); delay(500);

  client.printf("MAIL FROM:<%s>\r\n", AUTHOR_EMAIL); delay(500);
  client.printf("RCPT TO:<%s>\r\n", RECIPIENT_EMAIL); delay(500);
  client.println("DATA"); delay(500);

  client.printf("Subject: %s\r\n", subject);
  client.println("Content-Type: text/plain");
  client.println();
  client.println(body);
  client.println(".");
  delay(500);
  client.println("QUIT");
  client.stop();
}

// ---- Upload to Firebase (Storm Node) ----
void uploadToFirebase(float ws, bool cyclone, String risk) {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"timestamp\":\"" + getTimeNow() + "\",";
  json += "\"wind_speed_mps\":" + String(ws, 2) + ",";
  json += "\"cyclone_detected\":" + String(cyclone ? 1 : 0) + ",";
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

// ---- Upload to Firebase (Fusion Node) ----
void uploadFusionToFirebase(float ws, String stormRisk, String fusedRisk, String fusedEvent) {
  if (WiFi.status() != WL_CONNECTED) return;

  // If you want fusion uploads ONLY after flood packet exists, uncomment:
  // if (!gotFlood) return;

  String json = "{";
  json += "\"timestamp\":\"" + getTimeNow() + "\",";
  json += "\"wind_speed_mps\":" + String(ws, 2) + ",";
  json += "\"storm_risk\":\"" + stormRisk + "\",";
  json += "\"flood_risk\":\"" + floodRiskText(lastFlood.floodRiskCode) + "\",";
  json += "\"rain_percent\":" + String(lastFlood.rainP) + ",";
  json += "\"water_percent\":" + String(lastFlood.waterP) + ",";
  json += "\"distance_cm\":" + String(lastFlood.distanceCm, 1) + ",";
  json += "\"fused_event\":\"" + fusedEvent + "\",";
  json += "\"fused_risk\":\"" + fusedRisk + "\"";
  json += "}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, FUSION_URL);
  http.addHeader("Content-Type", "application/json");
  http.POST(json);
  http.end();
}

// ---- Alerts ----
void sendCycloneAlert(float ws) {
  char msg[180];
  snprintf(msg, sizeof(msg),
           "CYCLONE ALERT!\nWind Speed: %.2f m/s\nTime: %s\nTake shelter immediately!",
           ws, getTimeNow().c_str());
  sendEmail("CYCLONE ALERT", msg);
}

void sendSafeAlert(float ws) {
  char msg[150];
  snprintf(msg, sizeof(msg),
           "SAFE NOW\nWind Speed: %.2f m/s\nTime: %s\nWeather stable.",
           ws, getTimeNow().c_str());
  sendEmail("SAFE NOW", msg);
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  pinMode(BUZZER_PIN, OUTPUT);

  lcd.print("Storm Prediction");
  delay(1500);
  lcd.clear();

  // ✅ Set STA mode once, then connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(400);

  // ✅ Start ESP-NOW receiver (coexists with WiFi STA)
  espnowInitReceiver();
}

// ---- Loop ----
void loop() {

  // ---- Read Wind Sensor ----
  int adcValue = analogRead(WIND_PIN);

  // Wind formula
  windSpeed = (adcValue / 4095.0) * 3.3 * 30.0;

  // ---- Risk Level (unchanged) ----
  String riskLevel = "Normal";
  cycloneDetected = false;

  if (windSpeed >= 18) {          // Cyclone threshold
    riskLevel = "Cyclone";
    cycloneDetected = true;
  }
  else if (windSpeed >= 6) {
    riskLevel = "Storm";
  }
  else {
    riskLevel = "Normal";
  }

  // ✅ Upload storm node (unchanged)
  uploadToFirebase(windSpeed, cycloneDetected, riskLevel);

  // ---- Email Logic (Only Cyclone) unchanged ----
  if (cycloneDetected) {
    if (!emailSent) {
      sendCycloneAlert(windSpeed);
      emailSent = true;
      safeEmailSent = false;
    }
  }

  if (!cycloneDetected && lastCycloneState) {
    if (!safeEmailSent) {
      sendSafeAlert(windSpeed);
      safeEmailSent = true;
      emailSent = false;
    }
  }

  lastCycloneState = cycloneDetected;

  // ===================================================================
  // ✅ Fusion Decision (Storm is Fusion Master)
  // ===================================================================
  bool floodHigh = (lastFlood.floodRiskCode >= 2);     // High/Critical
  bool heavyRain = (lastFlood.rainP >= 70);

  String fusedEvent = "Normal";
  String fusedRisk  = "Normal";

  if (cycloneDetected && floodHigh) {
    fusedEvent = "Cyclone+Flood";
    fusedRisk  = "Extreme";
  } else if (cycloneDetected && heavyRain) {
    fusedEvent = "Cyclone+HeavyRain";
    fusedRisk  = "HighRisk";
  } else if (!cycloneDetected && floodHigh) {
    fusedEvent = "FloodLikely";
    fusedRisk  = "HighRisk";
  } else if (cycloneDetected) {
    fusedEvent = "CycloneOnly";
    fusedRisk  = "Cyclone";
  } else if (riskLevel == "Storm") {
    fusedEvent = "StormOnly";
    fusedRisk  = "Storm";
  }

  // ✅ Upload fusion node
  uploadFusionToFirebase(windSpeed, riskLevel, fusedRisk, fusedEvent);
  // ===================================================================


  // ---- LCD + Buzzer (kept behavior; line 2 enhanced if flood data exists) ----
  if (millis() - lastBlink > 1500) {
    showAlert = !showAlert;
    lastBlink = millis();
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.printf("Wind: %.2f m/s", windSpeed);

  lcd.setCursor(0, 1);
  if (!gotFlood) {
    lcd.print("Level: " + riskLevel);  // same as before until flood arrives
  } else {
    // Keep within 16 chars best effort
    String shortFusion = fusedEvent;
    if (shortFusion.length() > 10) shortFusion = shortFusion.substring(0, 10);
    lcd.print(riskLevel + " " + shortFusion);
  }

  // ---- Buzzer behavior unchanged ----
  if (riskLevel == "Cyclone") {
    digitalWrite(BUZZER_PIN, HIGH);   // Continuous
  }
  else if (riskLevel == "Storm") {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);
    delay(200);
  }
  else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  Serial.printf("Wind: %.2f | Storm:%s | FloodRisk:%s | Fusion:%s\n",
                windSpeed,
                riskLevel.c_str(),
                floodRiskText(lastFlood.floodRiskCode).c_str(),
                fusedEvent.c_str());

  delay(3000);
}
