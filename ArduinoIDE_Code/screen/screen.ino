#include <WiFi.h>
#include <FirebaseESP32.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <vector>

// ================= USER CONFIGURATION =================
#define WIFI_SSID       "KoenigseggOne1"
#define WIFI_PASSWORD   "gwt110199"
#define FIREBASE_HOST   "localtest-0327-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "GPK6p0wRo1MRLG0woe3t4sisdGss9jMvaLl2Lq8s"
#define ROOM_ID         "room_001"  //Change according to desired room

// Hardware Pins
#define PIN_RELAY_LIGHTS  12
#define PIN_SOLENOID_LOCK 13
#define CTP_SDA          21
#define CTP_SCL          22
#define CTP_RST          33
#define FT6336U_ADDR     0x38

// ================= COLORS & STYLES =================
#define COLOR_BG        0x0000 // Black
#define COLOR_NAV_BG    0x10A2 // Dark Grey
#define COLOR_ACCENT    0x0400 // Dark Green
#define COLOR_BUSY      0xD000 // Red
#define COLOR_FREE      0x0400 // Green
#define COLOR_TEXT      0xFFFF // White
#define COLOR_CARD      0x2124 // Dark Charcoal

// ================= GLOBALS =================
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft); // <--- SPRITE OBJECT ADDED FOR SCROLLING FIX

FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 0); // no offset --> get UTC from HTML 

struct TimeSlot {
  int hour;           // 7-22 (7AM-10PM)
  bool isBooked;
  String bookedBy;
};

std::vector<TimeSlot> timeSlots; // 15 slots (7AM-10PM)
int16_t touchX = 0, touchY = 0;
bool lastTouchState = false;

// State Management
enum ScreenState { SCREEN_SCHEDULE, SCREEN_KEYPAD };
ScreenState currentScreen = SCREEN_SCHEDULE;
String enteredPIN = "";
String roomNameDisplay = "Loading..."; // <--- NEW DYNAMIC ROOM NAME
unsigned long lastSyncTime = 0;
unsigned long unlockStartTime = 0;
bool isUnlocked = false;

// Scrolling Variables
int scrollOffset = 0;           // Current scroll position (pixels)
int maxScrollOffset = 0;        // Maximum scroll limit
const int SLOT_HEIGHT = 60;     // Height of each time slot card
const int VISIBLE_HEIGHT = 230; // Visible content area height
int lastTouchY = 0;             // For drag scrolling
bool isDragging = false;

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  
  // 1. Hardware Init
  pinMode(PIN_RELAY_LIGHTS, OUTPUT);
  pinMode(PIN_SOLENOID_LOCK, OUTPUT); 
  digitalWrite(PIN_RELAY_LIGHTS, LOW);
  digitalWrite(PIN_SOLENOID_LOCK, LOW);
  
  Wire.begin(CTP_SDA, CTP_SCL);
  pinMode(CTP_RST, OUTPUT);
  digitalWrite(CTP_RST, LOW);
  delay(10);
  digitalWrite(CTP_RST, HIGH);
  delay(50);

  tft.init();
  tft.setRotation(1); // Landscape (480x320)
  tft.fillScreen(COLOR_BG);
  tft.invertDisplay(1);

  
  // <--- CREATE SPRITE (Crucial for Scrolling Fix) ---
  spr.createSprite(480, SLOT_HEIGHT); 

  // 2. WiFi & Time
  drawLoading("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  
  drawLoading("Syncing Time...");
  timeClient.begin();
  while(!timeClient.update()) {
    timeClient.forceUpdate();
    delay(500);
  }

  // 3. Firebase Init
  drawLoading("Connecting Database...");
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  config.timeout.serverResponse = 10000; 
  firebaseData.setBSSLBufferSize(16384, 1024); // allocate a large buffer for large SSL certificates
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // 4. Initialize Data
  initializeTimeSlots();
  fetchRoomName(); // <--- NEW FUNCTION CALL
  
  // 5. Initial Data Fetch
  fetchSchedule();
  drawFullUI();
}

// ================= MAIN LOOP =================
void loop() {
  timeClient.update();
  bool isTouched = readTouch();

  // 1. Handle Touch Interaction
  if (isTouched && !lastTouchState) {
    // Touch Started
    lastTouchY = touchY;
    isDragging = false;
    handleTouchStart(touchX, touchY);
  } else if (isTouched && lastTouchState) {
    // Touch Dragging
    if (currentScreen == SCREEN_SCHEDULE && touchY < 280) {
      int deltaY = touchY - lastTouchY;
      if (abs(deltaY) > 2) { 
        isDragging = true;
        scrollOffset -= deltaY; 
        
        // Clamp scroll
        if (scrollOffset < 0) scrollOffset = 0;
        if (scrollOffset > maxScrollOffset) scrollOffset = maxScrollOffset;
        
        drawScheduleContent();
        lastTouchY = touchY;
      }
    }
  } else if (!isTouched && lastTouchState) {
    // Touch Ended
    if (!isDragging) {
      handleTouchEnd(touchX, touchY);
    }
    isDragging = false;
  }
  
  lastTouchState = isTouched;

  // 2. Periodic Firebase Sync (Every 30s)
  if (millis() - lastSyncTime > 30000) {
    initializeTimeSlots();
    fetchSchedule();
    if (currentScreen == SCREEN_SCHEDULE) drawScheduleContent();
    drawHeader();
    lastSyncTime = millis();
  }

  // 3. Lock Timer
  if (isUnlocked && (millis() - unlockStartTime > 5000)) {
    digitalWrite(PIN_SOLENOID_LOCK, LOW);
    isUnlocked = false;
    Serial.println("Door Relocked");
  }

  delay(30); // Reduced delay for smoother scrolling
}

// ================= FIREBASE LOGIC =================

void initializeTimeSlots() {
  timeSlots.clear();
  int currentHour = getCurrentLocalHour();
  int startHour = max(7, currentHour);
  for (int h = startHour; h <= 21; h++) { // 7AM to 10PM
    TimeSlot slot;
    slot.hour = h;
    slot.isBooked = false;
    slot.bookedBy = "";
    timeSlots.push_back(slot);
  }
  
  // Calculate max scroll
  int totalHeight = timeSlots.size() * SLOT_HEIGHT;
  maxScrollOffset = totalHeight - VISIBLE_HEIGHT;
  if (maxScrollOffset < 0) maxScrollOffset = 0;
}

// <--- NEW: FETCH ROOM NAME ---
void fetchRoomName() {
  String path = "/rooms/" + String(ROOM_ID) + "/config/name";
  if (Firebase.getString(firebaseData, path)) {
    roomNameDisplay = firebaseData.stringData();
    roomNameDisplay.toUpperCase(); // Make it look like a title
  } else {
    roomNameDisplay = "ROOM 001"; // Fallback
  }
}

void fetchSchedule() {
  String path = "/rooms/" + String(ROOM_ID) + "/active_codes";
  
  // 1. Reset all slots
  for (auto &slot : timeSlots) {
    slot.isBooked = false;
    slot.bookedBy = "";
  }

  // 2. Fetch Data
  if (Firebase.get(firebaseData, path)) {
    FirebaseJson &json = firebaseData.jsonObject();
    size_t len = json.iteratorBegin();
    String key, value = "";
    int type = 0;

    // Timezone Logic (UTC to Malaysia)
    long nowUTC = timeClient.getEpochTime();
    long offsetSeconds = 28800; // 8 Hours
    long nowLocal = nowUTC + offsetSeconds; 
    long todayStartLocal = (nowLocal / 86400) * 86400; //not redundant --> rounding down
    long todayEndLocal = todayStartLocal + 86400;      

    for (size_t i = 0; i < len; i++) {
      json.iteratorGet(i, type, key, value); 
      
      FirebaseJson subJson;
      subJson.setJsonData(value);
      FirebaseJsonData result;
      
      long startTimeUTC = 0, endTimeUTC = 0;
      String createdBy = "";
      
      subJson.get(result, "start_time"); if(result.success) startTimeUTC = result.intValue;
      subJson.get(result, "end_time"); if(result.success) endTimeUTC = result.intValue;
      subJson.get(result, "created_by"); if(result.success) createdBy = result.stringValue;

      long startTimeLocal = startTimeUTC + offsetSeconds;
      long endTimeLocal   = endTimeUTC   + offsetSeconds;

      if (endTimeLocal > todayStartLocal && startTimeLocal < todayEndLocal) {
        time_t st = startTimeLocal;
        struct tm * tmStart = localtime(&st);
        int startHour = tmStart->tm_hour;
        
        time_t et = endTimeLocal;
        struct tm * tmEnd = localtime(&et);
        int endHour = tmEnd->tm_hour;
        if (tmEnd->tm_min > 0) endHour++; 

        if (startTimeLocal < todayStartLocal) startHour = 0; 
        if (endTimeLocal > todayEndLocal) endHour = 24;      

        for (auto &slot : timeSlots) {
          if (slot.hour >= startHour && slot.hour < endHour) {
            slot.isBooked = true;
            if (slot.bookedBy.length() == 0) {
              slot.bookedBy = createdBy.substring(0, createdBy.indexOf('@'));
            }
          }
        }
      }
    }
    json.iteratorEnd();
  }
}

void verifyPIN(String pin) {
  drawLoading("Verifying...");
  String path = "/rooms/" + String(ROOM_ID) + "/active_codes/" + pin;

  if (Firebase.get(firebaseData, path)) {
    if (firebaseData.dataType() == "json") {
      FirebaseJson &json = firebaseData.jsonObject();
      FirebaseJsonData jsonData;
      
      long startTime = 0, endTime = 0;
      bool useLights = false;

      json.get(jsonData, "start_time"); if(jsonData.success) startTime = jsonData.intValue;
      json.get(jsonData, "end_time"); if(jsonData.success) endTime = jsonData.intValue;
      json.get(jsonData, "use_lights"); if(jsonData.success) useLights = jsonData.boolValue;

      long now = timeClient.getEpochTime();

      if (now >= startTime && now <= endTime) {
        digitalWrite(PIN_SOLENOID_LOCK, HIGH);
        if(useLights) digitalWrite(PIN_RELAY_LIGHTS, HIGH);
        isUnlocked = true;
        unlockStartTime = millis();
        Firebase.setBool(firebaseData, path + "/has_checked_in", true);
        showAccessResult(true);
      } else {
        showAccessResult(false);
      }
    } else {
      showAccessResult(false);
    }
  } else {
    showAccessResult(false);
  }
  
  delay(2000);
  enteredPIN = "";
  drawFullUI();
}

// ================= UI DRAWING =================

void drawFullUI() {
  tft.fillScreen(COLOR_BG);
  drawHeader();
  drawNavBar();
  
  if (currentScreen == SCREEN_SCHEDULE) drawScheduleContent();
  else drawKeypadContent(false);
}

void drawHeader() {
  // Determine current status
  long now = timeClient.getEpochTime();
  long nowLocal = now + 28800;
  time_t nowT = nowLocal;
  struct tm * tmNow = localtime(&nowT);
  int currentHour = tmNow->tm_hour;
  
  bool occupied = false;
  for (const auto &slot : timeSlots) {
    if (slot.hour == currentHour && slot.isBooked) {
      occupied = true;
      break;
    }
  }

  tft.fillRect(0, 0, 480, 40, COLOR_CARD);
  tft.drawLine(0, 40, 480, 40, TFT_DARKGREY);
  
  tft.setTextFont(4); 
  tft.setTextColor(COLOR_TEXT, COLOR_CARD);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(roomNameDisplay, 10, 20); // <--- USING DYNAMIC NAME

  // Status Badge
  uint16_t badgeColor = occupied ? COLOR_BUSY : COLOR_FREE;
  String statusText = occupied ? "OCCUPIED" : "AVAILABLE";
  
  tft.fillRoundRect(348, 4, 125, 24, 8, badgeColor);
  tft.setTextColor(TFT_WHITE, badgeColor);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(statusText, 410, 20);
}

void drawNavBar() {
  int y = 280;
  
  // Schedule Tab
  uint16_t c1 = (currentScreen == SCREEN_SCHEDULE) ? COLOR_ACCENT : COLOR_NAV_BG;
  tft.fillRoundRect(10, y, 225, 35, 8, c1);
  tft.setTextColor(TFT_WHITE, c1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString("SCHEDULE", 122, y + 17);

  // Keypad Tab
  uint16_t c2 = (currentScreen == SCREEN_KEYPAD) ? COLOR_ACCENT : COLOR_NAV_BG;
  tft.fillRoundRect(245, y, 225, 35, 8, c2);
  tft.setTextColor(TFT_WHITE, c2);
  tft.drawString("ENTER PIN", 357, y + 17);
}

// <--- FIXED: SCROLLING & STACKING USING SPRITE ---
void drawScheduleContent() {
  // 1. Set Viewport to clip text at the top and bottom
  tft.setViewport(0, 45, 480, 235);
  
  // 2. Loop through all time slots
  for (size_t i = 0; i < timeSlots.size(); i++) {
    int slotY = (i * SLOT_HEIGHT) - scrollOffset;
    
    // Skip if off-screen (Optimisation)
    if (slotY <= -SLOT_HEIGHT || slotY >= 235) continue;
    
    TimeSlot &slot = timeSlots[i];
    
    // --- DRAW ON SPRITE (Memory Buffer) ---
    // This overwrites all pixels for this row, eliminating "stacking"
    spr.fillSprite(COLOR_BG); 
    
    // Draw Card Background on Sprite
    spr.fillRoundRect(10, 5, 460, SLOT_HEIGHT - 5, 5, COLOR_CARD);
    
    // Draw Time Label
    char timeStr[10];
    int localHour = slot.hour;
    if(localHour > 12) localHour -= 12;
    else if(localHour == 0) localHour = 12;
    sprintf(timeStr, "%02d:00 %s", localHour, slot.hour >= 12 ? "PM" : "AM");
    
    spr.setTextFont(2);
    spr.setTextSize(2);
    spr.setTextColor(COLOR_TEXT, COLOR_CARD);
    spr.setCursor(25, 15);
    spr.print(timeStr);
    
    // Draw Badge
    uint16_t statusColor = slot.isBooked ? COLOR_BUSY : COLOR_FREE;
    String statusText = slot.isBooked ? "BOOKED" : "FREE";
    spr.fillRoundRect(350, 15, 100, 35, 5, statusColor);
    spr.setTextColor(TFT_WHITE, statusColor);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(1);
    spr.drawString(statusText, 400, 32);
    
    // Draw Name
    if (slot.isBooked && slot.bookedBy.length() > 0) {
      spr.setTextColor(TFT_LIGHTGREY, COLOR_CARD);
      spr.setTextDatum(ML_DATUM);
      int timeRightX = 25 + spr.textWidth(timeStr) + 70; // 10 px spacing
      spr.drawString(slot.bookedBy, timeRightX, 25);
    }
    
    // --- PUSH SPRITE TO SCREEN ---
    spr.pushSprite(0, slotY);
  }
  
  // 3. Clear bottom area if list is shorter than screen
  int totalListH = timeSlots.size() * SLOT_HEIGHT;
  int endY = totalListH - scrollOffset;
  if (endY < 235) {
     tft.fillRect(0, endY, 480, 235 - endY, COLOR_BG);
  }

  // 4. Draw Scrollbar (Direct to TFT, over sprites)
  if (maxScrollOffset > 0) {
    int h = 235;
    int indH = (h * h) / (timeSlots.size() * SLOT_HEIGHT);
    if(indH < 10) indH = 10;
    int indY = (scrollOffset * (h - indH)) / maxScrollOffset;
    tft.fillRect(475, 0, 5, h, COLOR_NAV_BG);
    tft.fillRect(475, indY, 5, indH, COLOR_ACCENT);
  }

  tft.resetViewport();
}

// Change signature to accept the boolean flag
void drawKeypadContent(bool partialUpdate) {
  
  // --- STATIC DRAWING (Buttons & Background) ---
  // Only runs once when you first open the tab
  if (!partialUpdate) {
    tft.fillRect(0, 45, 480, 235, COLOR_BG);
    
    // Draw Input Box Border
    tft.drawRoundRect(100, 50, 280, 40, 5, TFT_WHITE);
    
    // Draw Keypad Buttons (4x3 Grid)
    String labels[12] = {"1","2","3","C", 
                         "4","5","6","0", 
                         "7","8","9","E"};
    
    int startX = 90; int startY = 105;
    int w = 65; int h = 48; int gap = 10;
    tft.setTextFont(4);
    
    for(int i = 0; i < 12; i++) {
      int r = i / 4; int c = i % 4;
      int x = startX + c * (w + gap);
      int y = startY + r * (h + gap);
      
      uint16_t color = COLOR_CARD;
      if(labels[i] == "C") color = COLOR_BUSY;
      else if(labels[i] == "E") color = COLOR_ACCENT;
      else if(labels[i] == "0") color = TFT_DARKGREY;
      
      tft.fillRoundRect(x, y, w, h, 8, color);
      tft.setTextColor(TFT_WHITE, color);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(labels[i], x + w / 2, y + h / 2 + 2);
    }
  }

  // --- DYNAMIC DRAWING (Text Input) ---
  // Runs every time you press a key. 
  // We use a Viewport to restrict drawing to ONLY the text box area.
  
  // 1. Define active area (Inside the white border)
  tft.setViewport(102, 52, 276, 36);
  
  // 2. Clear ONLY this small box
  tft.fillScreen(COLOR_CARD); 
  
  // 3. Draw the PIN
  tft.setTextColor(TFT_WHITE, COLOR_CARD);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  
  // Coordinates are now relative to the viewport (0,0 is top-left of box)
  // Center of box (276/2 = 138, 36/2 = 18)
  tft.drawString(enteredPIN, 138, 18); 
  
  // 4. Reset to full screen for other functions
  tft.resetViewport();
}

void showAccessResult(bool success) {
  tft.fillScreen(success ? COLOR_FREE : COLOR_BUSY);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.drawString(success ? "ACCESS GRANTED" : "ACCESS DENIED", 240, 160);
}

void drawLoading(String msg) {
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(TFT_WHITE, COLOR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.drawString(msg, 240, 160);
}

// ================= TOUCH LOGIC =================

void handleTouchStart(int x, int y) {
  // Placeholder for touch start (used for drag detection)
}

void handleTouchEnd(int x, int y) {
  // Check Nav Bar
  if (y > 280) {
    if (x < 240) {
      if(currentScreen != SCREEN_SCHEDULE) {
        currentScreen = SCREEN_SCHEDULE;
        scrollOffset = 0; // Reset scroll
        drawFullUI();
      }
    } else {
      if(currentScreen != SCREEN_KEYPAD) {
        currentScreen = SCREEN_KEYPAD;
        drawFullUI();
      }
    }
    return;
  }
  
  // Keypad Logic
  if (currentScreen == SCREEN_KEYPAD && y > 105 && y < 275 && x > 90 && x < 390) {
    int startX = 90; int startY = 105; int w = 65; int h = 48; int gap = 10;
    
    int col = (x - startX) / (w + gap);
    int row = (y - startY) / (h + gap);
    
    if (col >= 0 && col < 4 && row >= 0 && row < 3) {
      int index = row * 4 + col;
      String labels[12] = {"1","2","3","C","4","5","6","0","7","8","9","E"};
      String key = labels[index];
      
      if (key == "C") {
        enteredPIN = "";
      } else if (key == "E") {
        if(enteredPIN.length() > 0) verifyPIN(enteredPIN);
      } else if (enteredPIN.length() < 6) {
        enteredPIN += key;
      }
      
      drawKeypadContent(true);
    }
  }
}

bool readTouch() {
  Wire.beginTransmission(FT6336U_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission() != 0) return false;
  
  Wire.requestFrom(FT6336U_ADDR, 6);
  if (Wire.available() < 6) return false;
  
  uint8_t points = Wire.read() & 0x0F;
  if (points == 0) return false;
  
  uint8_t xH = Wire.read();
  uint8_t xL = Wire.read();
  uint8_t yH = Wire.read();
  uint8_t yL = Wire.read();
  
  int16_t tempX = ((xH & 0x0F) << 8) | xL;
  int16_t tempY = ((yH & 0x0F) << 8) | yL;
  
  touchX = tempY; 
  touchY = 320 - tempX;
  
  return true;
}

// Add this helper function after the globals section
int getCurrentLocalHour() {
  long nowUTC = timeClient.getEpochTime();
  long nowLocal = nowUTC + 28800; // Malaysia timezone offset
  time_t nowT = nowLocal;
  struct tm * tmNow = localtime(&nowT);
  return tmNow->tm_hour;
}
