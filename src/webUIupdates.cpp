#include <Dusk2Dawn.h>
#include <EspStrUtil.h>
#include <basics.h>
#include <github.h>
#include <jarolift.h>
#include <shutterPos.h>
#include <language.h>
#include <message.h>
#include <timer.h>
#include <webUI.h>
#include <webUIupdates.h>

/* S E T T I N G S ****************************************************/
#define WEBUI_SLOW_REFRESH_TIME_MS 3000
#define WEBUI_FAST_REFRESH_TIME_MS 100

/* P R O T O T Y P E S ********************************************************/
void updateSystemInfoElements();

/* D E C L A R A T I O N S ****************************************************/

static muTimer refreshTimer1 = muTimer();   // timer to refresh other values
static muTimer refreshTimer2 = muTimer();   // timer to refresh other values
static muTimer otaProgessTimer = muTimer(); // timer to refresh other values

static char tmpMessage[300] = {'\0'};
static bool refreshRequest = false;
static uint16_t devCntNew, devCntOld = 0;
static JsonDocument jsonDoc;
static int logLine, logIdx = 0;
static bool logReadActive = false;
JsonDocument jsonLog;
static const char *TAG = "WEB"; // LOG TAG
extern uint8_t srvShutter;      // service page selection, owned by webUIcallback.cpp
static auto &ota = EspSysUtil::OTA::getInstance();
static auto &wdt = EspSysUtil::Wdt::getInstance();
GithubRelease ghLatestRelease;
GithubReleaseInfo ghReleaseInfo;

/**
 * *******************************************************************
 * functions to create a JSON Buffer that contains webUI element updates
 * *******************************************************************/

/**
 * *******************************************************************
 * @brief   update all values (only call once)
 * @param   none
 * @return  none
 * *******************************************************************/
void updateAllElements() {

  refreshRequest = true; // start combined json refresh

  webUI.wsUpdateWebLanguage(LANG::CODE[config.lang]);

  if (setupMode) {
    webUI.wsShowElementClass("setupModeBar", true);
  }
}

/**
 * *******************************************************************
 * @brief   update System informations
 * @param   none
 * @return  none
 * *******************************************************************/
/**
 * *******************************************************************
 * @brief   push shutter positions and the calibration panel to the webUI
 * @details Sent on the slow cycle. A moving shutter therefore lags by up to
 *          that interval, which is deliberate: pushing a position while the
 *          user is dragging the slider would pull the handle out from under
 *          them, and the slider only reports on release anyway.
 * @param   none
 * @return  none
 * *******************************************************************/
void updateShutterPositions() {

  webUI.initJsonBuffer(jsonDoc);

  for (uint8_t i = 0; i < 16; i++) {
    if (!config.jaro.ch_enable[i]) {
      continue;
    }
    char id[32];
    snprintf(id, sizeof(id), "p01_pos_%d", i);
    int8_t pos = shutterPosGet(i);
    // the slider has to sit somewhere, but the label must not claim a position
    // that has never been established
    if (pos == SHUTTER_POS_UNKNOWN) {
      snprintf(tmpMessage, sizeof(tmpMessage), "--");
    } else {
      snprintf(tmpMessage, sizeof(tmpMessage), "%d", pos);
    }
    webUI.addJson(jsonDoc, id, tmpMessage);
  }

  // calibration panel - always for the shutter selected on the service page
  if (config.jaro.ch_travel_down[srvShutter] > 0) {
    snprintf(tmpMessage, sizeof(tmpMessage), "%.1f s", config.jaro.ch_travel_down[srvShutter] / 1000.0);
  } else {
    snprintf(tmpMessage, sizeof(tmpMessage), "--");
  }
  webUI.addJson(jsonDoc, "p04_travel_down", tmpMessage);

  if (config.jaro.ch_travel_up[srvShutter] > 0) {
    snprintf(tmpMessage, sizeof(tmpMessage), "%.1f s", config.jaro.ch_travel_up[srvShutter] / 1000.0);
  } else {
    snprintf(tmpMessage, sizeof(tmpMessage), "--");
  }
  webUI.addJson(jsonDoc, "p04_travel_up", tmpMessage);

  webUI.addJson(jsonDoc, "p04_calib_status",
                shutterCalibIsActive(srvShutter) ? WEB_TXT::CALIB_RUNNING[config.lang] : WEB_TXT::CALIB_IDLE[config.lang]);

  webUI.wsUpdateWebJSON(jsonDoc);
}

void updateSystemInfoElements() {

  refreshNetworkInfo();

  webUI.initJsonBuffer(jsonDoc);

  // WiFi information
  if (config.wifi.enable) {
    webUI.addJson(jsonDoc, "p09_wifi_ip", wifi.ipAddress);
    snprintf(tmpMessage, sizeof(tmpMessage), "%i %%", wifi.signal);
    webUI.addJson(jsonDoc, "p09_wifi_signal", tmpMessage);
    snprintf(tmpMessage, sizeof(tmpMessage), "%ld dbm", wifi.rssi);
    webUI.addJson(jsonDoc, "p09_wifi_rssi", tmpMessage);

    if (!WiFi.isConnected()) {
      webUI.addJson(jsonDoc, "p00_wifi_icon", "i_wifi_nok");
    } else if (wifi.rssi < -80) {
      webUI.addJson(jsonDoc, "p00_wifi_icon", "i_wifi_1");
    } else if (wifi.rssi < -70) {
      webUI.addJson(jsonDoc, "p00_wifi_icon", "i_wifi_2");
    } else if (wifi.rssi < -60) {
      webUI.addJson(jsonDoc, "p00_wifi_icon", "i_wifi_3");
    } else {
      webUI.addJson(jsonDoc, "p00_wifi_icon", "i_wifi_4");
    }

  } else {
    webUI.addJson(jsonDoc, "p00_wifi_icon", "");
    webUI.addJson(jsonDoc, "p09_wifi_ip", "-.-.-.-");
    webUI.addJson(jsonDoc, "p09_wifi_signal", "0");
    webUI.addJson(jsonDoc, "p09_wifi_rssi", "0 dbm");
  }

  webUI.addJson(jsonDoc, "p09_eth_ip", strlen(eth.ipAddress) ? eth.ipAddress : "-.-.-.-");
  webUI.addJson(jsonDoc, "p09_eth_status", eth.connected ? WEB_TXT::CONNECTED[config.lang] : WEB_TXT::NOT_CONNECTED[config.lang]);

  // ETH information
  if (config.eth.enable) {
    if (eth.connected) {
      webUI.addJson(jsonDoc, "p00_eth_icon", "i_eth_ok");
      snprintf(tmpMessage, sizeof(tmpMessage), "%d Mbps", eth.linkSpeed);
      webUI.addJson(jsonDoc, "p09_eth_link_speed", tmpMessage);
      webUI.addJson(jsonDoc, "p09_eth_full_duplex", eth.fullDuplex ? WEB_TXT::FULL_DUPLEX[config.lang] : "---");

    } else {
      webUI.addJson(jsonDoc, "p00_eth_icon", "i_eth_nok");
      webUI.addJson(jsonDoc, "p09_eth_link_speed", "---");
      webUI.addJson(jsonDoc, "p09_eth_full_duplex", "---");
    }
  } else {
    webUI.addJson(jsonDoc, "p00_eth_icon", "");
    webUI.addJson(jsonDoc, "p09_eth_link_speed", "---");
    webUI.addJson(jsonDoc, "p09_eth_full_duplex", "---");
  }

  // MQTT Status
  webUI.addJson(jsonDoc, "p09_mqtt_status", config.mqtt.enable ? WEB_TXT::ACTIVE[config.lang] : WEB_TXT::INACTIVE[config.lang]);
  webUI.addJson(jsonDoc, "p09_mqtt_connection", mqttIsConnected() ? WEB_TXT::CONNECTED[config.lang] : WEB_TXT::NOT_CONNECTED[config.lang]);

  if (mqttGetLastError() != nullptr) {
    webUI.addJson(jsonDoc, "p09_mqtt_last_err", mqttGetLastError());
  } else {
    webUI.addJson(jsonDoc, "p09_mqtt_last_err", "---");
  }

  // ESP informations
  webUI.addJson(jsonDoc, "p09_esp_flash_usage", ESP.getSketchSize() * 100.0f / ESP.getFreeSketchSpace());
  webUI.addJson(jsonDoc, "p09_esp_heap_usage", (ESP.getHeapSize() - ESP.getFreeHeap()) * 100.0f / ESP.getHeapSize());
  webUI.addJson(jsonDoc, "p09_esp_maxallocheap", ESP.getMaxAllocHeap() / 1000.0f);
  webUI.addJson(jsonDoc, "p09_esp_minfreeheap", ESP.getMinFreeHeap() / 1000.0f);

  // Uptime
  char uptimeStr[64];
  getUptime(uptimeStr, sizeof(uptimeStr));
  webUI.addJson(jsonDoc, "p09_uptime", uptimeStr);

  // Device Counter
  devCntNew = jaroGetDevCnt();
  if (devCntNew != devCntOld) {
    devCntOld = devCntNew;
    webUI.addJson(jsonDoc, "p12_jaro_devcnt", devCntNew);
  }

  // act Time
  webUI.addJson(jsonDoc, "p09_act_time", EspStrUtil::getTimeString());

  webUI.wsUpdateWebJSON(jsonDoc);
}

/**
 * *******************************************************************
 * @brief   update System informations
 * @param   none
 * @return  none
 * *******************************************************************/
void updateSystemInfoElementsStatic() {

  webUI.initJsonBuffer(jsonDoc);

  // Version informations
  webUI.addJson(jsonDoc, "p00_version", VERSION);
  webUI.addJson(jsonDoc, "p09_sw_version", VERSION);
  webUI.addJson(jsonDoc, "p00_dialog_version", VERSION);

  webUI.addJson(jsonDoc, "p09_sw_date", EspStrUtil::getBuildDateTime());

  // restart reason
  webUI.addJson(jsonDoc, "p09_restart_reason", EspSysUtil::RestartReason::get());

  webUI.addJson(jsonDoc, "p12_jaro_devcnt", jaroGetDevCnt());

  // Sunrise, Sunset - "--:--" where there is no event at all (polar day/night),
  // rather than the 23:59 the sentinel used to be formatted into. This is the
  // only visible sign that an astro timer will stay idle today, so it must not
  // show a plausible-looking time.
  uint8_t sunriseHour, sunriseMinute;
  if (getSunriseOrSunset(TYPE_SUNRISE, 0, config.geo.latitude, config.geo.longitude, sunriseHour, sunriseMinute)) {
    snprintf(tmpMessage, sizeof(tmpMessage), "%02d:%02d", sunriseHour, sunriseMinute);
  } else {
    snprintf(tmpMessage, sizeof(tmpMessage), "--:--");
  }
  webUI.addJson(jsonDoc, "p09_sunrise", tmpMessage);

  uint8_t sundownHour, sundownMinute;
  if (getSunriseOrSunset(TYPE_SUNDOWN, 0, config.geo.latitude, config.geo.longitude, sundownHour, sundownMinute)) {
    snprintf(tmpMessage, sizeof(tmpMessage), "%02d:%02d", sundownHour, sundownMinute);
  } else {
    snprintf(tmpMessage, sizeof(tmpMessage), "--:--");
  }
  webUI.addJson(jsonDoc, "p09_sundown", tmpMessage);

  // Date
  webUI.addJson(jsonDoc, "p09_act_date", EspStrUtil::getDateString());

  // ESP-Info
  webUI.addJson(jsonDoc, "p09_esp_chip_series", espInfo.chipSeries);
  webUI.addJson(jsonDoc, "p09_esp_chip_model", espInfo.chipModel);
  webUI.addJson(jsonDoc, "p09_esp_chip_rev", espInfo.chipRev);
  webUI.addJson(jsonDoc, "p09_esp_chip_mhz", espInfo.chipMhz);
  webUI.addJson(jsonDoc, "p09_esp_flash_size", espInfo.flashSize);

  webUI.wsUpdateWebJSON(jsonDoc);
}

/**
 * *******************************************************************
 * @brief   check id read log buffer is active
 * @param   none
 * @return  none
 * *******************************************************************/
bool webLogRefreshActive() { return logReadActive; }

/**
 * *******************************************************************
 * @brief   start reading log buffer
 * @param   none
 * @return  none
 * *******************************************************************/
void webReadLogBuffer() {
  logReadActive = true;
  logLine = 0;
  logIdx = 0;
}

/**
 * *******************************************************************
 * @brief   update Logger output
 * @param   none
 * @return  none
 * *******************************************************************/
void webReadLogBufferCyclic() {

  jsonLog.clear();
  jsonLog["type"] = "logger";
  jsonLog["cmd"] = "add_log";
  JsonArray entryArray = jsonLog["entry"].to<JsonArray>();

  while (logReadActive) {

    if (logLine == 0 && logData.lastLine == 0) {
      // log empty
      logReadActive = false;
      return;
    }
    if (config.log.order == 1) {
      logIdx = (logData.lastLine - logLine - 1) % MAX_LOG_LINES;
    } else {
      if (logData.buffer[logData.lastLine][0] == '\0') {
        // buffer is not full - start reading at element index 0
        logIdx = logLine % MAX_LOG_LINES;
      } else {
        // buffer is full - start reading at element index "logData.lastLine"
        logIdx = (logData.lastLine + logLine) % MAX_LOG_LINES;
      }
    }
    if (logIdx < 0) {
      logIdx += MAX_LOG_LINES;
    }
    if (logIdx >= MAX_LOG_LINES) {
      logIdx = 0;
    }
    if (logLine == MAX_LOG_LINES - 1) {
      // end
      webUI.wsUpdateWebJSON(jsonLog);
      logReadActive = false;
      return;
    } else {
      if (logData.buffer[logIdx][0] != '\0') {
        entryArray.add(logData.buffer[logIdx]);
        logLine++;
      } else {
        // no more entries
        logReadActive = false;
        webUI.wsUpdateWebJSON(jsonLog);
        return;
      }
    }
  }
}

/**
 * *******************************************************************
 * @brief   callback function for OTA progress
 * @param   none
 * @return  none
 * *******************************************************************/
void otaProgressCallback(int progress) {
  if (otaProgessTimer.cycleTrigger(1000)) {
    webUI.wsSendHeartbeat();
    char buttonTxt[32];
    snprintf(buttonTxt, sizeof(buttonTxt), "updating: %i%%", progress);
    webUI.wsUpdateWebText("p00_update_btn", buttonTxt, false);
  }
}

/**
 * *******************************************************************
 * @brief   initiate GitHub version check
 * @param   none
 * @return  none
 * *******************************************************************/
bool startCheckGitHubVersion;
void requestGitHubVersion() { startCheckGitHubVersion = true; }

/**
 * *******************************************************************
 * @brief   free the cached GitHub release and reset it to an empty state
 * @details ghFreeRelease() only free()s the strings - it leaves every member of
 *          the struct dangling and the asset vector populated, so a second free
 *          would be a double free and flashFirmware() would strcmp() freed asset
 *          names. Overwriting the struct afterwards drops those pointers, which
 *          is what makes this safe to call on every path and more than once.
 *          ghReleaseInfo is derived from the same fetch and is cleared with it,
 *          so the two can never end up describing different releases.
 * @param   none
 * @return  none
 * *******************************************************************/
static void ghReleaseClear() {
  ghFreeRelease(ghLatestRelease);
  ghLatestRelease = GithubRelease{};
  ghReleaseInfo = GithubReleaseInfo{};
}

void processGitHubVersion() {
  if (startCheckGitHubVersion) {
    startCheckGitHubVersion = false;

    // ghGetLatestRelease() assigns a freshly allocated release over the struct,
    // so whatever is still held here has to go first. Without this, a repeated
    // "check version" while an update was available made the previous
    // allocation unreachable - a whole release including every asset string.
    ghReleaseClear();

    if (ghGetLatestRelease(&ghLatestRelease, &ghReleaseInfo, espInfo.chipSeries)) {
      webUI.wsUpdateWebBusy("p00_dialog_git_version", false);
      webUI.wsUpdateWebText("p00_dialog_git_version", ghReleaseInfo.tag, false);
      webUI.wsUpdateWebHref("p00_dialog_git_version", ghReleaseInfo.url);
      // if new version is available, show update button and keep the release -
      // processGitHubUpdate() needs its asset list to flash the firmware
      if (strcmp(ghReleaseInfo.tag, VERSION) != 0 && ghReleaseInfo.assetFound) {
        char buttonTxt[32];
        snprintf(buttonTxt, sizeof(buttonTxt), "Update %s", ghReleaseInfo.tag);
        webUI.wsUpdateWebText("p00_update_btn", buttonTxt, false);
        webUI.wsUpdateWebDisabled("p00_update_btn", false); // a failed attempt leaves it disabled
        webUI.wsUpdateWebHideElement("p00_update_btn_hide", false);
      } else {
        // nothing to flash: drop the release and hide a button that an earlier
        // check may have left on screen - clicking it used to hand the OTA an
        // asset list whose strings had already been freed
        ghReleaseClear();
        webUI.wsUpdateWebHideElement("p00_update_btn_hide", true);
      }
    } else {
      webUI.wsUpdateWebBusy("p00_dialog_git_version", false);
      webUI.wsUpdateWebText("p00_dialog_git_version", "error", false);
      ghReleaseClear();
      webUI.wsUpdateWebHideElement("p00_update_btn_hide", true);
    }
  }
}

/**
 * *******************************************************************
 * @brief   initiate GitHub version OTA update
 * @param   none
 * @return  none
 * *******************************************************************/
bool startGitHubUpdate;
void requestGitHubUpdate() { startGitHubUpdate = true; }
void processGitHubUpdate() {
  if (startGitHubUpdate) {
    startGitHubUpdate = false;

    // The button is only hidden client side, so a stale tab can still send this
    // after a later version check dropped the cached release. Refuse before
    // disabling the watchdog and raising the OTA flag - that flag also suspends
    // the cyclic WebUI refresh, and there is nothing to flash either way.
    if (!ghReleaseInfo.assetFound || ghLatestRelease.assets.empty()) {
      ESP_LOGW(TAG, "GitHub OTA-Update rejected: no release cached");
      webUI.wsUpdateWebText("p00_ota_upd_err", "no release cached", false);
      webUI.wsUpdateWebDialog("version_dialog", "close");
      webUI.wsUpdateWebDialog("ota_update_failed_dialog", "open");
      webUI.wsUpdateWebHideElement("p00_update_btn_hide", true);
      return;
    }

    ghSetProgressCallback(otaProgressCallback);
    webUI.wsUpdateWebText("p00_update_btn", "updating: 0%", false);
    webUI.wsUpdateWebDisabled("p00_update_btn", true);
    ota.setActive(true);
    wdt.disable();
    int result = ghStartOtaUpdate(ghLatestRelease, ghReleaseInfo.asset);
    if (result == OTA_SUCCESS) {
      webUI.wsUpdateWebText("p00_update_btn", "updating: 100%", false);
      webUI.wsUpdateWebDialog("version_dialog", "close");
      webUI.wsUpdateWebDialog("ota_update_done_dialog", "open");
      ESP_LOGI(TAG, "GitHub OTA-Update successful");
    } else {
      char errMsg[32];
      switch (result) {
      case OTA_NULL_URL:
        strcpy(errMsg, "URL is NULL");
        break;
      case OTA_CONNECT_ERROR:
        strcpy(errMsg, "Connection error");
        break;
      case OTA_BEGIN_ERROR:
        strcpy(errMsg, "Begin error");
        break;
      case OTA_WRITE_ERROR:
        strcpy(errMsg, "Write error");
        break;
      case OTA_END_ERROR:
        strcpy(errMsg, "End error");
        break;
      default:
        strcpy(errMsg, "Unknown error");
        break;
      }
      webUI.wsUpdateWebText("p00_ota_upd_err", errMsg, false);
      webUI.wsUpdateWebDialog("version_dialog", "close");
      webUI.wsUpdateWebDialog("ota_update_failed_dialog", "open");
      ESP_LOGE(TAG, "GitHub OTA-Update failed: %s", errMsg);
    }
    ota.setActive(false);
    wdt.enable();

    // The cached release has served its purpose either way: after a successful
    // flash the device is about to be restarted, after a failure the user has to
    // run a fresh version check before another attempt. Freeing it here keeps a
    // single owner for the allocation - ghStartOtaUpdate() no longer frees it -
    // and stops a retry from working on a stale asset list.
    ghReleaseClear();
    webUI.wsUpdateWebHideElement("p00_update_btn_hide", true);
  }
}

/**
 * *******************************************************************
 * @brief   cyclic update of webUI elements
 * @param   none
 * @return  none
 * *******************************************************************/
void webUIupdates() {

  // check if new version is available
  processGitHubVersion();
  // perform GitHub update
  processGitHubUpdate();

  // update webUI Logger
  if (webLogRefreshActive()) {
    webReadLogBufferCyclic();
  }

  // ON-BROWSER-REFRESH: refresh ALL elements - do this step by step not to stress the connection
  if (refreshTimer1.cycleTrigger(WEBUI_FAST_REFRESH_TIME_MS) && refreshRequest && !ota.isActive()) {

    updateSystemInfoElementsStatic(); // update static informations (≈ 200 Bytes)
    refreshRequest = false;
  }

  // CYCLIC: update SINGLE elemets every x seconds - do this step by step not to stress the connection
  if (refreshTimer2.cycleTrigger(WEBUI_SLOW_REFRESH_TIME_MS) && !refreshRequest && !ota.isActive()) {

    if (!setupMode) {
      if (getCC1101State() == false) {
        webUI.wsUpdateWebHideElement("errorBar", false);
        webUI.wsUpdateWebText("errorBarText", WEB_TXT::CC1101_NOT_FOUND[config.lang], false);
      } else if (config.jaro.masterLSB == 0 || config.jaro.masterMSB == 0) {
        webUI.wsUpdateWebHideElement("errorBar", false);
        webUI.wsUpdateWebText("errorBarText", WEB_TXT::JARO_KEYS_INVALID[config.lang], false);
      } else if (config.jaro.serial == 0) {
        webUI.wsUpdateWebHideElement("errorBar", false);
        webUI.wsUpdateWebText("errorBarText", WEB_TXT::SERIAL_INVALID[config.lang], false);
      } else {
        webUI.wsUpdateWebHideElement("errorBar", true);
      }
    }
    updateSystemInfoElements(); // refresh all "System" elements as one big JSON update (≈ 570 Bytes)
    updateShutterPositions();
  }
}
