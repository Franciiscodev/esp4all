LOCAL_PATH := $(call my-dir)

# ==============================================================================
# 1. Platform-agnostic ESP4All Core Static Library (reusable in external & internal hosts)
# ==============================================================================
include $(CLEAR_VARS)
LOCAL_MODULE := esp4all_core

LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -fno-rtti -fexceptions -fvisibility=hidden -ffunction-sections -fdata-sections
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/include \
    $(LOCAL_PATH)/include/UE4 \
    $(LOCAL_PATH)/include/Config \
    $(LOCAL_PATH)/include/ESP \
    $(LOCAL_PATH)/include/Window

CORE_FILES := \
    $(wildcard $(LOCAL_PATH)/src/UE4/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/ESP/*.c*)

LOCAL_SRC_FILES := $(CORE_FILES:$(LOCAL_PATH)/%=%)
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include
include $(BUILD_STATIC_LIBRARY)

# Shared Android overlay sources (Vulkan/GLES + ImGui 1.92 + Android 5–17 Window + Touch + DebugUI)
OVERLAY_COMMON_INCLUDES := \
    $(LOCAL_PATH)/include \
    $(LOCAL_PATH)/include/Draw \
    $(LOCAL_PATH)/include/Graphics \
    $(LOCAL_PATH)/include/Input \
    $(LOCAL_PATH)/include/Window \
    $(LOCAL_PATH)/include/ImGui \
    $(LOCAL_PATH)/include/Fun \
    $(LOCAL_PATH)/include/UE4 \
    $(LOCAL_PATH)/include/Config \
    $(LOCAL_PATH)/include/ESP \
    $(LOCAL_PATH)/include/UI

OVERLAY_COMMON_FILES := \
    $(wildcard $(LOCAL_PATH)/src/Draw/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/Graphics/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/Input/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/Window/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/ImGui/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/Fun/*.c*) \
    $(wildcard $(LOCAL_PATH)/src/UI/*.c*)

# ==============================================================================
# 2. Standalone Android Overlay Executable (External Mode)
# ==============================================================================
include $(CLEAR_VARS)
LOCAL_MODULE := core

LOCAL_CFLAGS := -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections
LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra
LOCAL_CPPFLAGS += -fno-rtti -fexceptions -fvisibility=hidden -ffunction-sections -fdata-sections -Wno-error=format-security

LOCAL_C_INCLUDES += $(OVERLAY_COMMON_INCLUDES)

APP_FILES := \
    $(wildcard $(LOCAL_PATH)/src/*.c*) \
    $(OVERLAY_COMMON_FILES)

LOCAL_SRC_FILES := $(APP_FILES:$(LOCAL_PATH)/%=%)
LOCAL_STATIC_LIBRARIES := esp4all_core
LOCAL_LDFLAGS += -lEGL -lGLESv2 -landroid -llog -Wl,-z,max-page-size=16384 -Wl,--gc-sections
include $(BUILD_EXECUTABLE)

# ==============================================================================
# 3. Data-Driven In-Process Shared Library (Internal Mode: libesp4all_internal.so)
# ==============================================================================
include $(CLEAR_VARS)
LOCAL_MODULE := esp4all_internal

LOCAL_CFLAGS := -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections
LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra
LOCAL_CPPFLAGS += -fno-rtti -fexceptions -fvisibility=hidden -ffunction-sections -fdata-sections -Wno-error=format-security

LOCAL_C_INCLUDES += $(OVERLAY_COMMON_INCLUDES) $(LOCAL_PATH)/include/Internal

INTERNAL_FILES := \
    $(wildcard $(LOCAL_PATH)/src/Internal/*.c*) \
    $(OVERLAY_COMMON_FILES)

LOCAL_SRC_FILES := $(INTERNAL_FILES:$(LOCAL_PATH)/%=%)
LOCAL_STATIC_LIBRARIES := esp4all_core
LOCAL_LDFLAGS += -lEGL -lGLESv2 -landroid -llog -Wl,-z,max-page-size=16384 -Wl,--gc-sections
include $(BUILD_SHARED_LIBRARY)
