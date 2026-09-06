#pragma once

void jaroCmdSetDevCnt(uint16_t value);
void jaroApplyRadioConfig();
bool getCC1101State();
uint8_t getCC1101Rssi();
uint16_t jaroGetDevCnt();

void jaroliftSetup();
void jaroliftCyclic();

// Sends a STOP for one channel immediately instead of queueing it. Position
// control needs this: processJaroCommands() only runs every SEND_CYCLE ms, and
// half a second of extra travel is a visible position error.
void jaroStopNow(uint8_t channel);

// defined in jarolift.cpp, published on <base>/status/shutter/<n>
void mqttSendPosition(uint8_t channel, uint8_t position);

enum JaroCmdType { CMD_UP, CMD_DOWN, CMD_STOP, CMD_SET_SHADE, CMD_SHADE };
enum JaroCmdGrpType { CMD_GRP_UP, CMD_GRP_DOWN, CMD_GRP_STOP, CMD_GRP_SHADE };
enum JaroCmdSrvType { CMD_LEARN, CMD_UNLEARN, CMD_SET_END_POINT_UP, CMD_DEL_END_POINT_UP, CMD_SET_END_POINT_DOWN, CMD_DEL_END_POINT_DOWN };

struct JaroCommand {
  enum CommandType { SINGLE, GROUP, SERVICE } cmdType;
  union {
    struct {
      JaroCmdType type;
      uint8_t channel;
    } single;
    struct {
      JaroCmdGrpType type;
      uint16_t group_mask;
    } group;
    struct {
      JaroCmdSrvType type;
      uint8_t channel;
    } service;
  };
};

void jaroCmd(JaroCmdType type, uint8_t channel);
void jaroCmd(JaroCmdGrpType type, uint16_t group_mask);
void jaroCmd(JaroCmdSrvType type, uint8_t channel);