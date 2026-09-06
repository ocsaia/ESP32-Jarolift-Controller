#include <basics.h>
#include <language.h>
#include <message.h>
#include <mqtt.h>
#include <mqttDiscovery.h>
#include <stdarg.h>

/* D E C L A R A T I O N S ****************************************************/
static const char *TAG = "HA-DISC"; // LOG TAG
char discoveryPrefix[128];
char deviceName[32];
char statePrefix[128];
char deviceId[32];
char swVersion[32];

static bool resetMqttConfig = false;

enum DeviceType { DEV_TEXT, DEV_BTN, DEV_SHUTTER };
enum statType { TYP_STATUS, TYP_INFO, TYP_WIFI, TYP_ETH, TYP_SYSINFO, TYP_CMD_BTN, TYP_SHUTTER, TYP_GROUP };
enum ValTmpType { VAL_SPLIT };

struct DeviceConfig {
  int num1;
  char *char1;
};

DeviceConfig nullPar() { return (DeviceConfig){0, 0}; }
DeviceConfig shutterPar(int channel, char *name) { return (DeviceConfig){channel, name}; }

/**
 * *******************************************************************
 * @brief   helper function to generate value template
 * @param   type string
 * @return  value template based on type
 * *******************************************************************/
const char *valueTmpl(ValTmpType type) {
  static char output[128];
  switch (type) {
  case VAL_SPLIT:
    snprintf(output, sizeof(output), "{{value.split(' ')[0]}}");
    break;

  default:
    break;
  }
  return output;
}

/**
 * *******************************************************************
 * @brief   format a topic and report instead of truncating silently
 * @details The topics are built from configured strings - the discovery prefix,
 *          the base topic and the device id - so their length is not a compile
 *          time property. They fit comfortably today, but a silently cut topic
 *          is the worst outcome available here: it does not fail, it publishes
 *          somewhere else, and two entities whose names differ only past the cut
 *          would quietly share one topic.
 * @param   dst, dstLen, fmt, ...
 * @return  true if the whole string fitted
 * *******************************************************************/
static bool topicPrintf(char *dst, size_t dstLen, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(dst, dstLen, fmt, args);
  va_end(args);

  if (needed < 0 || (size_t)needed >= dstLen) {
    ESP_LOGE(TAG, "topic does not fit (%d bytes needed, %u available) - entity skipped", needed, (unsigned)dstLen);
    return false;
  }
  return true;
}

/**
 * *******************************************************************
 * @brief   generate mqtt messages for home assistant auto discovery
 * @param   kmType
 * @param   name
 * @param   deviceClass
 * @param   component
 * @param   unit
 * @param   valueTemplate
 * @param   icon
 * @param   devType
 * @param   devCfg
 * @return  none
 * *******************************************************************/
void mqttHaConfig(statType statType, const char *name, const char *deviceClass, const char *component, const char *unit, const char *valueTemplate,
                  const char *icon, DeviceType devType, DeviceConfig devCfg) {

  if (strlen(discoveryPrefix) == 0 || strlen(statePrefix) == 0 || strlen(name) == 0) {
    return;
  }

  JsonDocument doc;
  char cmdTopic[256];
  char configTopic[256];

  if (!topicPrintf(configTopic, sizeof(configTopic), "%s/%s/%s/%s/config", discoveryPrefix, component, deviceId, name)) {

    return;

  }

  char stateTopic[256];
  switch (statType) {

  case TYP_SHUTTER:
    if (!topicPrintf(stateTopic, sizeof(stateTopic), "%s/status/shutter/%i", statePrefix, devCfg.num1)) {
      return;
    }
    doc["stat_t"] = stateTopic;
    break;

  case TYP_STATUS:
    if (!topicPrintf(stateTopic, sizeof(stateTopic), "%s/status/%s", statePrefix, name)) {
      return;
    }
    doc["stat_t"] = stateTopic;
    break;

  case TYP_WIFI:
    if (!topicPrintf(stateTopic, sizeof(stateTopic), "%s/wifi", statePrefix)) {
      return;
    }
    doc["stat_t"] = stateTopic;
    break;
  case TYP_ETH:
    if (!topicPrintf(stateTopic, sizeof(stateTopic), "%s/eth", statePrefix)) {
      return;
    }
    doc["stat_t"] = stateTopic;
    break;

  case TYP_SYSINFO:
    if (!topicPrintf(stateTopic, sizeof(stateTopic), "%s/sysinfo", statePrefix)) {
      return;
    }
    doc["stat_t"] = stateTopic;
    break;
  default:
    break;
  }

  if (devType == DEV_SHUTTER) {
    doc["name"] = devCfg.char1;
  } else {
    char friendlyName[64];
    EspStrUtil::replace_underscores(name, friendlyName, sizeof(friendlyName));
    doc["name"] = friendlyName;
  }

  char uniq_id[128];
  if (!topicPrintf(uniq_id, sizeof(uniq_id), "%s_%s", deviceName, name)) {
    return;
  }
  doc["uniq_id"] = uniq_id;

  if (deviceClass) {
    doc["dev_cla"] = deviceClass;
  }
  if (unit) {
    doc["unit_of_meas"] = unit;
  }
  if (valueTemplate) {
    doc["val_tpl"] = valueTemplate;
  }
  if (icon) {
    doc["icon"] = icon;
  }

  if (devType == DEV_SHUTTER) {

    // Enable optimistic mode for a device with no state feedback
    doc["optimistic"] = true;

    doc["pl_open"] = "OPEN";
    doc["pl_cls"] = "CLOSE";
    doc["pl_stop"] = "STOP";

    if (statType == TYP_GROUP) {
      if (!topicPrintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/group/%i", statePrefix, devCfg.num1)) {
        return;
      }
      doc["cmd_t"] = cmdTopic;
    } else if (statType == TYP_SHUTTER) {
      doc["state_open"] = "0";
      doc["state_closed"] = "100";
      if (!topicPrintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/shutter/%i", statePrefix, devCfg.num1)) {
        return;
      }
      doc["cmd_t"] = cmdTopic;
    }

  } else if (devType == DEV_BTN) {
    if (!topicPrintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/%s", statePrefix, name)) {
      return;
    }
    doc["cmd_t"] = cmdTopic;
    doc["payload_press"] = "true";
    doc["ent_cat"] = "config";
  } else if (devType != DEV_TEXT) {
    if (!topicPrintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/%s", statePrefix, name)) {
      return;
    }
    doc["cmd_t"] = cmdTopic;
  }

  if (statType == TYP_WIFI || statType == TYP_ETH || statType == TYP_SYSINFO) {
    doc["ent_cat"] = "diagnostic";
  }

  char willTopic[256];
  snprintf(willTopic, sizeof(willTopic), "%s/status", statePrefix);
  doc["avty_t"] = willTopic;

  // device
  JsonObject deviceObj = doc["dev"].to<JsonObject>();
  deviceObj["name"] = deviceName;
  JsonArray idsArray = deviceObj["ids"].to<JsonArray>();
  idsArray.add(deviceId);
  deviceObj["mf"] = "Jarolift";
  deviceObj["mdl"] = "MQTT_Controller";
  deviceObj["sw"] = swVersion;

  char jsonString[1024];
  // serializeJson() into a fixed array truncates rather than failing, and a
  // truncated discovery payload is not rejected loudly by Home Assistant - the
  // entity simply never appears, which is a hard thing to trace back to here.
  size_t jsonLen = measureJson(doc);
  if (jsonLen >= sizeof(jsonString)) {
    ESP_LOGE(TAG, "discovery payload for '%s' does not fit (%u bytes needed, %u available) - entity skipped", name, (unsigned)jsonLen,
             (unsigned)sizeof(jsonString));
    return;
  }
  serializeJson(doc, jsonString);

  if (resetMqttConfig && devType == DEV_SHUTTER) {
    mqttPublish(configTopic, "", false);
  } else {
    mqttPublish(configTopic, jsonString, false);
  }
}

/**
 * *******************************************************************
 * @brief   mqttDiscovery Setup function
 * @param   none
 * @return  none
 * *******************************************************************/
void mqttDiscoverySetup(bool reset) {

  resetMqttConfig = reset;

  // copy config values
  snprintf(discoveryPrefix, sizeof(discoveryPrefix), "%s", config.mqtt.ha_topic);
  snprintf(statePrefix, sizeof(statePrefix), "%s", config.mqtt.topic);
  snprintf(deviceName, sizeof(deviceName), "%s", config.mqtt.ha_device);
  snprintf(deviceId, sizeof(deviceId), "%s", config.mqtt.ha_device);
  snprintf(swVersion, sizeof(swVersion), "%s", VERSION);

  // Shutter Control 1..16
  for (int i = 0; i < 16; i++) {
    if (config.jaro.ch_enable[i]) {
      char shutter[32];
      snprintf(shutter, sizeof(shutter), "shutter%d", i + 1);
      mqttHaConfig(TYP_SHUTTER, shutter, "shutter", "cover", NULL, "{{ value | int }}", "mdi:window-shutter", DEV_SHUTTER,
                   shutterPar(i + 1, config.jaro.ch_name[i]));
    }
  }
  // Group Control 1..6
  for (int i = 0; i < 6; i++) {
    if (config.jaro.grp_enable[i]) {
      char group[32];
      snprintf(group, sizeof(group), "group%d", i + 1);
      mqttHaConfig(TYP_GROUP, group, "shutter", "cover", NULL, NULL, "mdi:window-shutter-settings", DEV_SHUTTER,
                   shutterPar(i + 1, config.jaro.grp_name[i]));
    }
  }
  // Service Buttons
  mqttHaConfig(TYP_CMD_BTN, "restart", NULL, "button", NULL, NULL, "mdi:restart", DEV_BTN, nullPar());
  mqttHaConfig(TYP_CMD_BTN, "reconfigure", NULL, "button", NULL, NULL, "mdi:cog-sync", DEV_BTN, nullPar());

  // System INFO
  mqttHaConfig(TYP_WIFI, "wifi_signal", NULL, "sensor", "%", "{{ value_json.signal }}", "mdi:signal", DEV_TEXT, nullPar());
  mqttHaConfig(TYP_WIFI, "wifi_rssi", NULL, "sensor", "dbm", "{{ value_json.rssi }}", "mdi:signal", DEV_TEXT, nullPar());
  mqttHaConfig(TYP_WIFI, "wifi_ip", NULL, "sensor", NULL, "{{ value_json.ip }}", "mdi:ip-outline", DEV_TEXT, nullPar());

  if (config.eth.enable) {
    mqttHaConfig(TYP_ETH, "eth_ip", NULL, "sensor", NULL, "{{ value_json.ip }}", "mdi:ip-network", DEV_TEXT, nullPar());
    mqttHaConfig(TYP_ETH, "eth_status", NULL, "sensor", NULL, "{{ value_json.status }}", "mdi:lan", DEV_TEXT, nullPar());
    mqttHaConfig(TYP_ETH, "eth_link_speed", NULL, "sensor", "Mbps", "{{ value_json.link_speed }}", "mdi:lan", DEV_TEXT, nullPar());
    mqttHaConfig(TYP_ETH, "eth_full_duplex", NULL, "sensor", NULL, "{{ value_json.full_duplex }}", "mdi:lan", DEV_TEXT, nullPar());
  }

  mqttHaConfig(TYP_SYSINFO, "restart_reason", NULL, "sensor", NULL, "{{ value_json.restart_reason }}", "mdi:information-outline", DEV_TEXT,
               nullPar());
  mqttHaConfig(TYP_SYSINFO, "heap", NULL, "sensor", "%", "{{ value_json.heap.split(' ')[0] }}", "mdi:memory", DEV_TEXT, nullPar());
  mqttHaConfig(TYP_SYSINFO, "flash", NULL, "sensor", "%", "{{ value_json.flash.split(' ')[0] }}", "mdi:harddisk", DEV_TEXT, nullPar());
  mqttHaConfig(TYP_SYSINFO, "sw_version", NULL, "sensor", NULL, "{{ value_json.sw_version }}", "mdi:github", DEV_TEXT, nullPar());
}
