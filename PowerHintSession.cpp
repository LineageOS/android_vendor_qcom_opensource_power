/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "PowerHintSession.h"
#include "hint-data.h"
#include "performance.h"
#include "utils.h"

#include <cutils/properties.h>
#include <dlfcn.h>
#include <cmath>

#define PERF_EXT_LIB_PROP "ro.vendor.extension_library"
#define ADPF_DEBUG_PROP "vendor.debug.enable.adpf"
#define PROP_VAL_LENGTH 92

#define CPU_BOOST_HINT 0x0000104E
#define MAX_THREADS 16
#define MAX_BOOST 200
#define MIN_BOOST -200

#define SCHED_TASK_LOAD_BOOST 0x43C04000
#define MAX_TLB_THREADS 5
#define MAX_TLB_PCT 80
#define MIN_TLB_PCT -80
#define INC_TLB_PCT 10
#define DEC_TLB_PCT -10

#include <android-base/logging.h>
#include "android/binder_auto_utils.h"
#define LOG_TAG "QTI PowerHAL"

std::unordered_map<PowerHintSessionImpl*, int32_t> mPowerHintSessions;
std::mutex mSessionLock;

static int validateBoost(int boostVal, int boostSum) {
    boostSum += boostVal;
    if (boostSum > MAX_BOOST)
        return MAX_BOOST;
    else if (boostSum < MIN_BOOST)
        return MIN_BOOST;
    return boostSum;
}

void PowerHintSessionImpl::resetBoost() {
    if (mHandle > 0) {
        release_request(mHandle);
        if (mDebug) LOG(INFO) << "Handle " << mHandle << " released";
    }
    mHandle = -1;
    mLastAction = LOAD_RESET;
}

static std::string printThreads(const std::vector<int32_t>& threadIds, int numThreads) {
    std::string str;
    for (int i = 0; i < numThreads; i++) {
        str = str + std::to_string(threadIds[i]) + " ";
    }
    return str;
}

bool PowerHintSessionImpl::perfBoost(int boostVal, int hintType) {
    int tBoostSum = mBoostSum;
    int mHandlePerfHint = -1;

    if (hintType == LOAD_RESET) {
        resetBoost();
        return true;
    }

    if (hintType == LOAD_RESUME && mLastAction != LOAD_RESET) {
        tBoostSum = 0;
    }

    tBoostSum = validateBoost(boostVal, tBoostSum);
    if (tBoostSum != 0) {
        mHandlePerfHint = perf_hint_enable(CPU_BOOST_HINT, tBoostSum);
        if (mHandlePerfHint < 0) {
            LOG(ERROR) << "Unable to acquire Perf hint for" << CPU_BOOST_HINT;
            return false;
        }
    }

    if (mHandle > 0) {
        release_request(mHandle);
        if (mDebug) LOG(INFO) << "Handle " << mHandle << " released";
    }
    mBoostSum = tBoostSum;
    mHandle = mHandlePerfHint;
    mLastAction = hintType;
    return true;
}

bool PowerHintSessionImpl::taskLoadBoost(int loadType) {
    int boostSum = mTLBoostSum;
    int handle = -1;

    if (loadType == LOAD_RESET) {
        if (mTLBHandle > 0) {
            release_request(mTLBHandle);
            if (mDebug) LOG(INFO) << "Handle " << mTLBHandle << " released";
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
        if (mDebug) LOG(INFO) << "Handle " << mTLBHandle << " released";
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
        if (mDebug)
            LOG(INFO) << "Handle " << handle << " for threads "
                      << printThreads(mThreadIds, numThreads);
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
    LOG(INFO) << "setPowerHintSession ";
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

void PowerHintSessionImpl::getPerfProperties() {
    char perfClientLib[PROPERTY_VALUE_MAX] = {0};
    if (!property_get(PERF_EXT_LIB_PROP, perfClientLib, nullptr)) {
        LOG(ERROR) << "Failed to get property " << PERF_EXT_LIB_PROP;
        return;
    }
    void* perfClientLibHandle = dlopen(perfClientLib, RTLD_NOW);
    if (!perfClientLibHandle) {
        LOG(ERROR) << "Unable to open " << perfClientLib << ": " << dlerror();
        return;
    }
    int (*perfGetPropExtn)(const char*, char*, size_t, const char*) = nullptr;
    perfGetPropExtn = (int (*)(const char*, char*, size_t, const char*))dlsym(perfClientLibHandle,
                                                                              "perf_get_prop_extn");
    if (!perfGetPropExtn) {
        LOG(ERROR) << "Unable to find perf_get_prop_extn function in " << perfClientLib << ": "
                   << dlerror();
    } else {
        char debugEnable[PROP_VAL_LENGTH] = {0};
        perfGetPropExtn(ADPF_DEBUG_PROP, debugEnable, PROP_VAL_LENGTH, "0");
        mDebug = atoi(debugEnable) == 1;
    }
    dlclose(perfClientLibHandle);
}

PowerHintSessionImpl::PowerHintSessionImpl(int32_t tgid, int32_t uid,
                                           const std::vector<int32_t>& threadIds,
                                           int64_t durationNanos) {
    mUid = uid;
    mTgid = tgid;
    mHandle = -1;
    mBoostSum = 0;
    mLastAction = -1;

    mThreadIds = threadIds;
    mNumPowerEfficiencyThreads = 0;
    mPowerEfficiencyMode = false;
    mDebug = false;
    getPerfProperties();

    mTargetWorkDurationNanos = -1;
    mThresholdNanos = -1;
    mConsecutiveDownCount = 0;
    mTLBHandle = -1;
    mTLBoostSum = 0;
    mIsTopAppGame = false;
    updateTargetWorkDuration(durationNanos);  // mTargetWorkDurationNanos, mThresholdNanos
    setSessionActivity(this, true);
}

PowerHintSessionImpl::~PowerHintSessionImpl() {
    close();
}

void PowerHintSessionImpl::releaseLowCpuUtil() {
    for (int i = 0; i < mNumPowerEfficiencyThreads; i++) {
        if (mThreadIds[i] == 0) continue;

        // TODO: Relegate(EngineHints::EH_HIGH_CPUUTIL, mTopAppName, mThreadIds[i], 0);
        if (mDebug) LOG(INFO) << "Released low cpu util hint for thread " << mThreadIds[i];
    }
    mNumPowerEfficiencyThreads = 0;
}

void PowerHintSessionImpl::hintLowCpuUtil() {
    int count = 0;
    for (auto it = mThreadIds.begin(); it != mThreadIds.end() && count < MAX_THREADS; ++it) {
        if (*it == 0) continue;

        // TODO: Relegate(EngineHints::EH_HIGH_CPUUTIL, mTopAppName, *it, 2);
        if (mDebug) LOG(INFO) << "Set thread " << *it << " to prefer power efficiency";
        ++count;
    }
    mNumPowerEfficiencyThreads = count;
}

double PowerHintSessionImpl::nextSupportedFPS(double fps) {
    const std::vector<double> supportedFPS = {30.0, 60.0, 90.0, 120.0, 144.0};
    auto it = std::lower_bound(supportedFPS.begin(), supportedFPS.end(), fps);
    if (it == supportedFPS.end()) {
        it = supportedFPS.end() - 1;
    }
    if (mDebug) LOG(INFO) << "Supported FPS: " << *it << " for requested FPS: " << fps;
    return *it;
}

ndk::ScopedAStatus PowerHintSessionImpl::updateTargetWorkDuration(int64_t in_targetDurationNanos) {
    // TODO: top app is game check
    LOG(INFO) << "PowerHintSessionImpl::updateTargetWorkDuration: " << in_targetDurationNanos;
    if (in_targetDurationNanos <= 0) {
        LOG(ERROR) << "Invalid target work duration";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    mTargetWorkDurationNanos = in_targetDurationNanos;
    mThresholdNanos = mTargetWorkDurationNanos - (mTargetWorkDurationNanos / 8);
    double durationInSeconds = static_cast<double>(mTargetWorkDurationNanos) / 1'000'000'000.0;
    double calculatedFps = 1.0 / durationInSeconds;
    double supportedFps = nextSupportedFPS(calculatedFps);
    // TODO: Relegate(EngineHints::EH_RENDER_RATE, mTopAppName, supportedFps, 1);
    // TODO: TFPS
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::reportActualWorkDuration(
        const std::vector<::aidl::android::hardware::power::WorkDuration>& in_durations) {
    // TODO: top app is game check
    LOG(INFO) << "PowerHintSessionImpl::reportActualWorkDuration: ";
    int64_t targetWorkDurationNanos = mTargetWorkDurationNanos;
    if (targetWorkDurationNanos == -1 || in_durations.empty()) {
        return ndk::ScopedAStatus::ok();
    }
    int64_t actualWorkDurationNanos = in_durations[0].durationNanos;
    for (const auto& duration : in_durations) {
        if (duration.durationNanos > actualWorkDurationNanos) {
            actualWorkDurationNanos = duration.durationNanos;
        }
    }
    if (mDebug)
        LOG(INFO) << "actual = " << actualWorkDurationNanos
                  << " ns, target = " << targetWorkDurationNanos
                  << " ns, last_boost = " << mTLBoostSum;
    if (actualWorkDurationNanos >= mThresholdNanos) {
        taskLoadBoost(LOAD_UP);
        mConsecutiveDownCount = 0;
    } else {
        if (mConsecutiveDownCount < 3) {
            mConsecutiveDownCount++;
        }
        if (mConsecutiveDownCount >= 3) {
            taskLoadBoost(LOAD_DOWN);
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::pause() {
    LOG(INFO) << "PowerHintSessionImpl::pause ";
    if (isSessionAlive(this)) {
        sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET);
        setSessionActivity(this, false);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::resume() {
    LOG(INFO) << "PowerHintSessionImpl::resume ";
    if (isSessionAlive(this)) {
        sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_RESUME);
        setSessionActivity(this, true);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::close() {
    LOG(INFO) << "PowerHintSessionImpl::close ";

    if (isSessionAlive(this)) {
        sendHint(aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET);
        taskLoadBoost(LOAD_RESET);
        releaseLowCpuUtil();
        mThreadIds.clear();

        mSessionLock.lock();
        mPowerHintSessions.erase(this);
        mSessionLock.unlock();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::sendHint(
        aidl::android::hardware::power::SessionHint hint) {
    LOG(INFO) << "PowerHintSessionImpl::sendHint ";
    if (!isSessionActive(this)) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    switch (hint) {
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_UP:
            perfBoost(20, LOAD_UP);
            break;
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_DOWN:
            perfBoost(-20, LOAD_DOWN);
            break;
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_RESET:
            perfBoost(0, LOAD_RESET);
            break;
        case aidl::android::hardware::power::SessionHint::CPU_LOAD_RESUME:
            perfBoost(0, LOAD_RESUME);
            break;
        case aidl::android::hardware::power::SessionHint::POWER_EFFICIENCY:
            perfBoost(-20, LOAD_DOWN);
            break;
        default:
            break;
    }
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus PowerHintSessionImpl::setThreads(const std::vector<int32_t>& threadIds) {
    LOG(INFO) << "PowerHintSessionImpl::setThreads "
              << printThreads(threadIds, static_cast<int>(threadIds.size()));
    if (threadIds.size() == 0) {
        LOG(ERROR) << "Threads list is empty";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    mThreadIds = threadIds;

    // reset task load boost
    mConsecutiveDownCount = 0;
    taskLoadBoost(LOAD_RESET);

    // reset session hint
    if (mPowerEfficiencyMode)
        setMode(aidl::android::hardware::power::SessionMode::POWER_EFFICIENCY,
                mPowerEfficiencyMode);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::setMode(aidl::android::hardware::power::SessionMode mode,
                                                 bool enabled) {
    // TODO: top app is game check
    switch (mode) {
        case aidl::android::hardware::power::SessionMode::POWER_EFFICIENCY:
            LOG(INFO) << "PowerHintSessionImpl::setMode: mode = POWER_EFFICIENCY, enabled = "
                      << enabled;
            releaseLowCpuUtil();
            if (enabled) {
                hintLowCpuUtil();
            }
            mPowerEfficiencyMode = enabled;
            return ndk::ScopedAStatus::ok();
        default:
            LOG(ERROR) << "PowerHintSessionImpl::setMode: Unsupported mode = "
                       << static_cast<int>(mode);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerHintSessionImpl::getSessionConfig(
        aidl::android::hardware::power::SessionConfig* _aidl_return) {
    _aidl_return->id = 1;
    return ndk::ScopedAStatus::ok();
}