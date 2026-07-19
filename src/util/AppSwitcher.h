#pragma once

// Dual-boot companion-app support. A second firmware (e.g. MicroSlate) can
// live in the other OTA app slot; these helpers expose it as a home menu
// entry and switch the boot partition to it.
//
// Display-name registry convention (shared with MicroSlate): NVS namespace
// "ota_names", key "ota_<slot>" holds each app's display name, written by the
// app itself on boot.
namespace app_switcher {

// True if the other OTA slot contains a valid app image. Result is cached
// after the first call (the slot cannot change without a reboot).
bool otherAppAvailable();

// Display name for the other app: NVS registry entry, else the image's
// build-time project name, else "Other App". Pointer stays valid for the
// lifetime of the program.
const char* otherAppName();

// Write our own display name into the registry so the other app can label
// its switch-back menu entry.
void registerSelfName(const char* name);

// Point otadata at the other app and restart. Returns only on failure.
bool switchToOtherApp();

}  // namespace app_switcher
