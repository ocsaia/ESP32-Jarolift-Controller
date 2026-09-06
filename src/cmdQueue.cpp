#include <cmdQueue.h>
#include <config.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

/* D E C L A R A T I O N S ****************************************************/

// Queue depth. The burst to absorb is a scene that moves every shutter at once,
// or a broker replaying retained commands, arriving while loop() sits inside
// jaroliftCyclic() transmitting - a CMD_SHADE takes ~2.3 s and cmdUnlearn ~5 s.
// 24 slots x 268 bytes = 6432 bytes of .bss. That is more than the old
// MAX_MQTT_CMD of 20, and the webUI had exactly one slot before.
#define CMD_QUEUE_LEN 24

// Slots the MQTT producer may not take. A broker replaying retained commands
// must not be able to push the button press of the person at the browser out of
// the queue: MQTT may use 18 slots, the webUI always has 6.
#define CMD_QUEUE_WEB_RESERVE 6

// Upper bound on how long cmdQueueCyclic() keeps draining. It is checked BEFORE
// each receive, so at least one command is always dispatched and one loop()
// iteration grows by at most this budget plus a single dispatch (~1.1 s worst
// case for /cmd/reconfigure, which sleeps once and republishes discovery).
// jaroliftCyclic() can already spend ~5 s transmitting in the same iteration,
// against the 10 s task watchdog that is only fed from loop().
#define CMD_DRAIN_BUDGET_MS 100

static const char *TAG = "CMDQ"; // LOG TAG

// Statically allocated on purpose: the 6.4 kB show up in the link map, creation
// cannot fail at runtime, and it cannot fragment the heap that AsyncTCP and the
// TLS stack need.
static StaticQueue_t cmdQueueStruct;
static uint8_t cmdQueueStorage[CMD_QUEUE_LEN * sizeof(s_Cmd)] __attribute__((aligned(4)));
static QueueHandle_t cmdQueue = NULL;

// Drop bookkeeping. A producer must not log: ESP_LOGx ends up in
// custom_vprintf() (message.cpp), which writes the shared logData ring and
// pushes the line into the telnet stream - both owned by loop(). So a producer
// only counts and cmdQueueCyclic() reports. The spinlock keeps the pair
// consistent and stays correct once F1/F2 add a second producer task.
static portMUX_TYPE cmdDropMux = portMUX_INITIALIZER_UNLOCKED;
// volatile: cmdQueueCyclic() tests cmdDropCount outside the critical section and
// the writer is in this same translation unit, so nothing stops the compiler
// from caching the load across calls otherwise
static volatile uint32_t cmdDropCount = 0;
static const char *volatile cmdDropReason = "";

/**
 * *******************************************************************
 * @brief   create the cross-task command queue
 * @details must run before any producer can be armed - i.e. before
 *          webUISetup() registers the element callback and before mqttSetup()
 *          subscribes
 * @param   none
 * @return  none
 * *******************************************************************/
void cmdQueueSetup() {

  if (cmdQueue != NULL) {
    return;
  }

  cmdQueue = xQueueCreateStatic(CMD_QUEUE_LEN, sizeof(s_Cmd), cmdQueueStorage, &cmdQueueStruct);

  if (cmdQueue == NULL) {
    ESP_LOGE(TAG, "command queue could not be created - mqtt and webUI commands will be ignored");
  } else {
    ESP_LOGI(TAG, "command queue created: %i slots of %u bytes", CMD_QUEUE_LEN, (unsigned)sizeof(s_Cmd));
  }
}

/**
 * *******************************************************************
 * @brief   record a command a producer had to refuse
 * @details called from the AsyncTCP task, so it counts instead of logging -
 *          see the note on cmdDropMux
 * @param   reason  static string, kept by pointer
 * @return  none
 * *******************************************************************/
void cmdQueueCountDrop(const char *reason) {
  portENTER_CRITICAL(&cmdDropMux);
  cmdDropCount++;
  cmdDropReason = reason;
  portEXIT_CRITICAL(&cmdDropMux);
}

/**
 * *******************************************************************
 * @brief   copy one command into the queue
 * @details runs in the AsyncTCP task and never blocks: a full queue means
 *          loop() is busy transmitting, and waiting here would stall every
 *          other TCP connection, including the browser session of the user who
 *          is waiting for the answer
 * @param   source, key, value, valueLen, reserve
 * @return  true if the command was queued
 * *******************************************************************/
static bool cmdQueuePush(CmdSource source, const char *key, const char *value, size_t valueLen, UBaseType_t reserve) {

  if (cmdQueue == NULL) {
    return false;
  }

  if (key == NULL) {
    cmdQueueCountDrop("no key");
    return false;
  }

  // A cut key could alias a shorter valid one, so refuse instead of truncating.
  // Unreachable for MQTT (the client caps topics at 128 bytes) and for the
  // element ids this firmware defines; it guards a hand-crafted WebSocket frame.
  size_t keyLen = strnlen(key, CMD_KEY_LEN);
  if (keyLen >= CMD_KEY_LEN) {
    cmdQueueCountDrop("key too long");
    return false;
  }

  if (reserve != 0 && uxQueueSpacesAvailable(cmdQueue) <= reserve) {
    cmdQueueCountDrop("queue full - webUI reserve");
    return false;
  }

  // The whole struct is copied into the queue, so nothing that was never
  // written may travel with it - that is exactly the class of bug A3 was.
  s_Cmd cmd = {};
  cmd.source = source;
  memcpy(cmd.key, key, keyLen);
  cmd.key[keyLen] = '\0';

  // The value is not NUL terminated on the way in - an AsyncMqttClient payload
  // is a slice of its receive buffer - so copy exactly valueLen bytes and
  // terminate the copy here. A value that does not fit is cut rather than
  // dropped: its destination is a 128 byte config field or a strcasecmp against
  // a command word, so it would be cut or rejected there anyway.
  if (value != NULL && valueLen != 0) {
    if (valueLen >= CMD_VALUE_LEN) {
      valueLen = CMD_VALUE_LEN - 1;
    }
    memcpy(cmd.value, value, valueLen);
    cmd.value[valueLen] = '\0';
  }

  if (xQueueSend(cmdQueue, &cmd, 0) != pdTRUE) {
    cmdQueueCountDrop("queue full");
    return false;
  }

  return true;
}

/**
 * *******************************************************************
 * @brief   queue an incoming mqtt message (AsyncTCP task)
 * @param   topic, payload, payloadLen
 * @return  true if the command was queued
 * *******************************************************************/
bool cmdQueuePushMqtt(const char *topic, const char *payload, size_t payloadLen) {
  return cmdQueuePush(CMD_SRC_MQTT, topic, payload, payloadLen, CMD_QUEUE_WEB_RESERVE);
}

/**
 * *******************************************************************
 * @brief   queue a webUI element change (AsyncTCP task)
 * @details EspWebUI passes the parsed JSON members straight through, so both
 *          arguments are NULL when a client sends a frame without them
 * @param   elementId, elementValue
 * @return  true if the command was queued
 * *******************************************************************/
bool cmdQueuePushWebElement(const char *elementId, const char *elementValue) {
  return cmdQueuePush(CMD_SRC_WEB_ELEMENT, elementId, elementValue, elementValue != NULL ? strlen(elementValue) : 0, 0);
}

/**
 * *******************************************************************
 * @brief   dispatch queued commands - called from loop()
 * @details drains as much as CMD_DRAIN_BUDGET_MS allows. The old webUI code
 *          handled exactly one event per iteration, which is why a settings
 *          form with several changed fields could never keep up with the
 *          AsyncTCP task.
 * @param   none
 * @return  none
 * *******************************************************************/
void cmdQueueCyclic() {

  if (cmdQueue == NULL) {
    return;
  }

  s_Cmd cmd;
  uint32_t drainStart = millis();

  while ((millis() - drainStart) < CMD_DRAIN_BUDGET_MS && xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
    switch (cmd.source) {
    case CMD_SRC_MQTT:
      // the same gate that guarded the mqttCyclic() call site in main.cpp: with
      // MQTT switched off in the configuration the broker connection survives
      // until the next restart, so messages keep arriving and must be discarded
      // rather than acted on - but the queue still has to drain, or it wedges
      if (config.mqtt.enable && !setupMode) {
        mqttHandleCommand(cmd.key, cmd.value);
      }
      break;
    case CMD_SRC_WEB_ELEMENT:
      // not gated: in setup mode the webUI is the only way to reach the device
      webCallback(cmd.key, cmd.value);
      break;
    }
  }

  // report what the producers refused - from here, because ESP_LOGx writes the
  // shared log buffer and the telnet stream, which belong to loop()
  if (cmdDropCount != 0) {
    portENTER_CRITICAL(&cmdDropMux);
    uint32_t dropped = cmdDropCount;
    const char *reason = cmdDropReason;
    cmdDropCount = 0;
    portEXIT_CRITICAL(&cmdDropMux);
    ESP_LOGE(TAG, "%u command(s) dropped (%s)", (unsigned)dropped, reason);
  }
}
