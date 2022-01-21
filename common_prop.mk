# Audio
PRODUCT_PROPERTY_OVERRIDES += \
    ro.config.vc_call_vol_steps=7 \
    vendor.audio.adm.buffering.ms=3

# AV
PRODUCT_PROPERTY_OVERRIDES += \
    vendor.mm.enable.qcom_parser=16777215

# Bluetooth
PRODUCT_PROPERTY_OVERRIDES += \
    persist.bluetooth.bluetooth_audio_hal.disabled=false \
    persist.vendor.qcom.bluetooth.a2dp_offload_cap=sbc-aptx-aptxhd-aac-ldac \
    persist.vendor.qcom.bluetooth.enable.splita2dp=true \
    persist.vendor.qcom.bluetooth.soc=cherokee \
    vendor.qcom.bluetooth.soc=cherokee

PRODUCT_SYSTEM_EXT_PROPERTIES += \
    ro.bluetooth.library_name=libbluetooth_qti.so

# Camera
PRODUCT_PROPERTY_OVERRIDES += \
    persist.vendor.camera.preview.ubwc=0 \
    vendor.video.disable.ubwc=1 \
    vidc.enc.dcvs.extra-buff-count=2

# Charger
PRODUCT_PRODUCT_PROPERTIES += \
    ro.charger.enable_suspend=true

# Codec2 switch
PRODUCT_PROPERTY_OVERRIDES += \
    debug.media.codec2=2

# Display
PRODUCT_PROPERTY_OVERRIDES += \
    debug.sf.enable_hwc_vds=1 \
    debug.sf.hw=1 \
    persist.hwc.enable_vds=1 \
    ro.qualcomm.cabl=0 \
    vendor.display.disable_skip_validate=1 \
    vendor.display.enable_default_color_mode=0 \
    vendor.gralloc.disable_ahardware_buffer=1 \
    vendor.gralloc.enable_fb_ubwc=1 \
    vendor.video.disable.ubwc=1 \
    video.disable.ubwc=1

# Doze
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sensor.proximity=true \
    ro.sensor.pickup=android.sensor.tilt_detector

# PROP from AOSPA
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    arm64.memtag.process.system_server=off \
    ro.control_privapp_permissions=enforce \
    ro.launcher.blur.appLaunch=0

# GMS
PRODUCT_PROPERTY_OVERRIDES += \
    ro.com.google.clientidbase.ms=android-xiaomi-rev2

# Graphics
PRODUCT_PROPERTY_OVERRIDES += \
    ro.hardware.egl=adreno \
    ro.hardware.vulkan=adreno \
    ro.opengles.version=196610

# HAL1 apps list
PRODUCT_PROPERTY_OVERRIDES += \
    camera.hal1.packagelist=com.whatsapp,com.android.camera,com.android.camera2,com.instagram.android \
    vendor.camera.hal1.packagelist= com.whatsapp,com.android.camera,com.android.camera2,com.instagram.android

# IMS
PRODUCT_PROPERTY_OVERRIDES += \
    persist.dbg.volte_avail_ovr=1 \
    persist.dbg.vt_avail_ovr=1  \
    persist.dbg.wfc_avail_ovr=1

# Media
PRODUCT_PROPERTY_OVERRIDES += \
    debug.stagefright.omx_default_rank.sw-audio=1 \
    debug.stagefright.omx_default_rank=0 \
    media.aac_51_output_enabled=true \
    media.stagefright.enable-aac=true \
    media.stagefright.enable-http=true \
    media.stagefright.enable-player=true \
    media.stagefright.enable-qcp=true \
    media.stagefright.enable-scan=true \
    media.stagefright.thumbnail.prefer_hw_codecs=true \
    mm.enable.qcom_parser=13631471 \
    mm.enable.smoothstreaming=true \
    mmp.enable.3g2=true \
    persist.mm.enable.prefetch=true \
    vendor.vidc.dec.enable.downscalar=1 \
    vendor.vidc.enc.disable_bframes=1 \
    vendor.vidc.enc.disable.pq=true \
    vidc.enc.target_support_bframe=1

# Netflix custom property
PRODUCT_PROPERTY_OVERRIDES += \
    ro.netflix.bsp_rev=Q660-13149-1

# OEM Unlock reporting
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.oem_unlock_supported=1

# Perf
PRODUCT_PROPERTY_OVERRIDES += \
    ro.vendor.perf-hal.ver=2.2 \
    ro.vendor.extension_library=libqti-perfd-client.so \
    vendor.power.pasr.enabled=true

# Proximity
PRODUCT_PROPERTY_OVERRIDES += \
    gsm.proximity.enable=true

# QCOM
PRODUCT_PROPERTY_OVERRIDES += \
    persist.timed.enable=true \
    persist.vendor.qcomsysd.enabled=1

# Radio
PRODUCT_PROPERTY_OVERRIDES += \
    persist.vendor.data.iwlan.enable=true \
    persist.vendor.dpmhalservice.enable=1 \
    persist.vendor.radio.data_con_rprt=1 \
    rild.libpath=/vendor/lib64/libril-qc-hal-qmi.so \
    ro.telephony.iwlan_operation_mode=legacy

# Sensor
PRODUCT_PROPERTY_OVERRIDES += \
    ro.vendor.sdk.sensors.gestures=false \
    ro.vendor.sensors.cmc=false \
    ro.vendor.sensors.facing=false \
    ro.vendor.sensors.mot_detect=true \
    ro.vendor.sensors.pmd=true \
    ro.vendor.sensors.sta_detect=true

# SurfaceFlinger
PRODUCT_PROPERTY_OVERRIDES += \
    debug.sf.early_app_phase_offset_ns=1500000 \
    debug.sf.early_gl_app_phase_offset_ns=15000000 \
    debug.sf.early_gl_phase_offset_ns=3000000 \
    debug.sf.early_phase_offset_ns=1500000 \
    ro.surface_flinger.force_hwc_copy_for_virtual_displays=true \
    ro.surface_flinger.max_frame_buffer_acquired_buffers=3 \
    ro.surface_flinger.max_virtual_display_dimension=4096 \
    ro.surface_flinger.protected_contents=true \
    ro.surface_flinger.vsync_event_phase_offset_ns=2000000 \
    ro.surface_flinger.vsync_sf_event_phase_offset_ns=6000000

# System restart
PRODUCT_PROPERTY_OVERRIDES += \
    persist.vendor.ssr.restart_level=ALL_ENABLE

# Telephony
PRODUCT_PROPERTY_OVERRIDES += \
    persist.radio.multisim.config=dsds \
    persist.vendor.radio.apm_sim_not_pwdn=1 \
    persist.vendor.radio.custom_ecc=1 \
    persist.vendor.radio.enableadvancedscan=true \
    persist.vendor.radio.procedure_bytes=SKIP \
    persist.vendor.radio.rat_on=combine \
    persist.vendor.radio.sib16_support=1

PRODUCT_SYSTEM_EXT_PROPERTIES += \
    DEVICE_PROVISIONED=1 \
    net.tethering.noprovisioning=true \
    persist.sys.fflag.override.settings_network_and_internet_v2=true \
    persist.vendor.cne.feature=1 \
    persist.vendor.data.mode=concurrent \
    persist.vendor.dpm.feature=11 \
    ril.subscription.types=NV,RUIM \
    ro.telephony.default_network=33,33 \
    ro.vendor.use_data_netmgrd=true \
    telephony.lteOnCdmaDevice=1

ifeq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_PROPERTY_OVERRIDES += \
    persist.vendor.ims.disableADBLogs=1 \
    persist.vendor.ims.disableDebugLogs=1 \
    persist.vendor.ims.disableIMSLogs=1 \
    persist.vendor.ims.disableQXDMLogs=1
endif

# Thermal configs path
PRODUCT_PROPERTY_OVERRIDES += \
    sys.thermal.data.path=/data/vendor/thermal/

# Trusted Time
PRODUCT_PRODUCT_PROPERTIES += \
    persist.backup.ntpServer=0.pool.ntp.org

# Time daemon
PRODUCT_PROPERTY_OVERRIDES += \
    persist.timed.enable=true
