#
# Copyright (C) 2020 Paranoid Android
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

#
# This file sets variables that control the way modules are built
# thorughout the system. It should not be used to conditionally
# disable makefiles (the proper mechanism to control what gets
# included in a build is to use PRODUCT_PACKAGES in a product
# definition file).
#

# Common Tree Path
COMMON_PATH := device/xiaomi/sdm660-common

# A/B
ifeq ($(ENABLE_AB), true)
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS ?= \
    boot \
    system \
    vendor
BOARD_BUILD_SYSTEM_ROOT_IMAGE := true
BOARD_USES_RECOVERY_AS_BOOT := true
TARGET_NO_RECOVERY := true
endif

# APEX image
DEXPREOPT_GENERATE_APEX_IMAGE := true

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=
TARGET_CPU_VARIANT := cortex-a73

# Architecture 2
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := kryo

# Audio
AUDIO_FEATURE_ENABLED_DYNAMIC_LOG := false

# Bootloader
TARGET_BOOTLOADER_BOARD_NAME := sdm660

# Board
BOARD_VENDOR := xiaomi

# Build Rules
BUILD_BROKEN_DUP_RULES := true

# Charger
BOARD_CHARGER_DISABLE_INIT_BLANK := true
BOARD_CHARGER_ENABLE_SUSPEND := true

# Display
TARGET_USES_GRALLOC1 := true
TARGET_USES_HWC2 := true

# GPS
LOC_HIDL_VERSION := 4.0

# HIDL
ifneq ($(filter lavender,$(TARGET_DEVICE)),)
DEVICE_MANIFEST_FILE := $(shell sed 's/3.0/4.0/g' $(COMMON_PATH)/manifest.xml > manifest.xml && echo manifest.xml)
else
DEVICE_MANIFEST_FILE := $(COMMON_PATH)/manifest.xml
endif
DEVICE_MATRIX_FILE := $(COMMON_PATH)/compatibility_matrix.xml

# Init
TARGET_INIT_VENDOR_LIB := //$(COMMON_PATH):libinit_sdm660
TARGET_PLATFORM_DEVICE_BASE := /devices/soc/
TARGET_RECOVERY_DEVICE_MODULES := libinit_sdm660

# Kernel
BOARD_KERNEL_CMDLINE := console=ttyMSM0,115200,n8 androidboot.console=ttyMSM0 earlycon=msm_serial_dm,0xc170000 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 sched_enable_hmp=1 sched_enable_power_aware=1 service_locator.enable=1 swiotlb=1 androidboot.configfs=true androidboot.usbcontroller=a800000.dwc3
BOARD_KERNEL_CMDLINE += usbcore.autosuspend=7
BOARD_KERNEL_CMDLINE += loop.max_part=16
BOARD_KERNEL_CMDLINE += androidboot.selinux=permissive
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_KERNEL_TAGS_OFFSET := 0x00000100
BOARD_RAMDISK_OFFSET     := 0x01000000
TARGET_KERNEL_APPEND_DTB := true
TARGET_KERNEL_VERSION := 4.4

# Enable stats logging in LMKD
TARGET_LMKD_STATS_LOG := true

# Partitions
BOARD_FLASH_BLOCK_SIZE := 262144
BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864
ifneq ($(ENABLE_AB), true)
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_CACHEIMAGE_PARTITION_SIZE := 268435456
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 67108864
endif
BOARD_SYSTEMIMAGE_PARTITION_TYPE := ext4
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3221225472
BOARD_VENDORIMAGE_PARTITION_SIZE := 2147483648
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_USERIMAGES_USE_EXT4 := true
ifneq ($(ENABLE_FBE), true)
TARGET_USERIMAGES_USE_F2FS := true
endif

# Directory
TARGET_COPY_OUT_PRODUCT := system/product
TARGET_COPY_OUT_VENDOR := vendor

# Peripheral manager
TARGET_PER_MGR_ENABLED := true

# Power
TARGET_TAP_TO_WAKE_NODE := "/sys/touchpanel/double_tap"

# Properties
BOARD_PROPERTY_OVERRIDES_SPLIT_ENABLED := true

# Recovery
ifneq ($(filter lavender,$(TARGET_DEVICE)),)
TARGET_RECOVERY_FSTAB := $(COMMON_PATH)/rootdir/etc/fstab_A.qcom
else ifeq ($(ENABLE_AB), true)
ifeq ($(ENABLE_FBE), true)
TARGET_RECOVERY_FSTAB := $(COMMON_PATH)/rootdir/etc/fstab_AB_fbe.qcom
else
TARGET_RECOVERY_FSTAB := $(COMMON_PATH)/rootdir/etc/fstab_AB.qcom
endif
else
TARGET_RECOVERY_FSTAB := $(COMMON_PATH)/rootdir/etc/fstab.qcom
endif

# Renderscript
OVERRIDE_RS_DRIVER := libRSDriver_adreno.so

# SELinux
BOARD_PLAT_PRIVATE_SEPOLICY_DIR += $(COMMON_PATH)/sepolicy/private
BOARD_PLAT_PUBLIC_SEPOLICY_DIR += $(COMMON_PATH)/sepolicy/public
BOARD_SEPOLICY_DIRS += $(COMMON_PATH)/sepolicy/vendor

# Symlinks
TARGET_MOUNT_POINTS_SYMLINKS := true

# Treble
PRODUCT_FULL_TREBLE_OVERRIDE := true

# VNDK
BOARD_VNDK_VERSION := current

# Wifi
BOARD_HAS_QCOM_WLAN := true
BOARD_WLAN_DEVICE := qcwcn
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
WIFI_DRIVER_FW_PATH_AP := "ap"
WIFI_DRIVER_FW_PATH_P2P := "p2p"
WIFI_DRIVER_FW_PATH_STA := "sta"
WIFI_DRIVER_OPERSTATE_PATH := "/sys/class/net/wlan0/operstate"
WIFI_HIDL_FEATURE_AWARE := true
WIFI_HIDL_FEATURE_DUAL_INTERFACE := true
WPA_SUPPLICANT_VERSION := VER_0_8_X

# Inherit the proprietary files
-include vendor/xiaomi/sdm660-common/BoardConfigVendor.mk
