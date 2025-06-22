//----------------------------------------------------------Headers----------------------------------------------------------
#include <NimBLEDevice.h> // NimBLE library for BLE communication - faster alternative to standard BLE
#include <WiFi.h>         // Required for creating WiFi access point
#include <WebServer.h>    // Enables hosting a web server to display heart rate data
#include <LittleFS.h>     // Filesystem library for storing static files like chart.js

//----------------------------------------------------------Constants----------------------------------------------------------

// Constants for BLE configuration
#define MAX_HR_HISTORY 15           // Maximum heart rate readings to store
#define CONNECTION_TIMEOUT_MS 120000 // 2 minutes timeout for connection attempts
#define HR_DATA_TIMEOUT_MS 45000    // 45 seconds timeout for heart rate data
#define SCAN_DURATION_MS 30000      // 30 seconds scan duration
#define CONNECTION_RETRY_DELAY_MS 8000 // 8 seconds between connection attempts
#define MAX_CONNECTION_ATTEMPTS 20  // Maximum connection attempts before rescan
#define SCAN_RESTART_DELAY_MS 1000  // Delay before restarting scan
#define INITIAL_CONNECTION_DELAY_MS 500  // Initial delay after device discovery
#define BLE_MTU_SIZE 232            // MTU size for Polar H10 compatibility
#define BLE_DATA_LENGTH 185         // Optimal data length for HR monitoring

// BLE Connection Parameter Constants (values are in specific units as noted)
#define BLE_CONN_INTERVAL_FAST_MIN 40    // Fast connection: 40 × 1.25ms = 50ms minimum
#define BLE_CONN_INTERVAL_FAST_MAX 80    // Fast connection: 80 × 1.25ms = 100ms maximum
#define BLE_CONN_INTERVAL_STABLE_MIN 120 // Stable connection: 120 × 1.25ms = 150ms minimum  
#define BLE_CONN_INTERVAL_STABLE_MAX 120 // Stable connection: 120 × 1.25ms = 150ms maximum
#define BLE_CONN_LATENCY 0               // No connection event skipping for reliable data
#define BLE_SUPERVISION_TIMEOUT_CONNECTING 600  // 6 seconds during connection attempts
#define BLE_SUPERVISION_TIMEOUT_STABLE 1000     // 10 seconds for unstable connections
#define BLE_SUPERVISION_TIMEOUT_CONNECTED 400   // 4 seconds when connected and stable

// Connection Timeout Constants (in seconds)
#define BLE_CONNECT_TIMEOUT_INITIAL 30   // Initial connection attempt timeout
#define BLE_CONNECT_TIMEOUT_RETRY 45     // Retry connection attempt timeout

// Pre-connection Delay Constants (in milliseconds)
#define PRE_CONNECTION_BASE_DELAY 400    // Base delay before connection attempt
#define PRE_CONNECTION_RETRY_INCREMENT 200  // Additional delay per retry attempt
#define CONNECTION_STABILIZATION_DELAY 500  // Delay after successful connection to allow stabilization
#define MTU_NEGOTIATION_DELAY 100       // Delay to allow MTU negotiation to complete

// Constant for watchdog
#define LOOP_DELAY_MS 10            // Main loop delay to prevent watchdog issues

// WiFi network credentials - creates access point with these details
const char *ssid = "HRM-ESP32";    // Network name for the ESP32's access point
const char *password = "12345678"; // Password for the access point (make stronger for real use)

// Polar H10 Device Configuration Constants
const char *POLAR_H10_MAC = "a0:9e:1a:e4:c5:6b"; // Your specific Polar H10 MAC address
const char *POLAR_H10_PARTIAL_MAC = "e4:c5:6b";   // Partial MAC for broader matching
const char *HR_SERVICE_UUID_SHORT = "180D";        // Heart Rate Service UUID (uppercase)
const char *HR_SERVICE_UUID_LOWER = "180d";        // Heart Rate Service UUID (lowercase) 
const char *HR_SERVICE_UUID_FULL = "0000180d-0000-1000-8000-00805f9b34fb"; // Full 128-bit UUID
const uint16_t POLAR_COMPANY_ID = 107;             // Polar's Bluetooth SIG company ID
const char *POLAR_DEVICE_NAME_1 = "Polar";         // Primary device name identifier
const char *POLAR_DEVICE_NAME_2 = "H10";           // Secondary device name identifier

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

// Heart rate data storage for chart
static int hrData[MAX_HR_HISTORY] = {0}; // Array to store last 15 heart rate readings
static int hrDataIndex = 0;  // Current index in the array

// Web server on port 80
WebServer server(80);

// Forward declarations
void startScan();
bool connectToDevice();
void cleanupBLEResources();
void logMemoryUsage();
class MyScanCallback;

// Global scan callback instance
static MyScanCallback *scanCallback = nullptr;

//----------------------------------------------------------Memory_Management----------------------------------------------------------

// Clean up BLE resources to prevent memory leaks
void cleanupBLEResources() {
    Serial.println("Cleaning up BLE resources...");
    
    // Clean up client connection
    if (pClient != nullptr) {
        if (pClient->isConnected()) {
            pClient->disconnect();
        }
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
    }
    
    // Clean up service and characteristic references
    pService = nullptr;
    pRemoteCharacteristic = nullptr;
    
    // Clean up device reference
    if (polarH10Device != nullptr) {
        delete polarH10Device;
        polarH10Device = nullptr;
    }
    
    // Reset state variables
    deviceConnected = false;
    scanActive = false;
}

// Log current memory usage for debugging
void logMemoryUsage() {
    Serial.print("Free heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" bytes, Min free heap: ");
    Serial.print(ESP.getMinFreeHeap());
    Serial.println(" bytes");
}

//----------------------------------------------------------Search_Function----------------------------------------------------------

// Scan for BLE devices callback
class MyScanCallback : public NimBLEScanCallbacks{
public:
    void onDiscovered(const NimBLEAdvertisedDevice *advertisedDevice) override{
        // Only do the quick MAC check here
        std::string deviceAddress = advertisedDevice->getAddress().toString();

        // Find your specific Polar H10 by MAC address - fastest and most reliable method
        if (deviceAddress.find(POLAR_H10_MAC) != std::string::npos){
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
                Serial.println(serviceUUID.c_str());                // Heart Rate Service standard UUID is 180D (with variations)
                if (serviceUUID == HR_SERVICE_UUID_LOWER || serviceUUID == HR_SERVICE_UUID_SHORT || serviceUUID == HR_SERVICE_UUID_FULL){
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
                if (companyID == POLAR_COMPANY_ID){ // Polar's company ID
                    Serial.println("*** FOUND POLAR DEVICE BY MANUFACTURER! ***");
                    polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                    NimBLEDevice::getScan()->stop();
                    scanActive = false;
                    return;
                }
            }
        }        // DISCOVERY METHOD 3: Find by partial MAC address
        std::string deviceAddress = advertisedDevice->getAddress().toString();
        if (deviceAddress.find(POLAR_H10_PARTIAL_MAC) != std::string::npos){
            Serial.println("*** FOUND POLAR H10 WITH PARTIAL MAC! ***");
            polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
            NimBLEDevice::getScan()->stop();
            scanActive = false;
            return;
        }        // DISCOVERY METHOD 4: Find by name
        if (advertisedDevice->haveName()){
            std::string name = advertisedDevice->getName();
            if (name.find(POLAR_DEVICE_NAME_1) != std::string::npos || name.find(POLAR_DEVICE_NAME_2) != std::string::npos){
                Serial.print("Quick found Polar device by name: ");
                Serial.println(name.c_str());
                polarH10Device = new NimBLEAdvertisedDevice(*advertisedDevice);
                NimBLEDevice::getScan()->stop();
                scanActive = false;
                return;
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
        Serial.println(pclient->getMTU());        // Request a larger MTU

        // Then exchange MTU with no parameters
        if (pclient->exchangeMTU()){
            Serial.println("MTU exchange requested...");
            delay(MTU_NEGOTIATION_DELAY); // Give time for negotiation using defined constant
            Serial.print("Final negotiated MTU: ");
            Serial.println(pclient->getMTU());
        }else{
            Serial.println("MTU exchange failed");
        }       
        
        // Set data length
        bool dataLenResult = pclient->setDataLen(BLE_DATA_LENGTH);
        if (dataLenResult) {
            Serial.print("Data length extension requested: ");
            Serial.print(BLE_DATA_LENGTH);
            Serial.println(" bytes (actual negotiated value may be lower)"); // Didn't find in NIMBLE docs an function that returns negotiated data length
        }else{
            Serial.println("Failed to request data length extension"); // Can cause problem with RR intervals history
        }

        // Update connection parameters for stable data transfer
        pclient->updateConnParams(120, 120, 0, 400);  // Changed from 60 to 400
        // Post-connection parameters breakdown:
        // 120: minInterval (120 × 1.25ms = 150ms) - Slower interval for stable data transfer
        // 120: maxInterval (120 × 1.25ms = 150ms) - Fixed interval (min=max) for consistent timing
        // 0: latency - No connection event skipping for reliable heart rate data
        // 400: supervisionTimeout (400 × 10ms = 4 seconds) - Reasonable timeout for stable connection

        delay(200); // Short delay to allow parameter update to take effect before service discovery
    }    
    
    // Called when connection is lost or disconnected
    void onDisconnect(NimBLEClient *pclient, int reason){
        deviceConnected = false;
        connectionStatus = "Disconnected";
        Serial.print("Disconnected, reason: ");
        Serial.println(reason);
        
        // Log memory usage before cleanup
        logMemoryUsage();

        // Use centralized cleanup function
        cleanupBLEResources();

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

        // Add new heart rate data to array
        hrData[hrDataIndex] = hr;
        hrDataIndex = (hrDataIndex + 1) % MAX_HR_HISTORY; // Circular buffer

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
    bool scanResult = pBLEScan->start(SCAN_DURATION_MS / 1000, false, true);

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
bool connectToDevice(){    // Safety check - ensure device was found
    if (polarH10Device == nullptr){
        Serial.println("No device to connect to");
        return false;
    }

    Serial.println("Connecting to Polar H10...");
    connectionStatus = "Connecting...";
    
    // Log memory usage before connection attempt
    logMemoryUsage();

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
    while (retries < 6){
        Serial.print("Connection attempt #");
        Serial.print(retries + 1);
        Serial.print(" with ");
        Serial.print(delayMs);
        Serial.println("ms delay");

        // Reset BT stack for a clean connection attempt
        // on specific retry attempts
        if (retries == 0 || retries == 4){
            NimBLEDevice::deinit(true);
            delay(500);
            NimBLEDevice::init("ESP32-HR-Monitor");

            // Explicitly disable security for simpler connection
            NimBLEDevice::setSecurityAuth(false);

            // Create and configure client
            pClient = NimBLEDevice::createClient();
            pClient->setClientCallbacks(new MyClientCallback());            // Adjust connection parameters based on retry count

            if (retries == 0){
                // First attempt - use fast connection parameters (see constants at top)
                pClient->setConnectionParams(BLE_CONN_INTERVAL_FAST_MIN, BLE_CONN_INTERVAL_FAST_MAX, BLE_CONN_LATENCY, BLE_SUPERVISION_TIMEOUT_CONNECTING);                
                // Using constants: BLE_CONN_INTERVAL_FAST_MIN (40×1.25ms=50ms), BLE_CONN_INTERVAL_FAST_MAX (80×1.25ms=100ms)
                // BLE_CONN_LATENCY (0=no skipping), BLE_SUPERVISION_TIMEOUT_CONNECTING (600×10ms=6s during connection)
                
                pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_INITIAL);  // Use constant for initial timeout

            }else{
                // Later attempts - use stable connection parameters for difficult connections
                pClient->setConnectionParams(BLE_CONN_INTERVAL_STABLE_MIN, BLE_CONN_INTERVAL_STABLE_MAX, BLE_CONN_LATENCY, BLE_SUPERVISION_TIMEOUT_STABLE); 
                // Using constants: BLE_CONN_INTERVAL_STABLE_MIN/MAX (120×1.25ms=150ms for stability)
                // BLE_CONN_LATENCY (0=no skipping), BLE_SUPERVISION_TIMEOUT_STABLE (1000×10ms=10s for unstable connections)
                
                pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_RETRY);  // Use constant for retry timeout
            }
        }        
        
        // Pre-connection delay - increases with retry count to respect Polar H10's advertising cycle
        Serial.println("Pre Connection Delay");
        delay(PRE_CONNECTION_BASE_DELAY + (retries * PRE_CONNECTION_RETRY_INCREMENT));
        // Using constants: PRE_CONNECTION_BASE_DELAY (400ms) + retries × PRE_CONNECTION_RETRY_INCREMENT (200ms)
        // Purpose: Allows Polar H10 to complete advertising cycles and be ready for connection
        // Example: Attempt 1=400ms, Attempt 2=600ms, Attempt 3=800ms, etc.

        // Attempt connection
        Serial.println("Attempting to connect to device");
        // connect(device, deleteExisting, useWhitelist, useRandomAddr)
        // - true: Delete any existing connection to this device first (prevents conflicts)
        // - false: Don't use whitelist (connect to any device, not just whitelisted ones)
        // - false: Use static/public address (normal BLE addressing, not random for privacy)
        if (pClient->connect(polarH10Device, true, false, false)){
            connected = true;
            break;
        }

        // Connection failed - exponential backoff to prevent overwhelming the device
        Serial.println("Connection attempt failed, retrying...");
        delay(delayMs);         // Wait before next retry (starts at 100ms)
        delayMs *= 2;           // Double the delay each time (100ms → 200ms → 400ms → 800ms...)
        // Purpose: Gives increasing time between retries to avoid rapid-fire connection attempts that can interfere with Polar H10's BLE stack or cause connection blocking
        retries++;
    }

    // If all retries failed
    if (!connected){
        Serial.println("All connection attempts failed");
        connectionStatus = "Connection Failed";
        if (pClient != nullptr){
            // Delete client to guarantee no memory leaks
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
        }
        return false;
    }    
    
    // Connection succeeded - continue with service discovery
    Serial.println("Connected successfully!");
    delay(CONNECTION_STABILIZATION_DELAY); // Allow connection to stabilize using defined constant
    
    Serial.println("Connected! Trying to discover Heart Rate Service");
    pService = pClient->getService(NimBLEUUID(HR_SERVICE_UUID_SHORT)); // Using HR_SERVICE_UUID_SHORT constant

    if (pService != nullptr){
        Serial.println("Heart Rate Service found! Discovering characteristics");

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
                    connectionStatus = "Notification Setup Failed";
                    return false; // Force reconnection attempt
                }
            }else{
                Serial.println("Heart Rate characteristic doesn't support notifications");
                connectionStatus = "Characteristic Not Compatible"; // If your device doesn't support notifications use polling instead (Polar h10 support, so i didn't implement it)
                return false; // Force reconnection attempt (i'm assuming that the Polar H10 supports notifications, so this trigger an reconnection attempt)
            }        
        }else{
            Serial.println("Heart Rate characteristic not found");
            connectionStatus = "Characteristic Discovery Failed";
            return false; // Force reconnection attempt
        }
    }else{
        // If HR service not found, attempt reconnection after delay, this can happen if the Polar H10 hasn't fully initialized its GATT services yet
        Serial.println("Heart Rate Service not found - will retry on next connection attempt");
        connectionStatus = "Service Discovery Failed";
        return false; // Force reconnection attempt
    }
}

//----------------------------------------------------------WebSite----------------------------------------------------------
void handleRoot(){
    // Create HTML with advanced styling and Chart.js (website structure can be seem at batimento_mockup.html)
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'/>"
                  "<title>ESP32 HR Monitor</title>"
                  "<style>"
                  "body{font-family:Arial,sans-serif;text-align:center;padding:10px;"
                  "background:linear-gradient(#0a657e,#053f4f);display:flex;flex-direction:column;"
                  "justify-content:center;align-items:center;min-height:100vh;margin:0;gap:5px;}"
                  "h2{color:white;font-size:3.5em;margin:5px 0;border-bottom:4px solid #5d8d9a;}"
                  ".status{padding:8px 16px;margin:5px 0;border-radius:4px;font-size:1em;text-align:center;}"
                  ".scanning{background:#ffeb3b;}"
                  ".connecting{background:#ff9800;color:white;}"
                  ".connected{background:#4caf50;color:white;}"
                  ".heart{animation:pulse 1s infinite;}"
                  "@keyframes pulse{0%,100%{transform:scale(1);}50%{transform:scale(1.4);}}"
                  ".chart-container{color:black;background:rgba(255,255,255,0.9);width:100%;"
                  "max-width:800px;margin:15px 0;border-radius:8px;box-shadow:0 4px 6px rgba(0,0,0,0.2);"
                  "padding:2px;overflow:hidden;}" 
                  "canvas{width:100% !important;height:auto !important;display:block;margin:0}" 
                  "</style>"
                  "<script src=\"/chart.min.js\"></script>"
                  "</head><body>"
                  "<h2>ESP32 HR Monitor</h2>";

    // Add status indicator
    String statusClass = deviceConnected ? "connected" : (polarH10Device ? "connecting" : "scanning");
    html += "<div class='status " + statusClass + "'>" + String(connectionStatus) + "</div>";

    // Add heart rate display
    if (deviceConnected && currentHeartRate > 0){
        html += "<div style='font-size:4em;color:white;margin:15px;'>"
                "<span class='heart'>❤</span> " + String(currentHeartRate) + " BPM</div>";
    }else{
        html += "<div style='font-size:4em;color:white;margin:15px;'>"
                "<span class='heart'>❤</span> -- BPM</div>";
    }

    // Add chart container
    html += "<div class='chart-container'><canvas id='bpmChart'></canvas></div>";

    // Add JavaScript to create chart with actual data (using standard Chart.js syntax)
    html += "<script>"
            "const ctx=document.getElementById('bpmChart').getContext('2d');"
            "const bpmData=[";
    
    // Add heart rate data from array
    for(int i = 0; i < MAX_HR_HISTORY; i++){
        if(i > 0) html += ",";
        html += String(hrData[i]);
    }
    
    html += "];"
            "const bpmChart=new Chart(ctx,{"
            "type:'line',"
            "data:{"
            "labels:['-14s','-13s','-12s','-11s','-10s','-9s','-8s','-7s','-6s','-5s','-4s','-3s','-2s','-1s','Now'],"
            "datasets:[{"
            "label:'Heart Rate (BPM)',"
            "data:bpmData,"
            "borderColor:'red',"
            "backgroundColor:'rgba(255,0,0,0.2)',"
            "pointBackgroundColor:'red',"
            "pointBorderColor:'black',"
            "fill:true,"
            "tension:0.4,"
            "pointRadius:5,"
            "pointHoverRadius:8"
            "}]},"
            "options:{"
            "responsive:true,"
            "maintainAspectRatio:false,"
            "plugins:{legend:{display:true,position:'top'}},"
            "scales:{"
            "x:{title:{display:true,text:'Time'}},"
            "y:{title:{display:true,text:'BPM'},suggestedMin:40,suggestedMax:180}"
            "}}"
            "});";
            
    html += "</script></body></html>";

    server.send(200, "text/html", html);
}

//----------------------------------------------------------ESP32-Setup----------------------------------------------------------

void setup(){
    Serial.begin(115200);
    Serial.println("ESP32 HR Monitor Start");
    
    // Log initial memory state
    logMemoryUsage();

    // Initialize BLE with simplified setup
    NimBLEDevice::init("ESP32-HR-Monitor");    // Improve BLE configuration for better range and stability
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum transmission power for better range
    // Alternative power levels: ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, ESP_PWR_LVL_N6, ESP_PWR_LVL_N3, ESP_PWR_LVL_N0, ESP_PWR_LVL_P3, ESP_PWR_LVL_P6, ESP_PWR_LVL_P9

    NimBLEDevice::setMTU(BLE_MTU_SIZE);              // MTU that Polar H10 supports for larger data packets

    // Setup WiFi in access point mode (creates its own network)
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    // Setup web server routes
    server.on("/", handleRoot); // Main HR display page
    server.begin();    Serial.println("HTTP server started");
    
    // Initialize LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("An Error has occurred while mounting LittleFS");
        return;
    }
    Serial.println("LittleFS initialized successfully");
    
    // List files in LittleFS for debugging
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    Serial.println("Files in LittleFS:");
    while(file){
        Serial.print("FILE: ");
        Serial.println(file.name());
        file = root.openNextFile();
    }
    
    // Serve chart.min.js from LittleFS
    server.on("/chart.min.js", HTTP_GET, []() {
        Serial.println("Request for chart.min.js received");
        File file = LittleFS.open("/chart.min.js", "r");
        if (!file) {
            Serial.println("Failed to open chart.min.js");
            server.send(404, "text/plain", "File not found - check LittleFS upload");
            return;
        }
        Serial.println("Serving chart.min.js");
        server.streamFile(file, "application/javascript");
        file.close();
    });

    // Start scanning for Polar H10 HR data
    startScan();
}

void loop(){    
    // Handle incoming web client requests
    server.handleClient();

    delay(LOOP_DELAY_MS); // Small delay to prevent watchdog triggering (not monopolizing processor time)

    // If device found but not yet connected, try to connect
    if (polarH10Device != nullptr && !deviceConnected && !scanActive){
        // Track connection attempts
        static int connectionAttempts = 0;
        static unsigned long lastAttemptTime = 0;
        static unsigned long connectionStartTime = 0;

        // Add initial delay before first connection attempt, Some devices often need time after discovery before they're ready
        static bool initialDelayComplete = false;
        if (!initialDelayComplete){            
            Serial.println("Device found, waiting before initial connection attempt...");
            initialDelayComplete = true;
            lastAttemptTime = millis();
            delay(INITIAL_CONNECTION_DELAY_MS);
            return; // return early to avoid immediate connection attempt
        }        
        
        // Space out connection attempts - trying too frequently causes errors
        if (millis() - lastAttemptTime > CONNECTION_RETRY_DELAY_MS){
            // 8000ms (8 seconds) spacing between connection attempts in main loop
            // Purpose: Respects Polar H10's advertising interval and prevents BLE stack overload
            // Shorter intervals can cause connection failures due to timing conflicts
            lastAttemptTime = millis();
            connectionAttempts++;

            // If too many failed attempts, restart scanning
            if (connectionAttempts > MAX_CONNECTION_ATTEMPTS){ // Multiple attempts before giving up
                Serial.println("Max connection attempts reached, rescanning...");
                connectionAttempts = 0;
                initialDelayComplete = false; // Reset for next time
                startScan();
                return; // Restart scanning instead of trying to connect
            }else{
                Serial.print("Connection attempt #");
                Serial.println(connectionAttempts);
                connectToDevice();
            }
        }

        // Global timeout - if device found but can't connect after long time
        if (connectionStartTime == 0){
            connectionStartTime = millis(); // Start tracking connection time
        }else if(millis() - connectionStartTime > CONNECTION_TIMEOUT_MS){ // 2 minutes timeout
            Serial.println("Connection timeout reached, restarting scan...");
            connectionStartTime = 0;
            initialDelayComplete = false; // Reset for next time
            startScan();
            return; // Restart scanning instead of trying to connect
        }
    }    
    
    // If scan completed but no device found, restart scanning
    if (!scanActive && polarH10Device == nullptr){
        delay(SCAN_RESTART_DELAY_MS); // Wait before restarting scan to avoid rapid restarts
        startScan();
    }

    // Monitor heart rate data to ensure connection is active
    // Reconnect if no data received for a while
    if (deviceConnected && (millis() - lastNotificationTime) > HR_DATA_TIMEOUT_MS){
        Serial.println("No HR data received in 45 seconds. Make sure chest strap has good contact.");
        Serial.println("Reconnecting...");
        
        cleanupBLEResources();  // Properly clean all resources
        logMemoryUsage();   // Log memory status after cleanup
        
        delay(500); // Short delay to ensure cleanup completes
        
        startScan();
        return;  // Exit loop iteration
    }

    delay(LOOP_DELAY_MS); // Small delay to prevent ESP32 watchdog triggering (not monopolizing processor time)
}