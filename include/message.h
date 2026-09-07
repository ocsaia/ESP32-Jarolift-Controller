#pragma once
#include <Arduino.h>

/* D E C L A R A T I O N S ****************************************************/

// The ring costs MAX_LOG_LINES * MAX_LOG_ENTRY of static RAM - 25.6 kB as it
// stands. It cannot grow: raising it to 320 adds 15 kB and overflows the
// ESP32-S2's dram0_0_seg by 1128 bytes at link time. That target has roughly
// 14 kB of static headroom in total, which the build output does not show -
// see CLAUDE.md. What the WebUI sends in one dump is bounded separately in
// webUIupdates.cpp and does not depend on this.
#define MAX_LOG_LINES 200 // max log lines
#define MAX_LOG_ENTRY 128 // max length of one entry

struct s_logdata {
  int lastLine;
  char buffer[MAX_LOG_LINES][MAX_LOG_ENTRY];
};

extern s_logdata logData;

/* P R O T O T Y P E S ********************************************************/
void messageSetup();
void messageCyclic();
void addLogBuffer(const char *message);
void clearLogBuffer();
void setLogLevel(uint8_t level);
