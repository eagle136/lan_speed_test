LOCAL_PATH := $(call my-dir)
CORE_DIR := $(abspath $(LOCAL_PATH)/../../../../../core)

include $(CLEAR_VARS)
LOCAL_MODULE := lanspeed
LOCAL_SRC_FILES := lanspeed_jni.c $(CORE_DIR)/ls_core.c $(CORE_DIR)/ls_disc.c
LOCAL_C_INCLUDES := $(CORE_DIR)
LOCAL_CFLAGS := -O2 -std=c11 -Wall -Wextra
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
