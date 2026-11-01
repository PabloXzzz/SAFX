LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE        := SAFX64
LOCAL_SRC_FILES     := main.cpp
LOCAL_C_INCLUDES    := $(LOCAL_PATH)
LOCAL_LDLIBS        := -llog -lGLESv2 -lEGL
include $(BUILD_SHARED_LIBRARY)
