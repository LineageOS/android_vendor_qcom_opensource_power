/*
 * Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
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
#include "performance.h"
#include "power-common.h"
#include "utils.h"

static struct hint_handles handles[NUM_HINTS];
static int handleER = 0;

const int kMaxLaunchDuration = 5000;      /* ms */
const int kMaxInteractiveDuration = 5000; /* ms */
const int kMinInteractiveDuration = 500;  /* ms */

void power_init() {
    ALOGI("Initing");

    for (int i = 0; i < NUM_HINTS; i++) {
        handles[i].handle = 0;
        handles[i].ref_count = 0;
    }
}

void process_interaction_hint(void* data) {
    static struct timespec s_previous_boost_timespec;
    static int s_previous_duration = 0;
    static int prev_interaction_handle = -1;

    struct timespec cur_boost_timespec;
    long long elapsed_time;
    int duration = kMinInteractiveDuration;

    if (data) {
        int input_duration = *((int*)data);
        if (input_duration > duration) {
            duration = (input_duration > kMaxInteractiveDuration) ? kMaxInteractiveDuration
                                                                  : input_duration;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

    elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
    // don't hint if it's been less than 250ms since last boost
    // also detect if we're doing anything resembling a fling
    // support additional boosting in case of flings
    if (elapsed_time < 250000 && duration <= 750) {
        return;
    }
    s_previous_boost_timespec = cur_boost_timespec;
    s_previous_duration = duration;

    int interaction_handle =
            perf_hint_enable_with_type(VENDOR_HINT_SCROLL_BOOST, duration, SCROLL_VERTICAL);

    if (CHECK_HANDLE(prev_interaction_handle)) {
        release_request(prev_interaction_handle);
    }
    prev_interaction_handle = interaction_handle;
}

void process_activity_launch_hint(void* data) {
    static int launch_handle = -1;
    static int launch_mode = 0;

    // release lock early if launch has finished
    if (!data) {
        if (CHECK_HANDLE(launch_handle)) {
            release_request(launch_handle);
            launch_handle = -1;
        }
        launch_mode = 0;
        return;
    }

    if (!launch_mode) {
        launch_handle = perf_hint_enable_with_type(VENDOR_HINT_FIRST_LAUNCH_BOOST,
                                                   kMaxLaunchDuration, LAUNCH_BOOST_V1);
        if (!CHECK_HANDLE(launch_handle)) {
            ALOGE("Failed to perform launch boost");
            return;
        }
        launch_mode = 1;
    }
}

int __attribute__((weak)) power_hint_override(power_hint_t hint, void* data) {
    return HINT_NONE;
}

void power_hint(power_hint_t hint, void* data) {
    /* Check if this hint has been overridden. */
    if (power_hint_override(hint, data) == HINT_HANDLED) {
        /* The power_hint has been handled. We can skip the rest. */
        return;
    }
    switch (hint) {
        case POWER_HINT_VR_MODE:
            ALOGI("VR mode power hint not handled in power_hint_override");
            break;
        // fall through below, hints will fail if not defined in powerhint.xml
        case POWER_HINT_SUSTAINED_PERFORMANCE:
        case POWER_HINT_VIDEO_ENCODE:
            if (data) {
                if (handles[hint].ref_count == 0)
                    handles[hint].handle = perf_hint_enable((AOSP_DELTA + hint), 0);

                if (handles[hint].handle > 0) handles[hint].ref_count++;
            } else {
                if (handles[hint].handle > 0) {
                    if (--handles[hint].ref_count == 0) {
                        release_request(handles[hint].handle);
                        handles[hint].handle = 0;
                    }
                } else {
                    ALOGE("Lock for hint: %X was not acquired, cannot be released", hint);
                }
            }
            break;
        case POWER_HINT_INTERACTION:
            process_interaction_hint(data);
            break;
        case POWER_HINT_LAUNCH:
            process_activity_launch_hint(data);
            break;
        default:
            break;
    }
}

bool is_expensive_rendering_supported() {
    char property[PROPERTY_VALUE_MAX];
    strlcpy(property, perf_get_property("vendor.perf.expensive_rendering", "0").value,
            PROPERTY_VALUE_MAX);
    return atoi(property) == 1 ? true : false;
}

void set_expensive_rendering(bool enabled) {
    if (enabled) {
        handleER = perf_hint_enable(PERF_HINT_EXPENSIVE_RENDERING, 0);
    } else if (handleER > 0) {
        release_request(handleER);
    }
}

int __attribute__((weak)) set_interactive_override(int on) {
    return HINT_NONE;
}

#ifdef SET_INTERACTIVE_EXT
extern void power_set_interactive_ext(int on);
#endif

void set_interactive(int on) {
    static int display_hint_sent;

    if (!on) {
        /* Send Display OFF hint to perf HAL */
        perf_hint_enable(VENDOR_HINT_DISPLAY_OFF, 0);
    } else {
        /* Send Display ON hint to perf HAL */
        perf_hint_enable(VENDOR_HINT_DISPLAY_ON, 0);
    }

    /**
     * Ignore consecutive display-off hints
     * Consecutive display-on hints are already handled
     */
    if (display_hint_sent && !on) return;

    display_hint_sent = !on;

#ifdef SET_INTERACTIVE_EXT
    power_set_interactive_ext(on);
#endif

    if (set_interactive_override(on) == HINT_HANDLED) {
        return;
    } else {
        ALOGI("Hint not handled in set_interactive_override");
    }
}
