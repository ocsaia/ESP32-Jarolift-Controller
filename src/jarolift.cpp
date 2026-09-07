#include <EspSysUtil.h>
#include <JaroliftController.h>
#include <basics.h>
#include <config.h>
#include <esp_task_wdt.h>
#include <jarolift.h>
#include <mqtt.h>
#include <queue>
#include <shutterPos.h>
#include <timer.h>

#define MAX_CMD 20
#define SEND_CYCLE 500
// Topics are built from config.mqtt.topic (128 bytes, include/config.h:86) plus
// a suffix, so anything smaller truncates silently and publishes to a path
// nobody subscribed to. 256 also matches addTopic()'s own buffer.
#define MQTT_TOPIC_BUF_LEN 256
// Home Assistant's cover convention: 0 is closed, 100 is open. This firmware
// used the inverse until position tracking arrived, and the two cannot coexist -
// see the commit message for what it means for existing MQTT automations.
#define POS_OPEN 100
#define POS_CLOSE 0
#define POS_SHADE 10

static muTimer cmdTimer = muTimer();
static muTimer timerTimer = muTimer();
static const char *TAG = "JARO"; // LOG TAG
static auto &wdt = EspSysUtil::Wdt::getInstance();

/**
 * *******************************************************************
 * @brief   feed the task watchdog from inside a long radio sequence
 * @details Handed to the library so it can call this between frames. The
 *          isActive() guard matters: the watchdog is switched off during OTA
 *          and in setup mode, and esp_task_wdt_reset() on an unsubscribed task
 *          is an error rather than a no-op.
 * @param   none
 * @return  none
 * *******************************************************************/
static void jaroFeedWatchdog() {
  if (wdt.isActive()) {
    esp_task_wdt_reset();
  }
}

std::queue<JaroCommand> jaroCmdQueue;

JaroliftController jarolift;

/**
 * *******************************************************************
 * @brief   send expected position via mqtt
 * @param   channel, position
 * @return  none
 * *******************************************************************/
void mqttSendPosition(uint8_t channel, uint8_t position) {
  char topic[MQTT_TOPIC_BUF_LEN];
  char pos[16];

  // position is a uint8_t parameter, so only the upper bound can be violated
  if (position > 100)
    position = 100;

  if (mqttIsConnected()) {
    itoa(position, pos, 10);
    // Formatted straight from the configured base topic rather than through
    // addTopic(): a 64 byte buffer truncated the result for any base topic
    // beyond ~47 characters, and addTopic() hands out a pointer into a single
    // static that the AsyncTCP task overwrites from onMqttConnect().
    snprintf(topic, sizeof(topic), "%s/status/shutter/%d", config.mqtt.topic, channel + 1);
    mqttPublish(topic, pos, true);
  }
}

/**
 * *******************************************************************
 * @brief   send expected position via mqtt
 * @param   channel, position
 * @return  none
 * *******************************************************************/
void mqttSendPositionGroup(uint16_t group_mask, uint8_t position) {

  // check all 16 channels
  for (uint8_t c = 0; c < 16; c++) {
    // check if channel is in group
    if (group_mask & (1 << c)) {
      mqttSendPosition(c, position);
    }
  }
}

/**
 * *******************************************************************
 * @brief   tell the position tracker about a movement of a whole group
 * @param   group_mask, goingDown
 * @return  none
 * *******************************************************************/
void notifyPositionGroup(uint16_t group_mask, bool goingDown) {
  for (uint8_t c = 0; c < 16; c++) {
    if (group_mask & (1 << c)) {
      if (goingDown) {
        shutterPosNotifyDown(c);
      } else {
        shutterPosNotifyUp(c);
      }
    }
  }
}

/**
 * *******************************************************************
 * @brief   log a received remote signal and forward it via mqtt
 * @param   serial, function, channel
 * @return  none
 * *******************************************************************/
void mqttSendRemote(uint32_t serial, int8_t function, uint16_t channel) {

  char fun[8];
  char chBIN[18];
  int pos = 0;
  for (int i = 15; i >= 0; i--) {
    chBIN[pos++] = ((channel >> i) & 1) ? '1' : '0';
    if (i == 8) {
      chBIN[pos++] = ' ';
    }
  }
  chBIN[pos] = '\0';

  switch (function) {
  case 0x2:
    snprintf(fun, sizeof(fun), "DOWN");
    break;
  case 0x3:
    snprintf(fun, sizeof(fun), "SHADE");
    break;
  case 0x4:
    snprintf(fun, sizeof(fun), "STOP");
    break;
  case 0x8:
    snprintf(fun, sizeof(fun), "UP");
    break;
  default:
    snprintf(fun, sizeof(fun), "0x%x", function);
    break;
  }

  // The web log is the only feedback a user gets for a remote button press, so
  // it is written before anything that can bail out - with the broker down or
  // MQTT disabled the press used to disappear completely.
  ESP_LOGI(TAG, "received remote signal | serial: 0x%08lx | cmd: %s, | channel: %s", serial, fun, chBIN);

  /*
   * Look the remote up and tell the tracker BEFORE the connection check.
   *
   * A physical remote moves the shutter just as much as this controller does,
   * so the estimate has to follow it - otherwise one press of a wall remote
   * invalidates every position until the next end-stop. That reasoning does not
   * stop being true when the broker is unreachable: the shutter still moves,
   * and a WiFi or broker outage is exactly when someone reaches for the wall
   * switch instead of the app. Leaving the tracker behind the early return
   * meant the estimate silently desynced for the whole outage and stayed wrong
   * afterwards.
   *
   * The early return is upstream's and predates position tracking, when the
   * comment it carried - "everything below only produces MQTT traffic" - was
   * true. It stopped being true when the notify calls were added after it.
   *
   * Publishing from here while disconnected is harmless: mqttPublish() hands
   * straight to AsyncMqttClient, which fails quietly when it has no session.
   */
  const char *remoteName = "unknown"; // until a configured remote matches
  for (int i = 0; i < 16; i++) {
    if (config.jaro.remote_enable[i] && (serial >> 8 == config.jaro.remote_serial[i])) {
      remoteName = config.jaro.remote_name[i];

      // check if this remote is registered for one or more shutter
      for (int j = 0; j < 16; j++) {
        if (config.jaro.remote_mask[i] & (1 << j)) {
          switch (function) {
          case 0x2:
            shutterPosNotifyDown(j);
            break;
          case 0x8:
            shutterPosNotifyUp(j);
            break;
          case 0x4:
            shutterPosNotifyStop(j);
            break;
          case 0x3:
            mqttSendPosition(j, POS_SHADE);
            break;
          default:
            break;
          }
        }
      }
      break; // stop if a remote was found
    }
  }

  // Only the status telegram is left, and that has no consumer while the broker
  // is away - building the JSON for nobody would be pure heap churn.
  if (!mqttIsConnected()) {
    return;
  }

  char topic[MQTT_TOPIC_BUF_LEN];

  JsonDocument remoteJSON;
  remoteJSON["name"] = remoteName;
  remoteJSON["cmd"] = fun;
  remoteJSON["chBin"] = chBIN;
  remoteJSON["chDec"] = channel;

  char sendremoteJSON[255];
  serializeJson(remoteJSON, sendremoteJSON);
  // Same reasoning as mqttSendPosition(): one snprintf from the configured base
  // topic, no truncation and no pointer into addTopic()'s shared static.
  snprintf(topic, sizeof(topic), "%s/status/remote/%08lx", config.mqtt.topic, serial);

  mqttPublish(topic, sendremoteJSON, false);
}

/**
 * *******************************************************************
 * @brief   add jarolift command to buffer
 * @param   type, channel
 * @return  none
 * *******************************************************************/
void jaroCmd(JaroCmdType type, uint8_t channel) {
  if (jaroCmdQueue.size() < MAX_CMD) {
    jaroCmdQueue.push({JaroCommand::SINGLE, {.single = {type, channel}}});
    ESP_LOGD(TAG, "add single cmd to buffer: %i, %i", type, channel + 1);
  } else {
    ESP_LOGE(TAG, "too many commands within too short time");
  }
}

void jaroCmd(JaroCmdGrpType type, uint16_t group_mask) {
  if (jaroCmdQueue.size() < MAX_CMD) {
    jaroCmdQueue.push({JaroCommand::GROUP, {.group = {type, group_mask}}});
    ESP_LOGD(TAG, "add group cmd to buffer: %i, %04X", type, group_mask);
  } else {
    ESP_LOGE(TAG, "too many commands within too short time");
  }
}

void jaroCmd(JaroCmdSrvType type, uint8_t channel) {
  if (jaroCmdQueue.size() < MAX_CMD) {
    jaroCmdQueue.push({JaroCommand::SERVICE, {.service = {type, channel}}});
    ESP_LOGD(TAG, "add service cmd to buffer: %i, %i", type, channel + 1);
  } else {
    ESP_LOGE(TAG, "too many commands within too short time");
  }
}

/**
 * *******************************************************************
 * @brief   push the jarolift radio settings into the controller library
 * @details The library keeps master keys, base serial and learn mode in its own
 *          config struct and reads them lazily on every command: generateKey()
 *          rebuilds the device key per command, getSerial() derives the channel
 *          serial per command and cmdLearn() reads the learn mode when it runs.
 *          Storing the values is therefore all that is needed to make a WebUI
 *          change effective at once - begin() reads none of the four.
 * @param   none
 * @return  none
 * *******************************************************************/
void jaroApplyRadioConfig() {
  jarolift.setKeys(config.jaro.masterMSB, config.jaro.masterLSB);
  jarolift.setBaseSerial(config.jaro.serial);
  jarolift.setLegacyLearnMode(!config.jaro.learn_mode);
  ESP_LOGD(TAG, "jarolift radio config applied");
}

/**
 * *******************************************************************
 * @brief   Jarolift Setup function
 * @param   none
 * @return  none
 * *******************************************************************/
void jaroliftSetup() {

  // initialize
  ESP_LOGI(TAG, "initializing the CC1101 Transceiver");
  jarolift.setGPIO(config.gpio.sck, config.gpio.miso, config.gpio.mosi, config.gpio.cs, config.gpio.gdo0, config.gpio.gdo2);
  jaroApplyRadioConfig();
  jarolift.begin();

  if (jarolift.getCC1101State()) {
    ESP_LOGI(TAG, "CC1101 Transceiver connected!");
  } else {
    ESP_LOGE(TAG, "CC1101 Transceiver NOT connected!");
  }

  ESP_LOGI(TAG, "read Device Counter from FLASH: %i", jarolift.getDeviceCounter());

  jarolift.setRemoteCallback(mqttSendRemote);
  jarolift.setWatchdogCallback(jaroFeedWatchdog);

  shutterPosSetup();
}

/**
 * *******************************************************************
 * @brief   send a STOP for one channel without going through the queue
 * @details processJaroCommands() runs every SEND_CYCLE ms, so a queued STOP can
 *          be up to half a second late. On a shutter that takes ~25 s end to end
 *          that is a 2 % position error on every timed stop, which is the whole
 *          accuracy budget of this feature.
 * @param   channel
 * @return  none
 * *******************************************************************/
void jaroStopNow(uint8_t channel) {
  jarolift.cmdChannel(JaroliftController::CMD_STOP, channel);
  ESP_LOGI(TAG, "execute cmd: STOP (immediate) - channel: %i", channel + 1);
}

void jaroCmdSetDevCnt(uint16_t value) { jarolift.setDeviceCounter(value); };
uint16_t jaroGetDevCnt() { return jarolift.getDeviceCounter(); };
bool getCC1101State() { return jarolift.getCC1101State(); };
uint8_t getCC1101Rssi() { return jarolift.getRssi(); }

/**
 * *******************************************************************
 * @brief   execute jarolift commands from buffer
 * @param   type, channel
 * @return  none
 * *******************************************************************/
void processJaroCommands() {
  if (!jaroCmdQueue.empty()) {
    JaroCommand cmd = jaroCmdQueue.front();

    if (cmd.cmdType == JaroCommand::SINGLE) {
      switch (cmd.single.type) {
      case CMD_UP:
        jarolift.cmdChannel(JaroliftController::CMD_UP, cmd.single.channel);
        // The tracker owns the position from here on - it publishes when the
        // movement settles, so there is exactly one place that decides what the
        // position is. For an uncalibrated channel it settles immediately at the
        // end position, which is the behaviour this line used to provide.
        shutterPosNotifyUp(cmd.single.channel);
        ESP_LOGI(TAG, "execute cmd: UP - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_DOWN:
        jarolift.cmdChannel(JaroliftController::CMD_DOWN, cmd.single.channel);
        shutterPosNotifyDown(cmd.single.channel);
        ESP_LOGI(TAG, "execute cmd: DOWN - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_STOP:
        jarolift.cmdChannel(JaroliftController::CMD_STOP, cmd.single.channel);
        // A STOP used to publish nothing, because the position it landed on was
        // a guess. With a calibrated travel time it is an interpolation instead.
        shutterPosNotifyStop(cmd.single.channel);
        ESP_LOGI(TAG, "execute cmd: STOP - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_SET_SHADE:
        jarolift.cmdChannel(JaroliftController::CMD_SET_SHADE, cmd.single.channel);
        // Teaching the shade point stores wherever the shutter currently stands, it
        // does not move it. Publishing POS_SHADE here reported a movement that never
        // happened and left Home Assistant showing a position the shutter is not in.
        ESP_LOGI(TAG, "execute cmd: SETSHADE - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_SHADE:
        jarolift.cmdChannel(JaroliftController::CMD_SHADE, cmd.single.channel);
        // This is the command that actually drives to the shade point, so this is
        // where the position changes - matching CMD_GRP_SHADE and the remote path,
        // which have always published here.
        mqttSendPosition(cmd.single.channel, POS_SHADE);
        ESP_LOGI(TAG, "execute cmd: SHADE - channel: %i", cmd.single.channel + 1);
        break;
      }
    } else if (cmd.cmdType == JaroCommand::GROUP) {
      switch (cmd.group.type) {
      case CMD_GRP_UP:
        jarolift.cmdGroup(JaroliftController::CMD_UP, cmd.group.group_mask);
        ESP_LOGI(TAG, "execute group cmd: UP - mask: %04X", cmd.group.group_mask);
        notifyPositionGroup(cmd.group.group_mask, false);
        break;
      case CMD_GRP_DOWN:
        jarolift.cmdGroup(JaroliftController::CMD_DOWN, cmd.group.group_mask);
        ESP_LOGI(TAG, "execute group cmd: DOWN - mask: %04X", cmd.group.group_mask);
        notifyPositionGroup(cmd.group.group_mask, true);
        break;
      case CMD_GRP_STOP:
        jarolift.cmdGroup(JaroliftController::CMD_STOP, cmd.group.group_mask);
        ESP_LOGI(TAG, "execute group cmd: STOP - mask: %04X", cmd.group.group_mask);
        for (uint8_t c = 0; c < 16; c++) {
          if (cmd.group.group_mask & (1 << c)) {
            shutterPosNotifyStop(c);
          }
        }
        break;
      case CMD_GRP_SHADE:
        jarolift.cmdGroup(JaroliftController::CMD_SHADE, cmd.group.group_mask);
        mqttSendPositionGroup(cmd.group.group_mask, POS_SHADE);
        ESP_LOGI(TAG, "execute group cmd: SHADE - mask: %04X", cmd.group.group_mask);
        break;
      }
    } else if (cmd.cmdType == JaroCommand::SERVICE) {
      switch (cmd.service.type) {
      case CMD_LEARN:
        jarolift.cmdLearn(cmd.service.channel);
        ESP_LOGI(TAG, "execute service cmd: CMD_LEARN - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_UNLEARN:
        jarolift.cmdUnlearn(cmd.service.channel);
        ESP_LOGI(TAG, "execute service cmd: CMD_UNLEARN - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_SET_END_POINT_UP:
        jarolift.cmdSetEndPointUp(cmd.service.channel);
        ESP_LOGI(TAG, "execute service cmd: SET_END_POINT_UP - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_DEL_END_POINT_UP:
        jarolift.cmdDeleteEndPointUp(cmd.service.channel);
        ESP_LOGI(TAG, "execute service cmd: CMD_DEL_END_POINT_UP - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_SET_END_POINT_DOWN:
        jarolift.cmdSetEndPointDown(cmd.service.channel);
        ESP_LOGI(TAG, "execute service cmd: SET_END_POINT_DOWN - channel: %i", cmd.single.channel + 1);
        break;
      case CMD_DEL_END_POINT_DOWN:
        jarolift.cmdDeleteEndPointDown(cmd.service.channel);
        ESP_LOGI(TAG, "execute service cmd: CMD_DEL_END_POINT_DOWN - channel: %i", cmd.single.channel + 1);
        break;
      }
    }

    jaroCmdQueue.pop();
  }
}

/**
 * *******************************************************************
 * @brief   jarolift cyclic function
 * @param   none
 * @return  none
 * *******************************************************************/
void jaroliftCyclic() {

  if (cmdTimer.cycleTrigger(SEND_CYCLE)) {
    processJaroCommands();
  }

  // every pass, not on a timer: a timed stop should fire as soon as its
  // deadline passes, and the loop is already delayed by whatever the radio does
  shutterPosCyclic();

  if (timerTimer.cycleTrigger(10000)) {
    timerCyclic();
  }

  jarolift.loop();
}