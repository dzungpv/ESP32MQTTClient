#pragma once

#include <vector>
#include <string>
#include <mqtt_client.h>
#include <functional>
#include "esp_log.h"         
#include "esp_idf_version.h" // check IDF version

void onMqttConnect(esp_mqtt_client_handle_t client);

typedef std::function<void(const std::string &message)> MessageReceivedCallback;
typedef std::function<void(const std::string &topicStr, const std::string &message)> MessageReceivedCallbackWithTopic;

class ESP32MQTTClient
{
private:
    esp_mqtt_client_config_t _mqtt_config; // C so different naming
    esp_mqtt_client_handle_t _mqtt_client;
    MessageReceivedCallbackWithTopic _globalMessageReceivedCallback = nullptr;
	

    // MQTT related
    bool _mqttConnected;
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

public:
    ESP32MQTTClient(/* args */);
    ~ESP32MQTTClient();

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
    void setOnMessageCallback(MessageReceivedCallbackWithTopic callback);
    void setConnectionState(bool state);
    void setAutoReconnect(bool choice);
    bool setMaxOutPacketSize(const uint16_t size);
    bool setMaxPacketSize(const uint16_t size); // override the default value of 1024
    bool publish(const std::string &topic, const std::string &payload, int qos = 0, bool retain = false);
    bool subscribe(const std::string &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos = 0);
    bool subscribe(const std::string &topic, MessageReceivedCallbackWithTopic messageReceivedCallback, uint8_t qos = 0);
    bool unsubscribe(const std::string &topic);                                       // Unsubscribes from the topic, if it exists, and removes it from the CallbackList.
    void setKeepAlive(uint16_t keepAliveSeconds);                                // Change the keepalive interval (15 seconds by default)
    inline void setMqttClientName(const char *name) { _mqttClientName = name; }; // Allow to set client name manually (must be done in setup(), else it will not work.)

    /** Set mqtt uri include procotol and port **/
    inline void setURI(const char *uri, const char *username = "", const char *password = "")
    { 
        _mqttUri = uri;
        _mqttUsername = username;
        _mqttPassword = password;
    };

    /** Set mqtt url in raw string and lib will generate uri base on port **/
    inline void setURL(const char *url, const uint16_t port, const char *username = "", const char *password = "")
    { 
        char *uri = (char *)malloc(200);
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
        snprintf(uri, 200, "%s://%s:%u", scheme, url, port);
        if (_enableSerialLogs)
        {
            ESP_LOGI("ESP32MQTTClient", "MQTT uri %s", uri);
        }
        _mqttUri = uri;
        _mqttUsername = username;
        _mqttPassword = password;
    };

    inline bool isConnected() const { return _mqttConnected; };    
    inline bool isMyTurn(esp_mqtt_client_handle_t client) const { return _mqtt_client==client; }; // Return true if mqtt is connected

    inline const char *getClientName() { return _mqttClientName; };
    inline const char *getURI() { return _mqttUri; };

    void printError(esp_mqtt_error_codes_t *error_handle);
    
    bool loopStart();    
};
