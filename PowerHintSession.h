/*
* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef __POWERHINTSESSION__
#define __POWERHINTSESSION__

#include <aidl/android/hardware/power/WorkDuration.h>
#include <aidl/android/hardware/power/BnPowerHintSession.h>
#if (PLATFORM_SDK_VERSION >= 34)
#include <aidl/android/hardware/power/SessionHint.h>
#endif

std::shared_ptr<aidl::android::hardware::power::IPowerHintSession> setPowerHintSession();
int64_t getSessionPreferredRate();

class PowerHintSessionImpl : public aidl::android::hardware::power::BnPowerHintSession{
public:
    ndk::ScopedAStatus updateTargetWorkDuration(int64_t targetDurationNanos) override;
    ndk::ScopedAStatus reportActualWorkDuration(
            const std::vector<aidl::android::hardware::power::WorkDuration>& durations) override;
    ndk::ScopedAStatus pause() override;
    ndk::ScopedAStatus resume() override;
    ndk::ScopedAStatus close() override;
    #if (PLATFORM_SDK_VERSION >= 34)
    ndk::ScopedAStatus sendHint(aidl::android::hardware::power::SessionHint hint) override;
    ndk::ScopedAStatus setThreads(const std::vector<int32_t>& threadIds) override;
    #endif
};
#endif /* __POWERHINTSESSION__ */