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

int payloadMetadataColumns = 2; // Default number of columns
int payloadMetadataRows = 2;    // Default number of rows
int numSensors = payloadMetadataColumns * payloadMetadataRows; // Default number of sensors

// WiFi credentials
const char* ssid = "ssid";
const char* password = "password";

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Global jsonBuffer
String jsonBuffer = "";
int payloadLength = 0;
bool readingLength = true;
int bytesRead = 0;

void setup() {
    Serial.begin(9600);

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

    // Load saved settings
    brightness = preferences.getInt("brightness", 100);
    orientation = preferences.getInt("orientation", 0);
    autoSleepEnabled = preferences.getBool("autoSleepEnabled", true); // Default to ON if no value is saved yet
    amoled.setRotation(orientation / 90);
    amoled.setBrightness(brightness);

    // Show CatapultCase screen with WiFi connection status
    createRebootScreen();

    // Add touch event handler
    lv_obj_add_event_cb(lv_scr_act(), touchEventHandler, LV_EVENT_PRESSED, NULL);

    // Connect to WiFi
    connectToWiFi();

    // Initialize server
    server.on("/data", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Data received");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        handleIncomingDataChunk(data, len);
    });
    server.begin();

    // Initialize lastActivity
    lastActivity = millis();
}

void loop() {
    lv_task_handler(); // Handle LVGL tasks
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
        return; // Do not proceed with the rest of the loop if the screen is in sleep mode
    }

    if (settingsScreenActive || screenWakingUp) {
        return; // Do not proceed with the rest of the loop if settings screen is active or screen is waking up
    }

    handleSerialData();

    if (!serialActive) {
        // If serial connection is not active, display initial text and button
        lv_obj_t *screen = lv_scr_act();
        if (lv_obj_get_user_data(screen) != (void *)1) {
            createRebootScreen();
        }

        delay(100); // Delay for screen refresh
    }
}

void handleSerialData() {
    while (Serial.available() > 0) {
        char incomingByte = Serial.read();
        handleIncomingDataChunk((uint8_t*)&incomingByte, 1);
    }
}

void handleIncomingDataChunk(uint8_t *data, size_t len) {
    
    // Reset the inactivity timer since data is being received
    lastActivity = millis();
    
    if (data == nullptr || len == 0) {
        Serial.println("Error: Incoming data is null or empty.");
        return;
    }

    if (readingLength) {
        // Check the first byte of the incoming data
        char firstByte = data[0];
        if (firstByte == '{') {
            // If the data starts with '{', it indicates that there's no prefix length
            Serial.println("Data without prefix length detected. Responding 'ok'.");
            jsonBuffer = ""; // Clear buffer
            readingLength = true; // Stay in reading length mode
            bytesRead = 0; // Reset bytes read
            return;
        }

        // If the first byte is a number, assume the data includes a length prefix
        if (isdigit(firstByte)) {
            jsonBuffer += String((char*)data).substring(0, len);
            bytesRead += len;

            if (jsonBuffer.length() >= 8) {
                payloadLength = jsonBuffer.substring(0, 8).toInt(); // Convert the length prefix to an integer
                Serial.print("Length Prefix Detected: ");
                Serial.println(payloadLength);
                jsonBuffer = jsonBuffer.substring(8); // Remove the length prefix from the buffer
                readingLength = false; // Switch to reading the payload
            }
        }
    } else {
        // Append the data chunk to the buffer
        jsonBuffer += String((char*)data).substring(0, len);
        bytesRead += len;

        if (bytesRead >= payloadLength) {
            Serial.println("Processed Inbound Data");

            // Process the JSON data
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, jsonBuffer);

            if (error) {
                Serial.print("deserializeJson() failed: ");
                Serial.println(error.c_str());
            } else {
                // Successfully processed JSON data
                Serial.println("JSON data processed successfully.");
                fullPayload = "";
                serializeJsonPretty(doc, fullPayload);

                serialActive = true;
                settingsScreenActive = false; // Ensure settings menu is overridden
                screenSleep = false; // Ensure sleep mode is overridden

                // Handle metadata if present
                if (doc.containsKey("metadata")) {
                    JsonObject metadata = doc["metadata"];
                    payloadMetadataColumns = metadata["PayloadMetadataColumns"] | payloadMetadataColumns;
                    payloadMetadataRows = metadata["PayloadMetadataRows"] | payloadMetadataRows;
                    numSensors = payloadMetadataColumns * payloadMetadataRows;
                }

                Serial.println("Switching to stats screen...");
                switchToStatsScreen(); // Switch to the stats screen

                Serial.println("Updating stats screen...");
                updateStatsScreen(doc.as<JsonObject>()); // Update the screen with the new data

                // Delay for screen refresh
                delay(500);
            }

            // Clear buffer and reset states for next message
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
    lv_obj_set_style_text_color(label, lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -60);

    // Settings button
    lv_obj_t *btnSettings = lv_btn_create(scr);
    lv_obj_set_size(btnSettings, 160, 60); // Reduced height to fit WiFi status
    lv_obj_align(btnSettings, LV_ALIGN_CENTER, -100, 20);
    lv_obj_set_style_bg_color(btnSettings, lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
    lv_obj_set_style_text_color(btnSettings, lv_color_make(0x00, 0x00, 0x00), 0); // Black color
    lv_obj_add_event_cb(btnSettings, settingsEventHandler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *labelSettings = lv_label_create(btnSettings);
    lv_label_set_text(labelSettings, "Settings");
    lv_obj_set_style_text_font(labelSettings, &lv_font_montserrat_24, 0); // Larger font
    lv_obj_set_style_text_color(labelSettings, lv_color_make(0x00, 0x00, 0x00), 0); // Black color
    lv_obj_center(labelSettings);

    // Sleep button with countdown
    lv_obj_t *btnSleep = lv_btn_create(scr);
    lv_obj_set_size(btnSleep, 160, 60); // Reduced height to fit WiFi status
    lv_obj_align(btnSleep, LV_ALIGN_CENTER, 100, 20);
    lv_obj_set_style_bg_color(btnSleep, lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
    lv_obj_set_style_text_color(btnSleep, lv_color_make(0x00, 0x00, 0x00), 0); // Black color
    lv_obj_add_event_cb(btnSleep, sleepCountdownEventHandler, LV_EVENT_CLICKED, NULL);

    labelSleep = lv_label_create(btnSleep);
    lv_label_set_text(labelSleep, "Sleep");
    lv_obj_set_style_text_font(labelSleep, &lv_font_montserrat_24, 0); // Larger font
    lv_obj_set_style_text_color(labelSleep, lv_color_make(0x00, 0x00, 0x00), 0); // Black color
    lv_obj_center(labelSleep);

    // WiFi status label
    wifiStatusLabel = lv_label_create(scr);
    updateWiFiStatusLabel();
    lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
    lv_obj_set_style_text_font(wifiStatusLabel, &lv_font_montserrat_24, 0);
    lv_obj_align(wifiStatusLabel, LV_ALIGN_CENTER, 0, 100);

    lv_scr_load(scr);

    // Re-initialize touch event handler
    lv_obj_add_event_cb(lv_scr_act(), touchEventHandler, LV_EVENT_PRESSED, NULL);
    Serial.println("Reboot screen created and touch event handler initialized");
}

void createSettingsScreen() {
    settingsScreenActive = true; // Set the settings screen active flag
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_user_data(scr, (void *)3);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    // Create a grid of 2 columns and 5 rows, merge bottom row for WiFi status
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(scr, col_dsc, row_dsc);

    // Create buttons
    createButton(scr, "Orientation", 0, 0, orientationEventHandler);
    createButton(scr, autoSleepEnabled ? "Auto-Sleep: On" : "Auto-Sleep: Off", 1, 0, autoSleepEventHandler);
    createButton(scr, "Brightness +", 0, 1, brightnessUpEventHandler);
    createButton(scr, "Brightness -", 1, 1, brightnessDownEventHandler);

    // Placeholder buttons
    createButton(scr, "Placeholder 1", 0, 2, NULL);
    createButton(scr, "Placeholder 2", 1, 2, NULL);
    createButton(scr, "Placeholder 3", 0, 3, NULL);
    createButton(scr, "Return", 1, 3, returnEventHandler);

    // Add WiFi status label
    statusLabel = lv_label_create(scr);
    lv_obj_set_grid_cell(statusLabel, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 4, 1); // Merge bottom row
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(statusLabel, lv_color_make(0xFF, 0xFF, 0x00), 0);

    updateWiFiStatusLabel();

    lv_scr_load(scr);

    // Add touch event handler
    lv_obj_add_event_cb(scr, touchEventHandler, LV_EVENT_PRESSED, NULL);

    // Debug print
    Serial.println("Entered settings screen");
    printDebugInfo();
}

void createButton(lv_obj_t *parent, const char *text, int col, int row, lv_event_cb_t event_cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 240, 60); // Reduced height to fit WiFi status
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_CENTER, col, 1, LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_set_style_bg_color(btn, lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
    lv_obj_set_style_text_color(btn, lv_color_make(0x00, 0x00, 0x00), 0); // Black color
    if (event_cb != NULL) {
        lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0); // Larger font
    lv_obj_set_style_text_color(label, lv_color_make(0x00, 0x00, 0x00), 0); // Black color
    lv_obj_center(label);
}

void orientationEventHandler(lv_event_t *e) {
    if (screenWakingUp || screenSleep) return; // Do not handle event if screen is waking up or in sleep mode
    Serial.println("Orientation button pressed");
    orientation += 180; // Rotate by 180 degrees
    if (orientation >= 360) orientation = 0;
    amoled.setRotation(orientation / 90);
    preferences.putInt("orientation", orientation); // Save orientation
    lastActivity = millis(); // Reset the inactivity timer
    createSettingsScreen(); // Re-create the settings screen to adjust to new orientation

    // Debug print
    Serial.println("Orientation changed");
    printDebugInfo();
}

void sleepCountdownEventHandler(lv_event_t *e) {
    if (screenWakingUp || screenSleep) return; // Do not handle event if screen is waking up or in sleep mode
    Serial.println("Sleep button pressed, putting screen to sleep immediately");

    goToSleep();

    // Debug print
    Serial.println("Sleep initiated");
    printDebugInfo();
}

void autoSleepEventHandler(lv_event_t *e) {
    if (screenSleep) return; // Do not handle event if in sleep mode
    autoSleepEnabled = !autoSleepEnabled;
    preferences.putBool("autoSleepEnabled", autoSleepEnabled); // Save auto-sleep state to preferences
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    lv_label_set_text(label, autoSleepEnabled ? "Auto-Sleep: On" : "Auto-Sleep: Off");
    Serial.println(autoSleepEnabled ? "Auto-Sleep enabled" : "Auto-Sleep disabled");
    lastActivity = millis(); // Reset the inactivity timer

    // Debug print
    Serial.println("Auto-sleep toggled");
    printDebugInfo();
}

void brightnessUpEventHandler(lv_event_t *e) {
    if (screenWakingUp || screenSleep) return; // Do not handle event if screen is waking up or in sleep mode
    Serial.println("Brightness Up button pressed");
    brightness = min(brightness + 10, 100);
    amoled.setBrightness(brightness);
    preferences.putInt("brightness", brightness); // Save brightness
    Serial.print("Brightness increased to: ");
    Serial.println(brightness);
    lastActivity = millis(); // Reset the inactivity timer

    // Debug print
    Serial.println("Brightness increased");
    printDebugInfo();
}

void brightnessDownEventHandler(lv_event_t *e) {
    if (screenWakingUp || screenSleep) return; // Do not handle event if screen is waking up or in sleep mode
    Serial.println("Brightness Down button pressed");
    brightness = max(brightness - 10, 0);
    amoled.setBrightness(brightness);
    preferences.putInt("brightness", brightness); // Save brightness
    Serial.print("Brightness decreased to: ");
    Serial.println(brightness);
    lastActivity = millis(); // Reset the inactivity timer

    // Debug print
    Serial.println("Brightness decreased");
    printDebugInfo();
}

void returnEventHandler(lv_event_t *e) {
    if (screenWakingUp || screenSleep) return; // Do not handle event if screen is waking up or in sleep mode
    Serial.println("Return button pressed");
    settingsScreenActive = false; // Clear the settings screen active flag
    createRebootScreen();
    lastActivity = millis(); // Reset the inactivity timer

    // Debug print
    Serial.println("Exited settings screen");
    printDebugInfo();
}

void settingsEventHandler(lv_event_t *e) {
    if (screenWakingUp || screenSleep) return; // Do not handle event if screen is waking up or in sleep mode
    Serial.println("Settings button pressed");
    createSettingsScreen();
    lastActivity = millis(); // Reset the inactivity timer

    // Debug print
    Serial.println("Entered settings screen");
    printDebugInfo();
}

void createStatsScreen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_user_data(scr, (void *)2);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    int cellWidth = WIDTH / payloadMetadataColumns;
    int cellHeight = HEIGHT / payloadMetadataRows;

    labelNames = new lv_obj_t*[numSensors];
    labelValues = new lv_obj_t*[numSensors];

    for (int i = 0; i < numSensors; i++) {
        int row = i / payloadMetadataColumns;
        int col = i % payloadMetadataColumns;

        lv_obj_t *container = lv_obj_create(scr);
        lv_obj_set_size(container, cellWidth, cellHeight);
        lv_obj_set_style_bg_color(container, lv_color_black(), 0);
        lv_obj_set_style_border_width(container, 0, 0); // Remove border
        lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE); // Remove the scrollable flag
        lv_obj_align(container, LV_ALIGN_TOP_LEFT, col * cellWidth, row * cellHeight);

        labelNames[i] = lv_label_create(container);
        if (labelNames[i] == NULL) {
            Serial.print("Failed to create labelNames[");
            Serial.print(i);
            Serial.println("]");
        } else {
            lv_obj_set_style_text_color(labelNames[i], lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
            lv_obj_set_style_text_font(labelNames[i], &lv_font_montserrat_28, 0); // Larger font
            lv_obj_align(labelNames[i], LV_ALIGN_TOP_MID, 0, 20);
        }

        labelValues[i] = lv_label_create(container);
        if (labelValues[i] == NULL) {
            Serial.print("Failed to create labelValues[");
            Serial.print(i);
            Serial.println("]");
        } else {
            lv_obj_set_style_text_color(labelValues[i], lv_color_make(0xFF, 0xFF, 0x00), 0); // Yellow color
            lv_obj_set_style_text_font(labelValues[i], &lv_font_montserrat_40, 0); // Larger font
            lv_obj_align(labelValues[i], LV_ALIGN_CENTER, 0, 20);
        }
    }

    lv_scr_load(scr);

    // Add touch event handler
    lv_obj_add_event_cb(scr, touchEventHandler, LV_EVENT_PRESSED, NULL);

    // Debug print
    Serial.println("Created stats screen");
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

    for (int i = 0; i < numSensors; i++) {
        if (labelNames[i] == nullptr) {
            Serial.print("labelNames[");
            Serial.print(i);
            Serial.println("] is not initialized.");
        }

        if (labelValues[i] == nullptr) {
            Serial.print("labelValues[");
            Serial.print(i);
            Serial.println("] is not initialized.");
        }
    }

    int statIndex = 0;
    if (obj.containsKey("sensors")) {
        JsonObject sensors = obj["sensors"];
        for (JsonPair kv : sensors) {
            if (statIndex >= numSensors) break;

            String sensorTag = kv.key().c_str();
            Serial.print("SensorTag: ");
            Serial.println(sensorTag);

            if (!kv.value().is<JsonArray>()) {
                Serial.println("Expected JsonArray, but found different type");
                continue;
            }

            JsonArray sensorDataArray = kv.value().as<JsonArray>();
            if (sensorDataArray.size() == 0 || !sensorDataArray[0].is<JsonObject>()) {
                Serial.println("Expected non-empty JsonArray of JsonObject");
                continue;
            }

            JsonObject sensorData = sensorDataArray[0].as<JsonObject>();
            String unit = sensorData["Unit"].as<String>();
            String value = sensorData["Value"].as<String>();

            Serial.print("Updating sensor: ");
            Serial.print(sensorTag);
            Serial.print(" = ");
            Serial.print(value);
            Serial.print(" ");
            Serial.println(unit);

            if (labelNames[statIndex] == nullptr || labelValues[statIndex] == nullptr) {
                Serial.print("labelNames[");
                Serial.print(statIndex);
                Serial.print("] or labelValues[");
                Serial.print(statIndex);
                Serial.println("] is null.");
                continue;
            }

            lv_label_set_text(labelNames[statIndex], sensorTag.c_str());
            lv_label_set_text(labelValues[statIndex], (value + " " + unit).c_str());

            // Invalidate each label to refresh its content
            lv_obj_invalidate(labelNames[statIndex]);
            lv_obj_invalidate(labelValues[statIndex]);

            statIndex++;
        }
    }

    // Force an immediate refresh of the screen
    lv_refr_now(NULL);

    // Debug print
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

    // Debug print
    Serial.println("Updated WiFi status label");
    printDebugInfo();
}

// Touch event handler
void touchEventHandler(lv_event_t *e) {
    Serial.println("Touch detected");

    if (screenSleep) {
        Serial.println("Waking up from sleep");
        wakeUpScreen();
        lastActivity = millis(); // Reset the inactivity timer

        // Debug print
        Serial.println("Screen touched and woke up");
        printDebugInfo();
    } else {
        // Debug print
        Serial.println("Screen touched");
        printDebugInfo();
    }
}

void goToSleep() {
    screenSleep = true;
    amoled.setBrightness(1); // Set brightness to 1%
    Serial.println("Brightness set to 1%");
    lv_label_set_text(labelSleep, "Sleep"); // Reset the label text

    // Debug print
    Serial.println("Entering sleep mode");
    printDebugInfo();
}

void wakeUpScreen() {
    screenSleep = false;
    screenWakingUp = true;
    amoled.setBrightness(brightness); // Restore brightness
    Serial.print("Brightness restored to: ");
    Serial.println(brightness);
    delay(1000); // Delay to ensure wake-up is complete
    screenWakingUp = false;

    // Re-initialize touch event handler
    lv_obj_add_event_cb(lv_scr_act(), touchEventHandler, LV_EVENT_PRESSED, NULL);

    // Debug print
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

    // Debug print
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
