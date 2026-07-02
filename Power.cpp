/*
 * Copyright (c) 2019-2020 The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
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
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "QTI PowerHAL"
#define VENDOR_FEEDBACK_GPU_HEADROOM 0x00001603
#define VENDOR_FEEDBACK_CPU_HEADROOM 0x00001610

#define GPU_HEADROOM_AVG 2
#define GPU_HEADROOM_MIN 3
#define GPU_MIN_DURATION_MS 1000
#define GPU_MAX_DURATION_MS 60000

#define CPU_HEADROOM_TOTAL 6
#define CPU_MIN_DURATION_MS 1000
#define CPU_MAX_DURATION_MS 60000
static const char* pkg = "QTI PowerHAL";

#include "Power.h"
#include "PowerHintSession.h"
#include "utils.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <fmq/AidlMessageQueue.h>
#include <fmq/EventFlag.h>
#include <thread>

#include <aidl/android/hardware/power/BnPower.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::aidl::android::hardware::power::BnPower;
using ::aidl::android::hardware::power::Boost;
using ::aidl::android::hardware::power::ChannelMessage;
using ::aidl::android::hardware::power::CompositionData;
using ::aidl::android::hardware::power::CompositionUpdate;
using ::aidl::android::hardware::power::IPower;
using ::aidl::android::hardware::power::Mode;
using ::android::AidlMessageQueue;

using ::ndk::ScopedAStatus;
using ::ndk::SharedRefBase;

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace impl {

#ifdef MODE_EXT
extern bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return);
extern bool setDeviceSpecificMode(Mode type, bool enabled);
#endif

void setInteractive(bool interactive) {
    set_interactive(interactive ? 1 : 0);
}

template <class T>
constexpr size_t enum_size() {
    return static_cast<size_t>(*(ndk::enum_range<T>().end() - 1)) + 1;
}

ndk::ScopedAStatus Power::setMode(Mode type, bool enabled) {
    LOG(INFO) << "Power setMode: " << static_cast<int32_t>(type) << " to: " << enabled;
#ifdef MODE_EXT
    if (setDeviceSpecificMode(type, enabled)) {
        return ndk::ScopedAStatus::ok();
    }
#endif
    switch (type) {
#ifdef TAP_TO_WAKE_NODE
        case Mode::DOUBLE_TAP_TO_WAKE:
            ::android::base::WriteStringToFile(enabled ? "1" : "0", TAP_TO_WAKE_NODE, true);
            break;
#else
        case Mode::DOUBLE_TAP_TO_WAKE:
#endif
        case Mode::LOW_POWER:
        case Mode::DEVICE_IDLE:
        case Mode::DISPLAY_INACTIVE:
        case Mode::AUDIO_STREAMING_LOW_LATENCY:
        case Mode::CAMERA_STREAMING_SECURE:
        case Mode::CAMERA_STREAMING_LOW:
        case Mode::CAMERA_STREAMING_MID:
        case Mode::CAMERA_STREAMING_HIGH:
        case Mode::VR:
            LOG(INFO) << "Mode " << static_cast<int32_t>(type) << "Not Supported";
            break;
        case Mode::EXPENSIVE_RENDERING:
            set_expensive_rendering(enabled);
            break;
        case Mode::LAUNCH:
            power_hint(POWER_HINT_LAUNCH, enabled ? &enabled : NULL);
            break;
        case Mode::INTERACTIVE:
            setInteractive(enabled);
            break;
        case Mode::SUSTAINED_PERFORMANCE:
        case Mode::FIXED_PERFORMANCE:
            power_hint(POWER_HINT_SUSTAINED_PERFORMANCE, NULL);
            break;
        default:
            LOG(INFO) << "Mode " << static_cast<int32_t>(type) << "Not Supported";
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isModeSupported(Mode type, bool* _aidl_return) {
    LOG(INFO) << "Power isModeSupported: " << static_cast<int32_t>(type);
#ifdef MODE_EXT
    if (isDeviceSpecificModeSupported(type, _aidl_return)) {
        return ndk::ScopedAStatus::ok();
    }
#endif
    switch (type) {
        case Mode::EXPENSIVE_RENDERING:
            if (is_expensive_rendering_supported()) {
                *_aidl_return = true;
            } else {
                *_aidl_return = false;
            }
            break;
#ifdef TAP_TO_WAKE_NODE
        case Mode::DOUBLE_TAP_TO_WAKE:
#endif
        case Mode::LAUNCH:
        case Mode::INTERACTIVE:
        case Mode::SUSTAINED_PERFORMANCE:
        case Mode::FIXED_PERFORMANCE:
            *_aidl_return = true;
            break;
        default:
            *_aidl_return = false;
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::setBoost(Boost type, int32_t durationMs) {
    LOG(VERBOSE) << "Power setBoost: " << static_cast<int32_t>(type)
                 << ", duration: " << durationMs;
    switch (type) {
        case Boost::INTERACTION:
            power_hint(POWER_HINT_INTERACTION, &durationMs);
            break;
        default:
            LOG(INFO) << "Boost " << static_cast<int32_t>(type) << "Not Supported";
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isBoostSupported(Boost type, bool* _aidl_return) {
    LOG(INFO) << "Power isBoostSupported: " << static_cast<int32_t>(type);
    switch (type) {
        case Boost::INTERACTION:
            *_aidl_return = true;
            break;
        default:
            *_aidl_return = false;
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getCpuHeadroom(const CpuHeadroomParams& cpuHeadroomParams,
                                         CpuHeadroomResult* cpuHeadroomResult) {
    LOG(INFO) << "Power getCpuHeadroom";
    int durationMillis = cpuHeadroomParams.calculationWindowMillis;
    if (durationMillis > CPU_MAX_DURATION_MS) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (durationMillis < CPU_MIN_DURATION_MS) {
        durationMillis = CPU_MIN_DURATION_MS;
    }

    int args[] = {CPU_HEADROOM_TOTAL, durationMillis / 1000};
    int headroom = send_perf_get_feedback_extn(VENDOR_FEEDBACK_CPU_HEADROOM, pkg, 2, args);
    if (headroom < 0) {
        LOG(ERROR) << "Failed to get CPU headroom";
        return ndk::ScopedAStatus::ok();
    }
    cpuHeadroomResult->set<CpuHeadroomResult::Tag::globalHeadroom>(static_cast<float>(headroom));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getGpuHeadroom(const GpuHeadroomParams& gpuHeadroomParams,
                                         GpuHeadroomResult* gpuHeadroomResult) {
    LOG(INFO) << "Power getGpuHeadroom";
    int calculationType = -1;
    if (gpuHeadroomParams.calculationType == GpuHeadroomParams::CalculationType::MIN) {
        calculationType = GPU_HEADROOM_MIN;
    } else if (gpuHeadroomParams.calculationType == GpuHeadroomParams::CalculationType::AVERAGE) {
        calculationType = GPU_HEADROOM_AVG;
    } else {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    int durationMillis = gpuHeadroomParams.calculationWindowMillis;
    if (durationMillis > GPU_MAX_DURATION_MS) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (durationMillis < GPU_MIN_DURATION_MS) {
        durationMillis = GPU_MIN_DURATION_MS;
    }

    int args[] = {calculationType, durationMillis / 1000};
    int headroom = send_perf_get_feedback_extn(VENDOR_FEEDBACK_GPU_HEADROOM, pkg, 2, args);
    if (headroom < 0) {
        LOG(ERROR) << "Failed to get GPU headroom";
        return ndk::ScopedAStatus::ok();
    }
    gpuHeadroomResult->set<GpuHeadroomResult::Tag::globalHeadroom>(static_cast<float>(headroom));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::createHintSession(int32_t tgid, int32_t uid,
                                            const std::vector<int32_t>& threadIds,
                                            int64_t durationNanos,
                                            std::shared_ptr<IPowerHintSession>* _aidl_return) {
    LOG(INFO) << "Power createHintSession";
    if (threadIds.size() == 0) {
        LOG(ERROR) << "Error: threadIds.size() shouldn't be " << threadIds.size();
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *_aidl_return = setPowerHintSession(tgid, uid, threadIds, durationNanos);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::createHintSessionWithConfig(
        int32_t tgid, int32_t uid, const std::vector<int32_t>& threadIds, int64_t durationNanos,
        SessionTag tag, SessionConfig* config, std::shared_ptr<IPowerHintSession>* _aidl_return) {
    if (tag != SessionTag::OTHER && tag != SessionTag::SURFACEFLINGER && tag != SessionTag::HWUI &&
        tag != SessionTag::GAME) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    auto out = createHintSession(tgid, uid, threadIds, durationNanos, _aidl_return);
    static_cast<PowerHintSessionImpl*>(_aidl_return->get())->getSessionConfig(config);
    return out;
}

ndk::ScopedAStatus Power::getSessionChannel(int32_t, int32_t, ChannelConfig* _aidl_return) {
    static AidlMessageQueue<ChannelMessage, SynchronizedReadWrite> stubQueue{20, true};
    static std::thread stubThread([&] {
        ChannelMessage data;
        // This loop will only run while there is data waiting
        // to be processed, and blocks on a futex all other times
        while (stubQueue.readBlocking(&data, 1, 0)) {
        }
    });
    _aidl_return->channelDescriptor = stubQueue.dupeDesc();
    _aidl_return->readFlagBitmask = 0x01;
    _aidl_return->writeFlagBitmask = 0x02;
    _aidl_return->eventFlagDescriptor = std::nullopt;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::closeSessionChannel(int32_t, int32_t) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getHintSessionPreferredRate(int64_t* outNanoseconds) {
    LOG(INFO) << "Power getHintSessionPreferredRate";
    *outNanoseconds = getSessionPreferredRate();
    return ndk::ScopedAStatus::ok();
}

template <class E>
int64_t bitsForEnum() {
    return static_cast<int64_t>(std::bitset<enum_size<E>()>().set().to_ullong());
}

/**
 * For sessionHints, sessionModes, and sessionTags, each bit from the left corresponds to the
 * support status of that same value in the enum. For example, POWER_EFFICIENCY and
 * GRAPHICS_PIPELINE are enabled for SessionMode.
 * https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/power/aidl/android/hardware/power
 */
ndk::ScopedAStatus Power::getSupportInfo(SupportInfo* _aidl_return) {
    static SupportInfo supportInfo = {.usesSessions = true,
                                      .modes = 0b10001100,
                                      .boosts = 0b0,
                                      .sessionHints = 0b1100001111,
                                      .sessionModes = 0b0011,
                                      .sessionTags = 0b001111,
                                      .compositionData =
                                              {
                                                      .isSupported = false,
                                                      .disableGpuFences = false,
                                                      .maxBatchSize = 1,
                                                      .alwaysBatch = false,
                                              },
                                      .headroom = {
                                              .isCpuSupported = true,
                                              .isGpuSupported = true,
                                              .cpuMinIntervalMillis = 1000,
                                              .gpuMinIntervalMillis = 1000,
                                              .cpuMinCalculationWindowMillis = 50,
                                              .cpuMaxCalculationWindowMillis = CPU_MAX_DURATION_MS,
                                              .gpuMinCalculationWindowMillis = 50,
                                              .gpuMaxCalculationWindowMillis = GPU_MAX_DURATION_MS,
                                              .cpuMaxTidCount = 5,
                                      }};
    // Copy the support object into the binder
    *_aidl_return = supportInfo;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::sendCompositionData(const std::vector<CompositionData>&) {
    LOG(INFO) << "Composition data received!";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::sendCompositionUpdate(const CompositionUpdate&) {
    LOG(INFO) << "Composition update received!";
    return ndk::ScopedAStatus::ok();
}

}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
