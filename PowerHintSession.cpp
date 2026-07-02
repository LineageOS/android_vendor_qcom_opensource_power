/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "QTI PowerHAL"

#include "PowerHintSession.h"
#include <aidl/vendor/qti/hardware/display/config/IDisplayConfig.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include "android/binder_auto_utils.h"
#include "hint-data.h"
#include "performance.h"
#include "utils.h"

using aidl::vendor::qti::hardware::display::config::DisplayType;
using aidl::vendor::qti::hardware::display::config::IDisplayConfig;

#define VENDOR_FEEDBACK_WORKLOAD_TYPE 0x00001601
#define VENDOR_FEEDBACK_TOPAPP_NAME 0x00001611
#define VENDOR_HINT_PICARD_TOP_APP 0x0000104A
#define VENDOR_HINT_PICARD_RENDER_RATE 0x0000104B
#define VENDOR_HINT_PICARD_LOW_LAT 0x0000104C
#define VENDOR_HINT_PICARD_HIGH_CPUUTIL 0x0000104D
#define VENDOR_HINT_PICARD_LOAD_CHANGED 0x0000104E
#define SCHED_TASK_LOAD_BOOST 0x43C04000

#define GAME_WORKLOAD_TYPE 2

#define CPU_BOOST_PCT 10
#define GPU_BOOST_PCT 10
#define MAX_THREADS 16

#define MAX_TLB_THREADS 5
#define MAX_TLB_PCT 80
#define MIN_TLB_PCT -80
#define INC_TLB_PCT 10
#define DEC_TLB_PCT -10

std::unordered_map<PowerHintSessionImpl*, int32_t> mPowerHintSessions;
std::mutex mSessionLock;

struct timeval mBoostStartTv;
unsigned int mBoostCount;
unsigned int mMaxBoostCount;
unsigned int mBoostDurationSec;
std::mutex mBoostLock;

template <typename T>
static std::string printValues(const std::vector<T>& values, int count) {
    std::string str;
    int limit = std::min(count, static_cast<int>(values.size()));
    for (int i = 0; i < limit; i++) {
        str += std::to_string(values[i]);
        if (i < limit - 1) {
            str += " ";
        }
    }
    return str;
}

bool PowerHintSessionImpl::taskLoadBoost(int loadType) {
    int boostSum = mTLBoostSum;
    int handle = -1;

    if (loadType == LOAD_RESET) {
        if (mTLBHandle > 0) {
            release_request(mTLBHandle);
            if (mEnableDebug) LOG(INFO) << "Handle " << mTLBHandle << " released";
        }
        mTLBHandle = -1;
        mTLBoostSum = 0;
        return true;
    }

    // Calculate boost sum
    if (loadType == LOAD_UP) {
        boostSum += INC_TLB_PCT;
        if (boostSum > MAX_TLB_PCT) {
            boostSum = MAX_TLB_PCT;
        }
    } else if (loadType == LOAD_DOWN) {
        boostSum += DEC_TLB_PCT;
        if (boostSum < MIN_TLB_PCT) {
            boostSum = MIN_TLB_PCT;
        }
    }

    if (mTLBHandle > 0) {
        // If no change in boost, keep handle
        if (boostSum == mTLBoostSum) {
            return true;
        }
        // Release old boost
        release_request(mTLBHandle);
        if (mEnableDebug) LOG(INFO) << "Handle " << mTLBHandle << " released";
    }
    mTLBHandle = -1;

    // Acquire new boost
    if (boostSum != 0) {
        int numThreads =
                (mThreadIds.size() > MAX_TLB_THREADS) ? MAX_TLB_THREADS : mThreadIds.size();
        int size = numThreads * 2;
        int list[size];
        for (int i = 0; i < size; i += 2) {
            list[i] = SCHED_TASK_LOAD_BOOST;
            list[i + 1] = (std::abs(boostSum) & 0x7F) | ((boostSum < 0) ? 0x80 : 0x00) |
                          (mThreadIds[i / 2] << 8);
        }

        handle = interaction_with_handle(handle, 0, size, list);
        if (handle < 0) {
            LOG(ERROR) << "Unable to apply boost";
            // Do not update boost sum if handle is not acquired
            return false;
        }
        if (mEnableDebug)
            LOG(INFO) << "Handle " << handle << " for threads "
                      << printValues(mThreadIds, numThreads);
    }
    mTLBHandle = handle;
    mTLBoostSum = boostSum;
    return true;
}

bool isSessionAlive(PowerHintSessionImpl* session) {
    if (mPowerHintSessions.find(session) != mPowerHintSessions.end()) return true;
    return false;
}

bool isSessionActive(PowerHintSessionImpl* session) {
    if (!isSessionAlive(session)) return false;
    if (mPowerHintSessions[session] == 1) return true;
    return false;
}

std::shared_ptr<aidl::android::hardware::power::IPowerHintSession> setPowerHintSession(
        int32_t tgid, int32_t uid, const std::vector<int32_t>& threadIds, int64_t durationNanos) {
    LOG(DEBUG) << "setPowerHintSession ";
    std::shared_ptr<aidl::android::hardware::power::IPowerHintSession> mPowerSession =
            ndk::SharedRefBase::make<PowerHintSessionImpl>(tgid, uid, threadIds, durationNanos);

    if (mPowerSession == nullptr) {
        return nullptr;
    }
    return mPowerSession;
}

int64_t getSessionPreferredRate() {
    return 16666666L;
}

void setSessionActivity(PowerHintSessionImpl* session, bool flag) {
    std::lock_guard<std::mutex> mLockGuard(mSessionLock);
    if (flag)
        mPowerHintSessions[session] = 1;
    else
        mPowerHintSessions[session] = 0;
}

int getMaxBoostCount() {
    std::lock_guard<std::mutex> lock(mBoostLock);
    char property[PROPERTY_VALUE_MAX];
    strlcpy(property, perf_get_property("ro.vendor.perf.qape.max_boost_count", "3").value,
            PROPERTY_VALUE_MAX);
    return atoi(property);
}

int getMaxBoostDuration() {
    std::lock_guard<std::mutex> lock(mBoostLock);
    char property[PROPERTY_VALUE_MAX];
    strlcpy(property, perf_get_property("ro.vendor.perf.qape.boost_duration", "10").value,
            PROPERTY_VALUE_MAX);
    return atoi(property);
}

int getMaxPipelineNumber() {
    char property[PROPERTY_VALUE_MAX];
    strlcpy(property, perf_get_property("ro.vendor.perf.qape.max_pipeline_number", "3").value,
            PROPERTY_VALUE_MAX);
    return atoi(property);
}

bool enableAdpfDebug() {
    char property[PROPERTY_VALUE_MAX];
    strlcpy(property, perf_get_property("vendor.debug.enable.adpf", "0").value, PROPERTY_VALUE_MAX);
    return atoi(property) == 1;
}

bool initTopAppInfo(std::string& topAppName) {
    const char* tmp = send_perf_sync_request(VENDOR_FEEDBACK_TOPAPP_NAME);
    if (!tmp) {
        return false;
    }
    topAppName = tmp;
    free((char*)tmp);

    if (send_perf_hint(VENDOR_HINT_PICARD_TOP_APP, topAppName.c_str(), 0, 0) == -1) {
        return false;
    }
    return send_perf_get_feedback(VENDOR_FEEDBACK_WORKLOAD_TYPE, topAppName.c_str()) ==
           GAME_WORKLOAD_TYPE;
}

void initSupportedFps(std::vector<int32_t>& supportedFps) {
    ndk::SpAIBinder binder(AServiceManager_checkService(
            "vendor.qti.hardware.display.config.IDisplayConfig/default"));
    if (binder.get() == nullptr) {
        LOG(ERROR) << "DisplayConfig AIDL is not available";
        return;
    }
    auto aidlDisplayConfigIntf = IDisplayConfig::fromBinder(binder);
    if (aidlDisplayConfigIntf == nullptr) {
        LOG(ERROR) << "Failed to obtain DisplayConfig interface";
        return;
    }
    ndk::ScopedAStatus status = aidlDisplayConfigIntf->getSupportedDisplayRefreshRates(
            DisplayType::PRIMARY, &supportedFps);
    if (!status.isOk()) {
        LOG(ERROR) << "Failed to get supported display refresh rates";
        return;
    }
    std::sort(supportedFps.begin(), supportedFps.end());
    LOG(INFO) << "Supported display refresh rates: "
              << printValues(supportedFps, static_cast<int>(supportedFps.size()));
}

PowerHintSessionImpl::PowerHintSessionImpl(int32_t tgid, int32_t uid,
                                           const std::vector<int32_t>& threadIds,
                                           int64_t durationNanos) {
    mUid = uid;
    mTgid = tgid;
    mThreadIds = threadIds;

    mEnableDebug = enableAdpfDebug();
    mEnableAdpf = initTopAppInfo(mTopAppName);
    mMaxBoostCount = getMaxBoostCount();
    mBoostDurationSec = getMaxBoostDuration();
    mMaxGraphicsPipelineThreads = getMaxPipelineNumber();
    mNumGraphicsPipelineThreads = 0;
    mNumPowerEfficiencyThreads = 0;
    mGraphicsPipelineMode = false;
    mPowerEfficiencyMode = false;

    mTargetWorkDurationNanos = -1;
    mThresholdNanos = -1;
    mConsecutiveDownCount = 0;
    mTLBHandle = -1;
    mTLBoostSum = 0;
    initSupportedFps(mSupportedFps);
    updateTargetWorkDuration(durationNanos);
    setSessionActivity(this, true);
}

PowerHintSessionImpl::~PowerHintSessionImpl() {
    close();
}

void PowerHintSessionImpl::releaseThreadPipeline() {
    for (int i = 0; i < mNumGraphicsPipelineThreads; i++) {
        if (mThreadIds[i] == 0) continue;

        send_perf_hint(VENDOR_HINT_PICARD_LOW_LAT, mTopAppName.c_str(), mThreadIds[i], 0);
    }
    mNumGraphicsPipelineThreads = 0;
}

void PowerHintSessionImpl::releaseLowCpuUtil() {
    for (int i = 0; i < mNumPowerEfficiencyThreads; i++) {
        if (mThreadIds[i] == 0) continue;

        send_perf_hint(VENDOR_HINT_PICARD_HIGH_CPUUTIL, mTopAppName.c_str(), mThreadIds[i], 0);
    }
    mNumPowerEfficiencyThreads = 0;
}

void PowerHintSessionImpl::hintThreadPipeline() {
    int count = 0;
    for (auto it = mThreadIds.begin();
         it != mThreadIds.end() && count < mMaxGraphicsPipelineThreads; it++) {
        if (*it == 0) continue;

        if (send_perf_hint(VENDOR_HINT_PICARD_LOW_LAT, mTopAppName.c_str(), *it, 2) == -1) break;

        ++count;
    }
    mNumGraphicsPipelineThreads = count;
}

void PowerHintSessionImpl::hintLowCpuUtil() {
    int count = 0;
    for (auto it = mThreadIds.begin(); it != mThreadIds.end() && count < MAX_THREADS; ++it) {
        if (*it == 0) continue;

        if (send_perf_hint(VENDOR_HINT_PICARD_HIGH_CPUUTIL, mTopAppName.c_str(), *it, 2) == -1)
            break;

        ++count;
    }
    mNumPowerEfficiencyThreads = count;
}

ndk::ScopedAStatus PowerHintSessionImpl::updateTargetWorkDuration(int64_t in_targetDurationNanos) {
    LOG(DEBUG) << "PowerHintSessionImpl::updateTargetWorkDuration: " << in_targetDurationNanos;
    if (!mEnableAdpf || in_targetDurationNanos <= 0 ||
        in_targetDurationNanos == mTargetWorkDurationNanos) {
        return ndk::ScopedAStatus::ok();
    }
    mTargetWorkDurationNanos = in_targetDurationNanos;
    mThresholdNanos = mTargetWorkDurationNanos - (mTargetWorkDurationNanos / 8);
    LOG(INFO) << "Hint to TFPS disabled";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::reportActualWorkDuration(
        const std::vector<::aidl::android::hardware::power::WorkDuration>& in_durations) {
    if (!mEnableAdpf || mTargetWorkDurationNanos == -1 || in_durations.empty()) {
        if (mEnableDebug) LOG(INFO) << "PowerHintSessionImpl::reportActualWorkDuration:";
        return ndk::ScopedAStatus::ok();
    }
    int64_t actualWorkDurationNanos = in_durations[0].durationNanos;
    for (const auto& duration : in_durations) {
        if (duration.durationNanos > actualWorkDurationNanos) {
            actualWorkDurationNanos = duration.durationNanos;
        }
    }
    if (mEnableDebug)
        LOG(INFO) << "PowerHintSessionImpl::reportActualWorkDuration: actual = "
                  << actualWorkDurationNanos << " ns, target = " << mTargetWorkDurationNanos
                  << " ns";
    if (actualWorkDurationNanos >= mThresholdNanos) {
        sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_UP);
        mConsecutiveDownCount = 0;
    } else {
        if (++mConsecutiveDownCount >= 3) {
            sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_DOWN);
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::pause() {
    LOG(DEBUG) << "PowerHintSessionImpl::pause ";
    if (isSessionAlive(this)) {
        sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET);
        setSessionActivity(this, false);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::resume() {
    LOG(DEBUG) << "PowerHintSessionImpl::resume ";
    if (isSessionAlive(this)) {
        setSessionActivity(this, true);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::close() {
    LOG(DEBUG) << "PowerHintSessionImpl::close ";
    if (isSessionAlive(this)) {
        sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET);
        releaseLowCpuUtil();
        releaseThreadPipeline();
        mThreadIds.clear();

        mSessionLock.lock();
        mPowerHintSessions.erase(this);
        mSessionLock.unlock();
    }
    return ndk::ScopedAStatus::ok();
}

bool isBoostEligible() {
    std::lock_guard<std::mutex> lock(mBoostLock);
    struct timeval boostCurTv;
    gettimeofday(&boostCurTv, NULL);

    if (mBoostCount == 0) {
        mBoostStartTv = boostCurTv;
        mBoostCount++;
        return true;
    } else {
        double elapsedTimeMillis = 0.0;
        elapsedTimeMillis = (boostCurTv.tv_sec - mBoostStartTv.tv_sec) * 1000.0;
        elapsedTimeMillis += (boostCurTv.tv_usec - mBoostStartTv.tv_usec) / 1000.0;

        if (elapsedTimeMillis > (mBoostDurationSec * 1000)) {
            mBoostCount = 1;
            mBoostStartTv = boostCurTv;
            return true;
        } else {
            if (mBoostCount >= mMaxBoostCount) {
                return false;
            } else {
                mBoostCount++;
                return true;
            }
        }
    }
}

void PowerHintSessionImpl::boostCpu() {
    if (!isBoostEligible()) {
        LOG(ERROR) << "Boost too frequent";
        return;
    }
    send_perf_hint(VENDOR_HINT_PICARD_LOAD_CHANGED, mTopAppName.c_str(), CPU_BOOST_PCT, 0);
}

void PowerHintSessionImpl::boostGpu() {
    if (!isBoostEligible()) {
        LOG(ERROR) << "Boost too frequent";
        return;
    }
    send_perf_hint(VENDOR_HINT_PICARD_LOAD_CHANGED, mTopAppName.c_str(), GPU_BOOST_PCT + 100, 1);
}

ndk::ScopedAStatus PowerHintSessionImpl::sendHint(
        aidl::android::hardware::power::SessionHint hint) {
    if (mEnableDebug) LOG(INFO) << "PowerHintSessionImpl::sendHint: " << static_cast<int32_t>(hint);
    if (!mEnableAdpf) {
        return ndk::ScopedAStatus::ok();
    }
    if (!isSessionActive(this)) return ndk::ScopedAStatus::ok();

    switch (hint) {
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_UP:
            taskLoadBoost(LOAD_UP);
            break;
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_DOWN:
            taskLoadBoost(LOAD_DOWN);
            break;
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET:
            mConsecutiveDownCount = 0;
            [[fallthrough]];
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_RESUME:
            taskLoadBoost(LOAD_RESET);
            break;
        case aidl::android::hardware::power::SessionHint::POWER_EFFICIENCY:
        case aidl::android::hardware::power::SessionHint::GPU_LOAD_UP:
        case aidl::android::hardware::power::SessionHint::GPU_LOAD_DOWN:
        case aidl::android::hardware::power::SessionHint::GPU_LOAD_RESET:
            break;
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_SPIKE:
            boostCpu();
            break;
        case aidl::android::hardware::power::SessionHint::GPU_LOAD_SPIKE:
            boostGpu();
            break;
        default:
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::setThreads(const std::vector<int32_t>& threadIds) {
    LOG(DEBUG) << "PowerHintSessionImpl::setThreads: "
              << printValues(threadIds, static_cast<int>(threadIds.size()));
    if (threadIds.empty()) {
        LOG(ERROR) << "Threads list is empty";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET);
    releaseLowCpuUtil();
    releaseThreadPipeline();
    mPowerEfficiencyMode = false;
    mGraphicsPipelineMode = false;
    mThreadIds = threadIds;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::setMode(aidl::android::hardware::power::SessionMode mode,
                                                 bool enabled) {
    LOG(DEBUG) << "PowerHintSessionImpl::setMode: mode = " << static_cast<int>(mode)
              << ", enabled = " << enabled;
    if (!mEnableAdpf) {
        return ndk::ScopedAStatus::ok();
    }

    switch (mode) {
        case aidl::android::hardware::power::SessionMode::POWER_EFFICIENCY:
            releaseLowCpuUtil();
            if (enabled) {
                hintLowCpuUtil();
            }
            mPowerEfficiencyMode = enabled;
            break;
        case aidl::android::hardware::power::SessionMode::GRAPHICS_PIPELINE:
            releaseThreadPipeline();
            if (enabled) {
                hintThreadPipeline();
            }
            mGraphicsPipelineMode = enabled;
            break;
        default:
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::getSessionConfig(
        aidl::android::hardware::power::SessionConfig* _aidl_return) {
    _aidl_return->id = 1;
    return ndk::ScopedAStatus::ok();
}
