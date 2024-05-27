#include "ESP32MQTTClient.h"

ESP32MQTTClient::ESP32MQTTClient(/* args */)
{
    _mqttConnected = false;
    _mqttMaxInPacketSize = 1024;
    _mqttMaxOutPacketSize = _mqttMaxInPacketSize;
}

ESP32MQTTClient::~ESP32MQTTClient()
{
    esp_mqtt_client_destroy(_mqtt_client);
}

// =============== Configuration functions, most of them must be called before the first loop() call ==============

void ESP32MQTTClient::enableDebuggingMessages(const bool enabled)
{
    _enableSerialLogs = enabled;
}

void ESP32MQTTClient::disablePersistence()
{
    _disableMQTTCleanSession = 1;
}

void ESP32MQTTClient::enableLastWillMessage(const char *topic, const char *message, const bool retain)
{
    _mqttLastWillTopic = (char *)topic;
    _mqttLastWillMessage = (char *)message;
    _mqttLastWillRetain = retain;
}

void ESP32MQTTClient::disableAutoReconnect()
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.disable_auto_reconnect = true;
#else // IDF CHECK
    _mqtt_config.network.disable_auto_reconnect = true;
#endif // IDF CHECK
}

void ESP32MQTTClient::setTaskPrio(int prio)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.task_prio = prio;
#else  // IDF CHECK
    _mqtt_config.task.priority = prio;
#endif // IDF CHECK
}

void ESP32MQTTClient::setClientCert(const char *clientCert)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.client_cert_pem = clientCert;
#else  // IDF CHECK
    _mqtt_config.credentials.authentication.certificate = clientCert;
#endif // IDF CHECK
}

void ESP32MQTTClient::setCaCert(const char *caCert)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.cert_pem = caCert;
#else  // IDF CHECK
    _mqtt_config.broker.verification.certificate = caCert;
#endif // IDF CHECK
}

void ESP32MQTTClient::setKey(const char *clientKey)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.client_key_pem = clientKey;
#else  // IDF CHECK
    _mqtt_config.credentials.authentication.key = clientKey;
#endif // IDF CHECK
}
// =============== Public functions for interaction with thus lib =================

void ESP32MQTTClient::setConnectionState(bool state)
{
    _mqttConnected = state;
}

bool ESP32MQTTClient::setMaxOutPacketSize(const uint16_t size)
{

    _mqttMaxOutPacketSize = size;
    return true;
}

bool ESP32MQTTClient::setMaxPacketSize(const uint16_t size)
{
    _mqttMaxInPacketSize = size;
    _mqttMaxOutPacketSize = _mqttMaxInPacketSize;

    return true;
}

bool ESP32MQTTClient::publish(const String &topic, const String &payload, int qos, bool retain)
{
    // Do not try to publish if MQTT is not connected.
    if (!isConnected()) //! isConnected())
    {
        if (_enableSerialLogs)
            log_i("MQTT! Trying to publish when disconnected, skipping.");

        return false;
    }

    bool success = false;
    if (esp_mqtt_client_publish(_mqtt_client, topic.c_str(), payload.c_str(), 0, qos, retain) != -1)
    {
        success = true;
    }

    if (_enableSerialLogs)
    {
        if (success)
            log_i("MQTT << [%s] %s\n", topic.c_str(), payload.c_str());
        else
            log_i("MQTT! publish failed, is the message too long ? (see setMaxPacketSize())"); // This can occurs if the message is too long according to the maximum defined in PubsubClient.h
    }

    return success;
}

bool ESP32MQTTClient::subscribe(const String &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos)
{
    bool success = false;
    if (esp_mqtt_client_subscribe(_mqtt_client, topic.c_str(), qos) != -1)
    {
        success = true;
    }

    if (success)
    {
        // Add the record to the subscription list only if it does not exists.
        bool found = false;
        for (std::size_t i = 0; i < _topicSubscriptionList.size() && !found; i++)
            found = _topicSubscriptionList[i].topic.equals(topic);

        if (!found)
            _topicSubscriptionList.push_back({topic, messageReceivedCallback, NULL});
    }

    if (_enableSerialLogs)
    {
        if (success)
            log_i("MQTT: Subscribed to [%s]\n", topic.c_str());
        else
            log_i("MQTT! subscribe failed");
    }

    return success;
}

bool ESP32MQTTClient::subscribe(const String &topic, MessageReceivedCallbackWithTopic messageReceivedCallback, uint8_t qos)
{

    if (subscribe(topic, (MessageReceivedCallback)NULL, qos))
    {
        _topicSubscriptionList[_topicSubscriptionList.size() - 1].callbackWithTopic = messageReceivedCallback;
        return true;
    }

    return false;
}

bool ESP32MQTTClient::unsubscribe(const String &topic)
{

    // Do not try to unsubscribe if MQTT is not connected.
    if (!isConnected())
    {
        if (_enableSerialLogs)
            log_i("MQTT! Trying to unsubscribe when disconnected, skipping.");

        return false;
    }

    for (std::size_t i = 0; i < _topicSubscriptionList.size(); i++)
    {
        if (_topicSubscriptionList[i].topic.equals(topic))
        {
            if (esp_mqtt_client_unsubscribe(_mqtt_client, topic.c_str()) != -1)
            {
                _topicSubscriptionList.erase(_topicSubscriptionList.begin() + i);
                i--;

                if (_enableSerialLogs)
                    log_i("MQTT: Unsubscribed from %s\n", topic.c_str());
            }
            else
            {
                if (_enableSerialLogs)
                    log_i("MQTT! unsubscribe failed");

                return false;
            }
        }
    }

    return true;
}

void ESP32MQTTClient::setKeepAlive(uint16_t keepAliveSeconds)
{
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    _mqtt_config.keepalive = keepAliveSeconds;
#else  // IDF CHECK
    _mqtt_config.session.keepalive = keepAliveSeconds;
#endif // IDF CHECK
}

// ================== Private functions ====================-

void ESP32MQTTClient::printError(esp_mqtt_error_codes_t *error_handle)
{
    switch (error_handle->error_type)
    {
    case MQTT_ERROR_TYPE_NONE:
        log_i("ERROR TYPE: %s", "MQTT_ERROR_TYPE_NONE");
        break;
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        log_i("ERROR TYPE: %s", "MQTT_ERROR_TYPE_TCP_TRANSPORT");
        switch (error_handle->esp_transport_sock_errno)
        {
        case MQTT_ERROR_TYPE_NONE:
            log_i("ERROR CODE: %s", "MQTT_ERROR_TYPE_NONE");
            break;
        case MQTT_ERROR_TYPE_TCP_TRANSPORT:
            log_i("ERROR CODE: %s", "MQTT_ERROR_TYPE_TCP_TRANSPORT");
            break;
        case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
            log_i("ERROR CODE: %s", "MQTT_ERROR_TYPE_CONNECTION_REFUSED");
            break;

        default:
            log_i("ERROR CODE: %s", "MQTT_TRANSPORT_UNKONW_ERR");
            break;
        }

        break;
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        log_i("ERROR TYPE: %s", "MQTT_ERROR_TYPE_CONNECTION_REFUSED");
        switch (error_handle->connect_return_code)
        {
        case MQTT_CONNECTION_ACCEPTED:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_ACCEPTED");
            break;
        case MQTT_CONNECTION_REFUSE_PROTOCOL:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_PROTOCOL");
            break;
        case MQTT_CONNECTION_REFUSE_ID_REJECTED:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_ID_REJECTED");
            break;
        case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE");
            break;
        case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_BAD_USERNAME");
            break;
        case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED");
            break;

        default:
            log_i("ERROR CODE: %s", "MQTT_CONNECTION_UNKONW_ERR");
            break;
        }
        break;

    default:
        log_i("ERROR TYPE: %s", "MQTT_ERROR_TYPE_UNKOWN");
        break;
    }
}

// Try to connect to the MQTT broker and return True if the connection is successfull (blocking)
bool ESP32MQTTClient::loopStart()
{
    bool success = false;

    if (_mqttUri != nullptr)
    {
        if (_enableSerialLogs)
        {
            // if (_mqttUsername)
            //     log_i("MQTT: Connecting to broker \"%s\" with client name \"%s\" and username \"%s\" ... (%fs)", _mqttUri, _mqttClientName, _mqttUsername, millis() / 1000.0);
            // else
            //     log_i("MQTT: Connecting to broker \"%s\" with client name \"%s\" ... (%fs)", _mqttUri, _mqttClientName, millis() / 1000.0);
        }

        // explicitly set the server/port here in case they were not provided in the constructor
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        // IDF 4.x
        _mqtt_config.uri = _mqttUri;
        _mqtt_config.client_id = _mqttClientName;
        _mqtt_config.username = _mqttUsername;
        _mqtt_config.password = _mqttPassword;
        if (_mqttLastWillTopic != nullptr)
        {
            _mqtt_config.lwt_topic = _mqttLastWillTopic;
            _mqtt_config.lwt_msg = _mqttLastWillMessage;
            _mqtt_config.lwt_qos = _mqttLastWillQos;
            _mqtt_config.lwt_retain = _mqttLastWillRetain;
            _mqtt_config.lwt_msg_len = strlen(_mqttLastWillMessage);
        }
        _mqtt_config.disable_clean_session = _disableMQTTCleanSession;
        _mqtt_config.out_buffer_size = _mqttMaxOutPacketSize;
        _mqtt_config.buffer_size = _mqttMaxInPacketSize;

        _mqtt_config.event_handle = handleMQTT;
        _mqtt_client = esp_mqtt_client_init(&_mqtt_config);
#else  // IDF CHECK
       // IDF 5.x
        _mqtt_config.broker.address.uri = _mqttUri;
        _mqtt_config.credentials.client_id = _mqttClientName;
        _mqtt_config.credentials.username = _mqttUsername;
        _mqtt_config.credentials.authentication.password = _mqttPassword;
        if (_mqttLastWillTopic != nullptr)
        {
            _mqtt_config.session.last_will.topic = _mqttLastWillTopic;
            _mqtt_config.session.last_will.msg = _mqttLastWillMessage;
            _mqtt_config.session.last_will.qos = _mqttLastWillQos;
            _mqtt_config.session.last_will.retain = _mqttLastWillRetain;
            _mqtt_config.session.last_will.msg_len = strlen(_mqttLastWillMessage);
        }
        _mqtt_config.session.disable_clean_session = _disableMQTTCleanSession;
        _mqtt_config.buffer.out_size = _mqttMaxOutPacketSize;
        _mqtt_config.buffer.size = _mqttMaxInPacketSize;

        _mqtt_client = esp_mqtt_client_init(&_mqtt_config);
        esp_mqtt_client_register_event(_mqtt_client, MQTT_EVENT_ANY, handleMQTT, this);
#endif // IDF CHECK
        esp_mqtt_client_start(_mqtt_client);
    }
    else
    {
        if (_enableSerialLogs)
            log_i("MQTT: Broker server ip is not set, not connecting (%fs)\n", millis() / 1000.0);
        success = false;
    }

    if (_enableSerialLogs)
    {
        if (success)
            log_i(" - ok. (%fs) \n", millis() / 1000.0);
        else
        {

            log_i("MQTT: Retrying to connect in %i seconds.\n", _mqttReconnectionAttemptDelay / 1000);
        }
    }

    return success;
}

/**
 * Matching MQTT topics, handling the eventual presence of a single wildcard character
 *
 * @param topic1 is the topic may contain a wildcard
 * @param topic2 must not contain wildcards
 * @return true on MQTT topic match, false otherwise
 */
bool ESP32MQTTClient::mqttTopicMatch(const String &topic1, const String &topic2)
{
    int i = 0;

    if ((i = topic1.indexOf('#')) >= 0)
    {
        String t1a = topic1.substring(0, i);
        String t1b = topic1.substring(i + 1);
        if ((t1a.length() == 0 || topic2.startsWith(t1a)) &&
            (t1b.length() == 0 || topic2.endsWith(t1b)))
            return true;
    }
    else if ((i = topic1.indexOf('+')) >= 0)
    {
        String t1a = topic1.substring(0, i);
        String t1b = topic1.substring(i + 1);

        if ((t1a.length() == 0 || topic2.startsWith(t1a)) &&
            (t1b.length() == 0 || topic2.endsWith(t1b)))
        {
            if (topic2.substring(t1a.length(), topic2.length() - t1b.length()).indexOf('/') == -1)
                return true;
        }
    }
    else
    {
        return topic1.equals(topic2);
    }

    return false;
}

void ESP32MQTTClient::onMessageReceivedCallback(const char *topic, char *payload, unsigned int length)
{

    // Convert the payload into a String
    unsigned int strTerminationPos;
    if (strlen(topic) + length + 9 >= _mqttMaxInPacketSize)
    {
        strTerminationPos = length;

        if (_enableSerialLogs)
            log_i("MQTT! Your message may be truncated, please set setMaxPacketSize() to a higher value.\n");
    }
    else
        strTerminationPos = length;

    // Second, we add the string termination code at the end of the payload and we convert it to a String object
    String payloadStr;
    if (payload)
    {
        payload[strTerminationPos] = '\0';
        payloadStr = String(payload);
    }
    else
    {
        payloadStr = "";
    }
    String topicStr(topic);

    // Logging
    if (_enableSerialLogs)
        log_i("MQTT >> [%s] %s\n", topic, payloadStr.c_str());

    // Send the message to subscribers
    for (std::size_t i = 0; i < _topicSubscriptionList.size(); i++)
    {
        if (mqttTopicMatch(_topicSubscriptionList[i].topic, String(topic)))
        {
            if (_topicSubscriptionList[i].callback != NULL)
                _topicSubscriptionList[i].callback(payloadStr); // Call the callback
            if (_topicSubscriptionList[i].callbackWithTopic != NULL)
                _topicSubscriptionList[i].callbackWithTopic(topicStr, payloadStr); // Call the callback
        }
    }
}

void ESP32MQTTClient::onEventCallback(esp_mqtt_event_handle_t event)
{
    //_event = &event;
    if (event->client == _mqtt_client)
    {
        switch (event->event_id)
        {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("ESP32MQTTClient", "MQTT_EVENT_CONNECTED");
            log_i("onMqttConnect");
            setConnectionState(true);
            onMqttConnect(_mqtt_client);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI("ESP32MQTTClient", "MQTT_EVENT_DATA");
            onMessageReceivedCallback(String(event->topic).substring(0, event->topic_len).c_str(), event->data, event->data_len);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI("ESP32MQTTClient", "MQTT_EVENT_DISCONNECTED");
            setConnectionState(false);
            log_i("%s disconnected (%fs)", _mqttUri, millis() / 1000.0);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI("ESP32MQTTClient", "MQTT_EVENT_ERROR");
            printError(event->error_handle);
            break;
        default:
            break;
        }
    }
}