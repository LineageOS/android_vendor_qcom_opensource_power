/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "QTI PowerHAL"
#include <hardware/hardware.h>
#include <hardware/power.h>
#include <log/log.h>

#include "hint-data.h"
#include "metadata-defs.h"
#include "performance.h"
#include "power-common.h"
#include "utils.h"

#define MIN_VAL(X, Y) ((X > Y) ? (Y) : (X))

/**
 * Returns true if the target is SDM630/SDM455.
 */
static bool is_target_SDM630(void) {
    static int is_SDM630 = -1;
    int soc_id;

    if (is_SDM630 >= 0) return is_SDM630;

    soc_id = get_soc_id();
    is_SDM630 = soc_id == 318 || soc_id == 327 || soc_id == 385;

    return is_SDM630;
}

int set_interactive_override(int on) {
    char governor[80];
    int resource_values[20];
    int num_resources;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_HANDLED;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
        if (is_interactive_governor(governor)) {
            /*
                1. CPUfreq params
                       - hispeed freq for big - 1113Mhz
                       - go hispeed load for big - 95
                       - above_hispeed_delay for big - 40ms
               2. BusDCVS V2 params
                       - Sample_ms of 10ms
           */
            if (is_target_SDM630()) {
                int res[] = {0x41414000, 0x459, 0x41410000, 0x5F, 0x41400000, 0x4, 0x41820000, 0xA};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = ARRAY_SIZE(res);
            }
            /*
                1. CPUfreq params
                       - hispeed freq for little - 902Mhz
                       - go hispeed load for little - 95
                       - above_hispeed_delay for little - 40ms
                2. BusDCVS V2 params
                       - Sample_ms of 10ms
                3. Sched group upmigrate - 500
           */
            else {
                int res[] = {0x41414100, 0x386,      0x41410100, 0x5F,       0x41400100,
                             0x4,        0x41820000, 0xA,        0x40C54000, 0x1F4};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = ARRAY_SIZE(res);
            }
            perform_hint_action(DISPLAY_STATE_HINT_ID, resource_values, num_resources);
        }

    } else {
        /* Display on. */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
        }
    }
    return HINT_HANDLED;
}
