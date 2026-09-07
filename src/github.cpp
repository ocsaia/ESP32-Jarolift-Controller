#include <Arduino.h>
#include <basics.h>
#include <github.h>
#include <message.h>

/*
 * Where the WebUI's update check looks.
 *
 * This must be the fork, not upstream. The check is a plain string inequality -
 * webUIupdates.cpp asks whether the latest release tag differs from VERSION,
 * not whether it is newer - so any upstream release with a tag this firmware
 * does not carry appears as "an update is available", and one click installs
 * it over the fork.
 *
 * That is not a theoretical loss. Upstream firmware writes CFG_VERSION 2 back
 * over a V5 config, taking the sixteen measured travel times, the twilight
 * modes and timer slots 7-24 with it, and restoring the old inverted position
 * convention that Home Assistant is now configured against. The KeeLoq keys and
 * the base serial survive, so nothing has to be re-taught - but everything
 * built on top of them is gone.
 *
 * Note the direction of the trap: raising VERSION without changing this line
 * makes it worse, because upstream's current v1.9.0 would then differ from
 * VERSION and be offered as an update - a downgrade presented as an upgrade.
 */
#define GITHUB_OWNER "ocsaia"
#define GITHUB_REPO "ESP32-Jarolift-Controller"

static const char *TAG = "GITHUB"; // LOG TAG

GithubReleaseOTA ota(GITHUB_OWNER, GITHUB_REPO);

/**
 * *******************************************************************
 * @brief   check if the asset name contains the chip series
 * @param   assetName
 * @param   espSeries
 * @return  true if the asset name contains the chip series, else false
 * *******************************************************************/
bool isChipSeriesMatch(const char *assetName, const char *espSeries) {
  if (assetName == NULL || espSeries == NULL)
    return false;

  char formattedEspSeries[16];
  size_t j = 0;
  for (size_t i = 0; espSeries[i] != '\0' && j < sizeof(formattedEspSeries) - 1; i++) {
    if (espSeries[i] != '-') { // remove '-' from chip series
      formattedEspSeries[j++] = tolower((unsigned char)espSeries[i]);
    }
  }
  formattedEspSeries[j] = '\0';

  // check if the asset name contains the chip series
  size_t len = strlen(formattedEspSeries);
  return (strncasecmp(assetName, formattedEspSeries, len) == 0 && assetName[len] == '_');
}

/**
 * *******************************************************************
 * @brief   set the progress callback
 * @param   callback
 * @return  none
 * *******************************************************************/
void ghSetProgressCallback(void (*callback)(int)) { ota.setProgressCallback(callback); }

/**
 * *******************************************************************
 * @brief   get the latest release from GitHub
 * @param   release
 * @param   info
 * @param   espSeries
 * @return  true if successful, else false
 * *******************************************************************/
bool ghGetLatestRelease(GithubRelease *release, GithubReleaseInfo *info, const char *espSeries) {

  if (release == nullptr || info == nullptr) {
    return false;
  }

  // Get the latest release from GitHub
  *release = ota.getLatestRelease();

  // check if the release is valid
  // Order matters here: html_url has a default member initialiser in the
  // library's struct, tag_name has none. When the HTTP request fails,
  // getLatestRelease() returns an object whose tag_name still holds whatever was
  // on the stack, so it must be neither read nor free()d. A non-null html_url
  // proves makeRelease() ran, and with it that tag_name holds a real value.
  if (release->html_url == nullptr) {
    release->tag_name = nullptr; // may be indeterminate - keep the caller's free safe
    return false;
  }
  if (release->tag_name == nullptr) {
    return false;
  }

  snprintf(info->tag, sizeof(info->tag), "%s", release->tag_name);
  snprintf(info->url, sizeof(info->url), "%s", release->html_url);
  ESP_LOGD(TAG, "GitHub latest Release: %s", info->tag);

  // search for the first asset that contains the right "chipSeries" and "ota or UPDATE" in its name
  info->assetFound = false;
  for (const auto &asset : release->assets) {
    if (isChipSeriesMatch(asset.name, espSeries) && strcasestr(asset.name, "ota") != NULL) {
      ESP_LOGD(TAG, "GitHub OTA Asset found: %s", asset.name);
      snprintf(info->asset, sizeof(info->asset), "%s", asset.name);
      info->assetFound = true;
      break;
    }
  }

  return true;
}

/**
 * *******************************************************************
 * @brief   start the OTA update
 * @details the release stays owned by the caller, it is only read here
 * @param   release
 * @param   asset
 * @return  0 if successful, else error code
 * *******************************************************************/
int ghStartOtaUpdate(const GithubRelease &release, const char *asset) {
  int result = ota.flashFirmware(release, asset);

  if (result == 0) {
    ESP_LOGI(TAG, "Firmware updated successfully");
  } else {
    // no freeRelease() here: the release used to be taken by value, so freeing
    // it released the strings of a shallow copy and left the caller's struct
    // pointing at freed memory, which a retry then read back. One owner, in
    // webUIupdates.cpp.
    ESP_LOGE(TAG, "Firmware update failed: %i", result);
  }
  return result;
}

/**
 * *******************************************************************
 * @brief   free the release memory
 * @param   release
 * @return  none
 * *******************************************************************/
void ghFreeRelease(GithubRelease &release) { ota.freeRelease(release); }