#pragma once

/*
 * Stand-in for AsyncMqttClient.
 *
 * The tests here are about mqttHandleCommand(): which topic and payload map to
 * which action, and what the firmware answers on the /message topic. None of
 * that involves a broker, a socket or the AsyncTCP task - the real client only
 * has to exist so mqtt.cpp compiles, and to record what was published so a test
 * can assert on it.
 *
 * Deliberately not a broker simulation: it never delivers anything, never
 * changes state on its own, and reports whatever connected state the test set.
 * Anything more would be testing this file instead of the firmware.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AsyncMqttClientMessageProperties {
  uint8_t qos = 0;
  bool dup = false;
  bool retain = false;
};

enum class AsyncMqttClientDisconnectReason {
  TCP_DISCONNECTED,
  MQTT_UNACCEPTABLE_PROTOCOL_VERSION,
  MQTT_IDENTIFIER_REJECTED,
  MQTT_SERVER_UNAVAILABLE,
  MQTT_MALFORMED_CREDENTIALS,
  MQTT_NOT_AUTHORIZED,
  TLS_BAD_FINGERPRINT,
};

// what the firmware published, in order
struct FakePublish {
  std::string topic;
  std::string payload;
  bool retained;
};

inline std::vector<FakePublish> &fakeMqttPublished() {
  static std::vector<FakePublish> v;
  return v;
}
inline std::vector<std::string> &fakeMqttSubscribed() {
  static std::vector<std::string> v;
  return v;
}
inline bool &fakeMqttConnected() {
  static bool c = true;
  return c;
}
inline void fakeMqttReset() {
  fakeMqttPublished().clear();
  fakeMqttSubscribed().clear();
  fakeMqttConnected() = true;
}

class AsyncMqttClient {
public:
  using OnConnect = void (*)(bool);
  using OnDisconnect = void (*)(AsyncMqttClientDisconnectReason);
  using OnMessage = void (*)(char *, char *, AsyncMqttClientMessageProperties, size_t, size_t, size_t);

  AsyncMqttClient &setServer(const char *host, uint16_t port) {
    (void)host;
    (void)port;
    return *this;
  }
  AsyncMqttClient &setCredentials(const char *user, const char *pass) {
    (void)user;
    (void)pass;
    return *this;
  }
  AsyncMqttClient &setClientId(const char *id) {
    (void)id;
    return *this;
  }
  AsyncMqttClient &setKeepAlive(uint16_t s) {
    (void)s;
    return *this;
  }
  AsyncMqttClient &setWill(const char *topic, uint8_t qos, bool retain, const char *payload, size_t length = 0) {
    (void)qos;
    (void)length;
    willTopic = topic != nullptr ? topic : "";
    willPayload = payload != nullptr ? payload : "";
    willRetain = retain;
    return *this;
  }
  void onConnect(OnConnect cb) { connectCb = cb; }
  void onDisconnect(OnDisconnect cb) { disconnectCb = cb; }
  void onMessage(OnMessage cb) { messageCb = cb; }

  void connect() {}
  void disconnect(bool force = false) { (void)force; }
  bool connected() const { return fakeMqttConnected(); }

  uint16_t subscribe(const char *topic, uint8_t qos) {
    (void)qos;
    fakeMqttSubscribed().push_back(topic != nullptr ? topic : "");
    return 1;
  }
  uint16_t publish(const char *topic, uint8_t qos, bool retain, const char *payload, size_t length = 0) {
    (void)qos;
    (void)length;
    if (!fakeMqttConnected()) {
      return 0; // the real client drops it silently with no session
    }
    fakeMqttPublished().push_back({topic != nullptr ? topic : "", payload != nullptr ? payload : "", retain});
    return 1;
  }

  // the callbacks mqtt.cpp registered, so a test can drive them directly
  OnConnect connectCb = nullptr;
  OnDisconnect disconnectCb = nullptr;
  OnMessage messageCb = nullptr;

  std::string willTopic, willPayload;
  bool willRetain = false;
};
