#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Keypad.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ================= TODO: UPDATE YOUR CREDENTIALS =================
#define WIFI_SSID "KoenigseggOne1"
#define WIFI_PASSWORD "gwt110199"
#define FIREBASE_HOST "localtest-0327-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "GPK6p0wRo1MRLG0woe3t4sisdGss9jMvaLl2Lq8s"

#define ROOM_ID "room_001" // The ID of this specific room controller

// ================= HARDWARE PIN DEFINITIONS =================
const int PIN_RELAY_LIGHTS = 15;
const int PIN_SOLENOID_LOCK = 2;

// Keypad Setup (Adjust pins based on your wiring)
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {23, 22, 21, 19}; 
byte colPins[COLS] = {18, 5, 17, 16}; 
Keypad keypad = Keypad( makeKeymap(keys), rowPins, colPins, ROWS, COLS );

// ================= GLOBALS =================
FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;

WiFiUDP ntpUDP;
// Update offset for your timezone (e.g., UTC+8 = 8 * 3600 = 28800)
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0); 

String inputCode = "";
unsigned long unlockStartTime = 0;
bool isUnlocked = false;
const int UNLOCK_DURATION_MS = 5000; // Keep door open for 5 seconds
unsigned long activeSessionEndTime = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY_LIGHTS, OUTPUT);
  pinMode(PIN_SOLENOID_LOCK, OUTPUT);
  // Initialize outputs to OFF state (assuming active HIGH relay/lock driver)
  digitalWrite(PIN_RELAY_LIGHTS, LOW);
  digitalWrite(PIN_SOLENOID_LOCK, LOW);

  connectToWiFi();
  configTime(0, 0, "pool.ntp.org");

  // === FIX 1: KEYPAD SENSITIVITY (DEBOUNCE) ===
  // Increases stability. If keys still double-press, increase to 100.
  keypad.setDebounceTime(50);
  timeClient.begin();
  
  // === FIX 2: WAIT FOR VALID TIME ===
  Serial.print("Waiting for time sync...");
  while (timeClient.getEpochTime() < 1600000000) { // Wait until year > 2020
      timeClient.update();
      delay(500);
      Serial.print(".");
  }
  Serial.println("\nTime Synced!");
// === NEW LINES TO ADD HERE ===
  // 1. Set the size of the SSL buffer to handle larger certificates
  firebaseData.setBSSLBufferSize(1024 * 4 /* Rx buffer size */, 1024 /* Tx buffer size */);

  // 2. Set the timeout higher to avoid cutting off slow connections
  config.timeout.serverResponse = 10 * 1000;
  // ==============================


  // Firebase Setup
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  timeClient.update();
  handleKeypad();
  handleLockTimer();
  handleLightTimer();
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.print("Connected IP: ");
  Serial.println(WiFi.localIP());
}

void handleKeypad() {
  char key = keypad.getKey();
  if (key) {
    Serial.print("Key Pressed: "); Serial.println(key);

    if (key == '#') {
      // '#' signifies end of entry, check code
      if (inputCode.length() > 0) {
        checkAccessCode(inputCode);
        inputCode = ""; // Clear buffer
      }
    } else if (key == '*') {
       inputCode = ""; // Clear buffer if mistake made
       Serial.println("Buffer Cleared");
    } else if (key == 'A') {
      if (inputCode.length() > 0) {
        inputCode.remove(inputCode.length() - 1); // Delete last char
        Serial.println("Backspace: " + inputCode);
      } else {
        Serial.println("Buffer empty");
      }
    } else {
      inputCode += key; // Append number
    }
  }
}

void checkAccessCode(String enteredCode) {
  Serial.println("Checking code: " + enteredCode + "...");
  String path = "/rooms/" + String(ROOM_ID) + "/active_codes/" + enteredCode;

  if (Firebase.get(firebaseData, path)) {
    if (firebaseData.dataType() == "json") {
      FirebaseJson &json = firebaseData.jsonObject();
      
      unsigned long currentEpoch = timeClient.getEpochTime();
      long startTime = 0;
      long endTime = 0;
      long createdAt = 0; // <--- 1. NEW VARIABLE
      bool useLights = false;
      bool hasCheckedIn = false;

      // Parse JSON
      FirebaseJsonData jsonData;
      json.get(jsonData, "start_time");
      if (jsonData.success) startTime = jsonData.intValue;
      
      json.get(jsonData, "end_time");
      if (jsonData.success) endTime = jsonData.intValue;
      
      // <--- 2. FETCH CREATED_AT
      json.get(jsonData, "created_at");
      if (jsonData.success) createdAt = jsonData.intValue;

      json.get(jsonData, "use_lights");
      if (jsonData.success) useLights = jsonData.boolValue;
      
      json.get(jsonData, "has_checked_in");
      if (jsonData.success) hasCheckedIn = jsonData.boolValue;

      Serial.printf("Current: %lu, Start: %ld, Created: %ld\n", currentEpoch, startTime, createdAt);

      // <--- 3. SMART DEADLINE LOGIC
      // If booking was made AFTER start time (late booking), use creation time.
      // Otherwise, use the scheduled start time.
      long baseTime = (createdAt > startTime) ? createdAt : startTime;
      
      long lateLimit = baseTime + (3 * 60); // 3 Minute Grace Period from Base Time

      // === ACCESS LOGIC ===
      if (currentEpoch < endTime) { // Is the slot still active?
        
        if (hasCheckedIn) {
           // Case A: Already checked in. Allow re-entry.
           Serial.println("ACCESS GRANTED (Re-entry)");
           grantAccess(useLights, endTime);
        } 
        else {
           // Case B: First time entering.
           if (currentEpoch > lateLimit) {
             // It is past the 15-minute window
             Serial.println("ACCESS DENIED: Check-in deadline expired.");
             blinkFeedback(3); 
           } 
           else {
             // Case C: On Time (or Late Booking within 15m of booking time)
             Serial.println("ACCESS GRANTED (First Check-in)");
             
             // Update DB so the timer stops on the website
             Firebase.setBool(firebaseData, path + "/has_checked_in", true);
             
             grantAccess(useLights, endTime);
           }
        }

      } else {
        Serial.println("ACCESS DENIED: Booking has ended.");
        blinkFeedback(3);
      }
      
    } else {
       Serial.println("ACCESS DENIED: Invalid Data.");
       blinkFeedback(3);
    }
  } else {
    String errorReason = firebaseData.errorReason();
    if (errorReason == "path not exist") {
      Serial.println("ACCESS DENIED: PIN invalid.");
    } else {
      Serial.print("Error: "); Serial.println(errorReason);
    }
    blinkFeedback(3);
  }
}

void grantAccess(bool turnOnLights, long endTime) {
  digitalWrite(PIN_SOLENOID_LOCK, HIGH); // Unlock door
  if (turnOnLights) {
      digitalWrite(PIN_RELAY_LIGHTS, HIGH); // Turn on lights
      activeSessionEndTime = endTime; // Set the kill timer
  }
  isUnlocked = true;
  unlockStartTime = millis();
}

void handleLockTimer() {
  if (isUnlocked && (millis() - unlockStartTime > UNLOCK_DURATION_MS)) {
    digitalWrite(PIN_SOLENOID_LOCK, LOW); // Lock door again
    // Note: We are NOT turning off lights here automatically in this example.
    // You might want another timer logic or a manual switch to turn them off.
    isUnlocked = false;
    Serial.println("Door Relocked");
  }
}

void handleLightTimer() {
  // If a session is active AND current time > end time
  if (activeSessionEndTime > 0 && timeClient.getEpochTime() > activeSessionEndTime) {
      digitalWrite(PIN_RELAY_LIGHTS, LOW); 
      Serial.println("Session Expired: Lights OFF");
      activeSessionEndTime = 0; 
  }
}

// Simple visual feedback on the ESP32 built-in LED (usually pin 2) if something goes wrong
void blinkFeedback(int times) {
   pinMode(2, OUTPUT);
   for(int i=0; i<times; i++){
      digitalWrite(2, HIGH); delay(100); digitalWrite(2, LOW); delay(100);
   }
}