#include "term_profile.h"
#include "protocol.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <ble/core/ble_defs.h>

typedef struct {
    FuriHalBleProfileBase base; /* must be the first field */
    BleServiceSerial* serial_svc;
} BleProfileTerm;

static FuriHalBleProfileBase* term_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BleProfileTerm* profile = malloc(sizeof(BleProfileTerm));
    profile->base.config = term_profile;
    profile->serial_svc = ble_svc_serial_start();
    return &profile->base;
}

/* The core frees the profile memory (as with hid_profile.c); only stop services. */
static void term_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == term_profile);
    BleProfileTerm* profile = (BleProfileTerm*)base;
    ble_svc_serial_stop(profile->serial_svc);
}

static void term_profile_get_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memset(config, 0, sizeof(GapConfig));

    config->adv_service.UUID_Type = UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = 0x3080 | furi_hal_version_get_hw_color();
    config->appearance_char = 0x8600;
    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeShow; /* PIN shown on the Flipper screen */
    config->conn_param.conn_int_min = 0x06; /* 7.5 ms */
    config->conn_param.conn_int_max = 0x24; /* 45 ms */

    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += PITERM_MAC_OFFSET;

    /* The first byte is the AD record type (complete local name). */
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%c" PITERM_ADV_NAME_PREFIX " %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0],
        furi_hal_version_get_name_ptr());
}

static const FuriHalBleProfileTemplate term_profile_template = {
    .start = term_profile_start,
    .stop = term_profile_stop,
    .get_gap_config = term_profile_get_config,
};

const FuriHalBleProfileTemplate* const term_profile = &term_profile_template;

BleServiceSerial* term_profile_get_serial(FuriHalBleProfileBase* profile) {
    furi_check(profile && profile->config == term_profile);
    return ((BleProfileTerm*)profile)->serial_svc;
}
