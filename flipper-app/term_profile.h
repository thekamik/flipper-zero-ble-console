#pragma once

#include <furi_ble/profile_interface.h>
#include <services/serial_service.h>

/* Custom BLE profile with the serial service. Its template pointer differs from
 * ble_profile_serial, so the bt service does not attach mobile app RPC to it. */
extern const FuriHalBleProfileTemplate* const term_profile;

BleServiceSerial* term_profile_get_serial(FuriHalBleProfileBase* profile);
