#pragma once

#include <vector>
#include <string>
#include <mqtt_client.h>
#include <functional>
#include "esp_log.h"         
#include "esp_idf_version.h" // check IDF version

#define MAX_LEN_MQTT_URI 200

typedef std::function<void(const std::string &message)> MessageReceivedCallback;
typedef std::function<void(const std::string &topicStr, const std::string &message)> MessageReceivedCallbackWithTopic;

typedef std::function<void(esp_mqtt_client_handle_t client, bool sessionPresent)> OnConnectCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, bool sessionPresent)> OnDisconnectCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, int msgId)> OnSubscribeCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, int msgId)> OnUnsubscribeCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, char *topic, char *payload, int retain, int qos, bool dup)> OnMessageCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, int msgId)> OnPublishCallback;
typedef std::function<void(esp_mqtt_client_handle_t client, esp_mqtt_error_codes_t error)> OnErrorCallback;

typedef struct
{
    char *topic;
    int qos;
    OnMessageCallback callback;
} OnMessageCallback_t;

class ESP32MQTTClient
{
private:
    esp_mqtt_client_config_t _mqtt_config; // C so different naming
    esp_mqtt_client_handle_t _mqtt_client;

    // MQTT related
    bool _mqttConnected  = false;
    bool _mqttClientStop = false;
    const char *_mqttUri;
    const char *_mqttUsername;
    const char *_mqttPassword;
    const char *_mqttClientName;
    int _disableMQTTCleanSession;
    char *_mqttLastWillTopic;
    char *_mqttLastWillMessage;
    int _mqttLastWillQos;
    bool _mqttLastWillRetain;

    int _mqttMaxInPacketSize;
    int _mqttMaxOutPacketSize;

    char *_buffer = nullptr;
    char *_topic = nullptr;

    struct TopicSubscriptionRecord
    {
        std::string topic;
        MessageReceivedCallback callback;
        MessageReceivedCallbackWithTopic callbackWithTopic;
    };
    std::vector<TopicSubscriptionRecord> _topicSubscriptionList;

    // General behaviour related
    bool _enableSerialLogs;
    bool _drasticResetOnConnectionFailures;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    static esp_err_t handleMQTT(esp_mqtt_event_handle_t event);
#else  // IDF CHECK
    /*
     * @brief Event handler registered to receive MQTT events
     *
     *  This function is called by the MQTT client event loop.
     *
     * @param handler_args user data registered to the event.
     * @param base Event base for the handler(always MQTT Base).
     * @param event_id The id for the received event.
     * @param event_data The data for the event, esp_mqtt_event_handle_t.
     */
    static void handleMQTT(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
#endif // // IDF CHECK

    void onMessageReceivedCallback(const char *topic, char *payload, unsigned int length);
    bool mqttTopicMatch(const std::string &topic1, const std::string &topic2);
    void onEventCallback(esp_mqtt_event_handle_t event);

    std::vector<OnConnectCallback> _onConnectCallbacks;
    std::vector<OnDisconnectCallback> _onDisconnectCallbacks;
    std::vector<OnSubscribeCallback> _onSubscribeCallbacks;
    std::vector<OnUnsubscribeCallback> _onUnsubscribeCallbacks;
    std::vector<OnMessageCallback_t> _onMessageCallbacks;
    std::vector<OnPublishCallback> _onPublishCallbacks;
    std::vector<OnErrorCallback> _onErrorCallbacks;

    void _onConnect(esp_mqtt_event_handle_t &event_data);
    void _onDisconnect(esp_mqtt_event_handle_t &event_data);
    void _onSubscribe(esp_mqtt_event_handle_t &event_data);
    void _onUnsubscribe(esp_mqtt_event_handle_t &event_data);
    void _onMessage(esp_mqtt_event_handle_t &event_data);
    void _onPublish(esp_mqtt_event_handle_t &event_data);
    void _onError(esp_mqtt_event_handle_t &event_data);

public:
    /**
     * @brief Default constructor for ESP32MQTTClient.
     * 
     * Initializes the MQTT client with default settings:
     * - Connection state: false
     * - Max packet sizes: 1024 bytes
     * - Last will message: disabled
     */
    ESP32MQTTClient(/* args */);

    /**
     * @brief Destructor for ESP32MQTTClient.
     * 
     * Cleans up resources by:
     * - Disconnecting from MQTT broker
     * - Destroying the MQTT client
     * - Freeing allocated memory for buffers and topics
     * - Clearing callback vectors
     */
    ~ESP32MQTTClient();

    /**
     * @brief Registers a callback function to be called when the MQTT client is connected.
     *
     * @param callback The callback function with the signature void(bool sessionPresent) to be registered.
     */
    void setOnConnectCallback(OnConnectCallback callback);

    /**
     * @brief Registers a callback function to be called when the MQTT client is disconnected.
     *
     * @param callback The callback function with the signature void(bool sessionPresent) to
     * be registered.
     */
    void setOnDisonnectCallback(OnDisconnectCallback callback);

    /**
     * @brief Registers a callback function to be called when a topic is subscribed.
     *
     * @param callback The callback function with the signature void(int msgId) to be registered.
     */
    void setOnSubscribeCallback(OnSubscribeCallback callback);

    /**
     * @brief Registers a callback function to be called when a topic is unsubscribed.
     *
     * @param callback The callback function with the signature void(int msgId) to be registered.
     */
    void setOnUnsubscribeCallback(OnUnsubscribeCallback callback);

    /**
     * @brief Registers a callback function to be called when a message is received.
     * Multipart messages will be reassembled into the original message.
     *
     * @param callback The callback function with the signature void(char *topic,
     * char *payload, int msgId, int retain, int qos, bool dup) to be registered.
     */
    void setOnMessageCallback(OnMessageCallback callback);

    /**
     * @brief Registers a callback function to be called when a message is
     * received on a specific topic. Multipart messages will be
     * reassembled into the original message. Fully supports MQTT Wildcards.
     * Will automatically subscribe to all topics once the client is connected.
     *
     * @param topic The topic to listen for. MQTT Wildcards are fully supported.
     * @param qos The QoS level to listen for.
     * @param callback The callback function with the signature void(char *topic,
     * char *payload, int msgId, int retain, int qos, bool dup) to be registered.
     */
    void setOnTopicCallback(const char *topic, int qos, OnMessageCallback callback);

    /**
     * @brief Registers a callback function to be called when a message is published.
     *
     * @param callback The callback function with the signature void(int msgId) to be registered.
     */
    void setOnPublishCallback(OnPublishCallback callback);

    /**
     * @brief Registers a callback function to be called when an error occurs.
     *
     * @param callback The callback function with the signature void(esp_mqtt_error_codes_t error) to be registered.
     */
    void setOnErrorCallback(OnErrorCallback callback);

    /**
     * @brief Enables or disables debugging messages for the MQTT client.
     * 
     * When enabled, the library will print detailed information about:
     * - Connection status
     * - Message publishing and receiving
     * - Subscription operations
     * - Error conditions
     * 
     * @param enabled Set to true to enable debugging messages, false to disable.
     */
    void enableDebuggingMessages(const bool enabled = true);

    /**
     * @brief Disables persistent connections for the MQTT client.
     * 
     * By default, MQTT connections are persistent. This function disables
     * the clean session flag, which means the broker will not store
     * session information between connections.
     * 
     * @note Must be called before the first loopStart() execution.
     */
    void disablePersistence();

    /**
     * @brief Enables a last will and testament message for the MQTT client.
     * 
     * The last will message is sent by the broker when the client
     * disconnects unexpectedly. This is useful for detecting when
     * a device goes offline.
     * 
     * @param topic The topic to publish the last will message to.
     * @param message The message content to publish.
     * @param retain Whether the message should be retained by the broker.
     * 
     * @note Must be set before the first loopStart() call.
     */
    void enableLastWillMessage(const char *topic, const char *message, const bool retain = false);

    /**
     * @brief Enables drastic reset on connection failures.
     * 
     * This can be useful in special cases where the ESP32 board hangs
     * and needs resetting. When enabled, the device will perform a
     * hardware reset if connection failures persist.
     * 
     * @note Use with caution as this will restart the entire device.
     */
    void enableDrasticResetOnConnectionFailures() { _drasticResetOnConnectionFailures = true; }

    /**
     * @brief Disables automatic reconnection for the MQTT client.
     * 
     * By default, the MQTT client will automatically attempt to reconnect
     * when the connection is lost. This function disables that behavior.
     */
    void disableAutoReconnect();

    /**
     * @brief Sets the priority of the MQTT client task.
     * 
     * @param prio The task priority value. Higher values indicate higher priority.
     */
    void setTaskPrio(int prio);

    /**
     * @brief Sets the client certificate for TLS/SSL connections.
     * 
     * @param clientCert The PEM-encoded client certificate string.
     */
    void setClientCert(const char * clientCert);

    /**
     * @brief Sets the CA certificate for TLS/SSL connections.
     * 
     * @param caCert The PEM-encoded CA certificate string.
     */
    void setCaCert(const char * caCert);

    /**
     * @brief Sets the client private key for TLS/SSL connections.
     * 
     * @param clientKey The PEM-encoded client private key string.
     */
    void setKey(const char * clientKey);

    /**
     * @brief Sets the connection state of the MQTT client.
     * 
     * @param state True if connected, false if disconnected.
     */
    void setConnectionState(bool state);

    /**
     * @brief Enables or disables automatic reconnection.
     * 
     * @param choice True to enable auto-reconnect, false to disable.
     */
    void setAutoReconnect(bool choice);

    /**
     * @brief Sets the maximum outgoing packet size for the MQTT client.
     * 
     * @param size The maximum size in bytes for outgoing packets.
     * @return True if the size was set successfully, false otherwise.
     */
    bool setMaxOutPacketSize(const uint16_t size);

    /**
     * @brief Sets the maximum packet size for both incoming and outgoing packets.
     * 
     * This function sets both the maximum incoming and outgoing packet sizes
     * to the same value. The default value is 1024 bytes.
     * 
     * @param size The maximum size in bytes for packets.
     * @return True if the size was set successfully, false otherwise.
     */
    bool setMaxPacketSize(const uint16_t size);

    /**
     * @brief Publishes a message to a topic using std::string parameters.
     *
     * @param topic The topic to publish to.
     * @param payload The payload for the message.
     * @param qos The QoS level (0-2) for the message. If client not connected, 
     *            dropping message with QoS = 0.
     * @param retain The retain flag for the message.
     * @return True if the message was published successfully, false otherwise.
     */
    bool publish(const std::string &topic, const std::string &payload, int qos = 0, bool retain = false);

    /**
     * @brief Subscribes to a topic with a message received callback.
     * 
     * Server must be connected for a subscription to succeed.
     *
     * @param topic The topic to subscribe to.
     * @param messageReceivedCallback The callback function to handle received messages.
     * @param qos The QoS level for the subscription.
     * @return True if subscription was successful, false otherwise.
     */
    bool subscribe(const std::string &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos = 0);

    /**
     * @brief Subscribes to a topic with a message received callback that includes topic information.
     * 
     * Server must be connected for a subscription to succeed.
     *
     * @param topic The topic to subscribe to.
     * @param messageReceivedCallbackWithTopic The callback function to handle received messages with topic info.
     * @param qos The QoS level for the subscription.
     * @return True if subscription was successful, false otherwise.
     */
    bool subscribe(const std::string &topic, MessageReceivedCallbackWithTopic messageReceivedCallbackWithTopic, uint8_t qos = 0);

    /**
     * @brief Subscribes to a topic using C-style strings.
     * 
     * Server must be connected for a subscription to succeed.
     *
     * @param topic The topic to subscribe to.
     * @param qos The QoS level for the subscription.
     * @return Message ID on success, -1 on failure.
     */
    int subscribe(const char *topic, int qos);

    /**
     * @brief Unsubscribes from a topic using C-style strings.
     * 
     * Server must be connected for an unsubscription to succeed.
     *
     * @param topic The topic to unsubscribe from.
     * @return Message ID on success, -1 on failure.
     */
    int unsubscribe(const char *topic);

    /**
     * @brief Publishes a message to a topic using C-style parameters.
     *
     * @param topic The topic to publish to.
     * @param qos The QoS level (0-2) for the message. If client not connected, 
     *            dropping message with QoS = 0.
     * @param retain The retain flag for the message.
     * @param payload The payload for the message. Defaults to nullptr.
     * @param length The length of the payload. Defaults to 0.
     * @param async Whether to enqueue the message for asynchronous publishing.
     *               Defaults to true. False means blocking until the message is published.
     * @return Message ID on success, -1 on failure.
     */
    int publish(const char *topic, int qos, bool retain, const char *payload = nullptr, int length = 0, bool async = true);
    
    /**
     * @brief Unsubscribes from a topic and removes it from the callback list.
     * 
     * Server must be connected for an unsubscription to succeed.
     * This function also removes the topic from the internal subscription list.
     *
     * @param topic The topic to unsubscribe from.
     * @return True if unsubscription was successful, false otherwise.
     */
    bool unsubscribe(const std::string &topic); 

    /**
     * @brief Sets the keep alive interval in seconds for the MQTT connection.
     *
     * The keep alive interval determines how often the client sends ping
     * messages to the broker to maintain the connection.
     *
     * @param keepAliveSeconds The keep alive interval in seconds. Defaults to 60.
     *                         Native ESP-IDF default is 15.
     */
    void setKeepAlive(uint16_t keepAliveSeconds = 60);

    /**
     * @brief Sets the client name (ID) manually for the MQTT connection.
     *
     * @param mqttClientName The client name (ID). If not set, defaults to 
     *                       ESP32_%CHIPID% where %CHIPID% are the last 3 bytes 
     *                       of MAC address in hex format.
     * @note On older versions, this may not work if not set before other operations.
     */
    inline void setMqttClientName(const char *mqttClientName) { _mqttClientName = mqttClientName; };

    /**
     * @brief Sets the MQTT URI including protocol and port.
     * 
     * @param uri The complete MQTT URI (e.g., "mqtt://broker.example.com:1883").
     * @param username The username for authentication. Defaults to empty string.
     * @param password The password for authentication. Defaults to empty string.
     */
    inline void setURI(const char *uri, const char *username = "", const char *password = "")
    {
        _mqttUri = uri;
        _mqttUsername = username;
        _mqttPassword = password;
    };

    /**
     * @brief Sets the MQTT URL and automatically generates the URI based on port.
     * 
     * The library will automatically determine the appropriate protocol (mqtt/mqtts/ws/wss)
     * based on the port number and construct the full URI.
     * 
     * @param url The MQTT broker URL without protocol (e.g., "broker.example.com").
     * @param port The port number. Common ports:
     *             - 1883: MQTT (plain)
     *             - 8883: MQTT over TLS/SSL
     *             - 80: WebSocket (plain)
     *             - 443: WebSocket over TLS/SSL
     *             - 1884: WebSocket (plain, alternative)
     *             - 8884: WebSocket over TLS/SSL (alternative)
     * @param username The username for authentication. Defaults to empty string.
     * @param password The password for authentication. Defaults to empty string.
     */
    inline void setURL(const char *url, const uint16_t port, const char *username = "", const char *password = "")
    { 
        char *uri = (char *)malloc(MAX_LEN_MQTT_URI);
        // Determine the URI scheme based on the port, then build the URI in one call
        const char *scheme = nullptr;
        switch (port)
        {
        case 8883:
            scheme = "mqtts";
            break;
        case 1883:
            scheme = "mqtt";
            break;
        case 443:
        case 8884:
            scheme = "wss";
            break;
        case 80:
        case 1884:
            scheme = "ws";
            break;
        default:
            // Fallback for unexpected ports
            scheme = "mqtt";
            break;
        }
        // Use snprintf for safety; assume uri points to a buffer of size uri_len
        snprintf(uri, MAX_LEN_MQTT_URI, "%s://%s:%u", scheme, url, port);
        if (_enableSerialLogs)
        {
            ESP_LOGI("ESP32MQTTClient", "MQTT uri %s", uri);
        }
        _mqttUri = uri;
        _mqttUsername = username;
        _mqttPassword = password;
    };

    /**
     * @brief Checks if the MQTT client is currently connected to the broker.
     *
     * @return True if the client is connected, false otherwise.
     */
    inline bool isConnected() const { return _mqttConnected; };

    /**
     * @brief Checks if the MQTT client handle matches this instance.
     * 
     * This is useful when using multiple MQTT clients to ensure
     * the correct client is handling the event.
     *
     * @param client The MQTT client handle to check.
     * @return True if the client handle matches this instance, false otherwise.
     */
    inline bool isMyTurn(esp_mqtt_client_handle_t client) const { return _mqtt_client == client; };

    /**
     * @brief Gets the client name (ID) of the MQTT client.
     *
     * @return The client name as a const char pointer.
     */
    inline const char *getClientName() { return _mqttClientName; };

    /**
     * @brief Gets the URI of the MQTT broker.
     *
     * @return The broker URI as a const char pointer.
     */
    inline const char *getURI() { return _mqttUri; };

    /**
     * @brief Prints detailed error information for MQTT errors.
     * 
     * This function provides human-readable error messages for various
     * MQTT error conditions including connection, transport, and authentication errors.
     *
     * @param error_handle Pointer to the MQTT error codes structure.
     */
    void printError(esp_mqtt_error_codes_t *error_handle);

    /**
     * @brief Connects the MQTT client to the broker and starts the event loop.
     *
     * This function initializes the MQTT client with the configured settings
     * and attempts to establish a connection to the broker. The connection
     * process is blocking until either a connection is established or an
     * error occurs.
     * 
     * @note All connection parameters (URI, credentials, certificates, etc.)
     *       must be set before calling this method.
     * @return True if the connection is successful, false otherwise.
     */
    bool loopStart();

    /**
     * @brief Disconnects the MQTT client from the broker and stops the event loop.
     * 
     * This call is blocking and will wait until the client is stopped cleanly.
     * It will trigger the onDisconnect callbacks before stopping.
     */
    void disconnect();

    /**
     * @brief Forcefully stops the MQTT client and disconnects from the broker.
     * 
     * This function immediately stops the MQTT client without waiting for
     * clean disconnection. This does not trigger the onDisconnect callbacks.
     * Use this only when normal disconnect() is not working.
     */
    void forceStop();
};
