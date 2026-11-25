// Fingerprint database: recognises interesting devices from MAC OUIs, SSIDs,
// BLE names, BLE company IDs, BLE service UUIDs and Remote ID broadcasts.
#pragma once
#include "devices.h"

namespace signatures {

// (Re)classify a device in place: sets category, confidence, label, vendor, reason.
void classify(Device& d);

// MAC OUI -> vendor name (nullptr if unknown / randomised)
const char* ouiVendor(const uint8_t* mac);

// BLE company id -> name (nullptr if unknown)
const char* companyName(uint16_t id);

}  // namespace signatures
