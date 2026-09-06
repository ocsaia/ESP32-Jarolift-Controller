#include <WiFi.h>
#include <basics.h>
#include <cmdQueue.h>
#include <jarolift.h>
#include <language.h>
#include <message.h>
#include <mqtt.h>
#include <mqttDiscovery.h>
#include <shutterPos.h>

/* D E C L A R A T I O N S ****************************************************/
static AsyncMqttClient mqtt_client;
static bool bootUpMsgDone, setupDone = false;
static const char *TAG = "MQTT"; // LOG TAG
static char lastError[64] = "---";
// written from the AsyncTCP task in onMqttConnect(), read from loop() - both are
// aligned 32-bit accesses, volatile only to stop the compiler caching them
static volatile unsigned long mqttRetryDelay = MQTT_RECONNECT;
static volatile bool mqttFirstAttempt = true;
static unsigned long mqttAttemptMs = 0;
static muTimer mqttReconnectTimer;
// Availability topic. Static duration because setWill() keeps the pointer.
static char mqttStatusTopic[sizeof(config.mqtt.topic) + 16] = {0};

/**
 * *******************************************************************
 * @brief   mqtt publish wrapper
 * @param   topic, payload, retained
 * @return  none
 * *******************************************************************/
void mqttPublish(const char *topic, const char *payload, boolean retained) { mqtt_client.publish(topic, 0, retained, payload); }

/**
 * *******************************************************************
 * @brief   helper function to add subject to mqtt topic
 * @param   none
 * @return  none
 * *******************************************************************/
const char *addTopic(const char *suffix) {
  static char newTopic[256];
  snprintf(newTopic, sizeof(newTopic), "%s%s", config.mqtt.topic, suffix);
  return newTopic;
}

/**
 * *******************************************************************
 * @brief   helper function to add subject to mqtt topic
 * @param   none
 * @return  none
 * *******************************************************************/
const char *addCfgCmdTopic(const char *suffix) {
  static char newTopic[256];
  snprintf(newTopic, sizeof(newTopic), "%s/setvalue/%s", config.mqtt.topic, suffix);
  return newTopic;
}

/**
 * *******************************************************************
 * @brief   MQTT callback function for incoming message
 * @details Runs in the AsyncTCP task, so it may only hand the message over: no
 *          local message buffer, no dispatch, and no logging - every ESP_LOGx
 *          line goes through custom_vprintf(), which writes the shared logData
 *          ring and the telnet stream from the wrong task. Refused messages are
 *          counted here and reported by cmdQueueCyclic() in loop().
 * @param   topic, payload, properties, len, index, total
 * @return  none
 * *******************************************************************/
void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {

  // AsyncMqttClient delivers a payload that spans several TCP segments in
  // chunks and calls back once per chunk, so every chunk used to be queued as
  // its own command with the same topic. Every command this firmware knows is a
  // short word, so a chunk is never a valid command: drop it instead of acting
  // on an arbitrary slice, and do not build a reassembly buffer in the AsyncTCP
  // task for it. A zero length payload - the normal way to clear a retained
  // topic - arrives as len == index == total == 0 and still passes here.
  if (index != 0 || len != total) {
    cmdQueueCountDrop("fragmented mqtt message");
    return;
  }

  // payload is NULL for a zero length message and is not NUL terminated
  // otherwise - cmdQueuePushMqtt() copies exactly len bytes into a
  // zero-initialised slot and terminates the copy itself
  cmdQueuePushMqtt(topic, payload, len);
}

/**
 * *******************************************************************
 * @brief   callback function if MQTT gets connected
 * @param   none
 * @return  none
 * *******************************************************************/
void onMqttConnect(bool sessionPresent) {
  mqttRetryDelay = MQTT_RECONNECT; // a successful connect resets the backoff
  ESP_LOGI(TAG, "MQTT connected");

  // Birth message, retained so it survives a Home Assistant restart, and
  // published exactly once per connection - it is the counterpart of the
  // retained last will registered in mqttSetup(). It used to ride along in
  // sendWiFiInfo(), which messageCyclic() calls every ten seconds, so the
  // broker's retained store was rewritten thousands of times a day for a value
  // that only ever changes on connect and on disconnect.
  if (mqttStatusTopic[0] != 0) {
    mqttPublish(mqttStatusTopic, "online", true);
  }

  // Once connected, publish an announcement...
  sendWiFiInfo();
  // ... and resubscribe
  mqtt_client.subscribe(addTopic("/cmd/#"), 0);
  mqtt_client.subscribe(addTopic("/setvalue/#"), 0);
  mqtt_client.subscribe("homeassistant/status", 0);
}

/**
 * *******************************************************************
 * @brief   callback function if MQTT gets disconnected
 * @param   none
 * @return  none
 * *******************************************************************/
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {

  switch (reason) {
  case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
    snprintf(lastError, sizeof(lastError), "TCP DISCONNECTED");
    break;
  case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
    snprintf(lastError, sizeof(lastError), "MQTT UNACCEPTABLE PROTOCOL VERSION");
    break;
  case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
    snprintf(lastError, sizeof(lastError), "MQTT IDENTIFIER REJECTED");
    break;
  case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
    snprintf(lastError, sizeof(lastError), "MQTT SERVER UNAVAILABLE");
    break;
  case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
    snprintf(lastError, sizeof(lastError), "MQTT MALFORMED CREDENTIALS");
    break;
  case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
    snprintf(lastError, sizeof(lastError), "MQTT NOT AUTHORIZED");
    break;
  case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT:
    snprintf(lastError, sizeof(lastError), "TLS BAD FINGERPRINT");
    break;
  default:
    snprintf(lastError, sizeof(lastError), "UNKNOWN ERROR");
    break;
  }
}

/**
 * *******************************************************************
 * @brief   is MQTT connected
 * @param   none
 * @return  none
 * *******************************************************************/
bool mqttIsConnected() { return mqtt_client.connected(); }

const char *mqttGetLastError() { return lastError; }

/**
 * *******************************************************************
 * @brief   Basic MQTT setup
 * @param   none
 * @return  none
 * *******************************************************************/
void mqttSetup() {

  mqtt_client.onConnect(onMqttConnect);
  mqtt_client.onDisconnect(onMqttDisconnect);
  mqtt_client.onMessage(onMqttMessage);
  mqtt_client.setServer(config.mqtt.server, config.mqtt.port);
  mqtt_client.setClientId(config.wifi.hostname);
  mqtt_client.setCredentials(config.mqtt.user, config.mqtt.password);
  // setWill() stores the pointer it is given, it does not copy the string, and
  // addTopic() hands out a pointer into one shared static buffer that the next
  // caller overwrites. The last will was therefore registered on whatever topic
  // happened to be in that buffer when the CONNECT packet went out, so a device
  // that dropped off the network never went "offline" where Home Assistant was
  // listening. The availability topic now has storage of its own, which the
  // client keeps referencing for the lifetime of the connection - and using the
  // same buffer for the birth message guarantees the two cannot disagree.
  snprintf(mqttStatusTopic, sizeof(mqttStatusTopic), "%s/status", config.mqtt.topic);
  mqtt_client.setWill(mqttStatusTopic, 0, true, "offline");
  mqtt_client.setKeepAlive(10);
  mqtt_client.connected();

  ESP_LOGI(TAG, "MQTT setup done!");
}

/**
 * *******************************************************************
 * @brief   MQTT cyclic function
 * @param   none
 * @return  none
 * *******************************************************************/
void mqttCyclic() {

  // incoming messages are dispatched by cmdQueueCyclic() in loop(), so that one
  // queue and one drain point serve both the mqtt and the webUI producer

  // call setup when connection is established
  if (config.mqtt.enable && !setupMode && !setupDone && (eth.connected || wifi.connected)) {
    mqttSetup();
    setupDone = true;
  }

  // B2: this used to give up after five attempts and reboot. Since mqtt_retry was
  // only cleared by a successful connect, a broker that was down stayed down and
  // the controller restarted roughly every fifty seconds indefinitely - while
  // booting it serves no WebUI, runs no timer and drives no shutter, so the reboot
  // turned a broker outage into a total outage. Retry without a limit instead.
  if (!mqtt_client.connected() && (wifi.connected || eth.connected)) {
    if (mqttFirstAttempt) {
      mqttFirstAttempt = false;
      mqttAttemptMs = millis();
      mqtt_client.connect();
      ESP_LOGI(TAG, "MQTT - connecting to broker");
    } else if (mqttReconnectTimer.delayOnTrigger(true, mqttRetryDelay)) {
      mqttReconnectTimer.delayReset();

      // AsyncMqttClient::connect() sets its state to CONNECTING and then ignores
      // the return value of AsyncClient::connect(), which fails without ever
      // firing a callback when the async task cannot start or DNS fails outright.
      // The client then sits in CONNECTING for good. Rebooting used to be the
      // only way out of that; tearing the client down explicitly is the cheap one.
      if (millis() - mqttAttemptMs > MQTT_CONNECT_STALL) {
        ESP_LOGW(TAG, "MQTT - connect attempt did not resolve, forcing disconnect");
        mqtt_client.disconnect(true);
      }

      mqttAttemptMs = millis();
      mqtt_client.connect();
      ESP_LOGI(TAG, "MQTT - retry (next in %lu s)", mqttRetryDelay / 1000UL);

      if (mqttRetryDelay < MQTT_RECONNECT_MAX) {
        unsigned long next = mqttRetryDelay * 2;
        mqttRetryDelay = (next > MQTT_RECONNECT_MAX) ? MQTT_RECONNECT_MAX : next;
      }
    }
  }

  // send bootup messages after restart and established mqtt connection
  if (!bootUpMsgDone && mqtt_client.connected()) {
    bootUpMsgDone = true;
    ESP_LOGI(TAG, "ESP restarted (%s)", EspSysUtil::RestartReason::get());

    if (config.mqtt.ha_enable) {
      mqttDiscoverySetup(false);
    }
  }
}

/**
 * *******************************************************************
 * @brief   helper function to check shutter commands
 * @param   topicCopy, cmpTopic
 * @return  none
 * *******************************************************************/
int checkJaroCmd(const char *topicCopy, const char *cmpTopic, int maxChannel) {

  size_t cmpTopicLen = strlen(cmpTopic);

  if (strncmp(topicCopy, cmpTopic, cmpTopicLen) == 0) {
    const char *suffix = topicCopy + cmpTopicLen;
    char *endPtr;
    int channel = strtol(suffix, &endPtr, 10);

    if (*endPtr == '\0' && channel >= 1 && channel <= maxChannel) {
      return channel;
    }
  }
  return -1;
}

/**
 * *******************************************************************
 * @brief  parseMask:
 * @details - Removes any whitespace characters
 *  - Converts everything to lowercase
 *  - Determines the format based on prefix:
 *      "0x" => hexadecimal
 *      "0b" => binary
 *      otherwise => decimal
 * @param   topicCopy, cmpTopic
 * @return  the interpreted number as a uint16_t
 * *******************************************************************/

uint16_t parseMask(const char *payload) {
  char buffer[64];
  int idx = 0;

  // 1) Remove whitespaces and convert to lowercase
  while (*payload != '\0' && idx < (int)(sizeof(buffer) - 1)) {
    if (!isspace((unsigned char)*payload)) {
      buffer[idx++] = (char)tolower((unsigned char)*payload);
    }
    payload++;
  }
  buffer[idx] = '\0'; // null termination for the new string

  // 2) Detect prefix
  // "0x" => hexadecimal
  if (strncmp(buffer, "0x", 2) == 0) {
    // strtol parses the substring after "0x" in base 16
    return (uint16_t)strtol(buffer + 2, NULL, 16);
  }
  // "0b" => binary
  else if (strncmp(buffer, "0b", 2) == 0) {
    return (uint16_t)strtol(buffer + 2, NULL, 2);
  }
  // otherwise => decimal
  else {
    return (uint16_t)strtol(buffer, NULL, 10);
  }
}

/**
 * *******************************************************************
 * @brief   act on one mqtt command taken from the cross-task queue
 * @details called from cmdQueueCyclic() in loop(); topic and payload are
 *          NUL-terminated copies owned by the caller and valid for the duration
 *          of the call only
 * @param   topic, payload
 * @return  none
 * *******************************************************************/
void mqttHandleCommand(const char *topic, const char *payload) {

  // logged here rather than in onMqttMessage(), which runs in the AsyncTCP task
  ESP_LOGI(TAG, "msg received | topic: %s | payload: %s", topic, payload);

  // addTopic() returns the same static buffer on every call, so shutterTopic
  // and groupTopic alias - this is only correct because checkJaroCmd() consumes
  // the first one before the second call. Do not reorder these four lines.
  // Position commands need a topic of their own: the plain command topic
  // already maps the payloads "0".."4" to UP/DOWN/STOP/SHADE/SETSHADE, so a
  // percentage could not be told apart from a command word there. This is also
  // what Home Assistant's cover expects as set_position_topic.
  const char *setPosPrefix = addTopic("/cmd/shutter/");
  size_t setPosPrefixLen = strlen(setPosPrefix);
  if (strncmp(topic, setPosPrefix, setPosPrefixLen) == 0) {
    const char *rest = topic + setPosPrefixLen;
    char *endPtr = NULL;
    long ch = strtol(rest, &endPtr, 10);
    if (endPtr != rest && strcmp(endPtr, "/set_position") == 0) {
      if (ch < 1 || ch > 16) {
        mqttPublish(addTopic("/message"), "invalid channel", false);
        ESP_LOGW(TAG, "invalid channel for position cmd");
        return;
      }
      char *posEnd = NULL;
      long pct = strtol(payload, &posEnd, 10);
      if (posEnd == payload || *posEnd != 0 || pct < 0 || pct > 100) {
        mqttPublish(addTopic("/message"), "invalid position", false);
        ESP_LOGW(TAG, "invalid position payload: %s", payload);
        return;
      }
      if (!shutterPosSetTarget((uint8_t)(ch - 1), (uint8_t)pct)) {
        mqttPublish(addTopic("/message"), "position not available", false);
      }
      return;
    }
  }

  const char *shutterTopic = addTopic("/cmd/shutter/");
  int channel = checkJaroCmd(topic, shutterTopic, 16);
  const char *groupTopic = addTopic("/cmd/group/");
  int group = checkJaroCmd(topic, groupTopic, 6);

  ESP_LOGD(TAG, "channel: %i", channel);
  ESP_LOGD(TAG, "group: %i", group);

  // restart ESP command
  if (strcasecmp(topic, addTopic("/cmd/restart")) == 0) {
    EspSysUtil::RestartReason::saveLocal("mqtt command");
    yield();
    delay(1000);
    yield();
    ESP.restart();
    // reconfigure
  } else if (strcasecmp(topic, addTopic("/cmd/reconfigure")) == 0) {
    mqttDiscoverySetup(true);
    yield();
    delay(1000);
    yield();
    mqttDiscoverySetup(false);
    // homeassistant/status
  } else if (strcmp(topic, "homeassistant/status") == 0) {
    if (config.mqtt.ha_enable && strcmp(payload, "online") == 0) {
      mqttDiscoverySetup(false); // send actual discovery configuration
    }
    // Shutter commands
  } else if (channel != -1) {
    if (channel >= 1 && channel <= 16) {
      if (strcasecmp(payload, "UP") == 0 || strcasecmp(payload, "OPEN") == 0 || strcmp(payload, "0") == 0) {
        jaroCmd(CMD_UP, channel - 1);
      } else if (strcasecmp(payload, "DOWN") == 0 || strcasecmp(payload, "CLOSE") == 0 || strcmp(payload, "1") == 0) {
        jaroCmd(CMD_DOWN, channel - 1);
      } else if (strcasecmp(payload, "STOP") == 0 || strcmp(payload, "2") == 0) {
        jaroCmd(CMD_STOP, channel - 1);
      } else if (strcasecmp(payload, "SHADE") == 0 || strcmp(payload, "3") == 0) {
        jaroCmd(CMD_SHADE, channel - 1);
      } else if (strcasecmp(payload, "SETSHADE") == 0 || strcmp(payload, "4") == 0) {
        jaroCmd(CMD_SET_SHADE, channel - 1);
      } else {
        mqttPublish(addTopic("/message"), "invalid shutter cmd", false);
        ESP_LOGW(TAG, "invalid shutter cmd");
      }
    } else {
      mqttPublish(addTopic("/message"), "invalid channel", false);
      ESP_LOGW(TAG, "invalid channel for shutter cmd");
    }
    // Group commands
  } else if (group != -1) {
    if (group >= 1 && group <= 6) {
      if (strcasecmp(payload, "UP") == 0 || strcasecmp(payload, "OPEN") == 0 || strcmp(payload, "0") == 0) {
        jaroCmd(CMD_GRP_UP, config.jaro.grp_mask[group - 1]);
      } else if (strcasecmp(payload, "DOWN") == 0 || strcasecmp(payload, "CLOSE") == 0 || strcmp(payload, "1") == 0) {
        jaroCmd(CMD_GRP_DOWN, config.jaro.grp_mask[group - 1]);
      } else if (strcasecmp(payload, "STOP") == 0 || strcmp(payload, "2") == 0) {
        jaroCmd(CMD_GRP_STOP, config.jaro.grp_mask[group - 1]);
      } else if (strcasecmp(payload, "SHADE") == 0 || strcmp(payload, "3") == 0) {
        jaroCmd(CMD_GRP_SHADE, config.jaro.grp_mask[group - 1]);
      } else {
        mqttPublish(addTopic("/message"), "invalid group cmd", false);
        ESP_LOGW(TAG, "invalid group cmd");
      }
    } else {
      mqttPublish(addTopic("/message"), "invalid group", false);
      ESP_LOGW(TAG, "invalid channel for group cmd");
    }
    // Group commands with bitmask
  } else if (strcasecmp(topic, addTopic("/cmd/group/up")) == 0) {
    jaroCmd(CMD_GRP_UP, parseMask(payload));
  } else if (strcasecmp(topic, addTopic("/cmd/group/down")) == 0) {
    jaroCmd(CMD_GRP_DOWN, parseMask(payload));
  } else if (strcasecmp(topic, addTopic("/cmd/group/stop")) == 0) {
    jaroCmd(CMD_GRP_STOP, parseMask(payload));
  } else if (strcasecmp(topic, addTopic("/cmd/group/shade")) == 0) {
    jaroCmd(CMD_GRP_SHADE, parseMask(payload));
  } else {
    mqttPublish(addTopic("/message"), "unknown topic", false);
    ESP_LOGI(TAG, "unknown topic received");
  }
}
