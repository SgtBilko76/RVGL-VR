# RVGL Quest: build SDL2 (with the VR host) + SDL2_image; the engine and its
# audio libs are prebuilt (src/main/jniLibs).
TOP_JNI := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_PATH := $(TOP_JNI)
LOCAL_MODULE := openxr_loader
LOCAL_SRC_FILES := ../src/main/jniLibs/$(TARGET_ARCH_ABI)/libopenxr_loader.so
include $(PREBUILT_SHARED_LIBRARY)

include $(TOP_JNI)/SDL2/Android.mk
include $(TOP_JNI)/SDL2_image/Android.mk
