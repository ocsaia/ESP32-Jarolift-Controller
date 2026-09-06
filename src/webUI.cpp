#include <LittleFS.h>
#include <Update.h>
#include <basics.h>
#include <cmdQueue.h>
#include <language.h>
#include <message.h>
#include <webUI.h>
#include <webUIupdates.h>

const int MAX_WS_CLIENT = 3;
const int CHUNK_SIZE = 1024;

/* D E C L A R A T I O N S ****************************************************/
static muTimer heartbeatTimer = muTimer(); // timer to refresh other values
static muTimer onLoadTimer = muTimer();    // timer to refresh other values

EspWebUI webUI(80);

static const char *TAG = "WEB"; // LOG TAG
static bool webInitDone = false;
static const size_t BUFFER_SIZE = 512;

// Set by the AsyncTCP task, read and cleared by loop() - volatile so the
// compiler cannot keep it in a register across the cyclic check. It stays a
// latch rather than a queue entry on purpose: repeated reload requests have to
// collapse into a single updateAllElements(), which is what a latch does and a
// queue does not.
static volatile bool onLoadRequest = false;

static auto &wdt = EspSysUtil::Wdt::getInstance();
static auto &ota = EspSysUtil::OTA::getInstance();

/**
 * *******************************************************************
 * @brief   cyclic call for webUI - creates all webUI elements
 * @param   none
 * @return  none
 * *******************************************************************/
void webUISetup() {

  webUI.setCallbackOta([](EspWebUI::otaStatus otaState, const char *msg) {
    switch (otaState) {
    case EspWebUI::OTA_BEGIN:
      ota.setActive(true);
      wdt.disable();
      break;
    case EspWebUI::OTA_PROGRESS:
      webUI.wsUpdateOTAprogress(msg);
      break;
    case EspWebUI::OTA_FINISH:
      ota.setActive(false);
      wdt.enable();
      webUI.wsUpdateOTAprogress("100");
      webUI.wsUpdateWebDialog("ota_update_done_dialog", "open");
      break;
    case EspWebUI::OTA_ERROR:
      ota.setActive(false);
      wdt.enable();
      webUI.wsUpdateWebText("p00_ota_upd_err", msg, false);
      webUI.wsUpdateWebDialog("ota_update_failed_dialog", "open");
      break;
    }
  });

  webUI.setCallbackUpload([](EspWebUI::uploadStatus uploadState, const char *msg) {
    switch (uploadState) {
    case EspWebUI::UPLOAD_BEGIN:
      webUI.wsUpdateWebText("upload_status_txt", msg, false);
      break;
    case EspWebUI::UPLOAD_FINISH:
      webUI.wsUpdateWebText("upload_status_txt", msg, false);
      configLoadFromFile(); // load configuration
      webUI.wsUpdateWebLanguage(LANG::CODE[config.lang]);
      webUI.wsLoadConfigWebUI(); // update webUI settings
      break;
    case EspWebUI::UPLOAD_ERROR:
      webUI.wsUpdateWebText("upload_status_txt", msg, false);
      break;
    }
  });

  // callback for reload - the AsyncTCP task only sets the latch, loop() acts on it
  webUI.setCallbackReload([]() { onLoadRequest = true; });

  // callback for web elements - runs in the AsyncTCP task, so the event is only
  // copied into the cross-task command queue here and dispatched later by
  // cmdQueueCyclic(). cmdQueuePushWebElement() is NULL safe: EspWebUI passes the
  // parsed JSON members straight through and both are NULL when a client omits
  // them, which used to reach snprintf("%s", NULL).
  webUI.setCallbackWebElement([](const char *elementID, const char *elementValue) { cmdQueuePushWebElement(elementID, elementValue); });

  webUI.setCredentials(config.auth.user, config.auth.password);
  webUI.setAuthentication(config.auth.enable);

  webUI.begin();
} // END SETUP

/**
 * *******************************************************************
 * @brief   cyclic call for webUI - refresh elements by change
 * @param   none
 * @return  none
 * *******************************************************************/
void webUICyclic() {

  webUI.loop();

  // request for update alle elements - not faster than every 1s
  if (onLoadRequest && onLoadTimer.cycleTrigger(1000)) {
    updateAllElements();
    onLoadRequest = false;
    ESP_LOGD(TAG, "updateAllElements()");
  }

  // handling of update webUI elements
  webUIupdates();

  // web element callbacks are dispatched by cmdQueueCyclic() in loop() - this
  // block handled exactly one event per iteration, which is what lost all but
  // the last field of a settings form sent as a single burst

  webInitDone = true; // init done
}
