#include "ESP32MQTTClient.h"
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

/**
 * @brief Tag used for ESP logging throughout the ESP32MQTTClient implementation.
 */
static const char *TAG = "ESP32MQTTClient";

/**
 * @brief Maximum time disconnect() waits for the broker disconnection to be processed.
 */
static const int DISCONNECT_TIMEOUT_MS = 5000;

/**
 * @brief Keeps the esp-mqtt handle usable while an application task calls the esp-mqtt API.
 *
 * releaseClient() (called by loopStart() and the destructor) detaches the handle and waits for all
 * the ClientRef in progress to be gone before destroying it.
 */
class ESP32MQTTClient::ClientRef
{
public:
    explicit ClientRef(ESP32MQTTClient &owner) : _owner(owner)
    {
        if (_owner._mqtt_client.load(std::memory_order_relaxed) == nullptr)
            return;

        // The count is raised before the handle is read: a concurrent releaseClient() either
        // sees this user and waits for it, or detached the handle before and we read nullptr.
        _owner._clientUsers.fetch_add(1);
        _handle = _owner._mqtt_client.load();
        if (_handle == nullptr)
            _owner._clientUsers.fetch_sub(1);
        else
            _held = true;
    }
    ~ClientRef()
    {
        if (_held)
            _owner._clientUsers.fetch_sub(1);
    }
    ClientRef(const ClientRef &) = delete;
    ClientRef &operator=(const ClientRef &) = delete;

    esp_mqtt_client_handle_t get() const { return _handle; }

private:
    ESP32MQTTClient &_owner;
    esp_mqtt_client_handle_t _handle = nullptr;
    bool _held = false;
};

/**
 * @brief Detaches the esp-mqtt client from this object and destroys it.
 *
 * Waits for the application tasks still using the handle: on a stopped client they only run
 * short esp-mqtt API calls. Must not be called from the MQTT task.
 */
void ESP32MQTTClient::releaseClient()
{
    esp_mqtt_client_handle_t client = _mqtt_client.exchange(nullptr);
    if (client == nullptr)
        return;

    while (_clientUsers.load() != 0)
    {
        vTaskDelay(1);
    }
    esp_mqtt_client_destroy(client);
    _mqttStarted = false;
}

/**
 * @brief Constructs a new ESP32MQTTClient object with default settings.
 *
 * All members have default initializers in the class definition, so the object is in a
 * known state whatever its storage is (global, stack or heap).
 */
ESP32MQTTClient::ESP32MQTTClient(/* args */)
{
}

/**
 * @brief Destructor for ESP32MQTTClient.
 *
 * Cleans up MQTT client, disconnects, and frees all allocated resources.
 */
ESP32MQTTClient::~ESP32MQTTClient()
{
    if (_mqtt_client.load() != nullptr)
    {
        disconnect();
        releaseClient();
    }

    // Free the topics allocated by setOnTopicCallback()
    for (auto &callback : _onMessageCallbacks)
    {
        if (callback.topic != nullptr)
        {
            free(callback.topic); // Free the dynamically allocated topic
            callback.topic = nullptr;
        }
    }
    _onMessageCallbacks.clear(); // Clear the vector

    _topicSubscriptionList.clear();
}

// =============== Configuration functions, most of them must be called before the first loop() call ==============

/**
 * @brief Enables or disables serial debugging messages.
 *
 * @param enabled Set to true to enable serial logs, false to disable.
 */
void ESP32MQTTClient::enableDebuggingMessages(const bool enabled)
{
    _enableSerialLogs = enabled;
}

/**
 * @brief Requests a persistent MQTT session (clean session = false).
 *
 * Should be called before the first loopStart().
 */
void ESP32MQTTClient::enablePersistence()
{
    _mqttPersistentSession = true;
}

/**
 * @brief Requests a clean MQTT session (no persistence). This is the default.
 *
 * Should be called before the first loopStart().
 */
void ESP32MQTTClient::disablePersistence()
{
    _mqttPersistentSession = false;
}

/**
 * @brief Sets the MQTT last will message and topic.
 *
 * @param topic   The topic for the last will message.
 * @param message The message to send as last will.
 * @param retain  Whether the last will message should be retained.
 * @param qos     The QoS level of the last will message.
 */
void ESP32MQTTClient::enableLastWillMessage(const char *topic, const char *message, const bool retain, const int qos)
{
    _mqttLastWillTopic = topic ? topic : "";
    _mqttLastWillMessage = message ? message : "";
    _mqttLastWillRetain = retain;
    _mqttLastWillQos = qos;
    _mqttLastWillEnabled = !_mqttLastWillTopic.empty();
}

/**
 * @brief Disables automatic reconnection to the MQTT broker.
 */
void ESP32MQTTClient::disableAutoReconnect()
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.disable_auto_reconnect = true;
#else // IDF CHECK
    _mqtt_config.network.disable_auto_reconnect = true;
#endif // IDF CHECK
}

/**
 * @brief Sets the priority of the MQTT client task.
 *
 * @param prio The priority value to set.
 */
void ESP32MQTTClient::setTaskPrio(int prio)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.task_prio = prio;
#else  // IDF CHECK
    _mqtt_config.task.priority = prio;
#endif // IDF CHECK
}

/**
 * @brief Sets the client certificate for TLS/SSL connections.
 *
 * @param clientCert PEM-encoded client certificate string.
 */
void ESP32MQTTClient::setClientCert(const char *clientCert)
{
    if (clientCert) {
        _clientCertStr = clientCert;
    } else {
        _clientCertStr.clear();
    }
    const char* ptr = _clientCertStr.empty() ? nullptr : _clientCertStr.c_str();
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.client_cert_pem = ptr;
#else  // IDF CHECK
    _mqtt_config.credentials.authentication.certificate = ptr;
#endif // IDF CHECK
}

/**
 * @brief Sets the CA certificate for TLS/SSL connections.
 *
 * @param caCert PEM-encoded CA certificate string.
 */
void ESP32MQTTClient::setCaCert(const char *caCert)
{
    if (caCert) {
        _caCertStr = caCert;
    } else {
        _caCertStr.clear();
    }
    const char* ptr = _caCertStr.empty() ? nullptr : _caCertStr.c_str();
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.cert_pem = ptr;
#else  // IDF CHECK
    _mqtt_config.broker.verification.certificate = ptr;
#endif // IDF CHECK
}

/**
 * @brief Sets the client private key for TLS/SSL connections.
 *
 * @param clientKey PEM-encoded client private key string.
 */
void ESP32MQTTClient::setKey(const char *clientKey)
{
    if (clientKey) {
        _clientKeyStr = clientKey;
    } else {
        _clientKeyStr.clear();
    }
    const char* ptr = _clientKeyStr.empty() ? nullptr : _clientKeyStr.c_str();
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.client_key_pem = ptr;
#else  // IDF CHECK
    _mqtt_config.credentials.authentication.key = ptr;
#endif // IDF CHECK
}

/**
 * @brief Sets the MQTT URI including protocol and port. The strings are copied.
 *
 * @param uri The complete MQTT URI (e.g., "mqtt://broker.example.com:1883").
 * @param username The username for authentication.
 * @param password The password for authentication.
 */
void ESP32MQTTClient::setURI(const char *uri, const char *username, const char *password)
{
    _mqttUri = uri ? uri : "";
    _mqttUsername = username ? username : "";
    _mqttPassword = password ? password : "";
}

/**
 * @brief Sets the MQTT URL and generates the URI, the protocol is guessed from the port.
 *
 * @param url The MQTT broker host, optionally with a scheme ("mqtts://host") and/or a path ("host/mqtt").
 * @param port The port number.
 * @param username The username for authentication.
 * @param password The password for authentication.
 */
void ESP32MQTTClient::setURL(const char *url, const uint16_t port, const char *username, const char *password)
{
    if (url == nullptr)
    {
        ESP_LOGE(TAG, "MQTT URL is not set");
        return;
    }

    std::string host = url;
    std::string scheme;
    std::string path;

    // A scheme given explicitly in the url wins over the port based guess
    std::size_t pos = host.find("://");
    if (pos != std::string::npos)
    {
        scheme = host.substr(0, pos);
        host.erase(0, pos + 3);
    }

    // Optional path, required by most WebSocket brokers (e.g. "broker.example.com/mqtt")
    pos = host.find('/');
    if (pos != std::string::npos)
    {
        path = host.substr(pos);
        host.erase(pos);
    }

    if (scheme.empty())
    {
        // Determine the URI scheme based on the port
        switch (port)
        {
        case 8883:
            scheme = "mqtts";
            break;
        case 443:
        case 8884:
            scheme = "wss";
            break;
        case 80:
        case 1884:
            scheme = "ws";
            break;
        case 1883:
        default:
            // Fallback for unexpected ports
            scheme = "mqtt";
            break;
        }
    }

    _mqttUri = scheme + "://" + host + ":" + std::to_string(port) + path;
    _mqttUsername = username ? username : "";
    _mqttPassword = password ? password : "";

    if (_enableSerialLogs)
    {
        ESP_LOGI(TAG, "MQTT uri %s", _mqttUri.c_str());
    }
}
// =============== Public functions for interaction with this lib =================

/**
 * @brief Sets the connection state of the MQTT client.
 *
 * @param state The new connection state.
 */
void ESP32MQTTClient::setConnectionState(bool state)
{
    _mqttConnected = state;
}

/**
 * @brief Enables or disables automatic reconnection to the MQTT broker.
 *
 * @param choice Set to true to enable auto-reconnect, false to disable.
 */
void ESP32MQTTClient::setAutoReconnect(bool choice)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.disable_auto_reconnect = !choice;
#else // IDF CHECK
    _mqtt_config.network.disable_auto_reconnect = !choice;
#endif // IDF CHECK
}

/**
 * @brief Sets the maximum outgoing packet size for the MQTT client.
 *
 * @param size The new maximum outgoing packet size.
 * @return true if the size was set successfully, false otherwise.
 */
bool ESP32MQTTClient::setMaxOutPacketSize(const uint16_t size)
{

    _mqttMaxOutPacketSize = size;
    return true;
}

/**
 * @brief Sets the maximum packet size for both incoming and outgoing messages.
 *
 * @param size The new maximum packet size.
 * @return true if the size was set successfully, false otherwise.
 */
bool ESP32MQTTClient::setMaxPacketSize(const uint16_t size)
{
    _mqttMaxInPacketSize = size;
    _mqttMaxOutPacketSize = _mqttMaxInPacketSize;

    return true;
}

/**
 * @brief Publishes a message to the MQTT broker.
 *
 * @param topic The topic to publish to.
 * @param payload The payload of the message.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @param retain Whether the message should be retained.
 * @return true if the message was published successfully, false otherwise.
 */
bool ESP32MQTTClient::publish(const std::string &topic, const std::string &payload, int qos, bool retain)
{
    return publish(topic, reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), qos, retain);
}

/**
 * @brief Publishes a raw buffer to the MQTT broker.
 *
 * @param topic The topic to publish to.
 * @param payload The payload bytes, can be nullptr when payloadLength is 0.
 * @param payloadLength The number of bytes to send.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @param retain Whether the message should be retained.
 * @return true if the message was published successfully, false otherwise.
 */
bool ESP32MQTTClient::publish(const std::string &topic, const uint8_t *payload, size_t payloadLength, int qos, bool retain)
{
    if ((payload == nullptr && payloadLength > 0) ||
        payloadLength > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "Publish payload pointer or length is invalid");
        return false;
    }

    // Do not try to publish if MQTT is not connected.
    if (!isConnected()) //! isConnected())
    {
        if (_enableSerialLogs)
            ESP_LOGI(TAG, "Trying to publish when disconnected, skipping.");

        return false;
    }

    // The length is given explicitly so payloads containing '\0' are sent completely.
    // esp-mqtt takes a length of 0 as a C string and calls strlen() on the data, so an empty
    // payload is passed as nullptr: nothing is read past the buffer.
    // esp-mqtt returns the message id (>= 0) on success, -1 on failure and -2 when the outbox is full.
    const char *data = payloadLength == 0 ? nullptr : reinterpret_cast<const char *>(payload);
    ClientRef client(*this);
    const bool success = client.get() != nullptr &&
                         esp_mqtt_client_publish(client.get(), topic.c_str(), data, static_cast<int>(payloadLength), qos, retain) >= 0;

    if (_enableSerialLogs)
    {
        if (success)
            ESP_LOGI(TAG, "MQTT << [%s] (%u bytes)", topic.c_str(), (unsigned int)payloadLength);
        else
            ESP_LOGW(TAG, "Publish failed, is the message too long ? (see setMaxPacketSize())"); // This can occurs if the message is too long according to the maximum defined in PubsubClient.h
    }

    return success;
}

/**
 * @brief Common implementation of the subscribe() functions taking a callback.
 *
 * @param topic The topic to subscribe to.
 * @param callback The callback receiving the payload, or nullptr.
 * @param callbackWithTopic The callback receiving the topic and the payload, or nullptr.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @return true if the subscription was successful, false otherwise.
 */
bool ESP32MQTTClient::subscribeImpl(const std::string &topic, MessageReceivedCallback callback, MessageReceivedCallbackWithTopic callbackWithTopic, uint8_t qos)
{
    if (!isConnected())
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "Trying to subscribe when disconnected, skipping.");
        return false;
    }

    // esp-mqtt returns the message id (>= 0) on success, -1 on failure and -2 when the outbox is full.
    bool success;
    {
        ClientRef client(*this);
        success = client.get() != nullptr && esp_mqtt_client_subscribe(client.get(), topic.c_str(), qos) >= 0;
    }

    if (success)
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);

        // Add the record to the subscription list only if it does not exists.
        TopicSubscriptionRecord *record = nullptr;
        for (auto &item : _topicSubscriptionList)
        {
            if (item.topic == topic)
            {
                record = &item;
                break;
            }
        }

        if (record == nullptr)
        {
            _topicSubscriptionList.push_back({topic, std::move(callback), std::move(callbackWithTopic)});
        }
        else
        {
            // Already known (e.g. subscribed again after a reconnection): update the record of
            // this topic, and only the kind of callback that was given.
            if (callback)
                record->callback = std::move(callback);
            if (callbackWithTopic)
                record->callbackWithTopic = std::move(callbackWithTopic);
        }
    }

    if (_enableSerialLogs)
    {
        if (success)
            ESP_LOGI(TAG, "MQTT: Subscribed to [%s]", topic.c_str());
        else
            ESP_LOGW(TAG, "MQTT! subscribe failed");
    }

    return success;
}

/**
 * @brief Subscribes to a topic.
 *
 * @param topic The topic to subscribe to.
 * @param messageReceivedCallback The callback function to call when a message is received.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @return true if the subscription was successful, false otherwise.
 */
bool ESP32MQTTClient::subscribe(const std::string &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos)
{
    return subscribeImpl(topic, std::move(messageReceivedCallback), nullptr, qos);
}

/**
 * @brief Subscribes to a topic with a callback that also provides the topic.
 *
 * @param topic The topic to subscribe to.
 * @param messageReceivedCallbackWithTopic The callback function to call when a message is received, including the topic.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @return true if the subscription was successful, false otherwise.
 */
bool ESP32MQTTClient::subscribe(const std::string &topic, MessageReceivedCallbackWithTopic messageReceivedCallbackWithTopic, uint8_t qos)
{
    return subscribeImpl(topic, nullptr, std::move(messageReceivedCallbackWithTopic), qos);
}

/**
 * @brief Subscribes to a topic.
 *
 * @param topic The topic to subscribe to.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @return The message ID of the subscription, or -1 if subscription failed.
 */
int ESP32MQTTClient::subscribe(const char *topic, int qos)
{
    if (topic == nullptr)
        return -1;

    ClientRef client(*this);
    if (isConnected() && client.get() != nullptr)
    {
        if (_enableSerialLogs)
            ESP_LOGI(TAG, "Subscribing to topic %s with QoS %d", topic, qos);
        return esp_mqtt_client_subscribe(client.get(), topic, qos);
    }
    else
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT client not connected. Dropping subscription to topic %s with QoS %d.", topic, qos);
        return -1;
    }
}

/**
 * @brief Unsubscribes from a topic.
 *
 * @param topic The topic to unsubscribe from.
 * @return The message ID of the unsubscribe, or -1 if unsubscribe failed.
 */
int ESP32MQTTClient::unsubscribe(const char *topic)
{
    if (topic == nullptr)
        return -1;

    ClientRef client(*this);
    if (isConnected() && client.get() != nullptr)
    {
        if (_enableSerialLogs)
            ESP_LOGI(TAG, "Unsubscribing from topic %s", topic);
        return esp_mqtt_client_unsubscribe(client.get(), topic);
    }
    else
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT client not connected. Dropping unsubscription from topic %s.", topic);
        return -1;
    }
}


/**
 * @brief Publishes a message to the MQTT broker.
 *
 * @param topic The topic to publish to.
 * @param qos The Quality of Service level (0, 1, or 2).
 * @param retain Whether the message should be retained.
 * @param payload The payload of the message.
 * @param length The length of the payload.
 * @param async Whether the publish should be asynchronous (enqueued).
 * @return The message ID of the published message, or -1 if publish failed.
 */
int ESP32MQTTClient::publish(const char *topic, int qos, bool retain, const char *payload, int length, bool async)
{
    // A stopped client would keep the message until it is destroyed by the next loopStart()
    ClientRef client(*this);
    if (topic == nullptr || client.get() == nullptr || !_mqttStarted)
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT client not started. Dropping message.");
        return -1;
    }

    // drop message if not connected and QoS is 0
    if (!isConnected() && qos == 0)
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT client not connected. Dropping message with QoS = 0.");
        return -1;
    }

    if (async)
    {
        if (_enableSerialLogs)
            ESP_LOGV(TAG, "Enqueuing message to topic %s with QoS %d", topic, qos);
        return esp_mqtt_client_enqueue(client.get(), topic, payload, length, qos, retain, true);
    }
    else
    {
        if (_enableSerialLogs)
            ESP_LOGV(TAG, "Publishing message to topic %s with QoS %d", topic, qos);
        return esp_mqtt_client_publish(client.get(), topic, payload, length, qos, retain);
    }
}

/**
 * @brief Unsubscribes from a topic.
 *
 * @param topic The topic to unsubscribe from.
 * @return true if the unsubscribe was successful, false otherwise.
 */
bool ESP32MQTTClient::unsubscribe(const std::string &topic)
{

    // Do not try to unsubscribe if MQTT is not connected.
    if (!isConnected())
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "Trying to unsubscribe when disconnected, skipping.");

        return false;
    }

    bool found = false;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        for (std::size_t i = 0; i < _topicSubscriptionList.size() && !found; i++)
            found = _topicSubscriptionList[i].topic == topic;
        for (std::size_t i = 0; i < _onMessageCallbacks.size() && !found; i++)
            found = _onMessageCallbacks[i].topic != nullptr && topic == _onMessageCallbacks[i].topic;
    }

    // Nothing to do if the topic is not in the list
    if (!found)
        return true;

    int msgId;
    {
        ClientRef client(*this);
        msgId = client.get() != nullptr ? esp_mqtt_client_unsubscribe(client.get(), topic.c_str()) : -1;
    }
    if (msgId < 0)
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT! unsubscribe failed");

        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        for (auto it = _topicSubscriptionList.begin(); it != _topicSubscriptionList.end();)
        {
            if (it->topic == topic)
                it = _topicSubscriptionList.erase(it);
            else
                ++it;
        }
        for (auto it = _onMessageCallbacks.begin(); it != _onMessageCallbacks.end();)
        {
            if (it->topic != nullptr && topic == it->topic)
            {
                free(it->topic);
                it = _onMessageCallbacks.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT: Unsubscribed from %s", topic.c_str());

    return true;
}

/**
 * @brief Sets the keep-alive interval for the MQTT client.
 *
 * @param keepAliveSeconds The keep-alive interval in seconds.
 */
void ESP32MQTTClient::setKeepAlive(uint16_t keepAliveSeconds)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.keepalive = keepAliveSeconds;
#else  // IDF CHECK
    _mqtt_config.session.keepalive = keepAliveSeconds;
#endif // IDF CHECK
}

// ================== Private functions ====================-

/**
 * @brief Prints the error details from an MQTT error handle.
 *
 * @param error_handle The error handle containing error details.
 */
void ESP32MQTTClient::printError(esp_mqtt_error_codes_t *error_handle)
{
    if (error_handle == nullptr)
        return;

    switch (error_handle->error_type)
    {
    case MQTT_ERROR_TYPE_NONE:
        ESP_LOGE(TAG, "ERROR TYPE: %s", "MQTT_ERROR_TYPE_NONE");
        break;
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        ESP_LOGE(TAG, "ERROR TYPE: %s", "MQTT_ERROR_TYPE_TCP_TRANSPORT");
        ESP_LOGE(TAG, "ERROR CODE: esp-tls 0x%x, tls stack 0x%x, socket errno %d (%s)",
                 (unsigned int)error_handle->esp_tls_last_esp_err,
                 (unsigned int)error_handle->esp_tls_stack_err,
                 (int)error_handle->esp_transport_sock_errno,
                 strerror(error_handle->esp_transport_sock_errno));
        break;
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        ESP_LOGE(TAG, "ERROR TYPE: %s", "MQTT_ERROR_TYPE_CONNECTION_REFUSED");
        switch (error_handle->connect_return_code)
        {
        case MQTT_CONNECTION_ACCEPTED:
            ESP_LOGE(TAG, "ERROR CODE: %s", "MQTT_CONNECTION_ACCEPTED");
            break;
        case MQTT_CONNECTION_REFUSE_PROTOCOL:
            ESP_LOGE(TAG, "ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_PROTOCOL");
            break;
        case MQTT_CONNECTION_REFUSE_ID_REJECTED:
            ESP_LOGE(TAG, "ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_ID_REJECTED");
            break;
        case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
            ESP_LOGE(TAG, "ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE");
            break;
        case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
            ESP_LOGE(TAG, "ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_BAD_USERNAME");
            break;
        case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
            ESP_LOGE(TAG, "ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED");
            break;

        default:
            ESP_LOGE(TAG, "ERROR CODE: %s (%d)", "MQTT_CONNECTION_UNKONW_ERR", (int)error_handle->connect_return_code);
            break;
        }
        break;

    default:
        ESP_LOGE(TAG, "ERROR TYPE: %s (%d)", "MQTT_ERROR_TYPE_UNKOWN", (int)error_handle->error_type);
        break;
    }
}

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
/**
 * @brief Handles MQTT events for the static handleMQTT function.
 *
 * This function is registered as the event handler for the MQTT client.
 * It casts the user_context (set in loopStart()) back to the ESP32MQTTClient instance and calls onEventCallback.
 *
 * @param event The MQTT event handle.
 * @return ESP_OK on success.
 */
esp_err_t ESP32MQTTClient::handleMQTT(esp_mqtt_event_handle_t event)
{
    // Since this is a static function, the class instance is taken from the user context
    ESP32MQTTClient *instance = static_cast<ESP32MQTTClient*>(event->user_context);
    if (instance != nullptr)
        instance->onEventCallback(event);
    return ESP_OK;
}
#else
/**
 * @brief Handles MQTT events for the static handleMQTT function.
 *
 * This function is registered as the event handler for the MQTT client.
 * It casts the user_context back to the ESP32MQTTClient instance and calls onEventCallback.
 *
 * @param handler_args The user_context passed to esp_mqtt_client_register_event.
 * @param base The event base.
 * @param event_id The event ID.
 * @param event_data The event data (esp_mqtt_event_handle_t).
 */
void ESP32MQTTClient::handleMQTT(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    // Since this is a static function, we need to cast the first argument (void*) back to the class instance type
    ESP32MQTTClient *instance = static_cast<ESP32MQTTClient*>(handler_args);
    if (instance != nullptr && event != nullptr)
        instance->onEventCallback(event);
}
#endif

/**
 * @brief Adds a callback for when the MQTT client connects.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnConnectCallback(OnConnectCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onConnectCallbacks.push_back(std::move(callback));
}

/**
 * @brief Adds a callback for when the MQTT client disconnects.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnDisconnectCallback(OnDisconnectCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onDisconnectCallbacks.push_back(std::move(callback));
}

/**
 * @brief Adds a callback for when the MQTT client subscribes.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnSubscribeCallback(OnSubscribeCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onSubscribeCallbacks.push_back(std::move(callback));
}

/**
 * @brief Adds a callback for when the MQTT client unsubscribes.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnUnsubscribeCallback(OnUnsubscribeCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onUnsubscribeCallbacks.push_back(std::move(callback));
}

/**
 * @brief Adds a callback for when a message is received, whatever its topic.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnMessageCallback(OnMessageCallback callback)
{
    // A record without topic is a global callback
    OnMessageCallback_t subscription = {nullptr, 0, std::move(callback)};
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onMessageCallbacks.push_back(subscription);
}

/**
 * @brief Sets a callback for a specific topic.
 *
 * @param topic The topic to subscribe to.
 * @param qos The Quality of Service level.
 * @param callback The callback function to call when a message is received for this topic.
 */
void ESP32MQTTClient::setOnTopicCallback(const char *topic, int qos, OnMessageCallback callback)
{
    if (!topic)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_callbackMutex);

        // Add the record to the subscription list and subscribe only if it does not exists.
        // Records without topic are the global callbacks of setOnMessageCallback().
        for (const auto &record : _onMessageCallbacks)
        {
            if (record.topic != nullptr && strcmp(record.topic, topic) == 0)
                return;
        }

        char *topicCopy = strdup(topic);
        if (!topicCopy)
        {
            // handle allocation failure (out of memory)
            ESP_LOGE(TAG, "Out of memory, callback of topic %s not registered", topic);
            return;
        }
        OnMessageCallback_t subscription = {topicCopy, qos, std::move(callback)};
        _onMessageCallbacks.push_back(subscription);
    }

    if (isConnected())
    {
        subscribe(topic, qos);
    }
}

/**
 * @brief Adds a callback for when a message is published.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnPublishCallback(OnPublishCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onPublishCallbacks.push_back(std::move(callback));
}

/**
 * @brief Adds a callback for when an error occurs.
 *
 * @param callback The callback function to add.
 */
void ESP32MQTTClient::setOnErrorCallback(OnErrorCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _onErrorCallbacks.push_back(std::move(callback));
}

/**
 * @brief Creates and starts the MQTT client.
 *
 * The connection to the broker is established in the background by the esp-mqtt task.
 * Calling it again while the client is running only asks for an immediate reconnection.
 *
 * @return true if the client was started, false otherwise.
 */
bool ESP32MQTTClient::loopStart()
{
    bool success = false;
    esp_err_t err = ESP_OK;

    esp_mqtt_client_handle_t client = _mqtt_client.load();
    if (client != nullptr)
    {
        if (_mqttStarted)
        {
            // Already running: do not create a second client (it would be leaked and would fight
            // with the first one for the same client id). Only ask for an immediate reconnection,
            // which is a no-op if the client is not waiting to reconnect.
            if (_enableSerialLogs)
                ESP_LOGI(TAG, "MQTT client already started, reconnection requested. Call disconnect() first to apply new settings.");
            esp_mqtt_client_reconnect(client);
            return true;
        }

        // Stopped by disconnect() or forceStop(): release it, a new one is created with the current settings.
        // (A stopped esp-mqtt client has already discarded its queued messages.)
        releaseClient();
    }

    if (!_mqttUri.empty())
    {
        if (_enableSerialLogs)
        {
            if (!_mqttUsername.empty())
                ESP_LOGW(TAG, "Connecting to broker %s with client name %s and username %s ... (%lus)", _mqttUri.c_str(), _mqttClientName.c_str(), _mqttUsername.c_str(), (unsigned long)(esp_timer_get_time() / 1000000));
            else
                ESP_LOGW(TAG, "Connecting to broker %s with client name %s ... (%lus)", _mqttUri.c_str(), _mqttClientName.c_str(), (unsigned long)(esp_timer_get_time() / 1000000));
        }

        // Empty values are passed as NULL so esp-mqtt uses its defaults (generated client id, no credentials).
        // esp_mqtt_client_init() makes its own copy of all these strings.
        const char *clientName = _mqttClientName.empty() ? nullptr : _mqttClientName.c_str();
        const char *username = _mqttUsername.empty() ? nullptr : _mqttUsername.c_str();
        const char *password = _mqttPassword.empty() ? nullptr : _mqttPassword.c_str();

        // explicitly set the server/port here in case they were not provided in the constructor
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        // IDF 4.x
        _mqtt_config.uri = _mqttUri.c_str();
        _mqtt_config.client_id = clientName;
        _mqtt_config.username = username;
        _mqtt_config.password = password;
        if (_mqttLastWillEnabled)
        {
            _mqtt_config.lwt_topic = _mqttLastWillTopic.c_str();
            _mqtt_config.lwt_msg = _mqttLastWillMessage.c_str();
            _mqtt_config.lwt_qos = _mqttLastWillQos;
            _mqtt_config.lwt_retain = _mqttLastWillRetain;
            _mqtt_config.lwt_msg_len = static_cast<int>(_mqttLastWillMessage.size());
        }
        // A persistent session is requested by disabling the clean session flag
        _mqtt_config.disable_clean_session = _mqttPersistentSession ? 1 : 0;
        _mqtt_config.out_buffer_size = _mqttMaxOutPacketSize;
        _mqtt_config.buffer_size = _mqttMaxInPacketSize;

        // Ensure persistent cert pointers are used
        _mqtt_config.cert_pem = _caCertStr.empty() ? nullptr : _caCertStr.c_str();
        _mqtt_config.client_cert_pem = _clientCertStr.empty() ? nullptr : _clientCertStr.c_str();
        _mqtt_config.client_key_pem = _clientKeyStr.empty() ? nullptr : _clientKeyStr.c_str();

        // The legacy event callback gets the instance back through event->user_context
        _mqtt_config.user_context = this;
        _mqtt_config.event_handle = handleMQTT;
        client = esp_mqtt_client_init(&_mqtt_config);
        if (client == nullptr) {
            ESP_LOGE(TAG, "esp_mqtt_client_init failed");
            err = ESP_FAIL;
        }
#else  // IDF CHECK
       // IDF 5.x
        _mqtt_config.broker.address.uri = _mqttUri.c_str();
        _mqtt_config.credentials.client_id = clientName;
        _mqtt_config.credentials.username = username;
        _mqtt_config.credentials.authentication.password = password;
        if (_mqttLastWillEnabled)
        {
            _mqtt_config.session.last_will.topic = _mqttLastWillTopic.c_str();
            _mqtt_config.session.last_will.msg = _mqttLastWillMessage.c_str();
            _mqtt_config.session.last_will.qos = _mqttLastWillQos;
            _mqtt_config.session.last_will.retain = _mqttLastWillRetain;
            _mqtt_config.session.last_will.msg_len = static_cast<int>(_mqttLastWillMessage.size());
        }
        // A persistent session is requested by disabling the clean session flag
        _mqtt_config.session.disable_clean_session = _mqttPersistentSession;
        _mqtt_config.buffer.out_size = _mqttMaxOutPacketSize;
        _mqtt_config.buffer.size = _mqttMaxInPacketSize;

        // Ensure persistent cert pointers are used
        _mqtt_config.broker.verification.certificate = _caCertStr.empty() ? nullptr : _caCertStr.c_str();
        _mqtt_config.credentials.authentication.certificate = _clientCertStr.empty() ? nullptr : _clientCertStr.c_str();
        _mqtt_config.credentials.authentication.key = _clientKeyStr.empty() ? nullptr : _clientKeyStr.c_str();

        client = esp_mqtt_client_init(&_mqtt_config);
        if (client == nullptr) {
            ESP_LOGE(TAG, "esp_mqtt_client_init failed");
            err = ESP_FAIL;
        } else {
            err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, handleMQTT, this);
        }
#endif // IDF CHECK
        if (client != nullptr && err == ESP_OK)
        {
            // Published before the start: the events of the new client are compared with it
            _mqtt_client = client;
            _mqttClientStop = false;
            _mqttStarted = true;
            err = esp_mqtt_client_start(client);
            success = (err == ESP_OK);
            if (!success)
            {
                // Do not keep a client that did not start, so loopStart() can be called again
                releaseClient();
            }
        }
        else if (client != nullptr)
        {
            // Never published, nobody else can use it
            esp_mqtt_client_destroy(client);
        }
    }
    else
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "Broker server ip is not set, not connecting (%lus)", (unsigned long)(esp_timer_get_time() / 1000000));
        success = false;
    }

    if (_enableSerialLogs)
    {
        if (success)
            ESP_LOGI(TAG, "Connection ok. (%lus)", (unsigned long)(esp_timer_get_time() / 1000000));
        else
            ESP_LOGE(TAG, "Connection failed, error code: %d", (int)err);
    }

    return success;
}

/**
 * @brief Disconnects the MQTT client.
 *
 * This function initiates a clean disconnect of the MQTT client.
 * It waits (with a timeout) for the disconnect event to be processed, then stops the client.
 */
void ESP32MQTTClient::disconnect()
{
    esp_mqtt_client_handle_t client = _mqtt_client.load();
    if (client == nullptr)
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT client not started.");
        return;
    }

    // Reject new messages from publish() immediately while disconnecting
    const bool wasStarted = _mqttStarted.exchange(false);

    if (isConnected())
    {
        if (_enableSerialLogs)
            ESP_LOGI(TAG, "Disconnecting MQTT client.");
        // The flag is set again by the MQTT_EVENT_DISCONNECTED of this disconnection
        _mqttClientStop = false;
        setConnectionState(false);
        esp_mqtt_client_disconnect(client);

        // Wait for the disconnect event to be processed. The wait is limited: the event never
        // comes if the MQTT task is blocked, or if this function is called from a MQTT callback.
        // The time is measured, a 10 ms delay can be shorter or longer depending on the tick rate.
        const TickType_t delayTicks = pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1;
        const int64_t deadline = esp_timer_get_time() + (int64_t)DISCONNECT_TIMEOUT_MS * 1000;
        while (!_mqttClientStop && esp_timer_get_time() < deadline)
        {
            vTaskDelay(delayTicks);
        }
        if (!_mqttClientStop && _enableSerialLogs)
            ESP_LOGW(TAG, "No disconnect event after %d ms, stopping anyway.", DISCONNECT_TIMEOUT_MS);
    }

    if (wasStarted)
    {
        if (esp_mqtt_client_stop(client) == ESP_OK)
        {
            if (_enableSerialLogs)
                ESP_LOGI(TAG, "MQTT client stopped.");
        }
        else
        {
            _mqttStarted = true;
            ESP_LOGE(TAG, "MQTT client could not be stopped (disconnect() must not be called from a MQTT callback).");
        }
    }
}

/**
 * @brief Forces the MQTT client to stop.
 *
 * This function stops the MQTT client immediately, regardless of its current state.
 */
void ESP32MQTTClient::forceStop()
{
    esp_mqtt_client_handle_t client = _mqtt_client.load();
    if (client == nullptr)
    {
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT client not started.");
        return;
    }

    if (isConnected())
    {
        if (_enableSerialLogs)
            ESP_LOGI(TAG, "Forced stop MQTT client.");
    }
    if (_mqttStarted)
    {
        const esp_err_t err = esp_mqtt_client_stop(client);
        ESP_ERROR_CHECK_WITHOUT_ABORT(err);
        if (err == ESP_OK)
            _mqttStarted = false;
    }
    setConnectionState(false);
    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT client forcefully stopped.");
}

/**
 * @brief Topic matching on raw strings, used without building std::string objects for each message.
 *
 * @param filter The topic filter, it may contain wildcards.
 * @param filterLen The length of the filter.
 * @param topic The topic of a received message.
 * @param topicLen The length of the topic.
 * @return true on MQTT topic match, false otherwise.
 */
static bool matchTopic(const char *filter, std::size_t filterLen, const char *topic, std::size_t topicLen)
{
    if (filterLen == 0)
        return false;

    if (topicLen > 0 && topic[0] == '$' && (filter[0] == '#' || filter[0] == '+'))
        return false;

    std::size_t i = 0; // index in the filter (pattern)
    std::size_t j = 0; // index in the topic (actual)

    while (i < filterLen)
    {
        const char c = filter[i];

        if (c == '#')
        {
            // '#' matches all the remaining levels, it is only valid as the last character
            return i + 1 == filterLen;
        }

        if (c == '+')
        {
            // '+' matches exactly one level.
            // That level continues until the next '/' or end of the topic
            while (j < topicLen && topic[j] != '/')
                j++;
            i++; // Move past the '+' in pattern
            continue;
        }

        if (j >= topicLen)
        {
            // Topic exhausted: the only remaining match is the parent level of a trailing "/#" ("foo/#" matches "foo")
            return c == '/' && i + 2 == filterLen && filter[i + 1] == '#';
        }

        if (topic[j] != c)
            return false;

        i++;
        j++;
    }

    return j == topicLen;
}

/**
 * @brief Matches MQTT topics following the MQTT specification (section 4.7).
 *
 *  - '+' matches exactly one topic level (which can be empty), it can be used several times
 *  - '#' matches any number of levels including the parent level, it must be the last character
 *  - topics beginning with '$' are not matched by a filter starting with a wildcard
 *
 * @param topic1 is the topic filter, it may contain wildcards.
 * @param topic2 is the topic of a received message, it must not contain wildcards.
 * @return true on MQTT topic match, false otherwise.
 */
bool ESP32MQTTClient::mqttTopicMatch(const std::string &topic1, const std::string &topic2)
{
    return matchTopic(topic1.data(), topic1.size(), topic2.data(), topic2.size());
}

/**
 * @brief Delivers the complete received message (_rxTopic / _rxPayload) to the callbacks.
 *
 * Called on the MQTT task once all the parts of a message were received.
 * The callbacks are copied under the lock and called without it, so they are free to
 * register / remove callbacks, and the lists can be changed by another task at any time.
 *
 * @param event The MQTT event of the (last part of the) message.
 */
void ESP32MQTTClient::onMessageReceivedCallback(esp_mqtt_event_handle_t event)
{
    // Logging
    if (_enableSerialLogs)
    {
        ESP_LOGI(TAG, "MQTT >> [%s] %s", _rxTopic.c_str(), _rxPayload.c_str());
    }

    // The vectors of the previous message are reused, so no memory is allocated for each message.
    // They are moved out first, in case a callback leads to a nested call.
    auto subscribers = std::move(_rxSubscribersCache);
    auto messageCallbacks = std::move(_rxMessageCallbacksCache);
    subscribers.clear();
    messageCallbacks.clear();
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        for (const auto &record : _topicSubscriptionList)
        {
            if (matchTopic(record.topic.data(), record.topic.size(), _rxTopic.data(), _rxTopic.size()))
                subscribers.emplace_back(record.callback, record.callbackWithTopic);
        }
        for (const auto &record : _onMessageCallbacks)
        {
            // A record without topic is a global callback, it gets all the messages
            if (record.callback && (record.topic == nullptr || matchTopic(record.topic, strlen(record.topic), _rxTopic.data(), _rxTopic.size())))
                messageCallbacks.push_back(record.callback);
        }
    }

    // Send the message to subscribers
    for (const auto &subscriber : subscribers)
    {
        if (subscriber.first)
            subscriber.first(_rxPayload); // Call the callback
        if (subscriber.second)
            subscriber.second(_rxTopic, _rxPayload); // Call the callback
    }

    // std::string keeps a '\0' after its content, so the buffers can be given as C strings
    for (const auto &callback : messageCallbacks)
    {
        callback(event->client, &_rxTopic[0], &_rxPayload[0], event->retain, event->qos, event->dup);
    }

    subscribers.clear();
    messageCallbacks.clear();
    _rxSubscribersCache = std::move(subscribers);
    _rxMessageCallbacksCache = std::move(messageCallbacks);
}

/**
 * @brief Callback function for handling MQTT events.
 *
 * This function is registered as the event handler for the MQTT client.
 * It processes different event types (CONNECTED, DATA, SUBSCRIBED, etc.)
 * and calls the appropriate callback functions.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::onEventCallback(esp_mqtt_event_handle_t event)
{
    //_event = &event;
    if (event->client == _mqtt_client.load())
    {
        switch (event->event_id)
        {
        case MQTT_EVENT_BEFORE_CONNECT:
        {
            break;
        }
        case MQTT_EVENT_CONNECTED:
        {
            if (_enableSerialLogs)
                ESP_LOGI(TAG, "MQTT -->> onMqttConnect");
            setConnectionState(true);
            _onConnect(event);
            break;
        }
        case MQTT_EVENT_DATA:
        {
            if (_enableSerialLogs)
                ESP_LOGI(TAG, "MQTT -->> onMqttEventData");
            _onMessage(event);
            break;
        }
        case MQTT_EVENT_SUBSCRIBED:
        {
            _onSubscribe(event);
            break;
        }
        case MQTT_EVENT_UNSUBSCRIBED:
        {
            _onUnsubscribe(event);
            break;
        }
        case MQTT_EVENT_PUBLISHED:
        {
            _onPublish(event);
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
        {
            setConnectionState(false);
            // A message that was being received in several parts is lost
            _rxInProgress = false;
            _rxDropping = false;
            _rxPayload.clear();
            _onDisconnect(event);
            // The URI is not logged here: this runs on the MQTT task, while setURI() may run on another one
            if (_enableSerialLogs)
                ESP_LOGW(TAG, "MQTT -->> disconnected (%lus)", (unsigned long)(esp_timer_get_time() / 1000000));
            break;
        }
        case MQTT_EVENT_ERROR:
        {
            if (_enableSerialLogs)
            {
                printError(event->error_handle);
            }
            // The connection state is not changed here: an error is not always a disconnection
            // (e.g. a refused subscription), and a lost connection sends MQTT_EVENT_DISCONNECTED.
            _onError(event);
            break;
        }
        case MQTT_EVENT_DELETED:
        {
            break;
        }
        default:
            break;
        }
    }
}

/**
 * @brief Callback function for when the MQTT client connects.
 *
 * This function is called when the MQTT client successfully connects to the broker.
 * It resubscribes to all topics that were subscribed via setOnTopicCallback.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onConnect(esp_mqtt_event_handle_t &event)
{
    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

    std::vector<std::pair<std::string, int>> topics;
    std::vector<OnConnectCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        for (const auto &record : _onMessageCallbacks)
        {
            if (record.topic != nullptr)
                topics.emplace_back(record.topic, record.qos);
        }
        callbacks = _onConnectCallbacks;
    }

    // Resubscribe to all topics
    for (const auto &topic : topics)
    {
        subscribe(topic.first.c_str(), topic.second);
    }

    for (const auto &callback : callbacks)
    {
        if (callback)
            callback(event->client, event->session_present);
    }
}

/**
 * @brief Callback function for when the MQTT client disconnects.
 *
 * This function is called when the MQTT client disconnects from the broker.
 * It calls all registered disconnect callbacks.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onDisconnect(esp_mqtt_event_handle_t &event)
{
    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

    std::vector<OnDisconnectCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        callbacks = _onDisconnectCallbacks;
    }
    for (const auto &callback : callbacks)
    {
        if (callback)
            callback(event->client, event->session_present);
    }
    _mqttClientStop = true;
}

/**
 * @brief Callback function for when the MQTT client subscribes.
 *
 * This function is called when the MQTT client successfully subscribes to a topic.
 * It calls all registered subscribe callbacks.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onSubscribe(esp_mqtt_event_handle_t &event)
{
    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", (int)event->msg_id);

    std::vector<OnSubscribeCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        callbacks = _onSubscribeCallbacks;
    }
    for (const auto &callback : callbacks)
    {
        if (callback)
            callback(event->client, event->msg_id);
    }
}

/**
 * @brief Callback function for when the MQTT client unsubscribes.
 *
 * This function is called when the MQTT client successfully unsubscribes from a topic.
 * It calls all registered unsubscribe callbacks.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onUnsubscribe(esp_mqtt_event_handle_t &event)
{
    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", (int)event->msg_id);

    std::vector<OnUnsubscribeCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        callbacks = _onUnsubscribeCallbacks;
    }
    for (const auto &callback : callbacks)
    {
        if (callback)
            callback(event->client, event->msg_id);
    }
}

/**
 * @brief Callback function for when a message is received.
 *
 * A message bigger than the receive buffer is delivered by esp-mqtt in several events: only the
 * first one carries the topic, the next ones carry the following parts of the payload. The parts
 * are joined here, so all the callbacks get one complete message.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onMessage(esp_mqtt_event_handle_t &event)
{
    const std::size_t len = (event->data != nullptr && event->data_len > 0) ? static_cast<std::size_t>(event->data_len) : 0;
    const std::size_t total = event->total_data_len > 0 ? static_cast<std::size_t>(event->total_data_len) : 0;
    const std::size_t offset = event->current_data_offset > 0 ? static_cast<std::size_t>(event->current_data_offset) : 0;

    if (offset == 0 && event->topic != nullptr && event->topic_len > 0)
    {
        // First (and most of the time only) part of a message. The topic alone does not mark it:
        // with CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS esp-mqtt sends the topic with every part.
        _rxTopic.assign(event->topic, static_cast<std::size_t>(event->topic_len));
        _rxPayload.clear();
        _rxTotal = total;
        _rxReceived = 0;
        _rxInProgress = true;
        _rxDropping = false;

        if (total > len && _enableSerialLogs)
            ESP_LOGV(TAG, "MQTT_EVENT_DATA_MULTIPART_FIRST");

        if (total > _rxPayload.capacity())
        {
            // Make sure the complete message (plus string terminator and allocator overhead) can be stored
            const std::size_t freeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            if (freeBlock <= 64 || total > freeBlock - 64)
            {
                _rxDropping = true;
                ESP_LOGE(TAG, "MQTT! Message of %u bytes on [%s] dropped, not enough memory", (unsigned int)total, _rxTopic.c_str());
            }
            else
            {
                _rxPayload.reserve(total);
            }
        }
    }
    else if (!_rxInProgress)
    {
        // Next part of a message whose beginning was not seen (e.g. lost with a disconnection)
        if (_enableSerialLogs)
            ESP_LOGW(TAG, "MQTT! Part of a message received without its beginning, dropped.");
        return;
    }
    else if (offset != _rxReceived)
    {
        // A part is missing, the message can not be rebuilt. The next parts are still counted,
        // so the receive state is reset with the last one.
        _rxDropping = true;
        _rxReceived = offset;
    }
    // Otherwise it is the next part of the message in progress. Its offset can be 0 if the
    // first part had no payload byte (topic filling the whole receive buffer).

    if (offset + len > _rxTotal)
        _rxDropping = true;

    // The payload is copied with its length: nothing is written in the esp-mqtt buffer
    if (!_rxDropping && len > 0)
        _rxPayload.append(event->data, len);
    _rxReceived += len;

    if (_rxReceived < _rxTotal)
    {
        if (_enableSerialLogs)
            ESP_LOGV(TAG, "MQTT_EVENT_DATA_MULTIPART");
        return; // wait for the next part
    }

    _rxInProgress = false;
    if (!_rxDropping)
        onMessageReceivedCallback(event);

    // Give back the memory of big messages, keep the small buffer for the next ones
    _rxDropping = false;
    if (_rxPayload.capacity() > static_cast<std::size_t>(_mqttMaxInPacketSize > 0 ? _mqttMaxInPacketSize : 0))
        std::string().swap(_rxPayload);
    else
        _rxPayload.clear();
}

/**
 * @brief Callback function for when a message is published.
 *
 * This function is called when a message is successfully published to the MQTT broker.
 * It calls all registered publish callbacks.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onPublish(esp_mqtt_event_handle_t &event)
{
    if (_enableSerialLogs)
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", (int)event->msg_id);

    std::vector<OnPublishCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        callbacks = _onPublishCallbacks;
    }
    for (const auto &callback : callbacks)
    {
        if (callback)
            callback(event->client, event->msg_id);
    }
}

/**
 * @brief Callback function for when an error occurs.
 *
 * This function is called when an error event is received from the MQTT client.
 * It calls all registered error callbacks, whatever the error type.
 *
 * @param event The MQTT event handle.
 */
void ESP32MQTTClient::_onError(esp_mqtt_event_handle_t &event)
{
    if (_enableSerialLogs)
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
    if (event->error_handle == nullptr)
        return;
    // The error details are printed by printError() in onEventCallback() when the logs are enabled

    std::vector<OnErrorCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        callbacks = _onErrorCallbacks;
    }
    for (const auto &callback : callbacks)
    {
        if (callback)
            callback(event->client, *event->error_handle);
    }
}
