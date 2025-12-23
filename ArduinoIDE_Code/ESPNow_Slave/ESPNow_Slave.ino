#include <esp_now.h>
#include <WiFi.h>

// ================= CONFIGURATION =================
#define WIFI_SSID       "KoenigseggOne1"
#define WIFI_PASSWORD   "gwt110199"

// Pins on the Receiver ESP32
#define PIN_RELAY_LIGHTS  13
#define PIN_SOLENOID_LOCK 2

// Data structure must match the sender
typedef struct struct_message {
  char device[10];
  bool state;
} struct_message;

struct_message myData;

// Callback function when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  memcpy(&myData, incomingData, sizeof(myData));
  
  Serial.print("Command Received: ");
  Serial.print(myData.device);
  Serial.print(" -> ");
  Serial.println(myData.state ? "ON" : "OFF");

  if (String(myData.device) == "LIGHT") {
    digitalWrite(PIN_RELAY_LIGHTS, myData.state ? HIGH : LOW);
  } 
  else if (String(myData.device) == "LOCK") {
    digitalWrite(PIN_SOLENOID_LOCK, myData.state ? HIGH : LOW);
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_RELAY_LIGHTS, OUTPUT);
  pinMode(PIN_SOLENOID_LOCK, OUTPUT);
  
  // Initialize to OFF
  digitalWrite(PIN_RELAY_LIGHTS, LOW);
  digitalWrite(PIN_SOLENOID_LOCK, LOW);

  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Wait for connection (Ensures we are on the correct Channel)
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected. Channel: " + String(WiFi.channel()));

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register callback
  esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
  
  Serial.println("Receiver Ready. Waiting for commands...");
  Serial.print("My MAC Address: ");
  Serial.println(WiFi.macAddress()); // Use this MAC in the Sender code
}

void loop() {
  // Nothing to do here, ESP-NOW is interrupt based
  delay(1000); 
}