//----------------------------------------------------------Headers----------------------------------------------------------
#include <NimBLEDevice.h> // NimBLE library for BLE communication - faster alternative to standard BLE
#include <WiFi.h>         // Required for creating WiFi access point
#include <WebServer.h>    // Enables hosting a web server to display heart rate data

// WiFi network credentials - creates access point with these details
const char *ssid = "HRM-ESP32";    // Network name for the ESP32's access point
const char *password = "12345678"; // Password for the access point (make stronger for real use)

// Polar H10 Device Configuration
const char *POLAR_H10_MAC = "a0:9e:1a:e4:c5:6b"; // Your specific Polar H10 MAC address

// BLE variables - core objects that manage BLE functionality
static NimBLEAdvertisedDevice *polarH10Device = nullptr;            // Holds reference to discovered Polar device
static NimBLEClient *pClient = nullptr;                             // BLE client for connection management
static NimBLERemoteService *pService = nullptr;                     // Heart rate service object
static NimBLERemoteCharacteristic *pRemoteCharacteristic = nullptr; // Heart rate measurement characteristic
static NimBLEScan *pBLEScan = nullptr;                              // BLE scan object for discovering devices
static bool deviceConnected = false;                                // Tracks connection status with Polar device
static bool scanActive = true;                                      // Indicates if device scanning is currently running

// Heart rate variables
static int currentHeartRate = 0;                     // Latest heart rate reading from device
static unsigned long lastNotificationTime = 0;       // Timestamp of last heart rate update
static const char *connectionStatus = "Scanning..."; // Text status shown on web interface

// Web server on port 80
WebServer server(80);

// Forward declarations
void startScan();
bool connectToDevice();
class MyScanCallback;

// Global scan callback instance
static MyScanCallback *scanCallback = nullptr;

//----------------------------------------------------------Search_Function----------------------------------------------------------

// Scan for BLE devices callback
class MyScanCallback : public NimBLEScanCallbacks{
public:
    void onDiscovered(const NimBLEAdvertisedDevice *advertisedDevice) override{
        // Only do the quick MAC check here
        std::string deviceAddress = advertisedDevice->getAddress().toString();

        // Find your specific Polar H10 by MAC address - fastest and most reliable method
        if (deviceAddress.find("a0:9e:1a:e4:c5:6b") != std::string::npos){
            Serial.println("**** FOUND POLAR H10 BY MAC (DISCOVERY)! ****");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }
    }

    // Detailed discovery pass - called with full advertisement data
    // This provides more thorough device inspection but is slower
    void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override{
        // Print device details for debugging and monitoring
        Serial.print("Device found: Address: ");
        Serial.print(advertisedDevice->getAddress().toString().c_str());

        // Log the device name if available
        if (advertisedDevice->haveName()){
            Serial.print(" | Name: '");
            Serial.print(advertisedDevice->getName().c_str());
            Serial.print("'");
        }

        // Log signal strength to help identify close devices
        Serial.print(" | RSSI: ");
        Serial.print(advertisedDevice->getRSSI());

        // Print available service UUIDs for debugging
        if (advertisedDevice->haveServiceUUID()){
            Serial.print(" | Services: ");
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++){
                Serial.print(advertisedDevice->getServiceUUID(i).toString().c_str());
                Serial.print(" ");
            }
        }

        // DISCOVERY METHOD 1: Find by Heart Rate service
        // This works with any heart rate monitor, not just Polar devices
        if (advertisedDevice->haveServiceUUID()){
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++){
                std::string serviceUUID = advertisedDevice->getServiceUUID(i).toString();
                Serial.print("Checking service UUID: ");
                Serial.println(serviceUUID.c_str());

                // Heart Rate Service standard UUID is 180D (with variations)
                if (serviceUUID == "180d" || serviceUUID == "180D" || serviceUUID == "0000180d-0000-1000-8000-00805f9b34fb"){
                    Serial.println("*** FOUND A HEART RATE DEVICE! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }

        // DISCOVERY METHOD 2: Find by manufacturer data
        // Polar devices use company ID 107
        if (advertisedDevice->haveManufacturerData()){
            std::string manufData = advertisedDevice->getManufacturerData();
            if (manufData.length() >= 2){
                // Extract company ID from first two bytes
                uint16_t companyID = (uint8_t)manufData[0] | ((uint8_t)manufData[1] << 8);
                Serial.print("Manufacturer ID: ");
                Serial.println(companyID);

                if (companyID == 107)
                { // Polar's company ID
                    Serial.println("*** FOUND POLAR DEVICE BY MANUFACTURER! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }

        // DISCOVERY METHOD 3: Find by partial MAC address
        // Need to declare deviceAddress here since you're using it
        std::string deviceAddress = advertisedDevice->getAddress().toString();
        if (deviceAddress.find("e4:c5:6b") != std::string::npos)
        {
            Serial.println("*** FOUND POLAR H10 WITH PARTIAL MAC! ***");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }

        // DISCOVERY METHOD 4: Find by name
        if (advertisedDevice->haveName()){
            std::string name = advertisedDevice->getName();
            if (name.find("Polar") != std::string::npos || name.find("H10") != std::string::npos){
                Serial.print("Quick found Polar device by name: ");
                Serial.println(name.c_str());
                polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                NimBLEDevice::getScan()->stop();
                scanActive = false;
                return;
            }
        }

        // DISCOVERY METHOD 5: Find by Heart Rate Service UUID (case insensitive)
        if (advertisedDevice->haveServiceUUID()){
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++){
                std::string serviceUUID = advertisedDevice->getServiceUUID(i).toString();
                Serial.print("Checking UUID: ");
                Serial.println(serviceUUID.c_str());

                // Convert to lowercase for more flexible matching
                std::string lowerUUID = serviceUUID;
                std::transform(lowerUUID.begin(), lowerUUID.end(), lowerUUID.begin(), ::tolower);

                // Heart Rate Service UUID is 180D/180d
                if (lowerUUID.find("180d") != std::string::npos){
                    Serial.println("*** FOUND HEART RATE DEVICE! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }
    }

    // Called when scan completes - either timeout or manual stop
    void onScanEnd(const NimBLEScanResults &results, int reason) override {
        static int scanAttempts = 0;

        Serial.print("Scan ended. Reason: ");
        Serial.print(reason);
        Serial.print(", Found ");
        Serial.print(results.getCount());
        Serial.println(" devices");

        // If target device found, proceed to connection phase
        if (polarH10Device != nullptr){
            Serial.println("Device found - proceeding to connection phase");
            scanActive = false;
            return;
        }

        // If no device found, track attempts and continue scanning
        scanAttempts++;
        Serial.print("No Polar H10 found yet. Scan cycle #");
        Serial.print(scanAttempts);
        Serial.println(" - continuing scan");

        // Scan will automatically restart via third parameter (true) in startScan()
    }
};

//----------------------------------------------------------Connect/Disconnect----------------------------------------------------------

// Client connection callbacks - handle connect/disconnect events
class MyClientCallback : public NimBLEClientCallbacks{
    // Called when connection is established
    void onConnect(NimBLEClient *pclient){
        deviceConnected = true;
        connectionStatus = "Connected";
        Serial.println("Connected to Polar H10");

        // Report initial MTU
        Serial.print("Initial MTU: ");
        Serial.println(pclient->getMTU());

        // Request a larger MTU
        // Use setMTU instead of setPreferedMTU
        NimBLEDevice::setMTU(232);

        // Then exchange MTU with no parameters
        if (pclient->exchangeMTU()){
            Serial.println("MTU exchange requested...");
            delay(100); // Give time for negotiation
            Serial.print("Final negotiated MTU: ");
            Serial.println(pclient->getMTU());
        }else{
            Serial.println("MTU exchange failed");
        }

        // Set data length
        pclient->setDataLen(185);
        Serial.println("Data length set to 185");

        // Update connection parameters immediately for better stability
        pclient->updateConnParams(120, 120, 0, 60);
        delay(200); // Short delay before proceeding
    }

    // Called when connection is lost or disconnected
    void onDisconnect(NimBLEClient *pclient, int reason){
        deviceConnected = false;
        connectionStatus = "Disconnected";
        Serial.print("Disconnected, reason: ");
        Serial.println(reason);

        // Clean up all objects for proper memory management
        if (pClient != nullptr){
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
        }
        pService = nullptr;
        pRemoteCharacteristic = nullptr;

        // Delete the stored device reference
        if (polarH10Device != nullptr){
            delete polarH10Device;
            polarH10Device = nullptr;
        }

        delay(2000); // Wait before restarting scan
        startScan(); // Begin scanning again
    }
};

//----------------------------------------------------------HR_Notifications----------------------------------------------------------

// Handle heart rate notifications from the device
void notifyCallback(NimBLERemoteCharacteristic *pRemoteCharacteristic,uint8_t *pData, size_t length, bool isNotify){
    if (length > 0){
        // First byte contains flags according to BLE Heart Rate Service standard
        uint8_t flags = pData[0];

        // Check heart rate data format based on flags
        int hr;
        if (flags & 0x01){ // Check if value format is UINT16 (bit 0 set)
            // 16-bit format - bytes 1 & 2 contain the heart rate
            hr = pData[1] | (pData[2] << 8);
        }
        else{
            // 8-bit format - 2nd byte only contains heart rate
            hr = pData[1];
        }

        // Update global value and timestamp for monitoring
        currentHeartRate = hr;
        lastNotificationTime = millis();

        Serial.print("Heart Rate: ");
        Serial.println(currentHeartRate);
    }
}

//----------------------------------------------------------Start_Scan----------------------------------------------------------

// Start BLE scan - initiates the device discovery process
void startScan(){
    // Clean up any existing connections first
    if (pClient != nullptr){
        if (pClient->isConnected()){
            pClient->disconnect();
        }
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
    }

    // Reset BT stack before scan This helps resolve many connection issues by starting fresh
    NimBLEDevice::deinit(true);
    delay(200);
    NimBLEDevice::init("ESP32-HR-Monitor");

    // Explicitly disable security for simpler connection
    // Polar H10 doesn't require encryption for heart rate
    NimBLEDevice::setSecurityAuth(false);

    // Clean up device reference
    if (polarH10Device != nullptr){
        delete polarH10Device;
        polarH10Device = nullptr;
    }

    // Reset state variables
    deviceConnected = false;
    scanActive = true;
    connectionStatus = "Scanning...";

    Serial.println("\n=== Starting BLE scan ===");
    Serial.println("Looking for Polar H10 device...");

    pBLEScan = NimBLEDevice::getScan();

    // Create callback only once for efficiency
    if (scanCallback == nullptr){
        scanCallback = new MyScanCallback();
    }

    // Configure scan parameters
    pBLEScan->setScanCallbacks(scanCallback);
    pBLEScan->setActiveScan(true); // Active scanning gets more device data
    pBLEScan->setInterval(80);     // Scan more frequently (in ms)
    pBLEScan->setWindow(60);       // Spend more time scanning in each interval
    pBLEScan->setMaxResults(0);    // Don't store results in memory, use callbacks only

    // Start continuous scan with parameters:
    // 1. Duration: 30 seconds
    // 2. Don't continue scanning after connecting
    // 3. Restart automatically when scan ends (true)
    bool scanResult = pBLEScan->start(30000, false, true);

    // Handle scan start failure
    if (!scanResult){
        Serial.println("Failed to start scan! Restarting ESP32");
        delay(3000);
        ESP.restart(); // Last resort - restart the entire device
    }

    Serial.println("Continuous scan started successfully. Searching for device");
}

//----------------------------------------------------------Connect----------------------------------------------------------

// Connect to the Polar H10 after it's been discovered
bool connectToDevice(){
    // Safety check - ensure device was found
    if (polarH10Device == nullptr){
        Serial.println("No device to connect to");
        return false;
    }

    Serial.println("Connecting to Polar H10...");
    connectionStatus = "Connecting...";

    // Clean up existing client if any
    if (pClient != nullptr){
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
    }

    // Progressive retry implementation from legacy code
    bool connected = false;
    int retries = 0;
    int delayMs = 100; // Start with 100ms delay

    // Try up to 6 times with increasing delays
    while (retries < 6)
    {
        Serial.print("Connection attempt #");
        Serial.print(retries + 1);
        Serial.print(" with ");
        Serial.print(delayMs);
        Serial.println("ms delay");

        // Reset BT stack for a clean connection attempt
        // on specific retry attempts
        if (retries == 0 || retries == 4)
        {
            NimBLEDevice::deinit(true);
            delay(500);
            NimBLEDevice::init("ESP32-HR-Monitor");

            // Explicitly disable security for simpler connection
            NimBLEDevice::setSecurityAuth(false);

            // Create and configure client
            pClient = NimBLEDevice::createClient();
            pClient->setClientCallbacks(new MyClientCallback());

            // Adjust connection parameters based on retry count
            if (retries == 0){
                // First attempt - default parameters
                pClient->setConnectionParams(24, 40, 0, 400);
                pClient->setConnectTimeout(15);
            }
            else{
                // Later attempts - more relaxed parameters
                pClient->setConnectionParams(40, 80, 0, 1000); // More relaxed parameters optimized for Polar

                pClient->setConnectTimeout(30);
            }
        }

        // Pre-connection delay - increases with retry count
        Serial.println("Pre Connection Delay");
        delay(400 + (retries * 200));

        // Attempt connection
        Serial.println("Attempting to connect to device");
        // Attempt connection with explicit parameters
        if (pClient->connect(polarH10Device, true, false, false)){
            connected = true;
            break;
        }

        // Connection failed - exponential backoff
        Serial.println("Connection attempt failed, retrying...");
        delay(delayMs);
        delayMs *= 2; // Double delay each time
        retries++;
    }

    // If all retries failed
    if (!connected){
        Serial.println("All connection attempts failed");
        connectionStatus = "Connection Failed";
        if (pClient != nullptr){
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
        }
        return false;
    }

    // Connection succeeded - continue with service discovery
    Serial.println("Connected successfully!");
    delay(500); // Allow connection to stabilize

    // Set data length and report it
    pClient->setDataLen(185);

    // Find the heart rate service
    Serial.println("Connected! Trying to discover Heart Rate Service...");
    pService = pClient->getService(NimBLEUUID("180D")); // Heart Rate service

    if (pService != nullptr){
        Serial.println("Heart Rate Service found! Discovering characteristics...");

        // Get the heart rate measurement characteristic
        pRemoteCharacteristic = pService->getCharacteristic(NimBLEUUID("2A37"));

        if (pRemoteCharacteristic != nullptr){
            Serial.println("Heart Rate Characteristic found!");

            // Subscribe to notifications to receive HR updates
            if (pRemoteCharacteristic->canNotify()){
                Serial.println("Setting up notifications for Heart Rate...");
                if (pRemoteCharacteristic->subscribe(true, notifyCallback)){
                    Serial.println("Successfully subscribed to Heart Rate notifications!");
                    return true;
                }else{
                    Serial.println("Failed to subscribe to notifications");
                }
            }
        }
    }
    // ADD -><- HR service not found trying again (is possible?)
    return connected; // Return true even if service/char not found - we're still connected
}

//----------------------------------------------------------WebSite----------------------------------------------------------
void handleRoot(){
    // Create HTML with CSS styling and auto-refresh
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'/>"
                  "<style>body{font-family:Arial;text-align:center;padding:20px;background:#f0f0f0;}"
                  "h1{color:#e74c3c;font-size:4em;margin:0;}"
                  ".status{padding:8px;margin:10px;border-radius:4px;font-weight:bold;}"
                  ".scanning{background:#ffeb3b;}.connecting{background:#ff9800;color:white;}"
                  ".connected{background:#4caf50;color:white;}"
                  "</style></head><body><h1> ESP32 HR Monitor</h1>";

    // Add status indicator with appropriate styling based on connection state
    String statusClass = deviceConnected ? "connected" : (polarH10Device ? "connecting" : "scanning");
    html += "<div class='status " + statusClass + "'>" + String(connectionStatus) + "</div>";

    // Add heart rate display
    if (deviceConnected && currentHeartRate > 0){
        html += "<div style='font-size:3em;margin:20px;'>❤ " + String(currentHeartRate) + " BPM</div>";
    }
    else{
        html += "<div style='font-size:3em;margin:20px;'>-- BPM</div>";
    }

    html += "</body></html>";
    server.send(200, "text/html", html);
}

//----------------------------------------------------------ESP32-Setup----------------------------------------------------------

void setup(){
    Serial.begin(115200);
    Serial.println("ESP32 HR Monitor Start");

    // Initialize BLE with simplified setup (no security)
    NimBLEDevice::init("ESP32-HR-Monitor");

    // Improve BLE configuration
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Increase transmission power for better range 
    // -><- Test with different power levels -><-
    NimBLEDevice::setMTU(232);              // MTU that polar H10 broadcasts

    // Setup WiFi in access point mode (creates its own network)
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    // Setup web server routes
    server.on("/", handleRoot); // Main HR display page
    server.begin();
    Serial.println("HTTP server started");

    // Start scanning for Polar H10 HR data
    startScan();
}

void loop(){
    // Handle incoming web client requests
    server.handleClient();
    delay(10); // Small delay to prevent watchdog triggering (not monopolizing processor time)

    // If device found but not yet connected, try to connect
    if (polarH10Device != nullptr && !deviceConnected && !scanActive){
        // Track connection attempts
        static int connectionAttempts = 0;
        static unsigned long lastAttemptTime = 0;
        static unsigned long connectionStartTime = 0;

        // Add initial delay before first connection attempt
        // Polar devices often need time after discovery before they're ready
        static bool initialDelayComplete = false;
        if (!initialDelayComplete){
            Serial.println("Device found, waiting before initial connection attempt...");
            initialDelayComplete = true;
            lastAttemptTime = millis();
            delay(500);
            return;
        }

        // Space out connection attempts - trying too frequently causes errors
        if (millis() - lastAttemptTime > 8000){
            lastAttemptTime = millis();
            connectionAttempts++;

            // If too many failed attempts, restart scanning
            if (connectionAttempts > 20){ // Multiple attempts before giving up
                Serial.println("Max connection attempts reached, rescanning...");
                connectionAttempts = 0;
                initialDelayComplete = false; // Reset for next time
                startScan();
            }else{
                Serial.print("Connection attempt #");
                Serial.println(connectionAttempts);
                connectToDevice();
            }
        }

        // Global timeout - if device found but can't connect after long time
        if (connectionStartTime == 0){
            connectionStartTime = millis();
        }else if (millis() - connectionStartTime > 120000){ // 2 minutes timeout
            Serial.println("Connection timeout reached, restarting scan...");
            connectionStartTime = 0;
            initialDelayComplete = false; // Reset for next time
            startScan();
        }
    }

    // If scan completed but no device found, restart scanning
    if (!scanActive && polarH10Device == nullptr){
        delay(1000);
        startScan();
    }

    // Monitor heart rate data to ensure connection is active
    // Reconnect if no data received for a while
    if (deviceConnected && (millis() - lastNotificationTime) > 45000){
        Serial.println("No HR data received in 45 seconds. Make sure chest strap has good contact.");
        Serial.println("Reconnecting...");
        pClient->disconnect();
        startScan();
    }

    delay(10); // Small delay to prevent ESP32 watchdog triggering (not monopolizing processor time)
}