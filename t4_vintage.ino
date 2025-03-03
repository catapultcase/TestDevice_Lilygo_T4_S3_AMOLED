#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LV_Helper.h>
#include <Preferences.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// Use default Serial for USB debugging (UART0)
// Use UART1 for external communication: assign RX to GPIO18 and TX to GPIO48
#define UART1_RX_PIN 18
#define UART1_TX_PIN 48
// Note: On ESP32-S3, UART1 is already defined by the core.
// Therefore, simply use Serial1 without redeclaring it.
 
LilyGo_Class amoled;
#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

Preferences preferences;
bool serialActive = false;
bool settingsScreenActive = false;
bool screenSleep = false;
bool screenWakingUp = false;
bool autoSleepEnabled;
bool wifiConnected = false; // Global flag for WiFi connection status
String fullPayload = "";
int brightness = 100;
int orientation = 0;
unsigned long lastActivity = 0;
unsigned long autoSleepTimeout = 60000; // 60 seconds
lv_obj_t *labelSleep = nullptr;
lv_obj_t *wifiStatusLabel = nullptr;

// Pointers to labels for easy update
lv_obj_t **labelNames = nullptr;
lv_obj_t **labelValues = nullptr;
lv_obj_t *statusLabel = nullptr; // Label for WiFi status

int payloadMetadataColumns = 2;
int payloadMetadataRows = 2;
int numSensors = payloadMetadataColumns * payloadMetadataRows;

lv_obj_t *topBar = nullptr;
lv_obj_t *bottomBar = nullptr;
lv_obj_t *topBarLabel = nullptr;
lv_obj_t *bottomBarLabel = nullptr;
lv_obj_t *topTriangle = nullptr;
lv_obj_t *bottomTriangle = nullptr;

// WiFi credentials
const char* ssid = "Jon6";
const char* password = "fv4!F48P8&tR";

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Global jsonBuffer for chunked data processing
String jsonBuffer = "";
int payloadLength = 0;
bool readingLength = true;
int bytesRead = 0;

struct IndicatorData {
    lv_obj_t *triangle;
    lv_obj_t *label;
};

void createCustomScale(lv_obj_t *parent, lv_obj_t **bar, lv_obj_t **label, lv_obj_t **triangle);
void createStatsScreen();
void handleSerialData();
void handleIncomingDataChunk(uint8_t *data, size_t len);
void createRebootScreen();
void switchToStatsScreen();
void updateStatsScreen(const JsonObject& obj);
void updateWiFiStatusLabel();
void touchEventHandler(lv_event_t *e);
void goToSleep();
void wakeUpScreen();
void connectToWiFi();
void printDebugInfo();
void settingsEventHandler(lv_event_t *e);
void sleepCountdownEventHandler(lv_event_t *e);

lv_obj_t *sensor2Label = nullptr;
lv_obj_t *sensor3Label = nullptr;

// Global variable to accumulate inbound data from UART0 (USB serial)
String serialBuffer = "";

void setup() {
    // Initialize USB Serial (UART0) for debugging at 9600 baud
    Serial.begin(9600);
    
    // Initialize Serial1 on UART1 with RX on GPIO18 and TX on GPIO48 at 115200 baud.
    Serial1.begin(115200, SERIAL_8N1, UART1_RX_PIN, UART1_TX_PIN);

    // Initialize LVGL
    if (!amoled.begin()) {
        while (1) {
            Serial.println("There is a problem with the device!");
            delay(1000);
        }
    }
    beginLvglHelper(amoled);

    // Initialize Preferences
    preferences.begin("settings", false);
    brightness = preferences.getInt("brightness", 100);
    orientation = preferences.getInt("orientation", 0);
    autoSleepEnabled = preferences.getBool("autoSleepEnabled", true);
    amoled.setRotation(orientation / 90);
    amoled.setBrightness(brightness);

    // Show CatapultCase screen with WiFi status
    createRebootScreen();

    // Add touch event handler
    lv_obj_add_event_cb(lv_scr_act(), touchEventHandler, LV_EVENT_PRESSED, NULL);

    // Connect to WiFi
    connectToWiFi();

    // Initialize web server
    server.on("/data", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Data received");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        handleIncomingDataChunk(data, len);
    });
    server.begin();

    lastActivity = millis();
}

void loop() {
    lv_task_handler();
    delay(5);

    if (autoSleepEnabled && !screenSleep && !settingsScreenActive) {
        unsigned long currentTime = millis();
        unsigned long elapsedTime = currentTime - lastActivity;
        if (elapsedTime >= autoSleepTimeout) {
            Serial.println("Auto-sleep due to inactivity");
            goToSleep();
        } else {
            unsigned long remainingTime = autoSleepTimeout - elapsedTime;
            lv_label_set_text_fmt(labelSleep, "Sleep (%d)", remainingTime / 1000);
        }
    }

    if (screenSleep) {
        handleSerialData();
        return;
    }

    if (settingsScreenActive || screenWakingUp) {
        return;
    }

    handleSerialData();

    if (!serialActive) {
        lv_obj_t *screen = lv_scr_act();
        if (lv_obj_get_user_data(screen) != (void *)1) {
            createRebootScreen();
        }
        delay(100);
    }

    // Read data from UART1 (second UART)
    while (Serial1.available() > 0) {
        String data = Serial1.readStringUntil('\n');
        data.trim();
        if (data.length() > 0) {
            // Print data from UART1 to the Serial Monitor (UART0)
            Serial.print("UART1 Data: ");
            Serial.println(data);
        }
    }
}

void handleSerialData() {
    while (Serial.available() > 0) {
        char incomingByte = Serial.read();
        serialBuffer += incomingByte;
        if (incomingByte == '\n') {
            serialBuffer.trim();
            Serial.print("Received: ");
            Serial.println(serialBuffer);
            serialBuffer = "";
        }
    }
}

void handleIncomingDataChunk(uint8_t *data, size_t len) {
    lastActivity = millis();
    if (data == nullptr || len == 0) {
        Serial.println("Error: Incoming data is null or empty.");
        return;
    }
    if (readingLength) {
        char firstByte = data[0];
        if (firstByte == '{') {
            Serial.println("Data without prefix length detected. Responding 'ok'.");
            jsonBuffer = "";
            readingLength = true;
            bytesRead = 0;
            return;
        }
        if (isdigit(firstByte)) {
            jsonBuffer += String((char*)data).substring(0, len);
            bytesRead += len;
            if (jsonBuffer.length() >= 8) {
                payloadLength = jsonBuffer.substring(0, 8).toInt();
                Serial.print("Length Prefix Detected: ");
                Serial.println(payloadLength);
                jsonBuffer = jsonBuffer.substring(8);
                readingLength = false;
            }
        }
    } else {
        jsonBuffer += String((char*)data).substring(0, len);
        bytesRead += len;
        if (bytesRead >= payloadLength) {
            Serial.println("Processed Inbound Data");
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, jsonBuffer);
            if (error) {
                Serial.print("deserializeJson() failed: ");
                Serial.println(error.c_str());
            } else {
                Serial.println("JSON data processed successfully.");
                fullPayload = "";
                serializeJsonPretty(doc, fullPayload);
                serialActive = true;
                settingsScreenActive = false;
                screenSleep = false;
                if (doc.containsKey("metadata")) {
                    JsonObject metadata = doc["metadata"];
                    payloadMetadataColumns = metadata["PayloadMetadataColumns"] | payloadMetadataColumns;
                    payloadMetadataRows = metadata["PayloadMetadataRows"] | payloadMetadataRows;
                    numSensors = payloadMetadataColumns * payloadMetadataRows;
                }
                Serial.println("Switching to stats screen...");
                switchToStatsScreen();
                Serial.println("Updating stats screen...");
                updateStatsScreen(doc.as<JsonObject>());
                delay(500);
            }
            jsonBuffer = "";
            readingLength = true;
            bytesRead = 0;
        }
    }
}

void createRebootScreen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_user_data(scr, (void *)1);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "CATAPULTCASE");
    lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -60);
    lv_obj_t *btnSettings = lv_btn_create(scr);
    lv_obj_set_size(btnSettings, 160, 60);
    lv_obj_align(btnSettings, LV_ALIGN_CENTER, -100, 20);
    lv_obj_set_style_bg_color(btnSettings, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_color(btnSettings, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_add_event_cb(btnSettings, settingsEventHandler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *labelSettings = lv_label_create(btnSettings);
    lv_label_set_text(labelSettings, "Settings");
    lv_obj_set_style_text_font(labelSettings, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(labelSettings, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_center(labelSettings);
    lv_obj_t *btnSleep = lv_btn_create(scr);
    lv_obj_set_size(btnSleep, 160, 60);
    lv_obj_align(btnSleep, LV_ALIGN_CENTER, 100, 20);
    lv_obj_set_style_bg_color(btnSleep, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_color(btnSleep, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_add_event_cb(btnSleep, sleepCountdownEventHandler, LV_EVENT_CLICKED, NULL);
    labelSleep = lv_label_create(btnSleep);
    lv_label_set_text(labelSleep, "Sleep");
    lv_obj_set_style_text_font(labelSleep, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(labelSleep, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_center(labelSleep);
    wifiStatusLabel = lv_label_create(scr);
    updateWiFiStatusLabel();
    lv_obj_set_style_text_color(wifiStatusLabel, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_font(wifiStatusLabel, &lv_font_montserrat_24, 0);
    lv_obj_align(wifiStatusLabel, LV_ALIGN_CENTER, 0, 100);
    lv_scr_load(scr);
    lv_obj_add_event_cb(lv_scr_act(), touchEventHandler, LV_EVENT_PRESSED, NULL);
    Serial.println("Reboot screen created and touch event handler initialized");
}

void createCustomScale(lv_obj_t *parent, lv_obj_t **bar, lv_obj_t **label, lv_obj_t **triangle) {
    *bar = lv_bar_create(parent);
    lv_obj_set_size(*bar, lv_pct(90), 10);
    lv_obj_align(*bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(*bar, 0, 100);
    lv_bar_set_value(*bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(*bar, lv_palette_main(LV_PALETTE_YELLOW), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*bar, lv_palette_main(LV_PALETTE_YELLOW), LV_PART_MAIN);
    *triangle = lv_canvas_create(parent);
    static lv_color_t cbuf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(20, 20)];
    lv_canvas_set_buffer(*triangle, cbuf, 20, 20, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(*triangle, lv_color_black(), LV_OPA_COVER);
    lv_point_t points[3] = {{10, 20}, {0, 0}, {20, 0}};
    lv_draw_rect_dsc_t draw_dsc;
    lv_draw_rect_dsc_init(&draw_dsc);
    draw_dsc.bg_color = lv_palette_main(LV_PALETTE_RED);
    lv_canvas_draw_polygon(*triangle, points, 3, &draw_dsc);
    lv_obj_align_to(*triangle, *bar, LV_ALIGN_OUT_TOP_MID, 0, -5);
    *label = lv_label_create(parent);
    lv_label_set_text(*label, "50");
    lv_obj_set_style_text_color(*label, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_font(*label, &lv_font_montserrat_20, 0);
    lv_obj_align_to(*label, *triangle, LV_ALIGN_OUT_TOP_MID, 0, -5);
    IndicatorData *ind_data = (IndicatorData *)malloc(sizeof(IndicatorData));
    ind_data->triangle = *triangle;
    ind_data->label = *label;
    lv_obj_add_event_cb(*bar, [](lv_event_t *e) {
        lv_obj_t *bar = lv_event_get_target(e);
        IndicatorData *ind_data = (IndicatorData *)lv_event_get_user_data(e);
        lv_obj_t *triangle = ind_data->triangle;
        lv_obj_t *label = ind_data->label;
        int value = lv_bar_get_value(bar);
        lv_coord_t bar_width = lv_obj_get_width(bar);
        lv_coord_t canvas_x = (bar_width * value) / 100;
        lv_obj_set_x(triangle, lv_obj_get_x(bar) + canvas_x - 10);
        lv_label_set_text_fmt(label, "%d", value);
        lv_obj_align_to(label, triangle, LV_ALIGN_OUT_TOP_MID, 0, -5);
    }, LV_EVENT_VALUE_CHANGED, ind_data);
    lv_coord_t bar_width = lv_obj_get_width(*bar);
    lv_coord_t bar_x = lv_obj_get_x(*bar);
    for (int i = 0; i <= 100; i += 20) {
        lv_obj_t *markerLine = lv_line_create(parent);
        static lv_point_t line_points[] = {{0, 0}, {0, 10}};
        lv_line_set_points(markerLine, line_points, 2);
        lv_obj_set_style_line_color(markerLine, lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_obj_set_style_line_width(markerLine, 2, 0);
        lv_coord_t marker_x = bar_x + (bar_width * i / 100);
        lv_obj_set_pos(markerLine, marker_x - 1, lv_obj_get_y(*bar) + 15);
        lv_obj_t *markerLabel = lv_label_create(parent);
        lv_label_set_text_fmt(markerLabel, "%d", i);
        lv_obj_set_pos(markerLabel, marker_x - 5, lv_obj_get_y(*bar) + 30);
        lv_obj_set_style_text_color(markerLabel, lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_obj_set_style_text_font(markerLabel, &lv_font_montserrat_16, 0);
    }
}

void createStatsScreen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_user_data(scr, (void *)2);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 10, 0);
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(scr, col_dsc, row_dsc);
    lv_obj_t *topScaleContainer = lv_obj_create(scr);
    lv_obj_set_grid_cell(topScaleContainer, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_bg_color(topScaleContainer, lv_color_black(), LV_PART_MAIN);
    createCustomScale(topScaleContainer, &topBar, &topBarLabel, &topTriangle);
    lv_obj_t *midContainer = lv_obj_create(scr);
    lv_obj_set_grid_cell(midContainer, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_style_bg_color(midContainer, lv_color_black(), 0);
    static lv_coord_t midCol_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t midRow_dsc[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(midContainer, midCol_dsc, midRow_dsc);
    sensor2Label = lv_label_create(midContainer);
    lv_obj_set_style_text_color(sensor2Label, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_font(sensor2Label, &lv_font_montserrat_28, 0);
    lv_label_set_text(sensor2Label, "Sensor 2");
    lv_obj_set_grid_cell(sensor2Label, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    sensor3Label = lv_label_create(midContainer);
    lv_obj_set_style_text_color(sensor3Label, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_font(sensor3Label, &lv_font_montserrat_28, 0);
    lv_label_set_text(sensor3Label, "Sensor 3");
    lv_obj_set_grid_cell(sensor3Label, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_t *bottomScaleContainer = lv_obj_create(scr);
    lv_obj_set_grid_cell(bottomScaleContainer, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_set_style_bg_color(bottomScaleContainer, lv_color_black(), LV_PART_MAIN);
    createCustomScale(bottomScaleContainer, &bottomBar, &bottomBarLabel, &bottomTriangle);
    lv_scr_load(scr);
    lv_obj_add_event_cb(scr, touchEventHandler, LV_EVENT_PRESSED, NULL);
    Serial.println("Created stats screen with padding");
    printDebugInfo();
}

void switchToStatsScreen() {
    lv_obj_t *currentScreen = lv_scr_act();
    if (lv_obj_get_user_data(currentScreen) != (void *)2) {
        Serial.println("Switching to stats screen...");
        createStatsScreen();
    }
}

void updateStatsScreen(const JsonObject& obj) {
    Serial.println("updateStatsScreen called");
    if (obj.isNull()) {
        Serial.println("JsonObject is null");
        return;
    }
    if (obj.containsKey("metadata")) {
        JsonObject metadata = obj["metadata"];
        if (metadata.containsKey("CustomMetadata")) {
            JsonObject customMetadata = metadata["CustomMetadata"];
            payloadMetadataColumns = customMetadata["Cols"] | payloadMetadataColumns;
            payloadMetadataRows = customMetadata["Rows"] | payloadMetadataRows;
            numSensors = payloadMetadataColumns * payloadMetadataRows;
        }
    }
    int statIndex = 0;
    int sensorValues[4] = {0};
    String sensorLabels[4];
    String sensorUnits[4];
    if (obj.containsKey("sensors")) {
        JsonObject sensors = obj["sensors"];
        for (JsonPair kv : sensors) {
            if (statIndex >= numSensors) break;
            const char* label = kv.key().c_str();
            JsonArray sensorDataArray = kv.value().as<JsonArray>();
            if (sensorDataArray.size() == 0 || !sensorDataArray[0].is<JsonObject>()) {
                Serial.println("Expected non-empty JsonArray of JsonObject");
                continue;
            }
            JsonObject sensorData = sensorDataArray[0].as<JsonObject>();
            String value = sensorData["Value"].as<String>();
            String unit = sensorData["Unit"].as<String>();
            Serial.print("Updating sensor: ");
            Serial.print(statIndex + 1);
            Serial.print(" = ");
            Serial.println(value);
            if (value != "Sensor Lost") {
                sensorValues[statIndex] = value.toInt();
                sensorLabels[statIndex] = label;
                sensorUnits[statIndex] = unit;
            } else {
                sensorValues[statIndex] = 0;
                sensorLabels[statIndex] = label;
                sensorUnits[statIndex] = unit;
            }
            statIndex++;
        }
    }
    if (topBar != nullptr) {
        lv_bar_set_value(topBar, sensorValues[0], LV_ANIM_OFF);
        lv_label_set_text_fmt(topBarLabel, "%d", sensorValues[0]);
        lv_event_send(topBar, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (bottomBar != nullptr) {
        lv_bar_set_value(bottomBar, sensorValues[3], LV_ANIM_OFF);
        lv_label_set_text_fmt(bottomBarLabel, "%d", sensorValues[3]);
        lv_event_send(bottomBar, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (sensor2Label != nullptr && !sensorLabels[1].isEmpty()) {
        lv_label_set_text_fmt(sensor2Label, "%s: %d %s", sensorLabels[1].c_str(), sensorValues[1], sensorUnits[1].c_str());
    }
    if (sensor3Label != nullptr && !sensorLabels[2].isEmpty()) {
        lv_label_set_text_fmt(sensor3Label, "%s: %d %s", sensorLabels[2].c_str(), sensorValues[2], sensorUnits[2].c_str());
    }
    lv_refr_now(NULL);
    Serial.println("Updated stats screen");
    printDebugInfo();
}

void updateWiFiStatusLabel() {
    if (wifiConnected) {
        String ipAddress = WiFi.localIP().toString();
        String statusText = "Status: Connected to WiFi (" + ipAddress + ")";
        lv_label_set_text(wifiStatusLabel, statusText.c_str());
    } else {
        lv_label_set_text(wifiStatusLabel, "Status: Unable to connect to WiFi");
    }
    Serial.println("Updated WiFi status label");
    printDebugInfo();
}

void touchEventHandler(lv_event_t *e) {
    Serial.println("Touch detected");
    if (screenSleep) {
        Serial.println("Waking up from sleep");
        wakeUpScreen();
        lastActivity = millis();
        Serial.println("Screen touched and woke up");
        printDebugInfo();
    } else {
        Serial.println("Screen touched");
        printDebugInfo();
    }
}

void settingsEventHandler(lv_event_t *e) {
    Serial.println("Settings button clicked");
}

void sleepCountdownEventHandler(lv_event_t *e) {
    Serial.println("Sleep button clicked");
}

void goToSleep() {
    screenSleep = true;
    amoled.setBrightness(1);
    Serial.println("Brightness set to 1%");
    lv_label_set_text(labelSleep, "Sleep");
    Serial.println("Entering sleep mode");
    printDebugInfo();
}

void wakeUpScreen() {
    screenSleep = false;
    screenWakingUp = true;
    amoled.setBrightness(brightness);
    Serial.print("Brightness restored to: ");
    Serial.println(brightness);
    delay(1000);
    screenWakingUp = false;
    lv_obj_add_event_cb(lv_scr_act(), touchEventHandler, LV_EVENT_PRESSED, NULL);
    Serial.println("Waking up from sleep");
    printDebugInfo();
}

void connectToWiFi() {
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
        lv_label_set_text(wifiStatusLabel, "Connecting to WiFi...");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Connected to WiFi");
        wifiConnected = true;
        String ipAddress = WiFi.localIP().toString();
        String statusText = "Status: Connected to WiFi (" + ipAddress + ")";
        lv_label_set_text(wifiStatusLabel, statusText.c_str());
    } else {
        Serial.println("Failed to connect to WiFi");
        wifiConnected = false;
        lv_label_set_text(wifiStatusLabel, "Failed to connect to WiFi");
    }
    Serial.println("WiFi connection status updated");
    printDebugInfo();
}

void printDebugInfo() {
    Serial.print("Debug info: ");
    Serial.print("autoSleepEnabled=");
    Serial.print(autoSleepEnabled);
    Serial.print(", screenSleep=");
    Serial.print(screenSleep);
    Serial.print(", settingsScreenActive=");
    Serial.print(settingsScreenActive);
    Serial.print(", screenWakingUp=");
    Serial.println(screenWakingUp);
}
