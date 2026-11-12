// Host tests of ESP32MQTTClient.
//
// The library is compiled with the real mqtt_client.h of ESP-IDF (4.4 or 5.3, see the Makefile)
// and driven through the fake esp-mqtt implementation below, which delivers events the same
// way the real esp-mqtt task does. Run them with: make -C test/host
#include "ESP32MQTTClient.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define IDF4 (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))

int g_log_verbose = 0;
static std::atomic<int> g_fail{0};
#define CHECK(cond)                                                                           \
    do                                                                                        \
    {                                                                                         \
        if (!(cond))                                                                          \
        {                                                                                     \
            g_fail++;                                                                         \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
        }                                                                                     \
    } while (0)

// ---------------------------------------------------------------- fake esp-mqtt
struct esp_mqtt_client
{
    esp_mqtt_client_config_t cfg;
    std::string uri, client_id, username, password, lwt_topic, lwt_msg;
    bool has_client_id = false, has_username = false, has_password = false;
#if !IDF4
    esp_event_handler_t handler = nullptr;
    void *handler_arg = nullptr;
#endif
    bool started = false;
    int reconnects = 0, stops = 0, starts = 0;
    bool disconnect_requested = false;
    std::vector<std::pair<std::string, std::string>> enqueued;
    std::vector<std::string> subs, unsubs;
    std::vector<std::pair<std::string, std::string>> pubs;
};

static int g_init = 0, g_destroy = 0, g_publish_ret = 7, g_subscribe_ret = 3;
static size_t g_largest_free = 1 << 20;
static esp_mqtt_client *g_last = nullptr;       // last client created
static std::vector<esp_mqtt_client *> g_live;    // all the clients not destroyed (several instances)
static std::mutex g_fake_mutex; // the real esp-mqtt API is locked internally
static std::string g_order;
static std::atomic<int> g_delays{0};
static std::atomic<int64_t> g_now_us{42000000}; // fake clock, advanced by vTaskDelay()
static bool g_fire_disconnect = true;
static void fireSimple(esp_mqtt_client *c, esp_mqtt_event_id_t id);

extern "C"
{
    int64_t esp_timer_get_time(void) { return g_now_us.load(); }
    size_t heap_caps_get_largest_free_block(uint32_t) { return g_largest_free; }

    void vTaskDelay(TickType_t ticks)
    {
        g_delays++;
        g_now_us += (int64_t)ticks * portTICK_PERIOD_MS * 1000;
        std::this_thread::yield();
        // The "MQTT task" of each client processes its requested disconnection while the caller waits
        if (!g_fire_disconnect || g_delays < 3)
            return;
        for (esp_mqtt_client *c : std::vector<esp_mqtt_client *>(g_live))
        {
            if (c->disconnect_requested)
            {
                c->disconnect_requested = false;
                fireSimple(c, MQTT_EVENT_DISCONNECTED);
            }
        }
    }

    esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
    {
        g_init++;
        auto *c = new esp_mqtt_client();
        c->cfg = *config;
#if IDF4
        const char *uri = config->uri, *id = config->client_id, *user = config->username, *pass = config->password;
        const char *lt = config->lwt_topic, *lm = config->lwt_msg;
#else
        const char *uri = config->broker.address.uri, *id = config->credentials.client_id, *user = config->credentials.username;
        const char *pass = config->credentials.authentication.password;
        const char *lt = config->session.last_will.topic, *lm = config->session.last_will.msg;
#endif
        c->uri = uri ? uri : "";
        c->has_client_id = id != nullptr;
        c->client_id = id ? id : "";
        c->has_username = user != nullptr;
        c->username = user ? user : "";
        c->has_password = pass != nullptr;
        c->password = pass ? pass : "";
        c->lwt_topic = lt ? lt : "";
        c->lwt_msg = lm ? lm : "";
        g_last = c;
        g_live.push_back(c);
        return c;
    }
    esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t, esp_event_handler_t h, void *arg)
    {
        if (!client)
            return ESP_ERR_INVALID_ARG;
#if !IDF4
        client->handler = h;
        client->handler_arg = arg;
#else
        (void)h;
        (void)arg;
#endif
        return ESP_OK;
    }
    esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
    {
        client->started = true;
        client->starts++;
        return ESP_OK;
    }
    esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
    {
        g_order += "stop;";
        if (!client->started)
            return ESP_FAIL;
        client->started = false;
        client->stops++;
        return ESP_OK;
    }
    esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client)
    {
        client->disconnect_requested = true;
        return ESP_OK;
    }
    esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t client)
    {
        client->reconnects++;
        return ESP_FAIL;
    }
    esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
    {
        g_destroy++;
        if (g_last == client)
            g_last = nullptr;
        for (auto it = g_live.begin(); it != g_live.end(); ++it)
        {
            if (*it == client)
            {
                g_live.erase(it);
                break;
            }
        }
        delete client;
        return ESP_OK;
    }
    int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic, const char *data, int len, int, int, bool)
    {
        std::lock_guard<std::mutex> l(g_fake_mutex);
        client->enqueued.emplace_back(topic, std::string(data ? data : "", (size_t)(len ? len : (data ? (int)strlen(data) : 0))));
        return 9;
    }
    int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic, const char *data, int len, int, int)
    {
        std::lock_guard<std::mutex> l(g_fake_mutex);
        if (g_publish_ret >= 0)
            client->pubs.emplace_back(topic, std::string(data, (size_t)(len ? len : (int)strlen(data))));
        return g_publish_ret;
    }
#if IDF4
    int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int)
#else
    int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t client, const char *topic, int)
#endif
    {
        std::lock_guard<std::mutex> l(g_fake_mutex);
        if (g_subscribe_ret >= 0)
            client->subs.emplace_back(topic);
        return g_subscribe_ret;
    }
    int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t client, const char *topic)
    {
        std::lock_guard<std::mutex> l(g_fake_mutex);
        client->unsubs.emplace_back(topic);
        return 5;
    }
}

// Deliver one event the way esp-mqtt does for this IDF version
static void fire(esp_mqtt_client *c, esp_mqtt_event_t &ev)
{
    ev.client = c;
#if IDF4
    ev.user_context = c->cfg.user_context;
    c->cfg.event_handle(&ev);
#else
    c->handler(c->handler_arg, "MQTT_EVENTS", ev.event_id, &ev);
#endif
}
static void fireSimple(esp_mqtt_client *c, esp_mqtt_event_id_t id)
{
    esp_mqtt_event_t ev = {};
    ev.event_id = id;
    fire(c, ev);
}

// Mirror of esp-mqtt deliver_publish(): the message is read in an in-buffer of exactly bufSize
// bytes (heap allocated, so ASan sees any write past it) and delivered in one or several events.
// Like esp-mqtt, the offset of a part is the sum of the payload bytes of the previous parts, so
// it is still 0 for the second part when the topic fills the whole first read.
static void deliver(esp_mqtt_client *c, const std::string &topic, const std::string &payload, size_t bufSize)
{
    const size_t header = 4;
    char *buf = (char *)malloc(bufSize);
    CHECK(header + topic.size() <= bufSize);
    memset(buf, 0xEE, bufSize);
    memcpy(buf + header, topic.data(), topic.size());
    size_t first = bufSize - header - topic.size();
    if (first > payload.size())
        first = payload.size();
    if (first)
        memcpy(buf + header + topic.size(), payload.data(), first);

    esp_mqtt_event_t ev = {};
    ev.event_id = MQTT_EVENT_DATA;
    ev.topic = buf + header;
    ev.topic_len = (int)topic.size();
    ev.data = first ? buf + header + topic.size() : nullptr;
    ev.data_len = (int)first;
    ev.total_data_len = (int)payload.size();
    ev.current_data_offset = 0;
    ev.qos = 1;
    ev.retain = 1;
    fire(c, ev);

    size_t done = first;
    while (done < payload.size())
    {
        size_t n = payload.size() - done;
        if (n > bufSize)
            n = bufSize;
        memcpy(buf, payload.data() + done, n);
        ev.topic = nullptr;
        ev.topic_len = 0;
        ev.data = buf;
        ev.data_len = (int)n;
        ev.current_data_offset = (int)done;
        fire(c, ev);
        done += n;
    }
    free(buf);
}

// ---------------------------------------------------------------- tests
struct Got
{
    std::vector<std::pair<std::string, std::string>> msgs;
    int count() const { return (int)msgs.size(); }
};

static esp_mqtt_client *startClient(ESP32MQTTClient &m)
{
    m.setURI("mqtt://host:1883");
    CHECK(m.loopStart());
    esp_mqtt_client *c = g_last;
    CHECK(c != nullptr);
    fireSimple(c, MQTT_EVENT_BEFORE_CONNECT);
    fireSimple(c, MQTT_EVENT_CONNECTED);
    return c;
}

static void testLifecycle()
{
    printf("lifecycle (IDF %s, tick %d ms)\n", IDF4 ? "4.4" : "5.3", (int)portTICK_PERIOD_MS);
    int init0 = g_init, destroy0 = g_destroy;
    {
        int connects = 0, disconnects = 0; // declared before the client: its destructor still calls the callbacks
        ESP32MQTTClient m;
        m.setOnConnectCallback([&](esp_mqtt_client_handle_t, bool) { connects++; });
        m.setOnDisconnectCallback([&](esp_mqtt_client_handle_t, bool) { disconnects++; g_order += "cb;"; });
        CHECK(!m.loopStart()); // no URI
        CHECK(g_init == init0);
        esp_mqtt_client *c = startClient(m); // the first event used to crash on IDF 4.x (null user_context)
        CHECK(m.isConnected() && connects == 1 && m.isMyTurn(c));
        CHECK(!c->has_client_id && !c->has_username && !c->has_password);

        CHECK(m.loopStart()); // second call while running: no new client
        CHECK(g_init == init0 + 1 && c->reconnects == 1 && c->starts == 1);

        // Unexpected disconnection + reconnection, then a clean disconnect():
        // it must still wait for the disconnect event (the flag used to stay set for ever)
        fireSimple(c, MQTT_EVENT_DISCONNECTED);
        CHECK(!m.isConnected() && disconnects == 1);
        fireSimple(c, MQTT_EVENT_CONNECTED);
        g_order.clear();
        g_delays = 0;
        m.disconnect();
        CHECK(g_order == "cb;stop;"); // callbacks before the stop
        CHECK(disconnects == 2 && c->stops == 1 && !m.isConnected());

        // Stopped: nothing is published, nothing to stop again
        CHECK(m.publish("t", 1, false, "x") == -1 && c->enqueued.empty());
        CHECK(!m.publish(std::string("t"), std::string("x")));
        m.disconnect();
        CHECK(c->stops == 1);

        // loopStart() after disconnect(): the stopped client is released, a new one is started
        m.setURI("mqtt://other:1883");
        CHECK(m.loopStart());
        CHECK(g_init == init0 + 2 && g_destroy == destroy0 + 1);
        c = g_last;
        CHECK(c->uri == "mqtt://other:1883" && c->started);
        CHECK(m.publish("t", 1, false, "x") == 9); // started, not connected: QoS 1 is queued

        // No disconnect event at all (e.g. MQTT task blocked): the wait is limited to 5 s,
        // whatever the tick rate (the time used to be counted in loop turns)
        fireSimple(c, MQTT_EVENT_CONNECTED);
        g_fire_disconnect = false;
        const int64_t t0 = g_now_us;
        m.disconnect();
        const int64_t waited = g_now_us - t0;
        g_fire_disconnect = true;
        CHECK(waited >= 5000000 && waited <= 5000000 + 2 * portTICK_PERIOD_MS * 1000);
        CHECK(c->stops == 1);

        // forceStop on a stopped client, then restart
        m.forceStop();
        CHECK(c->stops == 1);
        CHECK(m.loopStart());
        c = g_last;
        fireSimple(c, MQTT_EVENT_CONNECTED);
        m.forceStop();
        CHECK(c->stops == 1 && !m.isConnected());
        CHECK(m.loopStart());
        fireSimple(g_last, MQTT_EVENT_CONNECTED);
        g_delays = 0;
    } // destructor of a connected client: clean disconnect, then destroy
    CHECK(g_delays >= 3 && g_delays < 500);
    CHECK(g_init - init0 == g_destroy - destroy0);

    // An instance living in garbage memory (stack / heap) must be fully initialised
    alignas(ESP32MQTTClient) static unsigned char raw[sizeof(ESP32MQTTClient)];
    memset(raw, 0xAA, sizeof(raw));
    int d = g_destroy;
    ESP32MQTTClient *p = new (raw) ESP32MQTTClient;
    CHECK(!p->isConnected());
    CHECK(std::string(p->getURI()).empty() && std::string(p->getClientName()).empty());
    CHECK(!p->loopStart());
    CHECK(p->publish("t", 1, false, "x") == -1 && p->unsubscribe("t") == -1 && p->subscribe("t", 0) == -1);
    p->disconnect();
    p->forceStop();
    p->~ESP32MQTTClient();
    CHECK(g_destroy == d); // no destroy on a garbage handle
}

static void testConfig()
{
    printf("config\n");
    {
        ESP32MQTTClient m;
        std::string host = "broker.example.com", user = "u", pass = "p", will = "lwt", willMsg = "bye", ca = "CA-PEM";
        m.setURL(host.c_str(), 8883, user.c_str(), pass.c_str());
        m.setMqttClientName(std::string("dev1").c_str()); // temporaries: the library must keep its own copy
        m.enableLastWillMessage(will.c_str(), willMsg.c_str(), true, 1);
        m.setCaCert(ca.c_str());
        m.enablePersistence();
        host = user = pass = will = willMsg = ca = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";
        CHECK(m.loopStart());
        esp_mqtt_client *c = g_last;
        CHECK(c->uri == "mqtts://broker.example.com:8883");
        CHECK(c->client_id == "dev1" && c->username == "u" && c->password == "p");
        CHECK(c->lwt_topic == "lwt" && c->lwt_msg == "bye");
#if IDF4
        CHECK(c->cfg.disable_clean_session == 1 && c->cfg.lwt_qos == 1 && c->cfg.lwt_retain == 1 && c->cfg.lwt_msg_len == 3);
        CHECK(c->cfg.user_context == &m && std::string(c->cfg.cert_pem) == "CA-PEM");
#else
        CHECK(c->cfg.session.disable_clean_session && c->cfg.session.last_will.qos == 1 && c->cfg.session.last_will.retain == 1 && c->cfg.session.last_will.msg_len == 3);
        CHECK(std::string(c->cfg.broker.verification.certificate) == "CA-PEM");
#endif
    }
    {
        ESP32MQTTClient m; // defaults: clean session, no last will, 1024 buffers
        m.disablePersistence();
        m.setURI("mqtt://h");
        CHECK(m.loopStart());
        esp_mqtt_client *c = g_last;
#if IDF4
        CHECK(c->cfg.disable_clean_session == 0 && c->cfg.lwt_topic == nullptr && c->cfg.buffer_size == 1024 && c->cfg.out_buffer_size == 1024);
#else
        CHECK(!c->cfg.session.disable_clean_session && c->cfg.session.last_will.topic == nullptr && c->cfg.buffer.size == 1024 && c->cfg.buffer.out_size == 1024);
#endif
    }
    struct U
    {
        const char *url;
        uint16_t port;
        const char *want;
    } urls[] = {
        {"h", 1883, "mqtt://h:1883"},
        {"h", 8883, "mqtts://h:8883"},
        {"h", 443, "wss://h:443"},
        {"h", 8884, "wss://h:8884"},
        {"h", 80, "ws://h:80"},
        {"h", 1884, "ws://h:1884"},
        {"h", 9001, "mqtt://h:9001"},
        {"h/mqtt", 443, "wss://h:443/mqtt"},
        {"mqtts://h", 443, "mqtts://h:443"},
        {"ws://h/mqtt?x=1", 8083, "ws://h:8083/mqtt?x=1"},
    };
    for (auto &u : urls)
    {
        ESP32MQTTClient m;
        m.setURL(u.url, u.port);
        if (std::string(m.getURI()) != u.want)
            printf("  setURL(%s,%u) -> %s, want %s\n", u.url, u.port, m.getURI(), u.want);
        CHECK(std::string(m.getURI()) == u.want);
    }
    {
        ESP32MQTTClient m;
        std::string longHost(400, 'a');
        m.setURL(longHost.c_str(), 1883);
        CHECK(std::string(m.getURI()) == "mqtt://" + longHost + ":1883"); // used to be cut at 199 chars
        m.setURL(nullptr, 1883);                                          // keeps the previous uri
        CHECK(std::string(m.getURI()) == "mqtt://" + longHost + ":1883");
        m.setURI(nullptr, nullptr, nullptr); // must not crash
        m.setMqttClientName(nullptr);
        m.enableLastWillMessage(nullptr, nullptr);
    }
}

static void testCallbacksApi()
{
    printf("global / topic callbacks\n");
    ESP32MQTTClient m;
    Got global, foo, bar;
    int qosSeen = -1, retainSeen = -1;
    // Same order as the shipped examples: this sequence used to crash in strcmp(NULL, ...)
    m.setOnMessageCallback([&](esp_mqtt_client_handle_t, char *t, char *p, int retain, int qos, bool) { global.msgs.emplace_back(t, p); qosSeen = qos; retainSeen = retain; });
    m.setOnTopicCallback("foo", 0, [&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { foo.msgs.emplace_back(t, p); });
    m.setOnTopicCallback("bar/#", 1, [&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { bar.msgs.emplace_back(t, p); });
    m.setOnTopicCallback("foo", 0, [&](esp_mqtt_client_handle_t, char *, char *, int, int, bool) { CHECK(false); }); // duplicate: ignored
    m.setOnTopicCallback(nullptr, 0, nullptr);

    esp_mqtt_client *c = startClient(m);
    CHECK(c->subs.size() == 2 && c->subs[0] == "foo" && c->subs[1] == "bar/#"); // subscribed on connect

    deliver(c, "foo", "1", 64);
    deliver(c, "bar/x", "2", 64);
    deliver(c, "other", "3", 64);
    CHECK(global.count() == 3); // the global callback used to be never called
    CHECK(foo.count() == 1 && foo.msgs[0].second == "1");
    CHECK(bar.count() == 1 && bar.msgs[0].first == "bar/x" && bar.msgs[0].second == "2");
    CHECK(qosSeen == 1 && retainSeen == 1);

    // Registered while connected: subscribed at once
    m.setOnTopicCallback("late/+", 0, [&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { foo.msgs.emplace_back(t, p); });
    CHECK(c->subs.size() == 3 && c->subs[2] == "late/+");

    // Reconnection: topic callbacks are subscribed again
    fireSimple(c, MQTT_EVENT_DISCONNECTED);
    fireSimple(c, MQTT_EVENT_CONNECTED);
    CHECK(c->subs.size() == 6);

    // Other event callbacks
    int subs = 0, unsubs = 0, pubs = 0, errs = 0;
    m.setOnSubscribeCallback([&](esp_mqtt_client_handle_t, int id) { subs += id; });
    m.setOnUnsubscribeCallback([&](esp_mqtt_client_handle_t, int id) { unsubs += id; });
    m.setOnPublishCallback([&](esp_mqtt_client_handle_t, int id) { pubs += id; });
    m.setOnErrorCallback([&](esp_mqtt_client_handle_t, esp_mqtt_error_codes_t e) { errs += 1 + (int)e.error_type * 10; });
    esp_mqtt_event_t ev = {};
    ev.msg_id = 5;
    ev.event_id = MQTT_EVENT_SUBSCRIBED;
    fire(c, ev);
    ev.event_id = MQTT_EVENT_UNSUBSCRIBED;
    fire(c, ev);
    ev.event_id = MQTT_EVENT_PUBLISHED;
    fire(c, ev);
    CHECK(subs == 5 && unsubs == 5 && pubs == 5);

    // Errors: every type reaches the callbacks, and an error alone is not a disconnection
    esp_mqtt_error_codes_t err = {};
    ev.event_id = MQTT_EVENT_ERROR;
    ev.error_handle = &err;
    err.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED; // used to be filtered out
    fire(c, ev);
    CHECK(errs == 21);
    err.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT;
    fire(c, ev);
    CHECK(errs == 32);
    CHECK(m.isConnected());
    m.enableDebuggingMessages();
    fire(c, ev); // exercises printError()
    m.enableDebuggingMessages(false);

    // C-string publish / subscribe API
    CHECK(m.publish("t", 1, false, "abc", 3) == 9 && c->enqueued.size() == 1);
    CHECK(m.publish("t", 0, false, "abc", 3, false) == 7 && c->pubs.size() == 1);
    CHECK(m.subscribe("raw", 0) == 3 && m.unsubscribe("raw") == 5);
    fireSimple(c, MQTT_EVENT_DISCONNECTED);
    CHECK(m.publish("t", 0, false, "abc") == -1); // QoS 0 dropped when disconnected
    CHECK(m.publish("t", 1, false, "abc") == 9);  // QoS 1 queued
    CHECK(m.subscribe("raw", 0) == -1);
}

static void testReceive()
{
    printf("receive\n");
    ESP32MQTTClient m;
    Got global, sub, topicCb;
    m.setOnMessageCallback([&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { global.msgs.emplace_back(t, p); });
    m.setOnTopicCallback("big/#", 0, [&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { topicCb.msgs.emplace_back(t, p); });
    esp_mqtt_client *c = startClient(m);
    CHECK(m.subscribe("big/#", [&](const std::string &t, const std::string &p) { sub.msgs.emplace_back(t, p); }));

    // 1. Message that fills the receive buffer exactly
    deliver(c, "big/a", std::string(64 - 4 - 5, 'x'), 64);
    CHECK(global.count() == 1 && sub.count() == 1 && topicCb.count() == 1);
    CHECK(sub.msgs[0].first == "big/a" && sub.msgs[0].second == std::string(55, 'x'));
    CHECK(topicCb.msgs[0].second == std::string(55, 'x'));

    // 2. Message bigger than the buffer: one complete delivery to every kind of callback
    std::string big;
    for (int i = 0; i < 1000; i++)
        big += (char)('a' + i % 26);
    deliver(c, "big/b", big, 64);
    CHECK(global.count() == 2 && sub.count() == 2 && topicCb.count() == 2);
    CHECK(sub.msgs[1].first == "big/b" && sub.msgs[1].second == big);
    CHECK(topicCb.msgs[1].first == "big/b" && topicCb.msgs[1].second == big);
    CHECK(global.msgs[1].first == "big/b" && global.msgs[1].second == big);

    // 3. Single message as big as a large receive buffer (used to be copied on the MQTT task stack)
    std::string large(60000, 'L');
    deliver(c, "big/large", large, 65535);
    CHECK(topicCb.count() == 3 && topicCb.msgs[2].second == large && sub.msgs[2].second == large);

    // 4. Binary payload with embedded zeros
    std::string bin("ab\0cd\0\0ef", 9);
    deliver(c, "big/bin", bin, 64);
    CHECK(sub.count() == 4 && sub.msgs[3].second.size() == 9 && sub.msgs[3].second == bin);
    CHECK(topicCb.count() == 4 && topicCb.msgs[3].second == "ab"); // C string API

    // 5. Empty payload
    deliver(c, "big/empty", "", 64);
    CHECK(sub.count() == 5 && sub.msgs[4].first == "big/empty" && sub.msgs[4].second.empty());
    CHECK(topicCb.count() == 5 && topicCb.msgs[4].second.empty());

    // 6. Too big for the heap: dropped, and the next message is fine
    g_largest_free = 500;
    deliver(c, "big/huge", big, 64);
    g_largest_free = 1 << 20;
    CHECK(sub.count() == 5 && global.count() == 5 && topicCb.count() == 5);
    deliver(c, "big/after", "ok", 64);
    CHECK(sub.count() == 6 && sub.msgs[5].first == "big/after" && sub.msgs[5].second == "ok");

    // 7. Disconnection in the middle of a big message, then a new message
    {
        char buf[32];
        memset(buf, 'z', sizeof(buf));
        esp_mqtt_event_t ev = {};
        ev.event_id = MQTT_EVENT_DATA;
        ev.topic = (char *)"big/cut";
        ev.topic_len = 7;
        ev.data = buf;
        ev.data_len = 32;
        ev.total_data_len = 100;
        fire(c, ev);
        fireSimple(c, MQTT_EVENT_DISCONNECTED);
        fireSimple(c, MQTT_EVENT_CONNECTED);
        // A part without its beginning must not be delivered
        ev.topic = nullptr;
        ev.topic_len = 0;
        ev.current_data_offset = 68;
        fire(c, ev);
        CHECK(sub.count() == 6);
        deliver(c, "big/next", "fine", 64);
        CHECK(sub.count() == 7 && sub.msgs[6].second == "fine" && topicCb.msgs.back().second == "fine");
    }

    // 8. Topic filling the whole first read: the first part has no payload byte, so the second
    //    part also has offset 0 (it used to be taken for a new message without topic)
    {
        const std::string topic = "big/" + std::string(56, 't'); // 4 + 60 = 64 bytes
        deliver(c, topic, "0123456789", 64);
        CHECK(sub.count() == 8 && sub.msgs[7].first == topic && sub.msgs[7].second == "0123456789");
        CHECK(topicCb.count() == 8 && topicCb.msgs[7].first == topic);
        CHECK(global.msgs.back().first == topic);
    }

    // 9. A part is missing in the middle of a message: dropped, the next message is fine
    {
        char buf[16];
        memset(buf, 'm', sizeof(buf));
        esp_mqtt_event_t ev = {};
        ev.event_id = MQTT_EVENT_DATA;
        ev.topic = (char *)"big/gap";
        ev.topic_len = 7;
        ev.data = buf;
        ev.data_len = 16;
        ev.total_data_len = 48;
        fire(c, ev);
        ev.topic = nullptr;
        ev.topic_len = 0;
        ev.current_data_offset = 32; // the part at offset 16 was not delivered
        fire(c, ev);
        CHECK(sub.count() == 8);
        deliver(c, "big/ok", "yes", 64);
        CHECK(sub.count() == 9 && sub.msgs[8].first == "big/ok" && sub.msgs[8].second == "yes");
    }
}

static void testSubscribe()
{
    printf("subscribe / publish\n");
    ESP32MQTTClient m;
    int a = 0, b = 0, b2 = 0;
    CHECK(!m.subscribe("x", [](const std::string &) {})); // not started: no crash, false
    esp_mqtt_client *c = startClient(m);

    auto subscribeAll = [&]() {
        CHECK(m.subscribe("a", [&](const std::string &, const std::string &) { a++; })); // with topic, first
        CHECK(m.subscribe("b", [&](const std::string &) { b++; }));                      // plain, last
    };
    subscribeAll();
    subscribeAll(); // what the onConnect callback does after each reconnection
    deliver(c, "b", "1", 64);
    CHECK(b == 1 && a == 0); // the "a" callback used to be attached to the "b" record
    deliver(c, "a", "1", 64);
    CHECK(a == 1 && b == 1);

    // Both kinds of callbacks on one topic, and replacing one of them
    CHECK(m.subscribe("b", [&](const std::string &, const std::string &) { b2++; }));
    deliver(c, "b", "2", 64);
    CHECK(b == 2 && b2 == 1);
    CHECK(m.subscribe("b", [&](const std::string &) { b += 10; }));
    deliver(c, "b", "3", 64);
    CHECK(b == 12 && b2 == 2);

    // Unsubscribe
    CHECK(m.unsubscribe(std::string("b")));
    CHECK(c->unsubs.size() == 1);
    deliver(c, "b", "4", 64);
    CHECK(b == 12 && b2 == 2);
    CHECK(m.unsubscribe(std::string("unknown"))); // not in the list: nothing sent
    CHECK(c->unsubs.size() == 1);

    // Registering / removing from inside a callback (no deadlock, no use after free)
    int inner = 0, outer = 0;
    CHECK(m.subscribe("re", [&](const std::string &) {
        outer++;
        for (int i = 0; i < 20; i++)
        {
            m.subscribe("re/" + std::to_string(i), [&](const std::string &) { inner++; });
            m.setOnTopicCallback(("cb/" + std::to_string(i)).c_str(), 0, [&](esp_mqtt_client_handle_t, char *, char *, int, int, bool) { inner += 100; });
        }
        m.unsubscribe(std::string("re"));
    }));
    deliver(c, "re", "go", 64);
    deliver(c, "re", "again", 64);
    deliver(c, "re/7", "x", 64);
    deliver(c, "cb/3", "x", 64);
    CHECK(outer == 1 && inner == 101);

    // Outbox full (-2) and failure (-1) are not a success
    g_subscribe_ret = -2;
    CHECK(!m.subscribe("full", [](const std::string &) {}));
    g_subscribe_ret = 3;
    g_publish_ret = -2;
    CHECK(!m.publish("t", "p", 1));
    g_publish_ret = -1;
    CHECK(!m.publish("t", "p"));
    g_publish_ret = 0; // QoS 0 message id
    CHECK(m.publish("t", std::string("a\0b", 3)));
    CHECK(c->pubs.size() == 1 && c->pubs[0].second.size() == 3);
    CHECK(m.publish("t", ""));
    CHECK(c->pubs.size() == 2 && c->pubs[1].second.empty());
    g_publish_ret = 7;

    fireSimple(c, MQTT_EVENT_DISCONNECTED);
    CHECK(!m.publish("t", "p"));
    CHECK(!m.unsubscribe(std::string("a")));
}

static void testTopicMatch()
{
    printf("topic matching\n");
    struct T
    {
        const char *filter, *topic;
        bool want;
    } ts[] = {
        {"foo", "foo", true},
        {"foo", "foo/bar", false},
        {"foo/bar", "foo", false},
        {"foo", "fo", false},
        {"fo", "foo", false},
        {"bar/#", "bar/bar", true},
        {"bar/#", "bar/a/b/c", true},
        {"bar/#", "bar", true},
        {"bar/#", "bar/", true},
        {"bar/#", "barx/a", false},
        {"bar/#", "ba", false},
        {"#", "a/b", true},
        {"#", "a", true},
        {"#", "$SYS/broker", false},
        {"$SYS/#", "$SYS/broker", true},
        {"+/x", "$SYS/x", false},
        {"a/+", "a/b", true},
        {"a/+", "a/b/c", false},
        {"a/+", "a", false},
        {"a/+", "a/", true},
        {"+", "a", true},
        {"+", "a/b", false},
        {"+/+", "/finance", true},
        {"/+", "/finance", true},
        {"+", "/finance", false},
        {"a/+/c", "a/b/c", true},
        {"a/+/c", "a/b/d", false},
        {"a/+/c", "a/b/c/d", false},
        {"a/+/a", "a/a", false},
        {"a/+/b", "a//b", true},
        {"a/+/b/+", "a/x/b/y", true},
        {"a/+/b/+", "a/x/c/y", false},
        {"+/tele/#", "dev1/tele/state", true},
        {"+/tele/#", "dev1/tele", true},
        {"+/tele/#", "dev1/stat/x", false},
        {"+/+/+", "a/b/c", true},
        {"+/+/+", "a/b", false},
        {"home/+/temperature", "home/kitchen/temperature", true},
        {"sport/tennis/player1/#", "sport/tennis/player1/score/wimbledon", true},
        {"sport/+", "sport", false},
        {"sport/+", "sport/", true},
    };
    for (auto &t : ts)
    {
        ESP32MQTTClient m;
        int hits = 0, hits2 = 0;
        m.setOnTopicCallback(t.filter, 0, [&](esp_mqtt_client_handle_t, char *, char *, int, int, bool) { hits2++; });
        esp_mqtt_client *c = startClient(m);
        CHECK(m.subscribe(t.filter, [&](const std::string &) { hits++; }));
        deliver(c, t.topic, "p", 128);
        if ((hits == 1) != t.want)
            printf("  filter [%s] topic [%s] got %d want %d\n", t.filter, t.topic, hits, (int)t.want);
        CHECK((hits == 1) == t.want && hits2 == hits);
    }
}

// One task delivers messages and events while two others register / remove callbacks (run under TSan)
static void testThreads()
{
    printf("threads\n");
    ESP32MQTTClient m;
    std::atomic<int> hits{0};
    esp_mqtt_client *c = startClient(m);
    m.subscribe("t/#", [&](const std::string &) { hits++; });
    std::thread mqttTask([&]() {
        for (int i = 0; i < 3000; i++)
        {
            deliver(c, "t/" + std::to_string(i % 50), "payload", 64);
            if (i % 10 == 0)
            {
                esp_mqtt_event_t ev = {};
                ev.event_id = MQTT_EVENT_PUBLISHED;
                fire(c, ev);
                ev.event_id = MQTT_EVENT_SUBSCRIBED;
                fire(c, ev);
            }
        }
    });
    auto app = [&](int id) {
        for (int i = 0; i < 3000; i++)
        {
            std::string topic = "t/" + std::to_string((i + id) % 50);
            m.subscribe(topic, [&](const std::string &) { hits++; });
            if (i % 3 == 0)
                m.unsubscribe(topic);
            if (i % 50 == 0)
                m.setOnTopicCallback(("t/" + std::to_string(i + id)).c_str(), 0, [&](esp_mqtt_client_handle_t, char *, char *, int, int, bool) { hits++; });
            if (i % 100 == 0)
            {
                m.setOnMessageCallback([&](esp_mqtt_client_handle_t, char *, char *, int, int, bool) { hits++; });
                m.setOnPublishCallback([&](esp_mqtt_client_handle_t, int) { hits++; });
                m.setOnSubscribeCallback([&](esp_mqtt_client_handle_t, int) { hits++; });
            }
            (void)m.isConnected();
            m.publish("x", 0, false, "y");
        }
    };
    std::thread app1(app, 0), app2(app, 17);
    mqttTask.join();
    app1.join();
    app2.join();
    CHECK(hits >= 3000);
}

// Another task keeps using the client while it is restarted with disconnect() + loopStart():
// the old handle used to be destroyed while still in use (seen by ASan as a use after free)
static void testRestartWhileInUse()
{
    printf("restart while in use\n");
    ESP32MQTTClient m;
    startClient(m);
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> published{0};
    std::thread publisher([&]() {
        running = true;
        while (!stop)
        {
            if (m.publish("q1", 1, false, "x") >= 0)
                published++;
            m.publish(std::string("q0"), std::string("y"));
            m.subscribe("s", 0);
            m.unsubscribe("s");
            m.subscribe(std::string("s2"), [](const std::string &) {});
            m.unsubscribe(std::string("s2"));
        }
    });
    while (!running)
        std::this_thread::yield();
    // At least 300 restarts, and until the other task did use the client a few times
    // (how the two threads interleave depends on the scheduler)
    for (int i = 0; i < 300 || (published < 20 && i < 200000); i++)
    {
        m.disconnect();
        CHECK(m.loopStart());
        fireSimple(g_last, MQTT_EVENT_CONNECTED);
        std::this_thread::yield();
    }
    stop = true;
    publisher.join();
    CHECK(published > 0);
}

// Several clients in one firmware: everything (events, callbacks, receive state, handle) is per instance
static int g_sharedConnectA = 0, g_sharedConnectB = 0;
static ESP32MQTTClient *g_clientA = nullptr, *g_clientB = nullptr;
// One function registered on both clients, like onMqttConnect in the examples
static void sharedOnConnect(esp_mqtt_client_handle_t client, bool)
{
    if (g_clientA && g_clientA->isMyTurn(client))
        g_sharedConnectA++;
    if (g_clientB && g_clientB->isMyTurn(client))
        g_sharedConnectB++;
}

static void testTwoClients()
{
    printf("two clients\n");
    int destroy0 = g_destroy;
    {
        ESP32MQTTClient a, b;
        g_clientA = &a;
        g_clientB = &b;
        Got gotA, gotB, globalA, globalB;
        for (ESP32MQTTClient *m : {&a, &b})
        {
            m->setOnConnectCallback(sharedOnConnect);
            m->setMqttClientName(m == &a ? "dev-a" : "dev-b"); // same broker: the client ids must differ
        }
        a.setOnMessageCallback([&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { globalA.msgs.emplace_back(t, p); });
        b.setOnMessageCallback([&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { globalB.msgs.emplace_back(t, p); });
        a.setOnTopicCallback("x/#", 0, [&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { gotA.msgs.emplace_back(t, p); });
        b.setOnTopicCallback("x/#", 0, [&](esp_mqtt_client_handle_t, char *t, char *p, int, int, bool) { gotB.msgs.emplace_back(t, p); });

        a.setURI("mqtt://same-broker:1883");
        b.setURI("mqtt://same-broker:1883");
        CHECK(a.loopStart());
        esp_mqtt_client *ca = g_last;
        CHECK(b.loopStart());
        esp_mqtt_client *cb = g_last;
        CHECK(ca != cb && ca->client_id == "dev-a" && cb->client_id == "dev-b");
        CHECK(a.isMyTurn(ca) && !a.isMyTurn(cb) && b.isMyTurn(cb) && !b.isMyTurn(ca));

        fireSimple(ca, MQTT_EVENT_CONNECTED);
        CHECK(g_sharedConnectA == 1 && g_sharedConnectB == 0 && a.isConnected() && !b.isConnected());
        fireSimple(cb, MQTT_EVENT_CONNECTED);
        CHECK(g_sharedConnectA == 1 && g_sharedConnectB == 1 && b.isConnected());
        CHECK(ca->subs.size() == 1 && cb->subs.size() == 1); // each one subscribes its own topics

        // Messages go to the callbacks of the instance that received them only
        deliver(ca, "x/1", "for a", 64);
        deliver(cb, "x/1", "for b", 64);
        CHECK(gotA.count() == 1 && gotA.msgs[0].second == "for a" && globalA.count() == 1);
        CHECK(gotB.count() == 1 && gotB.msgs[0].second == "for b" && globalB.count() == 1);

        // Multipart messages received at the same time by both clients are rebuilt separately
        {
            std::string bigA(300, 'A'), bigB(200, 'B');
            char part[100];
            esp_mqtt_event_t ea = {}, eb = {};
            ea.event_id = eb.event_id = MQTT_EVENT_DATA;
            ea.topic = (char *)"x/big-a";
            ea.topic_len = 7;
            ea.total_data_len = 300;
            eb.topic = (char *)"x/big-b";
            eb.topic_len = 7;
            eb.total_data_len = 200;
            for (int off = 0; off < 300; off += 100)
            {
                memcpy(part, bigA.data() + off, 100);
                ea.data = part;
                ea.data_len = 100;
                ea.current_data_offset = off;
                fire(ca, ea);
                ea.topic = nullptr;
                ea.topic_len = 0;
                if (off < 200)
                {
                    memcpy(part, bigB.data() + off, 100);
                    eb.data = part;
                    eb.data_len = 100;
                    eb.current_data_offset = off;
                    fire(cb, eb);
                    eb.topic = nullptr;
                    eb.topic_len = 0;
                }
            }
            CHECK(gotA.count() == 2 && gotA.msgs[1].first == "x/big-a" && gotA.msgs[1].second == bigA);
            CHECK(gotB.count() == 2 && gotB.msgs[1].first == "x/big-b" && gotB.msgs[1].second == bigB);
        }

        // Bridge: a callback of one client publishes with the other one
        int bridged = 0;
        a.setOnTopicCallback("bridge/in", 0, [&](esp_mqtt_client_handle_t, char *, char *p, int, int, bool) {
            if (b.publish("bridge/out", 1, false, p) >= 0)
                bridged++;
        });
        deliver(ca, "bridge/in", "hello", 64);
        CHECK(bridged == 1 && cb->enqueued.size() == 1 && cb->enqueued[0].first == "bridge/out" && cb->enqueued[0].second == "hello");
        CHECK(ca->enqueued.empty());

        // Stopping and restarting one client does not touch the other one
        a.disconnect();
        CHECK(!a.isConnected() && b.isConnected() && ca->stops == 1 && cb->stops == 0 && cb->started);
        CHECK(a.publish("t", 1, false, "x") == -1 && b.publish("t", 1, false, "x") == 9);
        CHECK(a.loopStart());
        esp_mqtt_client *ca2 = g_last;
        CHECK(ca2 != cb && a.isMyTurn(ca2) && b.isMyTurn(cb) && g_destroy == destroy0 + 1);
        fireSimple(ca2, MQTT_EVENT_CONNECTED);
        CHECK(g_sharedConnectA == 2 && g_sharedConnectB == 1);
        deliver(cb, "x/2", "still b", 64);
        CHECK(gotB.count() == 3 && gotB.msgs[2].second == "still b" && gotA.count() == 2);

        // An event of one client never reaches the other instance
        fireSimple(cb, MQTT_EVENT_DISCONNECTED);
        CHECK(a.isConnected() && !b.isConnected());
        fireSimple(cb, MQTT_EVENT_CONNECTED);

        // Each "MQTT task" delivers to its own client at the same time, with a one-way bridge from
        // a to b (run under TSan). A two-way bridge from callbacks can deadlock on the real esp-mqtt
        // (each task holds its client's lock while running callbacks), see the README.
        std::atomic<int> received{0};
        a.setOnTopicCallback("t/a", 0, [&](esp_mqtt_client_handle_t, char *, char *p, int, int, bool) { received++; b.publish("t/from-a", 1, false, p); });
        b.setOnTopicCallback("t/b", 0, [&](esp_mqtt_client_handle_t, char *, char *, int, int, bool) { received++; });
        std::thread taskA([&]() { for (int i = 0; i < 2000; i++) deliver(ca2, "t/a", "pa", 64); });
        std::thread taskB([&]() { for (int i = 0; i < 2000; i++) deliver(cb, "t/b", "pb", 64); });
        taskA.join();
        taskB.join();
        CHECK(received == 4000);

        g_clientA = g_clientB = nullptr;
    } // both destroyed
    CHECK(g_destroy == destroy0 + 3);
    CHECK(g_live.empty());
}

int main(int argc, char **argv)
{
    g_log_verbose = argc > 1 && std::string(argv[1]) == "-v";
    testLifecycle();
    testConfig();
    testCallbacksApi();
    testReceive();
    testSubscribe();
    testTopicMatch();
    testThreads();
    testRestartWhileInUse();
    testTwoClients();
    if (g_fail)
        printf("FAILED: %d check(s)\n", g_fail.load());
    else
        printf("ALL PASSED\n");
    return g_fail ? 1 : 0;
}
