#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>

// WiFi network credentials
const char* ssid = "HRM-ESP32";
const char* password = "12345678";

// Polar H10 Device Configuration
const char* POLAR_H10_MAC = "a0:9e:1a:e4:c5:6b";  // Your specific Polar H10 MAC address
const char* POLAR_CUSTOM_UUID = "CAF6D558-83A4-AD1F-4892-B5C143F3DA3F"; // Custom Polar UUID

// BLE variables
static NimBLEAdvertisedDevice* polarH10Device = nullptr;
static NimBLEClient* pClient = nullptr;
static NimBLERemoteService* pService = nullptr;
static NimBLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static bool deviceConnected = false;
static bool scanActive = true;

// Heart rate variables
static int currentHeartRate = 0;
static unsigned long lastNotificationTime = 0;
static const char* connectionStatus = "Scanning...";

// Web server on port 80
WebServer server(80);

// Add these declarations at the top of your file (with your other static variables)
static NimBLEScan* pBLEScan = nullptr;
static uint32_t scanTime = 5; // Scan time in seconds

// Forward declarations
void startScan();
bool connectToDevice();

// ADD THIS LINE - Forward declare the class before using it
class MyScanCallback;  // <-- Add this forward declaration

// Global scan callback instance (reuse, don't recreate)
static MyScanCallback* scanCallback = nullptr;

// Scan for BLE devices callback
class MyScanCallback : public NimBLEScanCallbacks {
    // Add the onDiscovered callback (quick first pass)
    void onDiscovered(const NimBLEAdvertisedDevice* advertisedDevice) override {
        // Quick check for Polar devices by MAC or name only
        std::string deviceAddress = advertisedDevice->getAddress().toString();
        
        // Fast path for your specific Polar H10
        if (deviceAddress.find("a0:9e:1a:e4:c5:6b") != std::string::npos) {
            Serial.println("**** FOUND POLAR H10 BY MAC (DISCOVERY)! ****");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            
            // Add a brief delay before stopping scan to let any pending advertise data complete
            Serial.println("Preparing for connection...");
            delay(500);
            
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }
        
        // You can add other quick checks here
    }

    // Keep your existing onResult for detailed scanning
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        // Print device details for debugging
        Serial.print("Device found: Address: ");
        Serial.print(advertisedDevice->getAddress().toString().c_str());
        
        if (advertisedDevice->haveName()) {
            Serial.print(" | Name: '");
            Serial.print(advertisedDevice->getName().c_str());
            Serial.print("'");
        }
        
        Serial.print(" | RSSI: ");
        Serial.print(advertisedDevice->getRSSI());
        
        // Print service UUIDs if available
        if (advertisedDevice->haveServiceUUID()) {
            Serial.print(" | Services: ");
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++) {
                Serial.print(advertisedDevice->getServiceUUID(i).toString().c_str());
                Serial.print(" ");
            }
        }
        
        Serial.println();
        
        // Direct check for your exact Polar H10 by address string
        std::string deviceAddress = advertisedDevice->getAddress().toString();
        if (deviceAddress.find("a0:9e:1a:e4:c5:6b") != std::string::npos) {
            Serial.println("**** FOUND POLAR H10 BY HARDCODED MAC! ****");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }

        // Add debugging to see what's being compared
        Serial.print("MAC Address comparison: '");
        Serial.print(deviceAddress.c_str());
        Serial.print("' vs '");
        Serial.print(POLAR_H10_MAC);
        Serial.println("'");

        // Robust comparison - use both direct equality and string search
        if (deviceAddress == POLAR_H10_MAC || 
            deviceAddress.find(POLAR_H10_MAC) != std::string::npos) {
            Serial.println("*** FOUND YOUR EXACT POLAR H10 DEVICE BY MAC! ***");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }
        
        // Check for Heart Rate service
        if (advertisedDevice->haveServiceUUID()) {
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++) {
                std::string serviceUUID = advertisedDevice->getServiceUUID(i).toString();
                Serial.print("Checking service UUID: ");
                Serial.println(serviceUUID.c_str());
                
                // Heart Rate Service check (case insensitive)
                if (serviceUUID == "180d" || serviceUUID == "180D" || 
                    serviceUUID == "0000180d-0000-1000-8000-00805f9b34fb") {
                    Serial.println("*** FOUND HEART RATE DEVICE! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }
        
        // Check by Polar manufacturer data
        if (advertisedDevice->haveManufacturerData()) {
            std::string manufData = advertisedDevice->getManufacturerData();
            if (manufData.length() >= 2) {
                uint16_t companyID = (uint8_t)manufData[0] | ((uint8_t)manufData[1] << 8);
                Serial.print("Manufacturer ID: ");
                Serial.println(companyID);
                
                if (companyID == 107) { // Polar's company ID
                    Serial.println("*** FOUND POLAR DEVICE BY MANUFACTURER! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }
        
        // All other checks can remain as they are...
        
        // AGGRESSIVE DETECTION APPROACH:
        
        // 1. Check by name (case insensitive)
        if (advertisedDevice->haveName()) {
            std::string deviceName = advertisedDevice->getName();
            std::string lowerName = deviceName;
            // Convert to lowercase for better matching
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            
            // Check for your specific device by any name match
            if (lowerName.find("polar") != std::string::npos || 
                lowerName.find("h10") != std::string::npos) {
                Serial.print("*** FOUND POLAR DEVICE: ");
                Serial.print(deviceName.c_str());
                Serial.println(" ***");
                polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                NimBLEDevice::getScan()->stop();
                scanActive = false;
                return;
            }
        }
        
        // 2. Check by service UUID (Heart Rate Service)
        if (advertisedDevice->haveServiceUUID()) {
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++) {
                std::string serviceUUID = advertisedDevice->getServiceUUID(i).toString();
                Serial.print("Checking UUID: ");
                Serial.println(serviceUUID.c_str());
                
                // Make the check more flexible - case insensitive, partial match
                std::string lowerUUID = serviceUUID;
                std::transform(lowerUUID.begin(), lowerUUID.end(), lowerUUID.begin(), ::tolower);
                
                // Heart Rate Service UUID is 180D/180d
                if (lowerUUID.find("180d") != std::string::npos) {
                    Serial.println("*** FOUND HEART RATE DEVICE! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }
        
        // 3. If it's a Polar device by manufacturer ID
        if (advertisedDevice->haveManufacturerData()) {
            std::string manufData = advertisedDevice->getManufacturerData();
            // Polar's company ID is 0x006B (107 decimal)
            if (manufData.length() >= 2) {
                uint16_t companyID = (uint8_t)manufData[0] | ((uint8_t)manufData[1] << 8);
                if (companyID == 107) { // Polar company ID
                    Serial.println("*** FOUND POLAR DEVICE BY MANUFACTURER DATA! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }

        // 4. Check for your specific Polar H10 by MAC address
        // Don't redeclare deviceAddress, just reuse it
        if (deviceAddress.find("e4:c5:6b") != std::string::npos) {
            Serial.println("*** FOUND A DEVICE WITH YOUR POLAR H10 ID IN THE MAC! ***");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }

        // 5. NEW CODE: Check for your specific Polar custom service UUID
        if (advertisedDevice->haveServiceUUID()) {
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++) {
                std::string serviceUUID = advertisedDevice->getServiceUUID(i).toString();
                
                // Check for the custom Polar H10 UUID you found on your iPhone
                if (serviceUUID.find(POLAR_CUSTOM_UUID) != std::string::npos) {
                    Serial.println("*** FOUND YOUR SPECIFIC POLAR H10 BY CUSTOM UUID! ***");
                    Serial.print("Device name: ");
                    if (advertisedDevice->haveName()) {
                        Serial.println(advertisedDevice->getName().c_str());
                    } else {
                        Serial.println("(No name)");
                    }
                    
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }

        // 6. Last resort - connect to ANY BLE device nearby with strong signal
        // This can be commented out if you find it connects to the wrong devices
        if (advertisedDevice->getRSSI() > -50) {  // Only very close devices with strong signal
            Serial.println("*** FOUND NEARBY DEVICE WITH STRONG SIGNAL - ATTEMPTING CONNECTION ***");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }
    }
    
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        static int scanAttempts = 0;
        
        Serial.print("Scan ended. Reason: ");
        Serial.print(reason);
        Serial.print(", Found ");
        Serial.print(results.getCount());
        Serial.println(" devices");
        
        // If we found our device, don't restart scan
        if (polarH10Device != nullptr) {
            Serial.println("Device found - proceeding to connection phase");
            scanActive = false;
            return;
        }
        
        // Track attempts for debugging/logging
        scanAttempts++;
        Serial.print("No Polar H10 found yet. Scan cycle #");
        Serial.print(scanAttempts);
        Serial.println(" - continuing scan");
        
        // Every 5 scan cycles, try different parameters
        if (scanAttempts % 5 == 0) {
            Serial.println("Changing scan parameters for better detection...");
            pBLEScan->setInterval(scanAttempts % 10 == 0 ? 160 : 60);
            pBLEScan->setWindow(scanAttempts % 10 == 0 ? 150 : 55);
        }
        
        // The scan will automatically restart thanks to the third parameter (true)
        // in the start() call in startScan()
    }
};

// Client connection callbacks
class MyClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pclient) {
        deviceConnected = true;
        connectionStatus = "Connected";
        Serial.println("Connected to Polar H10");
    }

    void onDisconnect(NimBLEClient* pclient, int reason) {
        deviceConnected = false;
        connectionStatus = "Disconnected";
        Serial.print("Disconnected, reason: ");
        Serial.println(reason);
        
        // Clean up properly
        if (pClient != nullptr) {
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
        }
        pService = nullptr;
        pRemoteCharacteristic = nullptr;
        
        // Delete the stored device reference
        if (polarH10Device != nullptr) {
            delete polarH10Device;
            polarH10Device = nullptr;
        }
        
        delay(2000);
        startScan();
    }
};

// Handle notification callbacks with a function (not a class)
void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic, 
                    uint8_t* pData, size_t length, bool isNotify) {
    if (length > 0) {
        // First byte contains flags
        uint8_t flags = pData[0];
        
        // Check format - 2nd byte contains HR for most common format
        int hr;
        if (flags & 0x01) {  // Check if value format is UINT16
            // 16-bit format - bytes 1 & 2
            hr = pData[1] | (pData[2] << 8);
        } else {
            // 8-bit format - 2nd byte only
            hr = pData[1];
        }
        
        // Update global value and timestamp
        currentHeartRate = hr;
        lastNotificationTime = millis();
        
        Serial.print("Heart Rate: ");
        Serial.println(currentHeartRate);
    }
}

// Start BLE scan
void startScan() {
    // Clean up any existing connections
    if (pClient != nullptr) {
        if (pClient->isConnected()) {
            pClient->disconnect();
        }
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
    }
    
    // RESET BT STACK BEFORE SCAN - makes first connection more reliable
    NimBLEDevice::deinit(true);
    delay(200);
    NimBLEDevice::init("ESP32-HR-Monitor");
    
    // Clean up device reference
    if (polarH10Device != nullptr) {
        delete polarH10Device;
        polarH10Device = nullptr;
    }
    
    // Reset states
    deviceConnected = false;
    scanActive = true;
    connectionStatus = "Scanning...";
    
    Serial.println("\n=== Starting BLE scan ===");
    Serial.println("Looking for Polar H10 device...");
    
    pBLEScan = NimBLEDevice::getScan();
    
    // Create callback only once
    if (scanCallback == nullptr) {
        scanCallback = new MyScanCallback();
    }
    
    pBLEScan->setScanCallbacks(scanCallback);
    pBLEScan->setActiveScan(true); // Active scanning gets more device data
    pBLEScan->setInterval(80);     // Scan more frequently
    pBLEScan->setWindow(60);       // Spend more time scanning in each interval
    pBLEScan->setMaxResults(0);    // Don't store results in memory, use callbacks only
    
    // Start continuous scan for 30 seconds, then restart automatically via onScanEnd
    bool scanResult = pBLEScan->start(30000, false, true); // Restart with cleared cache
    
    if (!scanResult) {
        Serial.println("Failed to start scan! Restarting ESP...");
        delay(3000);
        ESP.restart();
    }
    
    Serial.println("Continuous scan started successfully. Waiting for devices...");
}

// Connect to the Polar H10 
bool connectToDevice() {
    if (polarH10Device == nullptr) {
        Serial.println("No device to connect to");
        return false;
    }
    
    Serial.println("Connecting to Polar H10...");
    connectionStatus = "Connecting...";
    
    // Clear any stale BT connections first
    NimBLEDevice::getServer()->disconnect(0);
    delay(200); // Short delay
    
    // Create client with proper settings
    if (pClient != nullptr) {
        NimBLEDevice::deleteClient(pClient);
    }
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    
    // IMPROVED CONNECTION PARAMETERS FOR POLAR H10
    // Use a much more conservative approach for first connection
    pClient->setConnectionParams(40, 80, 0, 1000);  // More relaxed parameters optimized for Polar
    pClient->setConnectTimeout(15);  // 15s for first attempt
    
    // Add a retry loop with progressive backoff
    bool connected = false;
    int retries = 0;
    int delayMs = 100; // Start with 100ms delay
    
    // Update your connection retry logic with these optimizations
    while (!connected && retries < 5) {
        Serial.print("Connection attempt #");
        Serial.print(retries + 1);
        Serial.print(" with ");
        Serial.print(delayMs);
        Serial.println("ms delay");
        
        // Purge BLE controller state before trying
        if (retries > 0) {
            NimBLEDevice::getServer()->disconnect(0);
            delay(100);
        }
        
        // Pre-emptive delay for Polar H10 (helps with status 13)
        if (retries == 0) {
            Serial.println("Waiting for Polar H10 to be ready to accept connections...");
            delay(1000);
        }
        
        // Try to connect with specific connection timeout based on attempt number
        pClient->setConnectTimeout(retries == 0 ? 15 : 8);
        
        // Try to connect
        if (pClient->connect(polarH10Device)) {
            connected = true;
            break;
        }
        
        // If connection failed
        Serial.println("Connection attempt failed, retrying...");
        
        // Progressive backoff - double the delay each time
        delay(delayMs);
        delayMs *= 2;
        retries++;
        
        // Add a BLE controller reset on the second retry (earlier than before)
        if (retries == 2) {
            Serial.println("Resetting BLE controller");
            NimBLEDevice::deinit(true);
            delay(500);
            NimBLEDevice::init("ESP32-HR-Monitor");
            
            // Recreate client after BLE reset
            pClient = NimBLEDevice::createClient();
            pClient->setClientCallbacks(new MyClientCallback());
            pClient->setConnectionParams(40, 80, 0, 1000);
        }
    }
    
    if (!connected) {
        Serial.println("All connection attempts failed");
        connectionStatus = "Connection Failed";
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        return false;
    }
    
    if (connected) {
        // Connection successful, allow more time to stabilize
        Serial.println("Connected! Allowing connection to stabilize...");
        delay(800);  // Longer stabilization period
        
        // Set encryption to more relaxed mode for Polar devices
        // (some older Polar devices can have issues with strict encryption)
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
        NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);
    }
    
    Serial.println("Connected! Requesting MTU...");
    
    // Request a larger MTU for better throughput
    NimBLEDevice::setMTU(185); // Set global MTU size
    
    // Allow connection to stabilize
    delay(500);
    
    // Continue with service discovery...
    Serial.println("Connected! Discovering services...");
    
    // Wait a moment for connection to stabilize
    delay(1000);
    
    // Get the Heart Rate service
    pService = pClient->getService(NimBLEUUID("180D"));
    if (pService == nullptr) {
        Serial.println("Heart Rate Service not found");
        pClient->disconnect();
        return false;
    }
    
    Serial.println("Heart Rate Service found!");
    
    // Get HR Measurement characteristic
    pRemoteCharacteristic = pService->getCharacteristic(NimBLEUUID("2A37"));
    if (pRemoteCharacteristic == nullptr) {
        Serial.println("Heart Rate Measurement Characteristic not found");
        pClient->disconnect();
        return false;
    }
    
    Serial.println("Heart Rate Characteristic found!");
    
    // Subscribe to notifications
    if (pRemoteCharacteristic->canNotify()) {
        if (!pRemoteCharacteristic->subscribe(true, notifyCallback)) {
            Serial.println("Failed to subscribe to notifications");
            pClient->disconnect();
            return false;
        }
        Serial.println("Successfully subscribed to Heart Rate notifications!");
        deviceConnected = true;
        connectionStatus = "Connected";
        return true;
    } else {
        Serial.println("Characteristic doesn't support notifications");
        pClient->disconnect();
        return false;
    }
}

// HTTP handlers
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'/>"
                  "<style>body{font-family:Arial;text-align:center;padding:20px;background:#f0f0f0;}"
                  "h1{color:#e74c3c;font-size:4em;margin:0;}"
                  ".status{padding:8px;margin:10px;border-radius:4px;font-weight:bold;}"
                  ".scanning{background:#ffeb3b;}.connecting{background:#ff9800;color:white;}"
                  ".connected{background:#4caf50;color:white;}"
                  ".heart{animation:pulse 1s infinite;}@keyframes pulse{0%,100%{transform:scale(1);}50%{transform:scale(1.1);}}"
                  "</style></head><body><h1> ESP32 HR Monitor</h1>";
    
    // Add status
    String statusClass = deviceConnected ? "connected" : (polarH10Device ? "connecting" : "scanning");
    html += "<div class='status " + statusClass + "'>" + String(connectionStatus) + "</div>";
    
    // Add heart rate
    if (deviceConnected && currentHeartRate > 0) {
        html += "<div style='font-size:3em;margin:20px;'><span class='heart'>❤</span> " + String(currentHeartRate) + " BPM</div>";
    } else {
        html += "<div style='font-size:3em;margin:20px;'>-- BPM</div>";
    }
    
    html += "<p><a href='/reset' style='display:inline-block; background:#ff9800; color:white; padding:10px 20px; text-decoration:none; border-radius:4px; margin-top:20px;'>Reset Connection</a></p>";
    
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleAPI() {
    String json = "{\"hr\":" + String(currentHeartRate) + 
                  ",\"status\":\"" + String(connectionStatus) + 
                  "\",\"connected\":" + String(deviceConnected ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 HR Monitor Start");
    
    // Initialize BLE
    NimBLEDevice::init("ESP32-HR-Monitor");
    
    // Setup WiFi in AP mode
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    
    // Setup web server
    server.on("/", handleRoot);
    server.on("/api/heartrate", handleAPI);
    server.on("/reset", []() {
        Serial.println("Web interface requested device reset");
        if (pClient != nullptr) {
            pClient->disconnect();
        }
        delay(500);
        startScan();
        server.sendHeader("Location", "/");
        server.send(303);
    });
    server.begin();
    Serial.println("HTTP server started");
    
    // Start scanning for Polar H10 HR data in a while
    startScan();
}

void loop() {
    // Handle web server clients
    server.handleClient();
    delay(10); // Small delay to prevent watchdog triggering
    // If we found a device but not connected, try to connect
    if (polarH10Device != nullptr && !deviceConnected && !scanActive) {
        static int connectionAttempts = 0;
        static unsigned long lastAttemptTime = 0;
        static unsigned long connectionStartTime = 0;
        
        // Add a buffer period after scan completes before trying to connect
        static bool initialDelayComplete = false;
        if (!initialDelayComplete) {
            Serial.println("Device found, waiting before initial connection attempt...");
            delay(1000); // Give the Polar H10 time to prepare for connections
            initialDelayComplete = true;
            lastAttemptTime = millis();
            return;
        }
        
        // Only try to connect every 8 seconds
        if (millis() - lastAttemptTime > 8000) {
            lastAttemptTime = millis();
            connectionAttempts++;
            
            if (connectionAttempts > 20) { // Increased from 5 to 10
                Serial.println("Max connection attempts reached, rescanning...");
                connectionAttempts = 0;
                startScan();
            } else {
                Serial.print("Connection attempt #");
                Serial.println(connectionAttempts);
                connectToDevice();
            }
        }
        
        // Increase global connection timeout to 60 seconds (was 30)
        if (connectionStartTime == 0) {
            connectionStartTime = millis();
        } else if (millis() - connectionStartTime > 60000) {
            Serial.println("Connection timeout reached, restarting scan...");
            connectionStartTime = 0;
            startScan();
        }
    }
    
    // If scan is done and no device found, restart scanning
    if (!scanActive && polarH10Device == nullptr) {
        delay(1000);
        startScan();
    }
    
    // Check if we haven't received HR data in a while (increased from 30 to 45 seconds)
    if (deviceConnected && (millis() - lastNotificationTime) > 45000) {
        Serial.println("No HR data received in 45 seconds. Make sure chest strap has good contact.");
        Serial.println("Reconnecting...");
        pClient->disconnect();
        startScan();
    }
    
    delay(10); // Small delay to prevent watchdog triggering
}