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

## Building the ESP-IDF Example

The library includes a native ESP-IDF example in the `examples/CppEspIdf` directory. To build it:

1.  **Set up ESP-IDF:** Ensure you have the ESP-IDF environment installed and configured.
2.  **Configure Wi-Fi:** Open `examples/CppEspIdf/main/main.cpp` and set your Wi-Fi SSID and password.
3.  **Build the project:**
    ```bash
    cd examples/CppEspIdf
    idf.py build
    ```
