LOCAL_PATH := $(call my-dir)

ifeq ($(call is-vendor-board-platform,QCOM),true)

include $(CLEAR_VARS)

LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_SHARED_LIBRARIES := \
    android.hardware.common-V2-ndk \
    android.hardware.common.fmq-V1-ndk \
    liblog \
    libcutils \
    libdl \
    libbase \
    libfmq \
    libutils \
    libbinder_ndk \
    android.hardware.power-V5-ndk

LOCAL_HEADER_LIBRARIES := \
    libhardware_headers

LOCAL_SRC_FILES := \
    power-common.c \
    metadata-parser.c \
    utils.c \
    list.c \
    hint-data.c \
    Power.cpp \
    main.cpp \
    PowerHintSession.cpp

LOCAL_CFLAGS += -Wall -Wextra -Werror

ifneq ($(BOARD_POWER_CUSTOM_BOARD_LIB),)
    LOCAL_WHOLE_STATIC_LIBRARIES += $(BOARD_POWER_CUSTOM_BOARD_LIB)
else

# Include target-specific files.
ifeq ($(call is-board-platform-in-list,msm8996), true)
LOCAL_SRC_FILES += power-8996.c
endif

ifeq ($(call is-board-platform-in-list,msm8937), true)
LOCAL_SRC_FILES += power-8937.c
endif

ifeq ($(call is-board-platform-in-list,msm8953), true)
LOCAL_SRC_FILES += power-8953.c
endif

ifeq ($(call is-board-platform-in-list,msm8998), true)
LOCAL_SRC_FILES += power-8998.c
endif

ifeq ($(call is-board-platform-in-list,sdm660), true)
LOCAL_SRC_FILES += power-660.c
endif

ifeq ($(call is-board-platform-in-list,sdm845), true)
LOCAL_SRC_FILES += power-845.c
endif

endif # End of board specific list

ifneq ($(TARGET_POWERHAL_MODE_EXT),)
    LOCAL_CFLAGS += -DMODE_EXT
    LOCAL_SRC_FILES += ../../../../$(TARGET_POWERHAL_MODE_EXT)
else ifneq ($(TARGET_POWERHAL_MODE_EXT_LIB),)
    LOCAL_CFLAGS += -DMODE_EXT
    LOCAL_STATIC_LIBRARIES += $(TARGET_POWERHAL_MODE_EXT_LIB)
endif

ifneq ($(TARGET_POWERHAL_SET_INTERACTIVE_EXT),)
    LOCAL_CFLAGS += -DSET_INTERACTIVE_EXT
    LOCAL_SRC_FILES += ../../../../$(TARGET_POWERHAL_SET_INTERACTIVE_EXT)
else ifneq ($(TARGET_POWERHAL_SET_INTERACTIVE_EXT_LIB),)
    LOCAL_CFLAGS += -DSET_INTERACTIVE_EXT
    LOCAL_STATIC_LIBRARIES += $(TARGET_POWERHAL_SET_INTERACTIVE_EXT_LIB)
endif

ifneq ($(TARGET_TAP_TO_WAKE_NODE),)
    LOCAL_CFLAGS += -DTAP_TO_WAKE_NODE=\"$(TARGET_TAP_TO_WAKE_NODE)\"
endif

ifeq ($(TARGET_USES_INTERACTION_BOOST),true)
    LOCAL_CFLAGS += -DINTERACTION_BOOST
endif

LOCAL_MODULE := android.hardware.power-service-qti
LOCAL_INIT_RC := android.hardware.power-service-qti.rc
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wno-unused-parameter -Wno-unused-variable
LOCAL_VENDOR_MODULE := true
LOCAL_VINTF_FRAGMENTS := power.xml

include $(BUILD_EXECUTABLE)

ifeq ($(TARGET_BOARD_PLATFORM), sun)
include $(CLEAR_VARS)

LOCAL_SHARED_LIBRARIES := android.hardware.common-V2-ndk android.hardware.common.fmq-V1-ndk libfmq liblog libcutils libdl libxml2 libbase libutils libbinder_ndk android.hardware.power-V5-ndk libbinder libclang_rt.ubsan_standalone
LOCAL_HEADER_LIBRARIES += libutils_headers
LOCAL_HEADER_LIBRARIES += libhardware_headers
LOCAL_SRC_FILES := power-common.c metadata-parser.c utils.c list.c hint-data.c powerhintparser.c Power.cpp fuzzer.cpp PowerHintSession.cpp
LOCAL_C_INCLUDES := external/libxml2/include \
                    external/icu/icu4c/source/common

ifeq ($(TARGET_USES_INTERACTION_BOOST),true)
    LOCAL_CFLAGS += -DINTERACTION_BOOST
endif

LOCAL_MODULE := aidl_fuzzer_power_service

LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wno-unused-parameter -Wno-unused-variable
LOCAL_VENDOR_MODULE := true

LOCAL_STATIC_LIBRARIES += libbinder_random_parcel

include $(BUILD_FUZZ_TEST)
endif

endif
