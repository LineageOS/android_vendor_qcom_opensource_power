/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __POWERHINTSESSION__
#define __POWERHINTSESSION__

#include <aidl/android/hardware/power/BnPowerHintSession.h>
#include <aidl/android/hardware/power/SessionHint.h>
#include <aidl/android/hardware/power/SessionMode.h>
#include <aidl/android/hardware/power/WorkDuration.h>
#include <mutex>
#include <set>
#include <unordered_map>

enum LOAD_TYPE { LOAD_UP, LOAD_DOWN, LOAD_RESET, LOAD_RESUME };

std::shared_ptr<aidl::android::hardware::power::IPowerHintSession> setPowerHintSession(
        int32_t tgid, int32_t uid, const std::vector<int32_t>& threadIds, int64_t durationNanos);
int64_t getSessionPreferredRate();

class PowerHintSessionImpl : public aidl::android::hardware::power::BnPowerHintSession {
  public:
    explicit PowerHintSessionImpl(int32_t tgid, int32_t uid, const std::vector<int32_t>& threadIds,
                                  int64_t durationNanos);
    ~PowerHintSessionImpl();
    ndk::ScopedAStatus updateTargetWorkDuration(int64_t targetDurationNanos) override;
    ndk::ScopedAStatus reportActualWorkDuration(
            const std::vector<aidl::android::hardware::power::WorkDuration>& durations) override;
    ndk::ScopedAStatus pause() override;
    ndk::ScopedAStatus resume() override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus sendHint(aidl::android::hardware::power::SessionHint hint) override;
    ndk::ScopedAStatus setThreads(const std::vector<int32_t>& threadIds) override;
    ndk::ScopedAStatus setMode(aidl::android::hardware::power::SessionMode mode,
                               bool enabled) override;
    ndk::ScopedAStatus getSessionConfig(
            aidl::android::hardware::power::SessionConfig* _aidl_return) override;
    bool taskLoadBoost(int loadType);
    void hintThreadPipeline();
    void hintLowCpuUtil();
    void releaseThreadPipeline();
    void releaseLowCpuUtil();
    void boostCpu();
    void boostGpu();

  private:
    int32_t mUid;
    int32_t mTgid;
    std::vector<int32_t> mThreadIds;
    std::vector<int32_t> mSupportedFps;

    bool mEnableDebug;
    bool mEnableAdpf;
    std::string mTopAppName;
    int mMaxGraphicsPipelineThreads;
    int mNumGraphicsPipelineThreads;
    int mNumPowerEfficiencyThreads;
    bool mGraphicsPipelineMode;
    bool mPowerEfficiencyMode;

    int64_t mTargetWorkDurationNanos;
    int64_t mThresholdNanos;
    unsigned int mConsecutiveDownCount;
    int mTLBHandle;
    int mTLBoostSum;
};
#endif /* __POWERHINTSESSION__ */
