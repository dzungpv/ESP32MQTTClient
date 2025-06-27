#pragma once

#include <vector>
#include <string>
#include <mqtt_client.h>
#include <functional>
#include "esp_log.h"         
#include "esp_idf_version.h" // check IDF version

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
    ESP32MQTTClient(/* args */);
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

    // Optional functionality
    void enableDebuggingMessages(const bool enabled = true);                                       // Allow to display useful debugging messages. Can be set to false to disable them during program execution
    void disablePersistence();                                                                 // Do not request a persistent connection. Connections are persistent by default. Must be called before the first loop() execution
    void enableLastWillMessage(const char *topic, const char *message, const bool retain = false); // Must be set before the first loop() call.
    void enableDrasticResetOnConnectionFailures() { _drasticResetOnConnectionFailures = true; }    // Can be usefull in special cases where the ESP board hang and need resetting (#59)

    void disableAutoReconnect();
    void setTaskPrio(int prio);

    /// Main loop, to call at each sketch loop()
    //void loop();

    // MQTT related
	void setClientCert(const char * clientCert);
	void setCaCert(const char * caCert);
	void setKey(const char * clientKey);
    void setConnectionState(bool state);
    void setAutoReconnect(bool choice);
    bool setMaxOutPacketSize(const uint16_t size);
    bool setMaxPacketSize(const uint16_t size); // override the default value of 1024
    bool publish(const std::string &topic, const std::string &payload, int qos = 0, bool retain = false);
    bool subscribe(const std::string &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos = 0);
    bool subscribe(const std::string &topic, MessageReceivedCallbackWithTopic messageReceivedCallback, uint8_t qos = 0);

    /**
     * @brief Subscribes to a topic. Server must be connected
     * for a subscription to succeed.
     *
     * @param topic The topic to subscribe to.
     * @param qos The QoS level for the subscription.
     * @return Message ID on success, -1 on failure.
     */
    int subscribe(const char *topic, int qos);

    /**
     * @brief Unsubscribes from a topic. Server must be connected
     * for an unsubscription to succeed.
     *
     * @param topic The topic to unsubscribe from.
     * @return Message ID on success, -1 on failure.
     */
    int unsubscribe(const char *topic);

    /**
     * @brief Publishes a message to a topic.
     *
     * @param topic The topic to publish to.
     * @param payload The payload for the message. Defaults to nullptr.
     * @param length The length of the payload. Defaults to 0.
     * @param qos The QoS level (0-2) for the message. If client not connected. Dropping message with QoS = 0.
     * @param retain The retain flag for the message.
     * @param async Whether to enqueue the message for asynchronous publishing.
     * Defaults to true. False means blocking until the message is published.
     * @return Message ID on success, -1 on failure.
     */
    int publish(const char *topic, const char *payload = nullptr, int length = 0, int qos = 1, bool retain = false, bool async = true);
    
    /**
     * @brief Unsubscribes from a topic. Server must be connected
     * for an unsubscription to succeed. And removes it from the CallbackList.
     *
     * @param topic The topic to unsubscribe from.
     * @return Message ID on success, -1 on failure.
     */
    bool unsubscribe(const std::string &topic); 

    /**
     * @brief Sets the keep alive interval in seconds for the MQTT connection.
     *
     * @param keepAlive The keep alive interval in seconds. Defaults to 60. Native default is 15
     * @return A reference to the PsychicMqttClient instance.
     */
    void setKeepAlive(uint16_t keepAliveSeconds = 60);

    /**
     * @brief Sets the client name (ID) manually for the MQTT connection.
     *
     * @param mqttClientName The client name (ID). Defaults to ESP32_%CHIPID% where
     * %CHIPID% are the last 3 bytes of MAC address in hex format.
     * @note On older version it may not work if not set this first.
     */
    inline void setMqttClientName(const char *mqttClientName) { _mqttClientName = mqttClientName; };

    inline void setURI(const char *uri, const char *username = "", const char *password = "")
    { // Allow setting the MQTT info manually (must be done in setup())
        _mqttUri = uri;
        _mqttUsername = username;
        _mqttPassword = password;
    };

    inline void setURL(const char *url, const uint16_t port, const char *username = "", const char *password = "")
    { // Allow setting the MQTT info manually (must be done in setup())
        char *uri = (char *)malloc(200);
        if (port == 8883)
        {
            sprintf(uri, "mqtts://%s:%u", url, port);
        }
        else
        {
            sprintf(uri, "mqtt://%s:%u", url, port);
        }
        if (_enableSerialLogs)
        {
            ESP_LOGI("ESP32MQTTClient", "MQTT uri %s", uri);
        }
        _mqttUri = uri;
        _mqttUsername = username;
        _mqttPassword = password;
    };

    /**
     * @brief Checks if the MQTT client is connected.
     *
     * @return True if the client is connected, false otherwise.
     */
    inline bool isConnected() const { return _mqttConnected; };

    /**
     * @brief Checks if the MQTT client is the right one. In the case using multi client
     *
     * @return True if the client is the right one, false otherwise.
     */
    inline bool isMyTurn(esp_mqtt_client_handle_t client) const { return _mqtt_client == client; };

    /**
     * @brief Gets the client name of the MQTT client.
     *
     * @return The client name.
     */
    inline const char *getClientName() { return _mqttClientName; };

    /**
     * @brief Gets the URI of the MQTT client.
     *
     * @return The URI.
     */
    inline const char *getURI() { return _mqttUri; };

    void printError(esp_mqtt_error_codes_t *error_handle);

    /**
     * @brief Connects the MQTT client to the server.
     *
     * @note All parameters must be set before calling this method.
     * @return True if if the connection is successful (blocking), false otherwise.
     */
    bool loopStart();

    /**
     * @brief Disconnects the MQTT client from the server.
     * This call might be blocking until the client is stopped cleanly
     */
    void disconnect();

    /**
     * @brief Forcefully stops the MQTT client and disconnects from the server.
     * This does not trigger the onDisconnect callbacks.
     */
    void forceStop();
};
