/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** Minimal customer-side machine-id collector for model.key issuance. */
#include <stdio.h>
#include <sodium.h>

#include "license_machine.h"

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "error: libsodium initialization failed\n");
        return 1;
    }
    lic_fp_info info;
    if (lic_machine_id_collect(&info) != 0) {
        fprintf(stderr, "error: machine id unavailable\n");
        fprintf(stderr, "  dt-serial: %s\n", info.note_dt);
        fprintf(stderr, "  otp: %s\n", info.note_otp);
        fprintf(stderr, "  mac: %s\n", info.note_mac);
        return 1;
    }
    puts(info.machine_id);
    fprintf(stderr, "source=%s path=%s\n", info.tag, info.path);
    return 0;
}
