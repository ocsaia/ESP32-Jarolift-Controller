#pragma once

/*
 * Stands in for include/basics.h during native tests.
 *
 * The real header pulls in WiFi, HTTPClient, mDNS and SPI, none of which
 * config.cpp uses - it needs muTimer for its change-detection timer and
 * EspStrUtil for the JSON string reads, the djb2 hash and the password
 * encryption. Both of those are the real libraries here, not fakes: the tests
 * exist to check what config.cpp does with them, so faking them would leave
 * nothing worth checking.
 *
 * This file shadows the real one because -I test/shim comes first on the
 * include path. If a module under test starts needing something from the real
 * basics.h, add it here deliberately rather than widening the include path.
 */

#include <ArduinoJson.h>
#include <EspStrUtil.h>  // the shim next to this file, not the library
#include <config.h>
#include <string>
#include <muTimer.h>

/*
 * The rest of what mqtt.cpp reaches for from the real basics.h. All of it is
 * either status the test sets directly or an action the test wants to observe -
 * a restart in particular, because "restart" is a command mqttHandleCommand()
 * accepts and a test has to be able to see that it was honoured without the
 * process going away.
 */

struct s_wifi {
  bool connected = false;
  long rssi = 0;
  int signal = 0;
  char ipAddress[20] = {0};
};
extern s_wifi wifi;

struct s_eth {
  bool connected = false;
  char ipAddress[20] = {0};
  uint8_t linkSpeed = 0;
  bool fullDuplex = false;
  bool linkUp = false;
};
extern s_eth eth;

void sendWiFiInfo();

inline void yield() {}

// Records instead of restarting. A test asserting that the restart command was
// honoured would otherwise have to end the test binary to find out.
struct FakeEsp {
  int restarts = 0;
  void restart() { restarts++; }
};
extern FakeEsp ESP;

class EspSysUtil {
public:
  class RestartReason {
  public:
    static const char *get() { return "Power-on reset"; }
    static void saveLocal(const char *reason) { lastSaved() = reason ? reason : ""; }
    static std::string &lastSaved() {
      static std::string s;
      return s;
    }
  };
};
