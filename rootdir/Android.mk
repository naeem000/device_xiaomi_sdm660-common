LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

include $(CLEAR_VARS)
LOCAL_MODULE       := fstab.qcom
LOCAL_MODULE_TAGS  := optional
LOCAL_MODULE_CLASS := ETC
ifneq ($(filter lavender,$(TARGET_DEVICE)),)
LOCAL_SRC_FILES    := etc/fstab_A.qcom
else ifeq ($(ENABLE_AB), true)
ifeq ($(ENABLE_FBE), true)
LOCAL_SRC_FILES    := etc/fstab_AB_fbe.qcom
else
LOCAL_SRC_FILES    := etc/fstab_AB.qcom
endif
else
LOCAL_SRC_FILES    := etc/fstab.qcom
endif
LOCAL_MODULE_PATH  := $(TARGET_OUT_VENDOR_ETC)
include $(BUILD_PREBUILT)
