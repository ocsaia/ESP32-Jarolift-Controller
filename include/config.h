#pragma once
#include <message.h>
#include <stdint.h>
/*-------------------------------------------------------------------------------
General Configuration
--------------------------------------------------------------------------------*/
#define VERSION "v1.9.0" // internal program version

#define WIFI_RECONNECT 30000     // First delay between wifi reconnection tries
#define WIFI_RECONNECT_MAX 300000 // Backoff cap for wifi reconnection tries
#define MQTT_RECONNECT 10000     // First delay between mqtt reconnection tries
#define MQTT_RECONNECT_MAX 300000 // Backoff cap for mqtt reconnection tries
#define MQTT_CONNECT_STALL 30000  // A connect attempt that has not resolved by now is wedged

struct s_cfg_jaro {
  unsigned long masterMSB;
  unsigned long masterLSB;
  bool learn_mode;
  uint32_t serial;
  char ch_name[16][64]{"\0"};
  bool ch_enable[16];
  // Full travel time per direction, in milliseconds, 0 = not calibrated.
  // These belong in the persisted config: they are a property of the
  // installation and change only on calibration. The estimated position does
  // NOT live here - configCyclic() hashes this whole struct every second and
  // rewrites the entire config.json on any change, so a value that moves while
  // a shutter runs would mean a flash write per movement.
  uint32_t ch_travel_down[16];
  uint32_t ch_travel_up[16];
  char grp_name[6][64]{"\0"};
  bool grp_enable[6];
  uint16_t grp_mask[6];
  char remote_name[16][64]{"\0"};
  uint32_t remote_serial[16];
  bool remote_enable[16];
  uint16_t remote_mask[16];
};

/*
 * How many independent schedules exist. Each one already carries a 16 bit
 * channel mask, so a single timer can address one shutter, several, or all of
 * them - the capability was never the limit, the count was. Sixteen channels
 * with an independent up and down time need thirty-two, which is more config
 * than most installations will ever use; twenty-four covers a full house with
 * room to spare and costs about a kilobyte of RAM.
 */
#define TIMER_COUNT 24

struct s_cfg_timer {
  bool enable;          // Timer enable
  uint8_t type;         // 0 = fixed time, 2 = sunrise, 3 = sunset
  char time_value[6];   // fixed Time value (hh:mm)
  int16_t offset_value; // offset value in minutes for sunrise/sunset
  uint8_t cmd;          // 0 = up, 1 = down, 2=shade
  bool monday;
  bool tuesday;
  bool wednesday;
  bool thursday;
  bool friday;
  bool saturday;
  bool sunday;
  uint16_t grp_mask; // Group mask for included channels
  bool use_min_time;
  char min_time_value[6]; // fixed Time value (hh:mm)
  bool use_max_time;
  char max_time_value[6]; // fixed Time value (hh:mm)
  uint8_t astro_mode;     // ASTRO_* - which definition of "sunrise" to use
  int8_t horizon_value;   // degrees of obstruction, only for ASTRO_HORIZON
};

struct s_cfg_geo {
  float latitude;
  float longitude;
};

struct s_cfg_wifi {
  bool enable = false;
  char ssid[128];
  char password[128];
  char hostname[128];
  bool static_ip = false;
  char ipaddress[17];
  char subnet[17];
  char gateway[17];
  char dns[17];
};

struct s_cfg_eth {
  bool enable = false;
  char hostname[128];
  bool static_ip = false;
  char ipaddress[17];
  char subnet[17];
  char gateway[17];
  char dns[17];
  int gpio_sck;
  int gpio_mosi;
  int gpio_miso;
  int gpio_cs;
  int gpio_irq;
  int gpio_rst;
};

struct s_cfg_mqtt {
  bool enable;
  char server[128];
  char user[128];
  char password[128];
  char topic[128];
  uint16_t port = 1883;
  bool ha_enable;
  char ha_topic[64];
  char ha_device[32];
};

struct s_cfg_ntp {
  bool enable = true;
  char server[128] = {"de.pool.ntp.org"};
  char tz[128] = {"CET-1CEST,M3.5.0,M10.5.0/3"};
};

struct s_cfg_gpio {
  int led_setup;
  int gdo0; // TX
  int gdo2; // RX
  int sck;
  int mosi;
  int miso;
  int cs;
};

struct s_cfg_auth {
  bool enable = true;
  char user[64];
  char password[64];
};

struct s_cfg_log {
  bool enable = true;
  int level = 3;
  int order = 0;
};

struct s_config {
  int version;
  int lang;
  s_cfg_wifi wifi;
  s_cfg_eth eth;
  s_cfg_mqtt mqtt;
  s_cfg_ntp ntp;
  s_cfg_gpio gpio;
  s_cfg_auth auth;
  s_cfg_log log;
  s_cfg_jaro jaro;
  s_cfg_timer timer[TIMER_COUNT];
  s_cfg_geo geo;
};

extern s_config config;
extern bool setupMode;
void configSetup();
void configCyclic();
void configSaveToFile();
void configLoadFromFile();
void configInitValue();
void configGPIO();