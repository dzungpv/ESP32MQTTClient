# ESP32MQTTClient

[![Arduino CI](https://github.com/cyijun/ESP32MQTTClient/actions/workflows/ci4main.yml/badge.svg?branch=main)](https://github.com/cyijun/ESP32MQTTClient/actions/workflows/ci4main.yml)
[![ESP-IDF CI](https://github.com/cyijun/ESP32MQTTClient/actions/workflows/esp_idf_ci.yml/badge.svg)](https://github.com/cyijun/ESP32MQTTClient/actions/workflows/esp_idf_ci.yml)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/cyijun/ESP32MQTTClient)

A thread-safe MQTT client for native ESP-IDF or Arduino ESP32. This library is compatible with `arduino-esp32` v2/v3+ and `ESP-IDF` v4.x/v5.x C++.

## Features🦄

- Encapsulated, thread-safe MQTT client based on the official `esp-mqtt` component.
- Uses standard C++ `std::string` instead of Arduino `String`.
- Logging is performed using the standard ESP-IDF `ESP_LOGX` macros.
- Provides both specific topic subscriptions and a global "catch-all" message callback.
- Interfaces inspired by [EspMQTTClient](https://github.com/plapointe6/EspMQTTClient).
- CA cert support by [dwolshin](https://github.com/dwolshin).
- Arduino-esp32 v3+ support by [dzungpv](https://github.com/dzungpv).

## API Reference

### Constructor and Destructor

```cpp
ESP32MQTTClient();  // Default constructor
~ESP32MQTTClient(); // Destructor - cleans up resources
```

### Configuration Functions

#### Debugging and Logging
```cpp
void enableDebuggingMessages(const bool enabled = true);
```
Enables or disables serial debugging messages for connection status, message publishing/receiving, and error conditions.

#### Connection Settings
```cpp
void setURI(const char *uri, const char *username = "", const char *password = "");
void setURL(const char *url, const uint16_t port, const char *username = "", const char *password = "");
void setMqttClientName(const char *mqttClientName);
void setKeepAlive(uint16_t keepAliveSeconds = 60);
void setAutoReconnect(bool choice);
void disableAutoReconnect();
void disablePersistence();
```

- `setURI()`: Sets complete MQTT URI (e.g., "mqtt://broker.example.com:1883")
- `setURL()`: Sets URL and automatically determines protocol based on port:
  - 1883: MQTT (plain)
  - 8883: MQTT over TLS/SSL
  - 80/443: WebSocket (plain/TLS)
  - 1884/8884: WebSocket alternatives
- `setMqttClientName()`: Sets client ID (defaults to ESP32_%CHIPID%)
- `setKeepAlive()`: Sets keep-alive interval (default: 60s, ESP-IDF default: 15s)
- `setAutoReconnect()`: Enables/disables automatic reconnection
- `disableAutoReconnect()`: Disables automatic reconnection
- `disablePersistence()`: Disables persistent connections (clean session)

#### Security and Certificates
```cpp
void setClientCert(const char *clientCert);
void setCaCert(const char *caCert);
void setKey(const char *clientKey);
```
Sets TLS/SSL certificates and keys for secure connections (PEM-encoded strings).

#### Last Will and Testament
```cpp
void enableLastWillMessage(const char *topic, const char *message, const bool retain = false);
```
Configures a last will message that the broker publishes when the client disconnects unexpectedly.

#### Advanced Configuration
```cpp
void setTaskPrio(int prio);
bool setMaxPacketSize(const uint16_t size);
bool setMaxOutPacketSize(const uint16_t size);
```

- `setTaskPrio()`: Sets MQTT client task priority
- `setMaxPacketSize()`: Sets maximum packet size for both incoming/outgoing (default: 1024 bytes)
- `setMaxOutPacketSize()`: Sets maximum outgoing packet size only

### Connection Management

```cpp
bool loopStart();
void disconnect();
void forceStop();
bool isConnected() const;
bool isMyTurn(esp_mqtt_client_handle_t client) const;
```

- `loopStart()`: Connects to broker and starts event loop (blocking)
- `disconnect()`: Cleanly disconnects and stops event loop (blocking)
- `forceStop()`: Immediately stops client without clean disconnect
- `isConnected()`: Returns current connection status
- `isMyTurn()`: Checks if client handle matches this instance (useful for multi-client setups)

### Publishing Messages

```cpp
// Using std::string
bool publish(const std::string &topic, const std::string &payload, int qos = 0, bool retain = false);

// Using C-style strings
int publish(const char *topic, int qos, bool retain, const char *payload = nullptr, int length = 0, bool async = true);
```

- Returns `true`/message ID on success, `false`/-1 on failure
- `async` parameter controls whether message is enqueued (true) or published immediately (false)
- QoS 0 messages are dropped if client is not connected

### Subscribing to Topics

```cpp
// With message callback
bool subscribe(const std::string &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos = 0);

// With topic-aware callback
bool subscribe(const std::string &topic, MessageReceivedCallbackWithTopic messageReceivedCallbackWithTopic, uint8_t qos = 0);

// C-style subscription
int subscribe(const char *topic, int qos);
```

### Unsubscribing from Topics

```cpp
bool unsubscribe(const std::string &topic);  // Removes from callback list
int unsubscribe(const char *topic);          // C-style unsubscription
```

### Callback Registration

#### Event Callbacks
```cpp
void setOnConnectCallback(OnConnectCallback callback);
void setOnDisonnectCallback(OnDisconnectCallback callback);
void setOnSubscribeCallback(OnSubscribeCallback callback);
void setOnUnsubscribeCallback(OnUnsubscribeCallback callback);
void setOnPublishCallback(OnPublishCallback callback);
void setOnErrorCallback(OnErrorCallback callback);
```

#### Message Callbacks
```cpp
void setOnMessageCallback(OnMessageCallback callback);
void setOnTopicCallback(const char *topic, int qos, OnMessageCallback callback);
```

- `setOnMessageCallback()`: Global message handler for all topics
- `setOnTopicCallback()`: Topic-specific message handler with automatic subscription

### Callback Function Signatures

```cpp
// Message callbacks
typedef std::function<void(const std::string &message)> MessageReceivedCallback;
typedef std::function<void(const std::string &topicStr, const std::string &message)> MessageReceivedCallbackWithTopic;
typedef std::function<void(esp_mqtt_client_handle_t client, char *topic, char *payload, int retain, int qos, bool dup)> OnMessageCallback;

// Event callbacks
typedef std::function<void(esp_mqtt_client_handle_t client, bool sessionPresent)> OnConnectCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, bool sessionPresent)> OnDisconnectCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, int msgId)> OnSubscribeCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, int msgId)> OnUnsubscribeCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, int msgId)> OnPublishCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, esp_mqtt_error_codes_t error)> OnErrorCallback;
```

### Utility Functions

```cpp
const char *getClientName();
const char *getURI();
void printError(esp_mqtt_error_codes_t *error_handle);
```

- `getClientName()`: Returns current client ID
- `getURI()`: Returns current broker URI
- `printError()`: Prints human-readable error information

## New Functions

### `setOnMessageCallback(MessageReceivedCallbackWithTopic callback)`

Sets a global callback function that is invoked for any incoming message, regardless of the topic. This is useful for centralized logging or handling all messages in one place.

**Example:**
```cpp
mqttClient.setOnMessageCallback([](const std::string &topic, const std::string &payload) {
    ESP_LOGI("MAIN", "Global handler: %s: %s", topic.c_str(), payload.c_str());
});
```

### `setAutoReconnect(bool choice)`

Enables or disables the automatic reconnection feature of the underlying ESP-IDF MQTT client. By default, auto-reconnect is enabled.

**Example:**
```cpp
// Disable automatic reconnection
mqttClient.setAutoReconnect(false);
```

## Usage Examples

### Basic Setup and Connection
```cpp
#include "ESP32MQTTClient.h"

ESP32MQTTClient mqttClient;

void setup() {
    // Configure MQTT client
    mqttClient.setURL("broker.example.com", 1883, "username", "password");
    mqttClient.setMqttClientName("ESP32_Client");
    mqttClient.enableDebuggingMessages(true);
    
    // Set callbacks
    mqttClient.setOnConnectCallback([](esp_mqtt_client_handle_t client, bool sessionPresent) {
        ESP_LOGI("MQTT", "Connected to broker");
    });
    
    // Connect
    mqttClient.loopStart();
}
```

### Publishing and Subscribing
```cpp
// Subscribe to a topic
mqttClient.subscribe("sensor/temperature", [](const std::string &message) {
    ESP_LOGI("SENSOR", "Temperature: %s", message.c_str());
}, 1);

// Publish a message
mqttClient.publish("device/status", "online", 1, true);
```

### Topic-Specific Callbacks
```cpp
// Set callback for specific topic with wildcard support
mqttClient.setOnTopicCallback("home/+/temperature", 1, 
    [](esp_mqtt_client_handle_t client, char *topic, char *payload, int retain, int qos, bool dup) {
        ESP_LOGI("TEMP", "Topic: %s, Value: %s", topic, payload);
    });
```

## Building Examples

### Building the Arduino Example

The library includes an Arduino example in the `examples/HelloToMyself` directory that demonstrates basic MQTT functionality.

#### Prerequisites
1. **Arduino IDE**: Install Arduino IDE 2.x or later
2. **ESP32 Board Package**: Install the ESP32 board package (arduino-esp32 v2/v3+)
3. **Library Installation**: Install this library via Arduino Library Manager or by copying the library to your Arduino libraries folder

#### Configuration
1. **Open the example**: Open `examples/HelloToMyself/HelloToMyself.ino` in Arduino IDE
2. **Configure Wi-Fi**: Update the Wi-Fi credentials in the sketch:
   ```cpp
   const char *ssid = "your_wifi_ssid";
   const char *pass = "your_wifi_password";
   ```
3. **Configure MQTT Broker**: Update the MQTT broker settings:
   ```cpp
   char *server = "mqtt://your_broker_address:1883";
   char *subscribeTopic = "your_subscribe_topic";
   char *publishTopic = "your_publish_topic";
   ```

#### Building and Uploading
1. **Select Board**: Choose your ESP32 board from Tools > Board menu
2. **Select Port**: Choose the correct COM port for your ESP32
3. **Compile**: Click the Verify button to compile the sketch
4. **Upload**: Click the Upload button to flash the sketch to your ESP32

#### Example Features
The HelloToMyself example demonstrates:
- Basic MQTT connection setup
- Last will and testament configuration
- Topic subscription with lambda callbacks
- Message publishing with counter
- Global message callback handling
- Topic-specific callbacks with wildcard support

### Building the ESP-IDF Example

The library includes a native ESP-IDF example in the `examples/CppEspIdf` directory that demonstrates MQTT functionality in a pure ESP-IDF environment.

#### Prerequisites
1. **ESP-IDF Environment**: Install ESP-IDF v4.x or v5.x and set up the development environment
   ```bash
   # For ESP-IDF v5.x
   . $HOME/esp/esp-idf/export.sh
   
   # For ESP-IDF v4.x
   . $HOME/esp/esp-idf/export.sh
   ```
2. **ESP-IDF Tools**: Ensure you have the required tools installed (CMake, Ninja, etc.)
3. **Library Integration**: The example includes the library as a local component

#### Project Structure
```
examples/CppEspIdf/
├── CMakeLists.txt              # Main project CMake file
├── sdkconfig                   # ESP-IDF configuration
├── main/
│   ├── CMakeLists.txt          # Main component CMake file
│   └── main.cpp                # Main application code
└── components/
    └── ESP32MQTTClient/
        └── CMakeLists.txt      # Library component CMake file
```

#### Configuration
1. **Navigate to the example directory**:
   ```bash
   cd examples/CppEspIdf
   ```

2. **Configure Wi-Fi settings**: Open `main/main.cpp` and update the Wi-Fi credentials:
   ```cpp
   #define WIFI_SSID      "your_wifi_ssid"
   #define WIFI_PASS      "your_wifi_password"
   ```

3. **Configure MQTT broker**: Update the MQTT broker URI:
   ```cpp
   #define MQTT_URI       "mqtt://your_broker_address:1883"
   ```

4. **Optional: Configure ESP-IDF settings**:
   ```bash
   idf.py menuconfig
   ```
   - Navigate to "Component config" > "MQTT Configuration" to adjust MQTT settings
   - Navigate to "Component config" > "WiFi" to configure WiFi settings

#### Building and Flashing
1. **Build the project**:
   ```bash
   idf.py build
   ```

2. **Flash to your ESP32** (replace `PORT` with your device port):
   ```bash
   idf.py -p /dev/ttyUSB0 flash
   # or on Windows:
   idf.py -p COM3 flash
   ```

3. **Monitor the output**:
   ```bash
   idf.py monitor
   ```

4. **Build, flash, and monitor in one command**:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

#### Example Features
The CppEspIdf example demonstrates:
- **Wi-Fi Connection**: Automatic Wi-Fi connection with event handling
- **MQTT Client Setup**: Complete MQTT client configuration with callbacks
- **Last Will and Testament**: Configures offline notification
- **Topic Subscriptions**: 
  - Direct topic subscription (`foo`)
  - Wildcard topic subscription (`bar/#`)
- **Message Publishing**: Periodic message publishing with counter
- **Callback Handling**:
  - Connection callback (`onMqttConnect`)
  - Global message callback (`onMqttMessage`)
  - Topic-specific callbacks (`onMqttTopic`)
- **Multi-threading**: Uses FreeRTOS tasks for background publishing

#### Testing the Example
1. **Connect to Wi-Fi**: The device will automatically connect to the configured Wi-Fi network
2. **MQTT Connection**: Once connected to Wi-Fi, the MQTT client will connect to the broker
3. **Message Publishing**: The device publishes messages to `bar/bar` topic every 2 seconds
4. **Message Reception**: Subscribe to `foo` or `bar/#` topics to receive messages
5. **Monitor Output**: Use `idf.py monitor` to see connection status and message logs

#### Troubleshooting
- **Wi-Fi Connection Issues**: Check SSID and password in `main.cpp`
- **MQTT Connection Issues**: Verify broker address and port in `MQTT_URI`
- **Build Errors**: Ensure ESP-IDF environment is properly set up
- **Flash Issues**: Check device port and ensure ESP32 is in download mode
