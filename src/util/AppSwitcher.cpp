#include "AppSwitcher.h"

#ifdef SIMULATOR

namespace app_switcher {
bool otherAppAvailable() { return false; }
const char* otherAppName() { return "Other App"; }
void registerSelfName(const char*) {}
bool switchToOtherApp() { return false; }
}  // namespace app_switcher

#else

#include <Logging.h>
#include <Preferences.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <cstdio>
#include <string>

#include "network/OtaBootSwitch.h"

namespace app_switcher {
namespace {

std::string cachedName;
const esp_partition_t* cachedPartition = nullptr;
bool scanned = false;

void nameKeyForSlot(const esp_partition_t* part, char* key, size_t keyLen) {
  const int slot = part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  snprintf(key, keyLen, "ota_%d", slot);
}

void scanOtherApp() {
  if (scanned) return;
  scanned = true;

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return;

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr) {
    const esp_partition_t* part = esp_partition_get(it);
    if (part && part != running && part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        part->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
      esp_app_desc_t desc;
      if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        cachedPartition = part;

        char key[8];
        nameKeyForSlot(part, key, sizeof(key));
        Preferences prefs;
        if (prefs.begin("ota_names", true)) {
          String name = prefs.getString(key, "");
          prefs.end();
          if (name.length() > 0) cachedName = name.c_str();
        }
        if (cachedName.empty() && desc.project_name[0] != '\0') cachedName = desc.project_name;
        if (cachedName.empty()) cachedName = "Other App";
        LOG_INF("BOOT", "Companion app in OTA slot %d: %s", part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0,
                cachedName.c_str());
        break;
      }
    }
    it = esp_partition_next(it);
  }
  if (it != nullptr) esp_partition_iterator_release(it);
}

}  // namespace

bool otherAppAvailable() {
  scanOtherApp();
  return cachedPartition != nullptr;
}

const char* otherAppName() {
  scanOtherApp();
  return cachedName.c_str();
}

void registerSelfName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!self || self->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 || self->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_15) {
    return;
  }
  char key[8];
  nameKeyForSlot(self, key, sizeof(key));
  Preferences prefs;
  if (!prefs.begin("ota_names", false)) return;
  if (prefs.getString(key, "") != name) {
    prefs.putString(key, name);
    LOG_INF("BOOT", "Registered app name \"%s\" for NVS key %s", name, key);
  }
  prefs.end();
}

bool switchToOtherApp() {
  scanOtherApp();
  if (!cachedPartition) return false;
  LOG_INF("BOOT", "Switching boot to \"%s\" (subtype 0x%02X)", cachedName.c_str(), cachedPartition->subtype);
  if (!ota_boot::switchTo(cachedPartition)) {
    LOG_ERR("BOOT", "otadata switch failed");
    return false;
  }
  esp_restart();
  return true;  // unreachable
}

}  // namespace app_switcher

#endif  // SIMULATOR
