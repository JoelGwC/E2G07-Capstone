#include <WiFi.h>
#include <FirebaseESP32.h>
#include <ESP32Servo.h>

// ================= CONFIGURATION =================
#define WIFI_SSID       "KoenigseggOne1"
#define WIFI_PASSWORD   "gwt110199"
#define FIREBASE_HOST   "localtest-0327-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "GPK6p0wRo1MRLG0woe3t4sisdGss9jMvaLl2Lq8s"
#define ROOM_ID         "room_001" 

// Hardware Pins
#define PIN_RELAY_LIGHTS  14
static const int servoPin = 13;

// Objects
Servo servo1;
bool currentLockState = false; // <--- ADD THIS to track the servo's current position
FirebaseData streamData; // <--- NEW: Dedicated object for streaming
// FirebaseData fbData; // For Reading
FirebaseData fbWrite; // For Writing
FirebaseConfig config;
FirebaseAuth auth;

// State Tracking
unsigned long lastCheckTime = 0;
const int checkInterval = 3000; // Check every 1 second

void setup() {
  Serial.begin(115200);

  // 1. Hardware Init
  pinMode(PIN_RELAY_LIGHTS, OUTPUT);
  digitalWrite(PIN_RELAY_LIGHTS, LOW); 
  
  servo1.attach(servoPin);
  servo1.write(180); 

  // 2. WiFi Connection
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected.");

  // 3. Firebase Init
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  streamData.setBSSLBufferSize(16384, 1024);
  fbWrite.setBSSLBufferSize(16384, 1024);
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  if (!Firebase.beginStream(streamData, "/rooms/" + String(ROOM_ID))) {
    Serial.println("Stream Begin Error: " + streamData.errorReason());
  }
  
  // When a change happens, run the function 'onStreamCallback'
  Firebase.setStreamCallback(streamData, onStreamCallback, onStreamTimeout);
}


void loop() {
  
}

// =========================================================
//  STREAM CALLBACK (Runs automatically on change)
// =========================================================
void onStreamCallback(StreamData data) {
  
  String path = data.dataPath(); 
  Serial.println("Stream Update: " + path + " -> " + data.stringData());

  // ---------------------------------------------
  // A. STATUS UPDATES (Lights or Lock changed)
  // ---------------------------------------------
  if (path == "/status/LIGHT") {
    bool targetState = data.boolData();
    digitalWrite(PIN_RELAY_LIGHTS, targetState ? HIGH : LOW);
  }

  else if (path == "/status/LOCK") {
    bool targetState = data.boolData();
    // Anti-Jitter Fix: Only move if different
    if (targetState != currentLockState) {
       servo1.write(targetState ? 0 : 180);
       currentLockState = targetState;
       Serial.println("Servo Moved.");
    }
  }

  // ---------------------------------------------
  // B. MANUAL COMMANDS (From Admin Panel)
  // ---------------------------------------------
  else if (path.indexOf("/manual_command") > -1) {
    // If we detect ANY change in "manual_command", we fetch the whole object
    // using fbWrite to ensure we get clean data (Device + State).
    
    if (Firebase.get(fbWrite, "/rooms/" + String(ROOM_ID) + "/manual_command")) {
       FirebaseJson &json = fbWrite.jsonObject();
       FirebaseJsonData result;
       
       String device = "";
       bool state = false;

       json.get(result, "device"); 
       if(result.success) device = result.stringValue;
       
       json.get(result, "state"); 
       if(result.success) state = result.boolValue;

       if (device != "") {
         Serial.println("EXECUTING CMD: " + device);
         
         // 1. Execute
         if (device == "LIGHT") digitalWrite(PIN_RELAY_LIGHTS, state ? HIGH : LOW);
         if (device == "LOCK")  servo1.write(state ? 0 : 180);

         // 2. Update Status (Execute logic, then tell DB we did it)
         Firebase.setBool(fbWrite, "/rooms/" + String(ROOM_ID) + "/status/" + device, state);

         // 3. Clear Command
         Firebase.deleteNode(fbWrite, "/rooms/" + String(ROOM_ID) + "/manual_command");
       }
    }
  }
}

// Helper for stream timeouts
void onStreamTimeout(bool timeout) {
  if (timeout) Serial.println("Stream Timeout... waiting...");
}
