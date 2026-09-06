#pragma once

/* I N C L U D E S ****************************************************/
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/* D E C L A R A T I O N S ****************************************************/

/*
 * One cross-task command queue for both producers that live in the AsyncTCP
 * task: the MQTT message callback (onMqttMessage) and the EspWebUI element
 * callback. Both used to hand work to loop() through memory that no primitive
 * protected - a std::queue/std::deque in mqtt.cpp, which corrupts the heap when
 * two tasks push and pop it, and a single set of char buffers plus a plain bool
 * in webUI.cpp, which silently kept only the newest event.
 *
 * The command travels BY VALUE through a statically allocated FreeRTOS queue.
 * That costs sizeof(s_Cmd) per slot of .bss, but the producer never allocates
 * inside a TCP callback, the consumer never has to release anything on a path
 * that may end in ESP.restart(), and there is no window in which a pointer sits
 * in the queue while what it points at is already gone.
 */

// Longest key that can reach the queue:
//   MQTT topic      - AsyncMqttClient's constructor calls setMaxTopicLength(128)
//                     and PublishPacket discards longer topics before the
//                     callback, so 128 characters is a hard bound
//   webUI elementId - "p04_cmd_end_down_delete" is the longest, 23 characters
// The old s_MqttMessage reserved 512 bytes for a topic that cannot reach it.
#define CMD_KEY_LEN 132

// Longest value worth carrying: webUIcallback.cpp writes into config fields of
// at most 128 bytes, so anything past 131 characters is cut by its destination
// anyway. MQTT payloads are command words ("UP") or a group mask
// ("0b1010101010101010").
#define CMD_VALUE_LEN 132

// A uint32_t underlying type keeps s_Cmd 4 byte aligned, which is what the
// memcpy inside xQueueSend() / xQueueReceive() wants.
enum CmdSource : uint32_t {
  CMD_SRC_MQTT = 0,       // key = topic, value = payload
  CMD_SRC_WEB_ELEMENT = 1 // key = webUI elementId, value = element value
};

// 268 bytes - small enough that 24 slots fit in .bss and that a copy on the 8 kB
// AsyncTCP stack is cheap. The old s_MqttMessage was 1028 bytes and
// onMqttMessage() kept two of them alive at the same time.
struct s_Cmd {
  CmdSource source;
  char key[CMD_KEY_LEN];     // always NUL terminated
  char value[CMD_VALUE_LEN]; // always NUL terminated
};

/* P R O T O T Y P E S ********************************************************/
void cmdQueueSetup();
void cmdQueueCyclic();

// producers - run in the AsyncTCP task: they never block and never log
bool cmdQueuePushMqtt(const char *topic, const char *payload, size_t payloadLen);
bool cmdQueuePushWebElement(const char *elementId, const char *elementValue);
void cmdQueueCountDrop(const char *reason);

// consumers - defined in mqtt.cpp / webUIcallback.cpp, called from loop()
void mqttHandleCommand(const char *topic, const char *payload);
void webCallback(const char *elementId, const char *value);
