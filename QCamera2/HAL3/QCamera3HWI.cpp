/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#define LOG_TAG "QCamera3HWI"
//#define LOG_NDEBUG 0

#define __STDC_LIMIT_MACROS

// To remove
#include <cutils/properties.h>

// System dependencies
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "utils/Timers.h"
#include "sys/ioctl.h"
#include <sync/sync.h>
#include "gralloc_priv.h"
#include <map>
#include "fdleak.h"
#include "memleak.h"
// Display dependencies
#include "qdMetaData.h"

// Camera dependencies
#include "android/QCamera3External.h"
#include "util/QCameraFlash.h"
#include "QCameraPerfTranslator.h"
#include "QCamera3HWI.h"
#include "QCamera3VendorTags.h"
#include "QCameraTrace.h"

extern "C" {
#include "mm_camera_dbg.h"
}

using namespace android;

namespace qcamera {

//blur range
#define MIN_BLUR 0
#define MAX_BLUR 100
#define BLUR_STEP 1

#define DATA_PTR(MEM_OBJ,INDEX) MEM_OBJ->getPtr( INDEX )

#define EMPTY_PIPELINE_DELAY 2
#define PARTIAL_RESULT_COUNT 2
#define FRAME_SKIP_DELAY     0

#define MAX_VALUE_8BIT ((1<<8)-1)
#define MAX_VALUE_10BIT ((1<<10)-1)
#define MAX_VALUE_12BIT ((1<<12)-1)

#define VIDEO_4K_WIDTH  3840
#define VIDEO_4K_HEIGHT 2160

#define MAX_EIS_WIDTH VIDEO_4K_WIDTH
#define MAX_EIS_HEIGHT VIDEO_4K_HEIGHT

#define MAX_RAW_STREAMS        1
#define MAX_STALLING_STREAMS   1
#define MAX_PROCESSED_STREAMS  3
/* Batch mode is enabled only if FPS set is equal to or greater than this */
#define MIN_FPS_FOR_BATCH_MODE (120)
#define PREVIEW_FPS_FOR_HFR    (30)
#define DEFAULT_VIDEO_FPS      (30.0)
#define TEMPLATE_MAX_PREVIEW_FPS (30.0)
#define MAX_HFR_BATCH_SIZE     (8)
#define REGIONS_TUPLE_COUNT    5
#define HDR_PLUS_PERF_TIME_OUT  (7000) // milliseconds
// Set a threshold for detection of missing buffers //seconds
#define MISSING_REQUEST_BUF_TIMEOUT 3
#define MISSING_BOKEH_REQUEST_BUF_TIMEOUT 5
#define FLUSH_TIMEOUT 3
#define METADATA_MAP_SIZE(MAP) (sizeof(MAP)/sizeof(MAP[0]))

#define CAM_QCOM_FEATURE_PP_SUPERSET_HAL3   ( CAM_QCOM_FEATURE_DENOISE2D |\
                                              CAM_QCOM_FEATURE_CROP |\
                                              CAM_QCOM_FEATURE_ROTATION |\
                                              CAM_QCOM_FEATURE_SHARPNESS |\
                                              CAM_QCOM_FEATURE_SCALE |\
                                              CAM_QCOM_FEATURE_CAC |\
                                              CAM_QCOM_FEATURE_CDS )
/* Per configuration size for static metadata length*/
#define PER_CONFIGURATION_SIZE_3 (3)

#define TIMEOUT_NEVER -1

#define MIN_DIM(dim1,dim2) ((dim1.width*dim1.height)>(dim2.width*dim2.height)?dim2:dim1)

/* Face rect indices */
#define FACE_LEFT              0
#define FACE_TOP               1
#define FACE_RIGHT             2
#define FACE_BOTTOM            3
#define FACE_WEIGHT            4

/* Face landmarks indices */
#define LEFT_EYE_X             0
#define LEFT_EYE_Y             1
#define RIGHT_EYE_X            2
#define RIGHT_EYE_Y            3
#define MOUTH_X                4
#define MOUTH_Y                5
#define TOTAL_LANDMARK_INDICES 6

#define UBWC_COMP_RATIO 1.26
#define PERF_CONFIG_PATH "/vendor/etc/camera/cameraconfig.txt"

cam_capability_t *gCamCapability[MM_CAMERA_MAX_NUM_SENSORS];
const camera_metadata_t *gStaticMetadata[MM_CAMERA_MAX_NUM_SENSORS];
extern pthread_mutex_t gCamLock;
volatile uint32_t gCamHal3LogLevel = 1;
extern uint8_t gNumCameraSessions;

const QCamera3HardwareInterface::QCameraPropMap QCamera3HardwareInterface::CDS_MAP [] = {
    {"On",  CAM_CDS_MODE_ON},
    {"Off", CAM_CDS_MODE_OFF},
    {"Auto",CAM_CDS_MODE_AUTO}
};
const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_video_hdr_mode_t,
        cam_video_hdr_mode_t> QCamera3HardwareInterface::VIDEO_HDR_MODES_MAP[] = {
    { QCAMERA3_VIDEO_HDR_MODE_OFF,  CAM_VIDEO_HDR_MODE_OFF },
    { QCAMERA3_VIDEO_HDR_MODE_ON,   CAM_VIDEO_HDR_MODE_ON }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_binning_correction_mode_t,
        cam_binning_correction_mode_t> QCamera3HardwareInterface::BINNING_CORRECTION_MODES_MAP[] = {
    { QCAMERA3_BINNING_CORRECTION_MODE_OFF,  CAM_BINNING_CORRECTION_MODE_OFF },
    { QCAMERA3_BINNING_CORRECTION_MODE_ON,   CAM_BINNING_CORRECTION_MODE_ON }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_ir_mode_t,
        cam_ir_mode_type_t> QCamera3HardwareInterface::IR_MODES_MAP [] = {
    {QCAMERA3_IR_MODE_OFF,  CAM_IR_MODE_OFF},
    {QCAMERA3_IR_MODE_ON, CAM_IR_MODE_ON},
    {QCAMERA3_IR_MODE_AUTO, CAM_IR_MODE_AUTO}
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_control_effect_mode_t,
        cam_effect_mode_type> QCamera3HardwareInterface::EFFECT_MODES_MAP[] = {
    { ANDROID_CONTROL_EFFECT_MODE_OFF,       CAM_EFFECT_MODE_OFF },
    { ANDROID_CONTROL_EFFECT_MODE_MONO,       CAM_EFFECT_MODE_MONO },
    { ANDROID_CONTROL_EFFECT_MODE_NEGATIVE,   CAM_EFFECT_MODE_NEGATIVE },
    { ANDROID_CONTROL_EFFECT_MODE_SOLARIZE,   CAM_EFFECT_MODE_SOLARIZE },
    { ANDROID_CONTROL_EFFECT_MODE_SEPIA,      CAM_EFFECT_MODE_SEPIA },
    { ANDROID_CONTROL_EFFECT_MODE_POSTERIZE,  CAM_EFFECT_MODE_POSTERIZE },
    { ANDROID_CONTROL_EFFECT_MODE_WHITEBOARD, CAM_EFFECT_MODE_WHITEBOARD },
    { ANDROID_CONTROL_EFFECT_MODE_BLACKBOARD, CAM_EFFECT_MODE_BLACKBOARD },
    { ANDROID_CONTROL_EFFECT_MODE_AQUA,       CAM_EFFECT_MODE_AQUA }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_control_awb_mode_t,
        cam_wb_mode_type> QCamera3HardwareInterface::WHITE_BALANCE_MODES_MAP[] = {
    { ANDROID_CONTROL_AWB_MODE_OFF,             CAM_WB_MODE_OFF },
    { ANDROID_CONTROL_AWB_MODE_AUTO,            CAM_WB_MODE_AUTO },
    { ANDROID_CONTROL_AWB_MODE_INCANDESCENT,    CAM_WB_MODE_INCANDESCENT },
    { ANDROID_CONTROL_AWB_MODE_FLUORESCENT,     CAM_WB_MODE_FLUORESCENT },
    { ANDROID_CONTROL_AWB_MODE_WARM_FLUORESCENT,CAM_WB_MODE_WARM_FLUORESCENT},
    { ANDROID_CONTROL_AWB_MODE_DAYLIGHT,        CAM_WB_MODE_DAYLIGHT },
    { ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT, CAM_WB_MODE_CLOUDY_DAYLIGHT },
    { ANDROID_CONTROL_AWB_MODE_TWILIGHT,        CAM_WB_MODE_TWILIGHT },
    { ANDROID_CONTROL_AWB_MODE_SHADE,           CAM_WB_MODE_SHADE }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_control_scene_mode_t,
        cam_scene_mode_type> QCamera3HardwareInterface::SCENE_MODES_MAP[] = {
    { ANDROID_CONTROL_SCENE_MODE_DISABLED,       CAM_SCENE_MODE_OFF },
    { ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY,  CAM_SCENE_MODE_FACE_PRIORITY },
    { ANDROID_CONTROL_SCENE_MODE_ACTION,         CAM_SCENE_MODE_ACTION },
    { ANDROID_CONTROL_SCENE_MODE_PORTRAIT,       CAM_SCENE_MODE_PORTRAIT },
    { ANDROID_CONTROL_SCENE_MODE_LANDSCAPE,      CAM_SCENE_MODE_LANDSCAPE },
    { ANDROID_CONTROL_SCENE_MODE_NIGHT,          CAM_SCENE_MODE_NIGHT },
    { ANDROID_CONTROL_SCENE_MODE_NIGHT_PORTRAIT, CAM_SCENE_MODE_NIGHT_PORTRAIT },
    { ANDROID_CONTROL_SCENE_MODE_THEATRE,        CAM_SCENE_MODE_THEATRE },
    { ANDROID_CONTROL_SCENE_MODE_BEACH,          CAM_SCENE_MODE_BEACH },
    { ANDROID_CONTROL_SCENE_MODE_SNOW,           CAM_SCENE_MODE_SNOW },
    { ANDROID_CONTROL_SCENE_MODE_SUNSET,         CAM_SCENE_MODE_SUNSET },
    { ANDROID_CONTROL_SCENE_MODE_STEADYPHOTO,    CAM_SCENE_MODE_ANTISHAKE },
    { ANDROID_CONTROL_SCENE_MODE_FIREWORKS ,     CAM_SCENE_MODE_FIREWORKS },
    { ANDROID_CONTROL_SCENE_MODE_SPORTS ,        CAM_SCENE_MODE_SPORTS },
    { ANDROID_CONTROL_SCENE_MODE_PARTY,          CAM_SCENE_MODE_PARTY },
    { ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT,    CAM_SCENE_MODE_CANDLELIGHT },
    { ANDROID_CONTROL_SCENE_MODE_BARCODE,        CAM_SCENE_MODE_BARCODE},
    { ANDROID_CONTROL_SCENE_MODE_HDR,            CAM_SCENE_MODE_HDR}
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_control_af_mode_t,
        cam_focus_mode_type> QCamera3HardwareInterface::FOCUS_MODES_MAP[] = {
    { ANDROID_CONTROL_AF_MODE_OFF,                CAM_FOCUS_MODE_OFF },
    { ANDROID_CONTROL_AF_MODE_OFF,                CAM_FOCUS_MODE_FIXED },
    { ANDROID_CONTROL_AF_MODE_AUTO,               CAM_FOCUS_MODE_AUTO },
    { ANDROID_CONTROL_AF_MODE_MACRO,              CAM_FOCUS_MODE_MACRO },
    { ANDROID_CONTROL_AF_MODE_EDOF,               CAM_FOCUS_MODE_EDOF },
    { ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE, CAM_FOCUS_MODE_CONTINOUS_PICTURE },
    { ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO,   CAM_FOCUS_MODE_CONTINOUS_VIDEO }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_color_correction_aberration_mode_t,
        cam_aberration_mode_t> QCamera3HardwareInterface::COLOR_ABERRATION_MAP[] = {
    { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF,
            CAM_COLOR_CORRECTION_ABERRATION_OFF },
    { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST,
            CAM_COLOR_CORRECTION_ABERRATION_FAST },
    { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY,
            CAM_COLOR_CORRECTION_ABERRATION_HIGH_QUALITY },
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_control_ae_antibanding_mode_t,
        cam_antibanding_mode_type> QCamera3HardwareInterface::ANTIBANDING_MODES_MAP[] = {
    { ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF,  CAM_ANTIBANDING_MODE_OFF },
    { ANDROID_CONTROL_AE_ANTIBANDING_MODE_50HZ, CAM_ANTIBANDING_MODE_50HZ },
    { ANDROID_CONTROL_AE_ANTIBANDING_MODE_60HZ, CAM_ANTIBANDING_MODE_60HZ },
    { ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO, CAM_ANTIBANDING_MODE_AUTO }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_control_ae_mode_t,
        cam_flash_mode_t> QCamera3HardwareInterface::AE_FLASH_MODE_MAP[] = {
    { ANDROID_CONTROL_AE_MODE_OFF,                  CAM_FLASH_MODE_OFF },
    { ANDROID_CONTROL_AE_MODE_ON,                   CAM_FLASH_MODE_OFF },
    { ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH,        CAM_FLASH_MODE_AUTO},
    { ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH,      CAM_FLASH_MODE_ON  },
    { ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE, CAM_FLASH_MODE_AUTO}
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_flash_mode_t,
        cam_flash_mode_t> QCamera3HardwareInterface::FLASH_MODES_MAP[] = {
    { ANDROID_FLASH_MODE_OFF,    CAM_FLASH_MODE_OFF  },
    { ANDROID_FLASH_MODE_SINGLE, CAM_FLASH_MODE_SINGLE },
    { ANDROID_FLASH_MODE_TORCH,  CAM_FLASH_MODE_TORCH }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_statistics_face_detect_mode_t,
        cam_face_detect_mode_t> QCamera3HardwareInterface::FACEDETECT_MODES_MAP[] = {
    { ANDROID_STATISTICS_FACE_DETECT_MODE_OFF,    CAM_FACE_DETECT_MODE_OFF     },
    { ANDROID_STATISTICS_FACE_DETECT_MODE_SIMPLE, CAM_FACE_DETECT_MODE_SIMPLE  },
    { ANDROID_STATISTICS_FACE_DETECT_MODE_FULL,   CAM_FACE_DETECT_MODE_FULL    }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_lens_info_focus_distance_calibration_t,
        cam_focus_calibration_t> QCamera3HardwareInterface::FOCUS_CALIBRATION_MAP[] = {
    { ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED,
      CAM_FOCUS_UNCALIBRATED },
    { ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE,
      CAM_FOCUS_APPROXIMATE },
    { ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_CALIBRATED,
      CAM_FOCUS_CALIBRATED }
};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_lens_state_t,
        cam_af_lens_state_t> QCamera3HardwareInterface::LENS_STATE_MAP[] = {
    { ANDROID_LENS_STATE_STATIONARY,    CAM_AF_LENS_STATE_STATIONARY},
    { ANDROID_LENS_STATE_MOVING,        CAM_AF_LENS_STATE_MOVING}
};

const int32_t available_thumbnail_sizes[] = {0, 0,
                                             176, 144,
                                             240, 144,
                                             256, 144,
                                             240, 160,
                                             256, 154,
                                             240, 240,
                                             320, 240};

const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_sensor_test_pattern_mode_t,
        cam_test_pattern_mode_t> QCamera3HardwareInterface::TEST_PATTERN_MAP[] = {
    { ANDROID_SENSOR_TEST_PATTERN_MODE_OFF,          CAM_TEST_PATTERN_OFF   },
    { ANDROID_SENSOR_TEST_PATTERN_MODE_SOLID_COLOR,  CAM_TEST_PATTERN_SOLID_COLOR },
    { ANDROID_SENSOR_TEST_PATTERN_MODE_COLOR_BARS,   CAM_TEST_PATTERN_COLOR_BARS },
    { ANDROID_SENSOR_TEST_PATTERN_MODE_COLOR_BARS_FADE_TO_GRAY, CAM_TEST_PATTERN_COLOR_BARS_FADE_TO_GRAY },
    { ANDROID_SENSOR_TEST_PATTERN_MODE_PN9,          CAM_TEST_PATTERN_PN9 },
    { ANDROID_SENSOR_TEST_PATTERN_MODE_CUSTOM1,      CAM_TEST_PATTERN_CUSTOM1},
};

/* Since there is no mapping for all the options some Android enum are not listed.
 * Also, the order in this list is important because while mapping from HAL to Android it will
 * traverse from lower to higher index which means that for HAL values that are map to different
 * Android values, the traverse logic will select the first one found.
 */
const QCamera3HardwareInterface::QCameraMap<
        camera_metadata_enum_android_sensor_reference_illuminant1_t,
        cam_illuminat_t> QCamera3HardwareInterface::REFERENCE_ILLUMINANT_MAP[] = {
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_FLUORESCENT, CAM_AWB_WARM_FLO},
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT_FLUORESCENT, CAM_AWB_CUSTOM_DAYLIGHT },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_COOL_WHITE_FLUORESCENT, CAM_AWB_COLD_FLO },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_STANDARD_A, CAM_AWB_A },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_D55, CAM_AWB_NOON },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_D65, CAM_AWB_D65 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_D75, CAM_AWB_D75 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_D50, CAM_AWB_D50 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_ISO_STUDIO_TUNGSTEN, CAM_AWB_CUSTOM_A},
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT, CAM_AWB_D50 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_TUNGSTEN, CAM_AWB_A },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_FINE_WEATHER, CAM_AWB_D50 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_CLOUDY_WEATHER, CAM_AWB_D65 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_SHADE, CAM_AWB_D75 },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_DAY_WHITE_FLUORESCENT, CAM_AWB_CUSTOM_DAYLIGHT },
    { ANDROID_SENSOR_REFERENCE_ILLUMINANT1_WHITE_FLUORESCENT, CAM_AWB_COLD_FLO},
};

const QCamera3HardwareInterface::QCameraMap<
        int32_t, cam_hfr_mode_t> QCamera3HardwareInterface::HFR_MODE_MAP[] = {
    { 60, CAM_HFR_MODE_60FPS},
    { 90, CAM_HFR_MODE_90FPS},
    { 120, CAM_HFR_MODE_120FPS},
    { 150, CAM_HFR_MODE_150FPS},
    { 180, CAM_HFR_MODE_180FPS},
    { 210, CAM_HFR_MODE_210FPS},
    { 240, CAM_HFR_MODE_240FPS},
    { 480, CAM_HFR_MODE_480FPS},
};

const QCamera3HardwareInterface::QCameraMap<
        qcamera3_ext_instant_aec_mode_t,
        cam_aec_convergence_type> QCamera3HardwareInterface::INSTANT_AEC_MODES_MAP[] = {
    { QCAMERA3_INSTANT_AEC_NORMAL_CONVERGENCE, CAM_AEC_NORMAL_CONVERGENCE},
    { QCAMERA3_INSTANT_AEC_AGGRESSIVE_CONVERGENCE, CAM_AEC_AGGRESSIVE_CONVERGENCE},
    { QCAMERA3_INSTANT_AEC_FAST_CONVERGENCE, CAM_AEC_FAST_CONVERGENCE},
};

const QCamera3HardwareInterface::QCameraMap<
        qcamera3_ext_exposure_meter_mode_t,
        cam_auto_exposure_mode_type> QCamera3HardwareInterface::AEC_MODES_MAP[] = {
    { QCAMERA3_EXP_METER_MODE_FRAME_AVERAGE, CAM_AEC_MODE_FRAME_AVERAGE },
    { QCAMERA3_EXP_METER_MODE_CENTER_WEIGHTED, CAM_AEC_MODE_CENTER_WEIGHTED },
    { QCAMERA3_EXP_METER_MODE_SPOT_METERING, CAM_AEC_MODE_SPOT_METERING },
    { QCAMERA3_EXP_METER_MODE_SMART_METERING, CAM_AEC_MODE_SMART_METERING },
    { QCAMERA3_EXP_METER_MODE_USER_METERING, CAM_AEC_MODE_USER_METERING },
    { QCAMERA3_EXP_METER_MODE_SPOT_METERING_ADV, CAM_AEC_MODE_SPOT_METERING_ADV },
    { QCAMERA3_EXP_METER_MODE_CENTER_WEIGHTED_ADV, CAM_AEC_MODE_CENTER_WEIGHTED_ADV },
};

const QCamera3HardwareInterface::QCameraMap<
        qcamera3_ext_iso_mode_t,
        cam_iso_mode_type> QCamera3HardwareInterface::ISO_MODES_MAP[] = {
    { QCAMERA3_ISO_MODE_AUTO, CAM_ISO_MODE_AUTO },
    { QCAMERA3_ISO_MODE_DEBLUR, CAM_ISO_MODE_DEBLUR },
    { QCAMERA3_ISO_MODE_100, CAM_ISO_MODE_100 },
    { QCAMERA3_ISO_MODE_200, CAM_ISO_MODE_200 },
    { QCAMERA3_ISO_MODE_400, CAM_ISO_MODE_400 },
    { QCAMERA3_ISO_MODE_800, CAM_ISO_MODE_800 },
    { QCAMERA3_ISO_MODE_1600, CAM_ISO_MODE_1600 },
    { QCAMERA3_ISO_MODE_3200, CAM_ISO_MODE_3200 },
};

camera3_device_ops_t QCamera3HardwareInterface::mCameraOps = {
    .initialize                         = QCamera3HardwareInterface::initialize,
    .configure_streams                  = QCamera3HardwareInterface::configure_streams,
    .register_stream_buffers            = NULL,
    .construct_default_request_settings = QCamera3HardwareInterface::construct_default_request_settings,
    .process_capture_request            = QCamera3HardwareInterface::process_capture_request,
    .get_metadata_vendor_tag_ops        = NULL,
    .dump                               = QCamera3HardwareInterface::dump,
    .flush                              = QCamera3HardwareInterface::flush,
    .reserved                           = {0},
};

// initialise to some default value
uint32_t QCamera3HardwareInterface::sessionId[] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};

/*===========================================================================
 * FUNCTION   : QCamera3HardwareInterface
 *
 * DESCRIPTION: constructor of QCamera3HardwareInterface
 *
 * PARAMETERS :
 *   @cameraId  : camera ID
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3HardwareInterface::QCamera3HardwareInterface(uint32_t cameraId,
        const camera_module_callbacks_t *callbacks)
    : mCameraId(cameraId),
      mBlurLevel(0),
      mCameraHandle(NULL),
      mCameraInitialized(false),
      mCallbackOps(NULL),
      mMetadataChannel(NULL),
      mPictureChannel(NULL),
      mRawChannel(NULL),
      mSupportChannel(NULL),
      mAnalysisChannel(NULL),
      mRawDumpChannel(NULL),
      mDummyBatchChannel(NULL),
      mPerfLockMgr(),
      m_thermalAdapter(QCameraThermalAdapter::getInstance()),
      mChannelHandle(0),
      mFirstConfiguration(true),
      mFlush(false),
      mStreamOnPending(false),
      mFlushPerf(false),
      mHdrFrameNum(0),
      mMultiFrameCaptureNumber(0),
      mHdrSnapshotRunning(false),
      mMultiFrameSnapshotRunning(false),
      mShouldSetSensorHdr(false),
      mParamHeap(NULL),
      mParameters(NULL),
      mPrevParameters(NULL),
      m_bIsVideo(false),
      m_bIs4KVideo(false),
      m_bEisSupportedSize(false),
      m_bEisEnable(false),
      m_bEis3PropertyEnabled(false),
      m_MobicatMask(0),
      mMinProcessedFrameDuration(0),
      mMinJpegFrameDuration(0),
      mMinRawFrameDuration(0),
      mMetaFrameCount(0U),
      mUpdateDebugLevel(false),
      mCallbacks(callbacks),
      mCaptureIntent(0),
      mCacMode(0),
      mBatchSize(0),
      mToBeQueuedVidBufs(0),
      mHFRVideoFps(DEFAULT_VIDEO_FPS),
      mOpMode(CAMERA3_STREAM_CONFIGURATION_NORMAL_MODE),
      mStreamConfig(false),
      mCommon(),
      mQCFACaptureChannel(NULL),
      m_pFovControl(NULL),
      mFirstFrameNumberInBatch(0),
      mNeedSensorRestart(false),
      mPreviewStarted(false),
      mMinInFlightRequests(MIN_INFLIGHT_REQUESTS),
      mMaxInFlightRequests(MAX_INFLIGHT_REQUESTS),
      mInstantAEC(false),
      mResetInstantAEC(false),
      mInstantAECSettledFrameNumber(0),
      mAecSkipDisplayFrameBound(0),
      mInstantAecFrameIdxCount(0),
      mCurrFeatureState(0),
      mLdafCalibExist(false),
      mLastCustIntentFrmNum(-1),
      mState(CLOSED),
      mIsDeviceLinked(false),
      mIsMainCamera(true),
      mLinkedCameraId(0),
      m_pDualCamCmdHeap(NULL),
      m_bSensorHDREnabled(false),
      mCurrentSceneMode(0),
      m_bOfflineIsp(false),
      m_bQuadraCfaSensor(false),
      mQuadraCfaStage(QCFA_INACTIVE),
      m_ppChannelCnt(1),
      mDualCamera(false),
      m_bNeedHalPP(false),
      mBundledSnapshot(false),
      mActiveCameras(CAM_TYPE_MAIN),
      mMasterCamera(CAM_TYPE_MAIN),
      mFallbackMode(CAM_NO_FALLBACK),
      mLPMEnable(true),
      m_bStopPicChannel(false),
      mHALZSL(CAM_HAL3_ZSL_TYPE_NONE),
      mFlashNeeded(false)
{
    getLogLevel();
    m_halPPType = CAM_HAL_PP_TYPE_NONE;
    mCommon.init(gCamCapability[cameraId]);
    mCameraDevice.common.tag = HARDWARE_DEVICE_TAG;
#ifdef USE_HAL_3_5
    mCameraDevice.common.version = CAMERA_DEVICE_API_VERSION_3_5;
#elif defined(USE_HAL_3_3)
    mCameraDevice.common.version = CAMERA_DEVICE_API_VERSION_3_4;
#else
    mCameraDevice.common.version = CAMERA_DEVICE_API_VERSION_3_3;
#endif
    mCameraDevice.common.close = close_camera_device;
    mCameraDevice.ops = &mCameraOps;
    mCameraDevice.priv = this;
    gCamCapability[cameraId]->version = CAM_HAL_V3;
    mZSLChannel = NULL;
    // TODO: hardcode for now until mctl add support for min_num_pp_bufs
    //TBD - To see if this hardcoding is needed. Check by printing if this is filled by mctl to 3
    gCamCapability[cameraId]->min_num_pp_bufs = 3;
    pthread_condattr_t mCondAttr;

    pthread_condattr_init(&mCondAttr);
    pthread_condattr_setclock(&mCondAttr, CLOCK_MONOTONIC);

    pthread_cond_init(&mBuffersCond, &mCondAttr);

    pthread_cond_init(&mRequestCond, &mCondAttr);
    pthread_cond_init(&mHdrRequestCond, &mCondAttr);

    pthread_condattr_destroy(&mCondAttr);

    mPendingLiveRequest = 0;
    mCurrentRequestId = -1;
    pthread_mutex_init(&mMutex, NULL);

    for (size_t i = 0; i < CAMERA3_TEMPLATE_COUNT; i++)
        mDefaultMetadata[i] = NULL;

    // Getting system props of different kinds
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.raw.dump", prop, "0");
    mEnableRawDump = atoi(prop);
    property_get("persist.vendor.camera.hal3.force.hdr", prop, "0");
    mForceHdrSnapshot = atoi(prop);

    if (mEnableRawDump)
        LOGD("Raw dump from Camera HAL enabled");

    memset(&mInputStreamInfo, 0, sizeof(mInputStreamInfo));
    memset(mLdafCalib, 0, sizeof(mLdafCalib));

    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.tnr.preview", prop, "1");
    m_bTnrPreview = (uint8_t)atoi(prop);

    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.swtnr.preview", prop, "1");
    m_bSwTnrPreview = (uint8_t)atoi(prop);

    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.tnr.video", prop, "1");
    m_bTnrVideo = (uint8_t)atoi(prop);

    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.avtimer.debug", prop, "0");
    m_debug_avtimer = (uint8_t)atoi(prop);
    LOGI("AV timer enabled: %d", m_debug_avtimer);

    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.cacmode.disable", prop, "0");
    m_cacModeDisabled = (uint8_t)atoi(prop);

    mRdiModeFmt = gCamCapability[mCameraId]->rdi_mode_stream_fmt;
    //Load and read GPU library.
    lib_surface_utils = NULL;
    LINK_get_surface_pixel_alignment = NULL;
    mSurfaceStridePadding = CAM_PAD_TO_32;
    lib_surface_utils = dlopen("libadreno_utils.so", RTLD_NOW);
    if (lib_surface_utils) {
        *(void **)&LINK_get_surface_pixel_alignment =
                dlsym(lib_surface_utils, "get_gpu_pixel_alignment");
         if (LINK_get_surface_pixel_alignment) {
             mSurfaceStridePadding = LINK_get_surface_pixel_alignment();
         }
         dlclose(lib_surface_utils);
    }

    m_bInSensorQCFA = false;
    if (gCamCapability[cameraId]->is_quadracfa_sensor) {
        m_bQuadraCfaSensor = true;

        if (gCamCapability[cameraId]->is_quadracfa_insensor) {
            m_bInSensorQCFA = true;
        }

        char prop[PROPERTY_VALUE_MAX];
        memset(prop, 0, sizeof(prop));
        property_get("persist.vendor.camera.quadcfa.insensor", prop, "");
        if (strlen(prop) > 0) {
            uint8_t enabled = atoi(prop);
            m_bInSensorQCFA = enabled > 0 ? true : false;
        }
        LOGI("Sensor support Quadra CFA mode in sensor cqfa %d",  m_bInSensorQCFA);
    }

    m_bQuadraCfaRequest = false;
    m_bPreSnapQuadraCfaRequest = false;
    m_bQuadraSizeConfigured = false;
    memset(&mStreamList, 0, sizeof(camera3_stream_configuration_t));
    m_bLPMEnabled = false;

    mDualCamera = is_dual_camera_by_idx(cameraId);
    memset(m_pDualCamCmdPtr, 0, sizeof(m_pDualCamCmdPtr));
#ifdef ENABLE_THROTTLE
    FSM=setupFSM((char*)PERF_CONFIG_PATH);
    memset(&mSettingInfo, 0, sizeof(mSettingInfo));
    mSessionId = 0;
    mHFRMode = CAM_HFR_MODE_OFF;
#endif
}

/*===========================================================================
 * FUNCTION   : ~QCamera3HardwareInterface
 *
 * DESCRIPTION: destructor of QCamera3HardwareInterface
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3HardwareInterface::~QCamera3HardwareInterface()
{
    LOGD("E");

    int32_t rc = 0;

    if (mState == STARTED && mChannelHandle && isSecureMode()) {
        uint8_t close_hint = 1;
        LOGD("set_parms for close hint");
        clear_metadata_buffer(mParameters);
        ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_CLOSE_HINT,
            close_hint);
        rc = mCameraHandle->ops->set_parms(
            get_main_camera_handle(mCameraHandle->camera_handle), mParameters);
        if (rc < 0) {
            LOGE("set_parms failed for close hint");
        }
    }

    // Disable power hint and enable the perf lock for close camera
    mPerfLockMgr.releasePerfLock(PERF_LOCK_POWERHINT_ENCODE);
    mPerfLockMgr.releasePerfLock(PERF_LOCK_POWERHINT_HFR);
    mPerfLockMgr.acquirePerfLock(PERF_LOCK_CLOSE_CAMERA);

    if (isDualCamera()) {
        setDCLowPowerMode(MM_CAMERA_DUAL_CAM);
        setDCDeferCamera(CAM_DEFER_FLUSH);
    }

    if (m_bLPMEnabled) {
         cam_dual_camera_perf_control_t perf_value[1];
         perf_value[0].perf_mode = CAM_PERF_NONE;
         perf_value[0].enable = 0;
         perf_value[0].priority = 0;
         m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_LOW_POWER_MODE;
         memcpy(&m_pDualCamCmdPtr[0]->value, &perf_value[0],
                 sizeof(cam_dual_camera_perf_control_t));
         rc =  mCameraHandle->ops->set_dual_cam_cmd(mCameraHandle->camera_handle);
         if (rc != NO_ERROR) {
             LOGE("LPM not reset, but still proceed to close");
         } else {
            m_bLPMEnabled = false;
         }
    }
    // unlink of dualcam during close camera
    if (mIsDeviceLinked) {
        cam_dual_camera_bundle_info_t *m_pRelCamSyncBuf =
                &(m_pDualCamCmdPtr[0]->bundle_info);
        m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_BUNDLE_INFO;
        m_pRelCamSyncBuf->sync_control = CAM_SYNC_RELATED_SENSORS_OFF;
        m_pRelCamSyncBuf->sync_mechanism = CAM_SYNC_NO_SYNC;
        pthread_mutex_lock(&gCamLock);

        if (mIsMainCamera == 1) {
            m_pRelCamSyncBuf->mode = CAM_MODE_PRIMARY;
            m_pRelCamSyncBuf->type = CAM_TYPE_MAIN;
            m_pRelCamSyncBuf->sync_3a_config =
                    {CAM_3A_SYNC_FOLLOW, CAM_3A_SYNC_FOLLOW};
            // related session id should be session id of linked session
            m_pRelCamSyncBuf->related_sensor_session_id = sessionId[mLinkedCameraId];
        } else {
            m_pRelCamSyncBuf->mode = CAM_MODE_SECONDARY;
            m_pRelCamSyncBuf->type = CAM_TYPE_AUX;
            m_pRelCamSyncBuf->sync_3a_config =
                    {CAM_3A_SYNC_FOLLOW, CAM_3A_SYNC_FOLLOW};
            m_pRelCamSyncBuf->related_sensor_session_id = sessionId[mLinkedCameraId];
        }
        pthread_mutex_unlock(&gCamLock);

        rc = mCameraHandle->ops->set_dual_cam_cmd(
                mCameraHandle->camera_handle);
        if (rc < 0) {
            LOGE("Dualcam: Unlink failed, but still proceed to close");
        }
    }

    /* We need to stop all streams before deleting any stream */
    if (mRawDumpChannel) {
        mRawDumpChannel->stop();
    }

    if (mQCFACaptureChannel) {
        mQCFACaptureChannel->stop();
    }
    // NOTE: 'camera3_stream_t *' objects are already freed at
    //        this stage by the framework
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
        it != mStreamInfo.end(); it++) {
        QCamera3ProcessingChannel *channel = (*it)->channel;
        if (channel) {
            channel->stop();
        }
    }
    if (mSupportChannel)
        mSupportChannel->stop();

    if (mAnalysisChannel) {
        mAnalysisChannel->stop();
    }
    if (mMetadataChannel) {
        mMetadataChannel->stop();
    }
    if (mPictureChannel && m_bIsVideo && !m_bIs4KVideo) {
        mPictureChannel->stopChannel();
    }

    if (mChannelHandle) {
        mCameraHandle->ops->stop_channel(mCameraHandle->camera_handle,
                mChannelHandle);
        LOGD("stopping channel %d", mChannelHandle);
    }

    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
        it != mStreamInfo.end(); it++) {
        QCamera3ProcessingChannel *channel = (*it)->channel;
        if (channel)
            delete channel;
        free (*it);
    }
    if (mSupportChannel) {
        delete mSupportChannel;
        mSupportChannel = NULL;
    }

    if (mAnalysisChannel) {
        delete mAnalysisChannel;
        mAnalysisChannel = NULL;
    }
    if (mRawDumpChannel) {
        delete mRawDumpChannel;
        mRawDumpChannel = NULL;
    }
    if (mDummyBatchChannel) {
        delete mDummyBatchChannel;
        mDummyBatchChannel = NULL;
    }
    if (mQCFACaptureChannel) {
        delete mQCFACaptureChannel;
        mQCFACaptureChannel = NULL;
    }

    mPictureChannel = NULL;
    m_bStopPicChannel = false;

    mZSLChannel = NULL;

    if (mMetadataChannel) {
        delete mMetadataChannel;
        mMetadataChannel = NULL;
    }

    /* Clean up all channels */
    if (mCameraInitialized) {
        if(!mFirstConfiguration){
            //send the last unconfigure
            cam_stream_size_info_t stream_config_info;
            memset(&stream_config_info, 0, sizeof(cam_stream_size_info_t));
            clear_metadata_buffer(mParameters);
            ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_STREAM_INFO,
                    stream_config_info);
            int rc = mCameraHandle->ops->set_parms(
                    get_main_camera_handle(mCameraHandle->camera_handle), mParameters);
            if (rc < 0) {
                LOGE("set_parms failed for unconfigure");
            }

            //unconfigure for aux
            if (isDualCamera()) {
                cam_stream_size_info_t stream_config_info;
                memset(&stream_config_info, 0, sizeof(cam_stream_size_info_t));
                clear_metadata_buffer(mAuxParameters);
                ADD_SET_PARAM_ENTRY_TO_BATCH(mAuxParameters, CAM_INTF_META_STREAM_INFO,
                        stream_config_info);
                int rc = mCameraHandle->ops->set_parms(
                        get_aux_camera_handle(mCameraHandle->camera_handle), mAuxParameters);
                if (rc < 0) {
                    LOGE("set_parms failed for unconfigure");
                }
            }

        }
        deinitParameters();
    }

    if (mChannelHandle) {
        mCameraHandle->ops->delete_channel(mCameraHandle->camera_handle,
                mChannelHandle);
        LOGH("deleting channel %d", mChannelHandle);
        mChannelHandle = 0;
    }

    if (mState != CLOSED)
        closeCamera();

    for (auto &req : mPendingBuffersMap.mPendingBuffersInRequest) {
        req.mPendingBufferList.clear();
    }
    mPendingBuffersMap.mPendingBuffersInRequest.clear();
    mPendingReprocessResultList.clear();
    for (pendingRequestIterator i = mPendingRequestsList.begin();
            i != mPendingRequestsList.end();) {
        i = erasePendingRequest(i);
    }
    for (size_t i = 0; i < CAMERA3_TEMPLATE_COUNT; i++)
        if (mDefaultMetadata[i])
            free_camera_metadata(mDefaultMetadata[i]);

    mPerfLockMgr.releasePerfLock(PERF_LOCK_CLOSE_CAMERA);

    if (m_pFovControl) {
        delete m_pFovControl;
        m_pFovControl = NULL;
    }

    pthread_cond_destroy(&mRequestCond);
    pthread_cond_destroy(&mBuffersCond);
    pthread_cond_destroy(&mHdrRequestCond);

    pthread_mutex_destroy(&mMutex);
    if (mStreamList.streams != NULL) {
        free(mStreamList.streams);
    }
    LOGD("X");
}

/*===========================================================================
 * FUNCTION   : erasePendingRequest
 *
 * DESCRIPTION: function to erase a desired pending request after freeing any
 *              allocated memory
 *
 * PARAMETERS :
 *   @i       : iterator pointing to pending request to be erased
 *
 * RETURN     : iterator pointing to the next request
 *==========================================================================*/
QCamera3HardwareInterface::pendingRequestIterator
        QCamera3HardwareInterface::erasePendingRequest (pendingRequestIterator i)
{
    if (i->input_buffer != NULL) {
        free(i->input_buffer);
        i->input_buffer = NULL;
    }
    if (i->settings != NULL)
        free_camera_metadata((camera_metadata_t*)i->settings);
    return mPendingRequestsList.erase(i);
}

/*===========================================================================
 * FUNCTION   : camEvtHandle
 *
 * DESCRIPTION: Function registered to mm-camera-interface to handle events
 *
 * PARAMETERS :
 *   @camera_handle : interface layer camera handle
 *   @evt           : ptr to event
 *   @user_data     : user data ptr
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3HardwareInterface::camEvtHandle(uint32_t /*camera_handle*/,
                                          mm_camera_event_t *evt,
                                          void *user_data)
{
    QCamera3HardwareInterface *obj = (QCamera3HardwareInterface *)user_data;
    if (obj && evt) {
        switch(evt->server_event_type) {
            case CAM_EVENT_TYPE_DAEMON_DIED:
                pthread_mutex_lock(&obj->mMutex);
                obj->mState = ERROR;
                pthread_mutex_unlock(&obj->mMutex);
                LOGE("Fatal, camera daemon died");
                break;

            case CAM_EVENT_TYPE_DAEMON_PULL_REQ:
                LOGD("HAL got request pull from Daemon");
                break;
            default:
                LOGW("Warning: Unhandled event %d",
                        evt->server_event_type);
                break;
        }
    } else {
        LOGE("NULL user_data/evt");
    }
}

/*===========================================================================
 * FUNCTION   : thermalEvtHandle
 *
 * DESCRIPTION: routine to handle thermal event notification
 *
 * PARAMETERS :
 *   @level      : thermal level
 *   @userdata   : userdata passed in during registration
 *   @data       : opaque data from thermal client
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::thermalEvtHandle(
        qcamera_thermal_level_enum_t *level, void *userdata, void *data)
{
    /* TODO: implementation for thermal events handling */

    // Make sure thermal events are logged
    LOGH(" level = %d, userdata = %p, data = %p",
         *level, userdata, data);

    if (*level == QCAMERA_THERMAL_SHUTDOWN) {
        pthread_mutex_lock(&mMutex);
        mState = ERROR;
        pthread_mutex_unlock(&mMutex);
        handleCameraDeviceError();
    }

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : openCamera
 *
 * DESCRIPTION: open camera
 *
 * PARAMETERS :
 *   @hw_device  : double ptr for camera device struct
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::openCamera(struct hw_device_t **hw_device)
{
    int rc = 0;
    int enable_fdleak=0;
    int enable_memleak=0;
    char prop[PROPERTY_VALUE_MAX];
    if (mState != CLOSED) {
        *hw_device = NULL;
        return PERMISSION_DENIED;
    }

    mPerfLockMgr.acquirePerfLock(PERF_LOCK_OPEN_CAMERA);
    LOGI("[KPI Perf]: E PROFILE_OPEN_CAMERA camera id %d",
             mCameraId);
#ifdef FDLEAK_FLAG
    property_get("persist.vendor.camera.fdleak.enable", prop, "0");
    enable_fdleak = atoi(prop);
    if (enable_fdleak) {
       LOGI("fdleak tool is enable for camera hal");
    hal_debug_enable_fdleak_trace();
    }
#endif
#ifdef MEMLEAK_FLAG
    property_get("persist.vendor.camera.memleak.enable", prop, "0");
    enable_memleak = atoi(prop);
    if (enable_memleak) {
       LOGI("memleak tool is enable for camera hal");
    hal_debug_enable_memleak_trace();
    }
#endif
    rc = openCamera();
    if (rc == 0) {
        *hw_device = &mCameraDevice.common;
        if (m_thermalAdapter.init(this) != 0) {
           LOGW("Init thermal adapter failed");
        }
    } else {
        *hw_device = NULL;
    }

    LOGI("[KPI Perf]: X PROFILE_OPEN_CAMERA camera id %d, rc: %d",
             mCameraId, rc);

    if (rc == NO_ERROR) {
        mState = OPENED;
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : openCamera
 *
 * DESCRIPTION: open camera
 *
 * PARAMETERS : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::openCamera()
{
    int rc = 0;
    char value[PROPERTY_VALUE_MAX];

    KPI_ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_OPENCAMERA);
    if (mCameraHandle) {
        LOGE("Failure: Camera already opened");
        return ALREADY_EXISTS;
    }

    rc = QCameraFlash::getInstance().reserveFlashForCamera(mCameraId);
    if (rc < 0) {
        LOGE("Failed to reserve flash for camera id: %d",
                mCameraId);
        return UNKNOWN_ERROR;
    }

    rc = camera_open((uint8_t)mCameraId, &mCameraHandle);
    if (rc) {
        LOGE("camera_open failed. rc = %d, mCameraHandle = %p", rc, mCameraHandle);
        return rc;
    }

    if (!mCameraHandle) {
        LOGE("camera_open failed. mCameraHandle = %p", mCameraHandle);
        return -ENODEV;
    }

    rc = mCameraHandle->ops->register_event_notify(mCameraHandle->camera_handle,
            camEvtHandle, (void *)this);

    if (rc < 0) {
        LOGE("Error, failed to register event callback");
        /* Not closing camera here since it is already handled in destructor */
        return FAILED_TRANSACTION;
    }

    mExifParams.debug_params =
            (mm_jpeg_debug_exif_params_t *) malloc (sizeof(mm_jpeg_debug_exif_params_t));
    if (mExifParams.debug_params) {
        memset(mExifParams.debug_params, 0, sizeof(mm_jpeg_debug_exif_params_t));
    } else {
        LOGE("Out of Memory. Allocation failed for 3A debug exif params");
        return NO_MEMORY;
    }
    mFirstConfiguration = true;

    //Notify display HAL that a camera session is active.
    //But avoid calling the same during bootup because camera service might open/close
    //cameras at boot time during its initialization and display service will also internally
    //wait for camera service to initialize first while calling this display API, resulting in a
    //deadlock situation. Since boot time camera open/close calls are made only to fetch
    //capabilities, no need of this display bw optimization.
    //Use "service.bootanim.exit" property to know boot status.
    property_get("service.bootanim.exit", value, "0");
    if (atoi(value) == 1) {
        pthread_mutex_lock(&gCamLock);
        if (gNumCameraSessions++ == 0) {
            setCameraLaunchStatus(true);
        }
        #ifdef ENABLE_THROTTLE
        //session id starts from 0
        mSessionId = gNumCameraSessions - 1;
        #endif
        pthread_mutex_unlock(&gCamLock);
    }

    //fill the session id needed while linking dual cam
    pthread_mutex_lock(&gCamLock);
    rc = mCameraHandle->ops->get_session_id(mCameraHandle->camera_handle,
        &sessionId[mCameraId]);
    pthread_mutex_unlock(&gCamLock);

    if (rc < 0) {
        LOGE("Error, failed to get sessiion id");
        return UNKNOWN_ERROR;
    } else {
        //Allocate related cam sync buffer
        //this is needed for the payload that goes along with bundling cmd for related
        //camera use cases
        //Handle Dual camera cmd buffer
        uint8_t buf_cnt = 1;
        if (isDualCamera()) {
            buf_cnt = MM_CAMERA_MAX_CAM_CNT;
        }

        m_pDualCamCmdHeap = new QCamera3HeapMemory(buf_cnt);
        rc = m_pDualCamCmdHeap->allocate(sizeof(cam_dual_camera_cmd_info_t));
        if(rc != OK) {
            rc = NO_MEMORY;
            LOGE("Dualcam: Failed to allocate Related cam sync Heap memory");
            return NO_MEMORY;
        }
        for (int i = 0; i < buf_cnt; i++) {
            m_pDualCamCmdPtr[i] = (cam_dual_camera_cmd_info_t *)
                    DATA_PTR(m_pDualCamCmdHeap, i);
        }

        //Map memory for related cam sync buffer
        rc = mCameraHandle->ops->map_buf(get_main_camera_handle(mCameraHandle->camera_handle),
                CAM_MAPPING_BUF_TYPE_DUAL_CAM_CMD_BUF,
                m_pDualCamCmdHeap->getFd(0),
                sizeof(cam_dual_camera_cmd_info_t),
                m_pDualCamCmdHeap->getPtr(0));
        if(rc < 0) {
            LOGE("Dualcam: failed to map Related cam sync buffer");
            rc = FAILED_TRANSACTION;
            return NO_MEMORY;
        }

        if (isDualCamera()) {
            rc = mCameraHandle->ops->map_buf(
                    get_aux_camera_handle(mCameraHandle->camera_handle),
                    CAM_MAPPING_BUF_TYPE_DUAL_CAM_CMD_BUF,
                    m_pDualCamCmdHeap->getFd(1),
                    sizeof(cam_dual_camera_cmd_info_t),
                    m_pDualCamCmdPtr[1]);
            if(rc < 0) {
                LOGE("failed to map Related cam sync buffer");
                rc = FAILED_TRANSACTION;
                return NO_MEMORY;
            }
        }
    }

    if (isDualCamera()) {
        // Create and initialize FOV-control object
        m_pFovControl = QCameraFOVControl::create(
                gCamCapability[mCameraId]->main_cam_cap,
                gCamCapability[mCameraId]->aux_cam_cap, true);
        if (m_pFovControl) {
            mDualCamType = (uint8_t)QCameraCommon::getDualCameraConfig(
                    gCamCapability[mCameraId]->main_cam_cap,
                    gCamCapability[mCameraId]->aux_cam_cap);
            m_pFovControl->setDualCameraConfig(mDualCamType);
        }
        mActiveCameras = MM_CAMERA_DUAL_CAM;
    }

    LOGH("mCameraId=%d",mCameraId);

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : closeCamera
 *
 * DESCRIPTION: close camera
 *
 * PARAMETERS : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::closeCamera()
{
    KPI_ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_CLOSECAMERA);
    int rc = NO_ERROR;
    char value[PROPERTY_VALUE_MAX];
#ifdef ENABLE_THROTTLE
    int perfLevel;
#endif
    LOGI("[KPI Perf]: E PROFILE_CLOSE_CAMERA camera id %d",
             mCameraId);

    // unmap memory for related cam sync buffer
    mCameraHandle->ops->unmap_buf(mCameraHandle->camera_handle,
            CAM_MAPPING_BUF_TYPE_DUAL_CAM_CMD_BUF);
    if (NULL != m_pDualCamCmdHeap) {
        m_pDualCamCmdHeap->deallocate();
        delete m_pDualCamCmdHeap;
        m_pDualCamCmdHeap = NULL;
        memset(m_pDualCamCmdPtr, 0, sizeof(m_pDualCamCmdPtr));
    }
#ifdef ENABLE_THROTTLE
    perfLevel = predictFSM(FSM, NULL, NULL, mSessionId);
    if (isDualCamera()) {
        perfLevel = predictFSM(FSM, NULL, NULL, CONFIG_INDEX_AUX);
    }
    m_thermalAdapter.SetPerfLevel(perfLevel);
#endif
    m_thermalAdapter.deinit();

    rc = mCameraHandle->ops->close_camera(mCameraHandle->camera_handle);
    mCameraHandle = NULL;

    //reset session id to some invalid id
    pthread_mutex_lock(&gCamLock);
    sessionId[mCameraId] = 0xDEADBEEF;
    pthread_mutex_unlock(&gCamLock);

    //Notify display HAL that there is no active camera session
    //but avoid calling the same during bootup. Refer to openCamera
    //for more details.
    property_get("service.bootanim.exit", value, "0");
    if (atoi(value) == 1) {
        pthread_mutex_lock(&gCamLock);
        if (--gNumCameraSessions == 0) {
            setCameraLaunchStatus(false);
        }
#ifdef ENABLE_THROTTLE
        if ((gNumCameraSessions == 0) && (FSM != NULL)) {
            closeFSM();
            FSM = NULL;
        }
#endif
        pthread_mutex_unlock(&gCamLock);
    }

    if (mExifParams.debug_params) {
        free(mExifParams.debug_params);
        mExifParams.debug_params = NULL;
    }
    if (QCameraFlash::getInstance().releaseFlashFromCamera(mCameraId) != 0) {
        LOGW("Failed to release flash for camera id: %d",
                mCameraId);
    }
    mState = CLOSED;
    LOGI("[KPI Perf]: X PROFILE_CLOSE_CAMERA camera id %d, rc: %d",
         mCameraId, rc);
    return rc;
}

/*===========================================================================
 * FUNCTION   : initialize
 *
 * DESCRIPTION: Initialize frameworks callback functions
 *
 * PARAMETERS :
 *   @callback_ops : callback function to frameworks
 *
 * RETURN     :
 *
 *==========================================================================*/
int QCamera3HardwareInterface::initialize(
        const struct camera3_callback_ops *callback_ops)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_INIT);
    int rc;

    LOGI("E :mCameraId = %d mState = %d", mCameraId, mState);
    pthread_mutex_lock(&mMutex);

    // Validate current state
    switch (mState) {
        case OPENED:
            /* valid state */
            break;
        default:
            LOGE("Invalid state %d", mState);
            rc = -ENODEV;
            goto err1;
    }

    rc = initParameters();
    if (rc < 0) {
        LOGE("initParamters failed %d", rc);
        goto err1;
    }
    mCallbackOps = callback_ops;

    rc = addZSLChannel();

    pthread_mutex_unlock(&mMutex);
    mCameraInitialized = true;
    mState = INITIALIZED;
    LOGI("X");
    return 0;

err1:
    pthread_mutex_unlock(&mMutex);
    return rc;
}

/*===========================================================================
 * FUNCTION   : validateStreamDimensions
 *
 * DESCRIPTION: Check if the configuration requested are those advertised
 *
 * PARAMETERS :
 *   @stream_list : streams to be configured
 *
 * RETURN     :
 *
 *==========================================================================*/
int QCamera3HardwareInterface::validateStreamDimensions(
        camera3_stream_configuration_t *streamList)
{
    int rc = NO_ERROR;
    size_t count = 0;

    camera3_stream_t *inputStream = NULL;
    /*
    * Loop through all streams to find input stream if it exists*
    */
    for (size_t i = 0; i< streamList->num_streams; i++) {
        if (streamList->streams[i]->stream_type == CAMERA3_STREAM_INPUT) {
            if (inputStream != NULL) {
                LOGE("Error, Multiple input streams requested");
                return -EINVAL;
            }
            inputStream = streamList->streams[i];
        }
    }
    /*
    * Loop through all streams requested in configuration
    * Check if unsupported sizes have been requested on any of them
    */
    for (size_t j = 0; j < streamList->num_streams; j++) {
        bool sizeFound = false;
        camera3_stream_t *newStream = streamList->streams[j];

        uint32_t rotatedHeight = newStream->height;
        uint32_t rotatedWidth = newStream->width;
        if ((newStream->rotation == CAMERA3_STREAM_ROTATION_90) ||
                (newStream->rotation == CAMERA3_STREAM_ROTATION_270)) {
            rotatedHeight = newStream->width;
            rotatedWidth = newStream->height;
        }

        /*
        * Sizes are different for each type of stream format check against
        * appropriate table.
        */
        switch (newStream->format) {
        case ANDROID_SCALER_AVAILABLE_FORMATS_RAW16:
        case ANDROID_SCALER_AVAILABLE_FORMATS_RAW_OPAQUE:
        case HAL_PIXEL_FORMAT_RAW10:
        case HAL_PIXEL_FORMAT_RAW8:
            count = MIN(gCamCapability[mCameraId]->supported_raw_dim_cnt, MAX_SIZES_CNT);
            for (size_t i = 0; i < count; i++) {
                if ((gCamCapability[mCameraId]->raw_dim[i].width == (int32_t)rotatedWidth) &&
                        (gCamCapability[mCameraId]->raw_dim[i].height == (int32_t)rotatedHeight)) {
                    sizeFound = true;
                    break;
                }
            }
            if (m_bQuadraCfaSensor && !sizeFound) {
                if ((int32_t)rotatedWidth  <= gCamCapability[mCameraId]->quadra_cfa_dim[0].width &&
                    (int32_t)rotatedHeight <= gCamCapability[mCameraId]->quadra_cfa_dim[0].height) {
                    sizeFound = true;
                    if (newStream->stream_type == CAMERA3_STREAM_INPUT) {
                        mQuadraCfaStage = QCFA_RAW_REPROCESS;
                    } else if (newStream->stream_type == CAMERA3_STREAM_OUTPUT) {
                        mQuadraCfaStage = QCFA_RAW_OUTPUT;
                    }
                }
            }
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            if (newStream->data_space !=  HAL_DATASPACE_DEPTH) {
                /* Verify set size against generated sizes table */
                count = MIN(gCamCapability[mCameraId]->picture_sizes_tbl_cnt, MAX_SIZES_CNT);
                for (size_t i = 0; i < count; i++) {
                    if (((int32_t)rotatedWidth ==
                            gCamCapability[mCameraId]->picture_sizes_tbl[i].width) &&
                            ((int32_t)rotatedHeight ==
                            gCamCapability[mCameraId]->picture_sizes_tbl[i].height)) {
                        sizeFound = true;
                        break;
                    }
                }
            } else {
                sizeFound = true;
            }
            if (m_bQuadraCfaSensor) {
                if (((int32_t)rotatedWidth  <= gCamCapability[mCameraId]->quadra_cfa_dim[0].width &&
                   (int32_t)rotatedHeight <= gCamCapability[mCameraId]->quadra_cfa_dim[0].height) &&
                    (((int32_t)rotatedWidth > gCamCapability[mCameraId]->raw_dim[0].width) ||
                       ((int32_t)rotatedHeight > gCamCapability[mCameraId]->raw_dim[0].height))) {
                    sizeFound = true;
                    LOGI("BLOB stream configured with quadracfa size");
                    m_bQuadraSizeConfigured = true;
                }
            }
            break;
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_Y8:
            sizeFound = true;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
        case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
        default:
            if (newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL
                    || newStream->stream_type == CAMERA3_STREAM_INPUT
                    || IS_USAGE_ZSL(newStream->usage)) {
                if (((int32_t)rotatedWidth ==
                                gCamCapability[mCameraId]->active_array_size.width) &&
                                ((int32_t)rotatedHeight ==
                                gCamCapability[mCameraId]->active_array_size.height)) {
                    sizeFound = true;
                    break;
                }
                /* We could potentially break here to enforce ZSL stream
                 * set from frameworks always is full active array size
                 * but it is not clear from the spc if framework will always
                 * follow that, also we have logic to override to full array
                 * size, so keeping the logic lenient at the moment
                 */
            }
            count = MIN(gCamCapability[mCameraId]->picture_sizes_tbl_cnt,
                    MAX_SIZES_CNT);
            for (size_t i = 0; i < count; i++) {
                if (((int32_t)rotatedWidth ==
                            gCamCapability[mCameraId]->picture_sizes_tbl[i].width) &&
                            ((int32_t)rotatedHeight ==
                            gCamCapability[mCameraId]->picture_sizes_tbl[i].height)) {
                    sizeFound = true;
                    break;
                }
            }
            break;
        } /* End of switch(newStream->format) */

        /* We error out even if a single stream has unsupported size set */
        if (!sizeFound) {
            LOGE("Error: Unsupported size: %d x %d type: %d array size: %d x %d",
                    rotatedWidth, rotatedHeight, newStream->format,
                    gCamCapability[mCameraId]->active_array_size.width,
                    gCamCapability[mCameraId]->active_array_size.height);
            rc = -EINVAL;
            break;
        }
    } /* End of for each stream */
    return rc;
}

/*==============================================================================
 * FUNCTION   : isSupportChannelNeeded
 *
 * DESCRIPTION: Simple heuristic func to determine if support channels is needed
 *
 * PARAMETERS :
 *   @stream_list : streams to be configured
 *   @stream_config_info : the config info for streams to be configured
 *
 * RETURN     : Boolen true/false decision
 *
 *==========================================================================*/
bool QCamera3HardwareInterface::isSupportChannelNeeded(
        camera3_stream_configuration_t *streamList,
        cam_stream_size_info_t stream_config_info)
{
    uint32_t i;
    bool pprocRequested = false;
    /* Check for conditions where PProc pipeline does not have any streams*/
    for (i = 0; i < streamList->num_streams; i++) {
        if (streamList->streams[i]->data_space == HAL_DATASPACE_DEPTH) {
            return false;
        }
    }
    for (i = 0; i < stream_config_info.num_streams; i++) {
        if (stream_config_info.type[i] != CAM_STREAM_TYPE_ANALYSIS &&
                stream_config_info.postprocess_mask[i] != CAM_QCOM_FEATURE_NONE) {
            pprocRequested = true;
            break;
        }
    }

    if (pprocRequested == false )
        return true;

    /* Dummy stream needed if only raw or jpeg streams present */
    for (i = 0; i < streamList->num_streams; i++) {
        switch(streamList->streams[i]->format) {
            case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            case HAL_PIXEL_FORMAT_RAW8:
            case HAL_PIXEL_FORMAT_RAW10:
            case HAL_PIXEL_FORMAT_RAW16:
            case HAL_PIXEL_FORMAT_BLOB:
                break;
            default:
                return false;
        }
    }
    return true;
}

/*==============================================================================
 * FUNCTION   : getSensorOutputSize
 *
 * DESCRIPTION: Get sensor output size based on current stream configuratoin
 *
 * PARAMETERS :
 *   @sensor_dim : sensor output dimension (output)
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::getSensorOutputSize(cam_sensor_config_t &sensor_dim,
        uint32_t cam_type)
{
    int32_t rc = NO_ERROR;

    cam_dimension_t max_dim = {0, 0};
    uint32_t config_index = CONFIG_INDEX_MAIN;

    if(IS_MULTI_CAMERA && cam_type == CAM_TYPE_AUX)
    {
        config_index = CONFIG_INDEX_AUX;
    }

    for (uint32_t i = 0; i < mStreamConfigInfo[config_index].num_streams; i++) {
        if (mStreamConfigInfo[config_index].stream_sizes[i].width > max_dim.width)
            max_dim.width = mStreamConfigInfo[config_index].stream_sizes[i].width;
        if (mStreamConfigInfo[config_index].stream_sizes[i].height > max_dim.height)
            max_dim.height = mStreamConfigInfo[config_index].stream_sizes[i].height;
    }

    if (cam_type == MM_CAMERA_TYPE_AUX) {
        clear_metadata_buffer(mAuxParameters);

        rc = ADD_SET_PARAM_ENTRY_TO_BATCH(mAuxParameters, CAM_INTF_PARM_MAX_DIMENSION,
                max_dim);
        if (rc != NO_ERROR) {
            LOGE("Failed to update table for CAM_INTF_PARM_MAX_DIMENSION");
            return rc;
        }

        rc = mCameraHandle->ops->set_parms(
                get_aux_camera_handle(mCameraHandle->camera_handle), mAuxParameters);
        if (rc != NO_ERROR) {
            LOGE("Failed to set CAM_INTF_PARM_MAX_DIMENSION");
            return rc;
        }

        clear_metadata_buffer(mAuxParameters);
        ADD_GET_PARAM_ENTRY_TO_BATCH(mAuxParameters, CAM_INTF_PARM_RAW_DIMENSION);

        rc = mCameraHandle->ops->get_parms(
                get_aux_camera_handle(mCameraHandle->camera_handle), mAuxParameters);
        if (rc != NO_ERROR) {
            LOGE("Failed to get CAM_INTF_PARM_RAW_DIMENSION");
            return rc;
        }

        READ_PARAM_ENTRY(mAuxParameters, CAM_INTF_PARM_RAW_DIMENSION, sensor_dim);
    } else {
        clear_metadata_buffer(mParameters);

        rc = ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_MAX_DIMENSION,
                max_dim);
        if (rc != NO_ERROR) {
            LOGE("Failed to update table for CAM_INTF_PARM_MAX_DIMENSION");
            return rc;
        }

        rc = mCameraHandle->ops->set_parms(
                get_main_camera_handle(mCameraHandle->camera_handle), mParameters);
        if (rc != NO_ERROR) {
            LOGE("Failed to set CAM_INTF_PARM_MAX_DIMENSION");
            return rc;
        }

        clear_metadata_buffer(mParameters);
        ADD_GET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_RAW_DIMENSION);

        rc = mCameraHandle->ops->get_parms(
                get_main_camera_handle(mCameraHandle->camera_handle),
                mParameters);
        if (rc != NO_ERROR) {
            LOGE("Failed to get CAM_INTF_PARM_RAW_DIMENSION");
            return rc;
        }

        READ_PARAM_ENTRY(mParameters, CAM_INTF_PARM_RAW_DIMENSION, sensor_dim);
    }
    LOGH("camtype %d, sensor output dimension = %d x %d",
            cam_type, sensor_dim.width, sensor_dim.height);
    return rc;
}

/*==============================================================================
 * FUNCTION   : addToPPFeatureMask
 *
 * DESCRIPTION: add additional features to pp feature mask based on
 *              stream type and usecase
 *
 * PARAMETERS :
 *   @stream_format : stream type for feature mask
 *   @stream_idx : stream idx within postprocess_mask list to change
 *
 * RETURN     : NULL
 *
 *==========================================================================*/
void QCamera3HardwareInterface::addToPPFeatureMask(int stream_format,
        uint32_t stream_idx, cam_stream_size_info_t* mStreamConfigInfo)
{
    char feature_mask_value[PROPERTY_VALUE_MAX];
    cam_feature_mask_t feature_mask;
    int args_converted;
    int property_len;

    /* Get feature mask from property */
#ifdef _LE_CAMERA_
    char swtnr_feature_mask_value[PROPERTY_VALUE_MAX];
    snprintf(swtnr_feature_mask_value, PROPERTY_VALUE_MAX, "%lld", CAM_QTI_FEATURE_SW_TNR);
    property_len = property_get("persist.vendor.camera.hal3.feature",
            feature_mask_value, swtnr_feature_mask_value);
#else
    property_len = property_get("persist.vendor.camera.hal3.feature",
            feature_mask_value, "0");
#endif
    if ((property_len > 2) && (feature_mask_value[0] == '0') &&
            (feature_mask_value[1] == 'x')) {
        args_converted = sscanf(feature_mask_value, "0x%llx", &feature_mask);
    } else {
        args_converted = sscanf(feature_mask_value, "%lld", &feature_mask);
    }
    if (1 != args_converted) {
        feature_mask = 0;
        LOGE("Wrong feature mask %s", feature_mask_value);
        return;
    }

    switch (stream_format) {
    case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED: {
        char prop[PROPERTY_VALUE_MAX];
        memset(prop, 0, sizeof(prop));
        int32_t fixedFOVCenabled = FALSE;
        property_get("persist.vendor.camera.fovc.enable", prop, "0");
        fixedFOVCenabled = atoi(prop);
        if (fixedFOVCenabled == 1) {
            LOGH("Fixed FOVC feature mask set for stream format");
            mStreamConfigInfo->postprocess_mask[stream_idx]
                    |= CAM_QTI_FEATURE_FIXED_FOVC;
        }
        /* Add LLVD to pp feature mask only if video hint is enabled */
        if ((m_bIsVideo) && (feature_mask & CAM_QTI_FEATURE_SW_TNR)) {
            mStreamConfigInfo->postprocess_mask[stream_idx]
                    |= CAM_QTI_FEATURE_SW_TNR;
            LOGH("Added SW TNR to pp feature mask");
        } else if ((m_bIsVideo) && (feature_mask & CAM_QCOM_FEATURE_LLVD)) {
            mStreamConfigInfo->postprocess_mask[stream_idx]
                    |= CAM_QCOM_FEATURE_LLVD;
            LOGH("Added LLVD SeeMore to pp feature mask");
        }

        if (feature_mask & CAM_QCOM_FEATURE_LCAC) {
            mStreamConfigInfo->postprocess_mask[stream_idx]
                    |= CAM_QCOM_FEATURE_LCAC;
            LOGH("Added LCAC to pp feature mask");
        }
        if ((m_bIsVideo) && (gCamCapability[mCameraId]->qcom_supported_feature_mask &
                CAM_QTI_FEATURE_BINNING_CORRECTION)) {
            mStreamConfigInfo->postprocess_mask[stream_idx] |=
                    CAM_QTI_FEATURE_BINNING_CORRECTION;
        }

        if (gCamCapability[mCameraId]->qcom_supported_feature_mask &
                CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR) {
            mStreamConfigInfo->postprocess_mask[stream_idx] |=
                    CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR;
            if ( mStreamConfigInfo->postprocess_mask[stream_idx] &
                    CAM_QTI_FEATURE_BINNING_CORRECTION) {
                mStreamConfigInfo->postprocess_mask[stream_idx] &=
                        ~CAM_QTI_FEATURE_BINNING_CORRECTION;
            }
        }

        break;
    }
    default:
        break;
    }
    LOGD("PP feature mask %llx",
            mStreamConfigInfo->postprocess_mask[stream_idx]);
}

/*==============================================================================
 * FUNCTION   : updateFpsInPreviewBuffer
 *
 * DESCRIPTION: update FPS information in preview buffer.
 *
 * PARAMETERS :
 *   @metadata    : pointer to metadata buffer
 *   @frame_number: frame_number to look for in pending buffer list
 *
 * RETURN     : None
 *
 *==========================================================================*/
void QCamera3HardwareInterface::updateFpsInPreviewBuffer(metadata_buffer_t *metadata,
        uint32_t frame_number)
{
    // Mark all pending buffers for this particular request
    // with corresponding framerate information
    for (List<PendingBuffersInRequest>::iterator req =
            mPendingBuffersMap.mPendingBuffersInRequest.begin();
            req != mPendingBuffersMap.mPendingBuffersInRequest.end(); req++) {
        for(List<PendingBufferInfo>::iterator j =
                req->mPendingBufferList.begin();
                j != req->mPendingBufferList.end(); j++) {
            QCamera3Channel *channel = (QCamera3Channel *)j->stream->priv;
            if ((req->frame_number == frame_number) &&
                (channel->getStreamTypeMask() &
                (1U << CAM_STREAM_TYPE_PREVIEW))) {
                IF_META_AVAILABLE(cam_fps_range_t, float_range,
                    CAM_INTF_PARM_FPS_RANGE, metadata) {
                    typeof (MetaData_t::refreshrate) cameraFps = float_range->max_fps;
                    struct private_handle_t *priv_handle =
                        (struct private_handle_t *)(*(j->buffer));
                    setMetaData(priv_handle, UPDATE_REFRESH_RATE, &cameraFps);
                }
            }
        }
    }
}

/*==============================================================================
 * FUNCTION   : updateTimeStampInPendingBuffers
 *
 * DESCRIPTION: update timestamp in display metadata for all pending buffers
 *              of a frame number
 *
 * PARAMETERS :
 *   @frame_number: frame_number. Timestamp will be set on pending buffers of this frame number
 *   @timestamp   : timestamp to be set
 *
 * RETURN     : None
 *
 *==========================================================================*/
void QCamera3HardwareInterface::updateTimeStampInPendingBuffers(
        uint32_t frameNumber, nsecs_t timestamp)
{
    for (auto req = mPendingBuffersMap.mPendingBuffersInRequest.begin();
            req != mPendingBuffersMap.mPendingBuffersInRequest.end(); req++) {
        if (req->frame_number != frameNumber)
            continue;

        for (auto k = req->mPendingBufferList.begin();
                k != req->mPendingBufferList.end(); k++ ) {
            struct private_handle_t *priv_handle =
                    (struct private_handle_t *) (*(k->buffer));
            setMetaData(priv_handle, SET_VT_TIMESTAMP, &timestamp);
        }
    }
    return;
}
bool QCamera3HardwareInterface::isPPUpscaleNeededForDim(const cam_dimension_t &dim)
{
    bool ret = false;
    cam_dimension_t isp_max = gCamCapability[mCameraId]->single_isp_max_size;
     if((dim.width > isp_max.width) || (dim.height > isp_max.height))
     {
        ret = true;
     }
     return ret;
}

bool QCamera3HardwareInterface::isAsymetricDim(const cam_dimension_t &dim)
{
    bool ret = false;
    if(isDualCamera())
    {
        if(!isDimSupportedbyCamType(dim,CAM_TYPE_MAIN)
                || !isDimSupportedbyCamType(dim,CAM_TYPE_AUX))
        {
            ret = true;
        }
    }
    return ret;
}

void QCamera3HardwareInterface::rectifyStreamDimIfNeeded(
        cam_dimension_t &dim, const cam_sync_type_t &type, bool &needUpScale)
{
    if(isDualCamera())
    {
        if(isPPUpscaleNeededForDim(dim) || isAsymetricDim(dim))
        {
            cam_dimension_t updatedDim = getOptimalSupportedDim(dim,type);
            if((updatedDim.width * updatedDim.height) < (dim.width * dim.height))
            {
                dim = updatedDim;
                needUpScale = true;
            }
        }
    }
}

void QCamera3HardwareInterface::rectifyStreamSizesByCamType(
        cam_stream_size_info_t* streamsInfo, const cam_sync_type_t &type)
{
    if(isDualCamera())
    {
        cam_stream_size_info_t *info = streamsInfo;

        for (uint32_t i = 0; i < info->num_streams; i++) {
            cam_dimension_t dim;
            dim.width = info->stream_sizes[i].width;
            dim.height = info->stream_sizes[i].height;
            //skipping for stream with pp mask set for upscaling/cropping
            if(isPPMaskSetForScaling(info->postprocess_mask[i])
                || (info->type[i] == CAM_STREAM_TYPE_RAW))
            {
                continue;
            }
            if(isPPUpscaleNeededForDim(dim) || isAsymetricDim(dim))
            {
                //setting stream size less then equal to requested dimension of same
                // aspectratio.
                cam_dimension_t optimal_dim = getOptimalSupportedDim(dim, type);
                info->stream_sizes[i].width = optimal_dim.width;
                info->stream_sizes[i].height = optimal_dim.height;
            }
        }
    }
}

/*===========================================================================
 * FUNCTION   : isDimSupportedbyCamType.
 *
 * DESCRIPTION: In DualCamera mode, compare the "dim" with the main or aux cam
 *              based on cam_sync_type passed in "type", if found returns true else false.
 *              In Single camera mode return true.
 * PARAMETERS : cam_dimension_t to compare with "type" cam dimensions.
 *
 * RETURN     : bool.
 *
 *==========================================================================*/
bool QCamera3HardwareInterface::isDimSupportedbyCamType(const cam_dimension_t &dim,
                                                                 const cam_sync_type_t &type)
{
    bool ret = false;
    if(isDualCamera())
    {
        uint32_t tableSize;
        cam_dimension_t *picture_dim = NULL;
        if(type == CAM_TYPE_MAIN)
        {
            tableSize = gCamCapability[mCameraId]->main_cam_cap->picture_sizes_tbl_cnt;
            picture_dim = gCamCapability[mCameraId]->main_cam_cap->picture_sizes_tbl;
        }else {
            tableSize = gCamCapability[mCameraId]->aux_cam_cap->picture_sizes_tbl_cnt;
            picture_dim = gCamCapability[mCameraId]->aux_cam_cap->picture_sizes_tbl;
        }

        for(uint32_t i = 0; i < tableSize; i++)
        {
            if(((uint32_t)dim.width == (uint32_t)picture_dim[i].width) &&
                ((uint32_t)dim.height == (uint32_t)picture_dim[i].height))
            {
                ret = true;
                break;
            }
        }
        ret = false;
    }
    return ret;
}

/*===========================================================================
 * FUNCTION   : getOptimalSupportedDim
 *
 * DESCRIPTION: For dualcamera: extract the nearest to supported dimension request
 *              of aspectRatio tolerence less than 0.4.
 *              For Bokeh: return the res supported by both cameras.
 * PARAMETERS :
 *   @cam_dimension_t : neareast dimension to be searched for.
 *   @cam_sync_type_t : if DualCamera, based on sync type search for optimal dim.
 *                      if Single Camera, ignore this param.
 * RETURN     :
 *   @cam_dimension_t : optimal dim to be supported.
 *
 *==========================================================================*/
cam_dimension_t QCamera3HardwareInterface::getOptimalSupportedDim(const cam_dimension_t &dim,
                                                                  const cam_sync_type_t &type)
{
    if(isDualCamera())
    {
        float aspectRatio = (float)dim.width/(float)dim.height;
        float aspectRatioTolerence = 0.04;
        cam_dimension_t *picture_dim = NULL;
        cam_dimension_t optimal_dim = {0,0};
        cam_dimension_t max_isp_res = getMaxSingleIspRes();
        cam_dimension_t max_sensor_dim = {0,0};
        cam_dimension_t max_supported_dim ={0,0};
        uint32_t tableSize;
        if(type == CAM_TYPE_MAIN)
        {
            tableSize = gCamCapability[mCameraId]->main_cam_cap->picture_sizes_tbl_cnt;
            picture_dim = gCamCapability[mCameraId]->main_cam_cap->picture_sizes_tbl;
        }else {
            tableSize = gCamCapability[mCameraId]->aux_cam_cap->picture_sizes_tbl_cnt;
            picture_dim = gCamCapability[mCameraId]->aux_cam_cap->picture_sizes_tbl;
        }

        max_sensor_dim = picture_dim[0];
        max_supported_dim = MIN_DIM(max_isp_res, max_sensor_dim);

        if((max_supported_dim.width >= dim.width)
                    && (max_supported_dim.height >= dim.height))
        {
            goto END;
        }
        //First match the aspect ratio then compare with dim.
        for(uint32_t i = 0; i < tableSize; i++)
        {
            //ignore the dimension above sensor max size.
            if((max_supported_dim.width < picture_dim[i].width)
                    ||  (max_supported_dim.height < picture_dim[i].height))
            {
                continue;
            }
            if((((float)picture_dim[i].width/(float)picture_dim[i].height) - aspectRatio)
                                                                       < aspectRatioTolerence)
            {
                optimal_dim = picture_dim[i];
                break;
            }
        }

        if((optimal_dim.width*optimal_dim.height) > 0)
        {
            return optimal_dim;
        }
    }
END:
    return dim;
}

/*===========================================================================
 * FUNCTION   : getMaxSingleIspRes
 *
 * DESCRIPTION: return's max resolution supported by single isp.
 *
 * PARAMETERS : none
 *
 * RETURN     : max supported dimension of single isp.
 *
 *==========================================================================*/
cam_dimension_t QCamera3HardwareInterface::getMaxSingleIspRes()
{
    return gCamCapability[mCameraId]->single_isp_max_size;
}

/*===========================================================================
 * FUNCTION   : configureStreams
 *
 * DESCRIPTION: Reset HAL camera device processing pipeline and set up new input
 *              and output streams.
 *
 * PARAMETERS :
 *   @stream_list : streams to be configured
 *
 * RETURN     :
 *
 *==========================================================================*/
int QCamera3HardwareInterface::configureStreams(
        camera3_stream_configuration_t *streamList)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_CFG_STRMS);
    int rc = 0;

    // Acquire perfLock before configure streams
    mPerfLockMgr.acquirePerfLock(PERF_LOCK_START_PREVIEW);
    rc = configureStreamsPerfLocked(streamList);
    mPerfLockMgr.releasePerfLock(PERF_LOCK_START_PREVIEW);

    return rc;
}

/*===========================================================================
 * FUNCTION   : cacheFwConfiguredStreams
 *
 * DESCRIPTION: Creating a copy of new streams_configuration if not copied
 *              previously.
 *
 * PARAMETERS :
 *   @stream_list : streams to be configured
 *
 * RETURN     :
 *   @int     : -ENOMEM if failed otherwise NO_ERROR.
 *==========================================================================*/
int QCamera3HardwareInterface::cacheFwConfiguredStreams(
    camera3_stream_configuration_t *streams_configuration)
{
    int rc = NO_ERROR;
    if (&mStreamList != streams_configuration) {
        mStreamList = *streams_configuration;
        mStreamList.streams =
       (camera3_stream_t **)malloc(streams_configuration->num_streams * sizeof(camera3_stream_t*));
        if(mStreamList.streams == NULL)
        {
            rc =  -ENOMEM;
        } else {
            memcpy(mStreamList.streams, streams_configuration->streams,
                (streams_configuration->num_streams * sizeof(camera3_stream_t*)));
            mStreamList.num_streams = streams_configuration->num_streams;
        }
    } else {
        for (size_t i = 0; i < streams_configuration->num_streams; i++) {
            streams_configuration->streams[i]->priv = NULL;
        }
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : getPhyIdListForCameraId
 *
 * DESCRIPTION: Returns list of physical id's backed by logical id. If cameraId
 *              passed is not logical id, same physical id will be returned.
 *
 * PARAMETERS :
 *   @cameraId : camera id for which phyical id's list requested.
 *
 * RETURN     :
 *   @Vector<uint32_t>: id list is returned in vector form.
 *==========================================================================*/
Vector<uint32_t> QCamera3HardwareInterface::getPhyIdListForCameraId(uint32_t cameraId)
{
    Vector<uint32_t> camId;
    camId.clear();

    if(is_dual_camera_by_idx(cameraId))
    {
        camId.push_back(get_main_camera_idx(cameraId));
        camId.push_back(get_aux_camera_idx(cameraId));
    } else {
        camId.push_back(cameraId);
    }

    return camId;
}

/*===========================================================================
 * FUNCTION   : validateStreamsPhyIds
 *
 * DESCRIPTION: Validate physical id's present in configuration streams list.
 *
 * PARAMETERS :
 *   @streamList : streamList requested for configuration.
 *
 * RETURN     :
 *   @int     :EINVAL if physical id is not found else NO_ERROR.
 *==========================================================================*/
int QCamera3HardwareInterface::validateStreamsPhyIds(
     camera3_stream_configuration_t *streamList)
{
    int rc = NO_ERROR;
    Vector<uint32_t> phyIds = getPhyIdListForCameraId(mCameraId);
    for(uint32_t i = 0; i < streamList->num_streams; i++)
    {
        const char *id = streamList->streams[i]->physical_camera_id;
        if((id != NULL) && (*id != '\0'))
        {
            uint32_t _id = atoi(id);
            bool found = false;
            for(auto pId = phyIds.begin(); pId != phyIds.end(); pId++)
            {
                LOGE("phyIds %d", *pId);
                if(*pId == _id)
                {
                    found = true;
                    break;
                }
            }
            if(!found)
            {
                LOGE("phyId %d not found for camera Id %d", _id, mCameraId);
                return -EINVAL;
            }
        }
    }

    return rc;
}


/*===========================================================================
 * FUNCTION   : configureStreamsPerfLocked
 *
 * DESCRIPTION: configureStreams while perfLock is held.
 *
 * PARAMETERS :
 *   @stream_list : streams to be configured
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::configureStreamsPerfLocked(
        camera3_stream_configuration_t *streamList)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_CFG_STRMS_PERF_LKD);
    int rc = 0;

    // Sanity check stream_list
    if (streamList == NULL) {
        LOGE("NULL stream configuration");
        return BAD_VALUE;
    }
    if (streamList->streams == NULL) {
        LOGE("NULL stream list");
        return BAD_VALUE;
    }

    if (streamList->num_streams < 1) {
        LOGE("Bad number of streams requested: %d",
                streamList->num_streams);
        return BAD_VALUE;
    }

    if (streamList->num_streams >= MAX_NUM_STREAMS) {
        LOGE("Maximum number of streams %d exceeded: %d",
                MAX_NUM_STREAMS, streamList->num_streams);
        return BAD_VALUE;
    }

    mOpMode = streamList->operation_mode;
    LOGD("mOpMode: %d", mOpMode);

    mCurrentSceneMode = 0;
    mQuadraCfaStage = QCFA_INACTIVE;
    m_ppChannelCnt = 1;
    m_bOfflineIsp = false;
    mStreamOnPending = false;

    /* cache fw stream configuration, for internally reconfigure streams and then config "back" */
    cacheFwConfiguredStreams(streamList);

    if (isDualCamera() && !mFirstConfiguration) {
        setDCLowPowerMode(MM_CAMERA_DUAL_CAM);
    }

    if (mState == STARTED && mChannelHandle && isSecureMode()) {
        uint8_t close_hint = 1;
        LOGD("set_parms for close hint");
        clear_metadata_buffer(mParameters);
        ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_CLOSE_HINT,
            close_hint);
        rc = mCameraHandle->ops->set_parms(
            get_main_camera_handle(mCameraHandle->camera_handle), mParameters);
        if (rc < 0) {
            LOGE("set_parms failed for close hint");
        }
    }

    /* first invalidate all the steams in the mStreamList
     * if they appear again, they will be validated */
    for (List<stream_info_t*>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end(); it++) {
        QCamera3ProcessingChannel *channel = (QCamera3ProcessingChannel*)(*it)->stream->priv;
        if (channel) {
            channel->stop();
        }
        (*it)->status = INVALID;
    }

    if (mRawDumpChannel) {
        mRawDumpChannel->stop();
    }

    if (mSupportChannel)
        mSupportChannel->stop();

    if (mAnalysisChannel) {
        mAnalysisChannel->stop();
    }
    if (mMetadataChannel) {
        /* If content of mStreamInfo is not 0, there is metadata stream */
        mMetadataChannel->stop();
    }
    if (mPictureChannel && m_bIsVideo && !m_bIs4KVideo) {
        mPictureChannel->stopChannel();
    }

    if (mChannelHandle) {
        mCameraHandle->ops->stop_channel(mCameraHandle->camera_handle,
                mChannelHandle);
        LOGD("stopping channel %d", mChannelHandle);
    }

    m_bStopPicChannel = false;

    pthread_mutex_lock(&mMutex);

    // Check state
    switch (mState) {
        case INITIALIZED:
        case CONFIGURED:
        case STARTED:
            /* valid state */
            break;
        default:
            LOGE("Invalid state %d", mState);
            pthread_mutex_unlock(&mMutex);
            return -ENODEV;
    }

    /* Check whether we have video stream */
    m_bIs4KVideo = false;
    m_bIsVideo = false;
    m_bEisSupportedSize = false;
    m_bTnrEnabled = false;
    bool isZsl = false;
    uint32_t videoWidth = 0U;
    uint32_t videoHeight = 0U;
    size_t rawStreamCnt = 0;
    size_t stallStreamCnt = 0;
    size_t processedStreamCnt = 0;
    // Number of streams on ISP encoder path
    size_t numStreamsOnEncoder = 0;
    size_t numYuv888OnEncoder = 0;
    bool bYuv888OverrideJpeg = false;
    cam_dimension_t largeYuv888Size = {0, 0};
    cam_dimension_t maxViewfinderSize = {0, 0};
    bool bJpegExceeds4K = false;
    bool bJpegOnEncoder = false;
    bool bUseCommonFeatureMask = false;
    cam_feature_mask_t commonFeatureMask = 0;
    bool bSmallJpegSize = false;
    uint32_t width_ratio;
    uint32_t height_ratio;
    maxViewfinderSize = gCamCapability[mCameraId]->max_viewfinder_size;
    camera3_stream_t *inputStream = NULL;
    bool isJpeg = false;
    cam_dimension_t jpegSize = {0, 0};
    cam_dimension_t previewSize = {0, 0};

    cam_padding_info_t padding_info = gCamCapability[mCameraId]->padding_info;

    /*EIS configuration*/
    uint8_t eis_prop_set;
    uint32_t maxEisWidth = 0;
    uint32_t maxEisHeight = 0;

    // Initialize all instant AEC related variables
    mInstantAEC = false;
    mResetInstantAEC = false;
    mInstantAECSettledFrameNumber = 0;
    mAecSkipDisplayFrameBound = 0;
    mInstantAecFrameIdxCount = 0;
    mCurrFeatureState = 0;
    mStreamConfig = true;
    m_bIsSecureMode = false;
    mHALZSL = CAM_HAL3_ZSL_TYPE_NONE;
    uint32_t num_cb_stream = 0;

    //MULTICAM
    is_main_configured = false;
    is_aux_configured = false;
    is_logical_configured = false;

    memset(&mInputStreamInfo, 0, sizeof(mInputStreamInfo));

    if(isDualCamera())
    {
        if(validateStreamsPhyIds(streamList) != NO_ERROR)
        {
            LOGE("invalid physical ids");
            return BAD_VALUE;
        }
    }

    size_t count = IS_TYPE_MAX;
    count = MIN(gCamCapability[mCameraId]->supported_is_types_cnt, count);
    for (size_t i = 0; i < count; i++) {
        if ((gCamCapability[mCameraId]->supported_is_types[i] == IS_TYPE_EIS_2_0) ||
            (gCamCapability[mCameraId]->supported_is_types[i] == IS_TYPE_EIS_3_0) ||
            (gCamCapability[mCameraId]->supported_is_types[i] == IS_TYPE_VENDOR_EIS)) {
            m_bEisSupported = true;
            break;
        }
    }

    if (m_bEisSupported) {
        maxEisWidth = MAX_EIS_WIDTH;
        maxEisHeight = MAX_EIS_HEIGHT;
    }

    /* EIS setprop control */
    char eis_prop[PROPERTY_VALUE_MAX];
    memset(eis_prop, 0, sizeof(eis_prop));
    property_get("persist.vendor.camera.eis.enable", eis_prop, "1");
    eis_prop_set = (uint8_t)atoi(eis_prop);

    m_bEisEnable = eis_prop_set && m_bEisSupported;

    LOGD("m_bEisEnable: %d, eis_prop_set: %d, m_bEisSupported: %d",
            m_bEisEnable, eis_prop_set, m_bEisSupported);

    /* stream configurations */
    for (size_t i = 0; i < streamList->num_streams; i++) {
        camera3_stream_t *newStream = streamList->streams[i];
        LOGI("stream[%d] type = %d, format = %d, width = %d, "
                "height = %d, rotation = %d, usage = 0x%x",
                 i, newStream->stream_type, newStream->format,
                newStream->width, newStream->height, newStream->rotation,
                newStream->usage);
        if (newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL ||
                newStream->stream_type == CAMERA3_STREAM_INPUT){
            isZsl = true;
        }
        if (newStream->stream_type == CAMERA3_STREAM_INPUT){
            inputStream = newStream;
        }

        if (newStream->format == HAL_PIXEL_FORMAT_BLOB &&
                newStream->data_space !=  HAL_DATASPACE_DEPTH) {
            isJpeg = true;
            jpegSize.width = newStream->width;
            jpegSize.height = newStream->height;
            if (newStream->width > VIDEO_4K_WIDTH ||
                    newStream->height > VIDEO_4K_HEIGHT)
                bJpegExceeds4K = true;
        }

        if (IS_USAGE_SECURE(newStream->usage)) {
            m_bIsSecureMode = true;
        }

        if ((HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED == newStream->format) &&
                (newStream->usage & private_handle_t::PRIV_FLAGS_VIDEO_ENCODER)) {
            m_bIsVideo = true;
            // In HAL3 we can have multiple different video streams.
            // The variables video width and height are used below as
            // dimensions of the biggest of them
            if (videoWidth < newStream->width ||
                videoHeight < newStream->height) {
              videoWidth = newStream->width;
              videoHeight = newStream->height;
            }
            if ((VIDEO_4K_WIDTH <= newStream->width) &&
                    (VIDEO_4K_HEIGHT <= newStream->height)) {
                m_bIs4KVideo = true;
            }
            m_bEisSupportedSize = (newStream->width <= maxEisWidth) &&
                                  (newStream->height <= maxEisHeight);
        }
        if (newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL ||
                newStream->stream_type == CAMERA3_STREAM_OUTPUT) {
            switch (newStream->format) {
            case HAL_PIXEL_FORMAT_BLOB:
                if (newStream->data_space !=  HAL_DATASPACE_DEPTH) {
                    stallStreamCnt++;
                    if (isOnEncoder(maxViewfinderSize, newStream->width,
                             newStream->height)) {
                        numStreamsOnEncoder++;
                        bJpegOnEncoder = true;
                    }
                    width_ratio =
                        CEIL_DIVISION(gCamCapability[mCameraId]->active_array_size.width,
                            newStream->width);
                    height_ratio =
                        CEIL_DIVISION(gCamCapability[mCameraId]->active_array_size.height,
                            newStream->height);;
                    FATAL_IF(gCamCapability[mCameraId]->max_downscale_factor == 0,
                            "FATAL: max_downscale_factor cannot be zero and so assert");
                    if ( (width_ratio > gCamCapability[mCameraId]->max_downscale_factor) ||
                        (height_ratio > gCamCapability[mCameraId]->max_downscale_factor)) {
                        LOGH("Setting small jpeg size flag to true");
                        bSmallJpegSize = true;
                    }
                } else {
                    rawStreamCnt++;
                }
                break;
            case HAL_PIXEL_FORMAT_RAW8:
            case HAL_PIXEL_FORMAT_RAW10:
            case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            case HAL_PIXEL_FORMAT_RAW16:
            case HAL_PIXEL_FORMAT_Y16:
            case HAL_PIXEL_FORMAT_Y8:
                rawStreamCnt++;
                break;
            case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
                processedStreamCnt++;
                if (isOnEncoder(maxViewfinderSize, newStream->width,
                        newStream->height)) {
                    if (newStream->stream_type != CAMERA3_STREAM_BIDIRECTIONAL &&
                            !IS_USAGE_ZSL(newStream->usage)) {
                        commonFeatureMask |= CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                    }
                    numStreamsOnEncoder++;
                }
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_888:
                processedStreamCnt++;
                num_cb_stream++;
                if (isOnEncoder(maxViewfinderSize, newStream->width,
                        newStream->height)) {
                    // If Yuv888 size is not greater than 4K, set feature mask
                    // to SUPERSET so that it support concurrent request on
                    // YUV and JPEG.
                    if (newStream->width <= VIDEO_4K_WIDTH &&
                            newStream->height <= VIDEO_4K_HEIGHT) {
                        commonFeatureMask |= CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                    }
                    numStreamsOnEncoder++;
                    numYuv888OnEncoder++;
                    largeYuv888Size.width = newStream->width;
                    largeYuv888Size.height = newStream->height;
                }
                break;
            default:
                processedStreamCnt++;
                if (isOnEncoder(maxViewfinderSize, newStream->width,
                        newStream->height)) {
                    commonFeatureMask |= CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                    numStreamsOnEncoder++;
                }
                break;
            }

        }
    }
    if (!m_bIsVideo) {
        m_bEisEnable = false;
    }

    char prop[PROPERTY_VALUE_MAX];
    uint8_t forceEnableTnr = 0;
    memset(prop, 0, sizeof(prop));
    property_get("vendor.debug.camera.tnr.forceenable", prop, "0");
    forceEnableTnr = (uint8_t)atoi(prop);

    /* Logic to enable/disable TNR based on specific config size/etc.*/
    if (((m_bTnrPreview || m_bTnrVideo) && m_bIsVideo) || forceEnableTnr) {
        m_bTnrEnabled = true;
    }

    /* Check if num_streams is sane */
    if (stallStreamCnt > MAX_STALLING_STREAMS ||
            rawStreamCnt > MAX_RAW_STREAMS ||
            processedStreamCnt > MAX_PROCESSED_STREAMS) {
        LOGE("Invalid stream configu: stall: %d, raw: %d, processed %d",
                 stallStreamCnt, rawStreamCnt, processedStreamCnt);
        pthread_mutex_unlock(&mMutex);
        return -EINVAL;
    }
    /* Check whether we have zsl stream or 4k video case */
    if (isZsl && m_bIs4KVideo) {
        LOGE("Currently invalid configuration ZSL & 4K Video!");
        pthread_mutex_unlock(&mMutex);
        return -EINVAL;
    }
    /* Check if stream sizes are sane */
    if (numStreamsOnEncoder > 2) {
        LOGE("Number of streams on ISP encoder path exceeds limits of 2");
        pthread_mutex_unlock(&mMutex);
        return -EINVAL;
    } else if (1 < numStreamsOnEncoder){
        bUseCommonFeatureMask = true;
        LOGH("Multiple streams above max viewfinder size, common mask needed");
    }

    /* Check if BLOB size is greater than 4k in 4k recording case */
    if (m_bIs4KVideo && bJpegExceeds4K) {
        LOGE("HAL doesn't support Blob size greater than 4k in 4k recording");
        pthread_mutex_unlock(&mMutex);
        return -EINVAL;
    }

    // When JPEG and preview streams share VFE output, CPP will not apply CAC2
    // on JPEG stream. So disable such configurations to ensure CAC2 is applied.
    // Don't fail for reprocess configurations. Also don't fail if bJpegExceeds4K
    // is not true. Otherwise testMandatoryOutputCombinations will fail with following
    // configurations:
    //    {[PRIV, PREVIEW] [PRIV, RECORD] [JPEG, RECORD]}
    //    {[PRIV, PREVIEW] [YUV, RECORD] [JPEG, RECORD]}
    //    (These two configurations will not have CAC2 enabled even in HQ modes.)
    if (!isZsl && bJpegOnEncoder && bJpegExceeds4K && bUseCommonFeatureMask) {
        ALOGE("%s: Blob size greater than 4k and multiple streams are on encoder output",
                __func__);
        pthread_mutex_unlock(&mMutex);
        return -EINVAL;
    }

    // If jpeg stream is available, and a YUV 888 stream is on Encoder path, and
    // the YUV stream's size is greater or equal to the JPEG size, set common
    // postprocess mask to NONE, so that we can take advantage of postproc bypass.
    if (numYuv888OnEncoder && isOnEncoder(maxViewfinderSize,
            jpegSize.width, jpegSize.height) &&
            largeYuv888Size.width > jpegSize.width &&
            largeYuv888Size.height > jpegSize.height) {
        bYuv888OverrideJpeg = true;
    } else if (!isJpeg && numStreamsOnEncoder > 1) {
        commonFeatureMask = CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
    }

    LOGH("max viewfinder width %d height %d isZsl %d bUseCommonFeature %x commonFeatureMask %llx",
            maxViewfinderSize.width, maxViewfinderSize.height, isZsl, bUseCommonFeatureMask,
            commonFeatureMask);
    LOGH("numStreamsOnEncoder %d, processedStreamCnt %d, stallcnt %d bSmallJpegSize %d",
            numStreamsOnEncoder, processedStreamCnt, stallStreamCnt, bSmallJpegSize);

    rc = validateStreamDimensions(streamList);
    if (rc == NO_ERROR) {
        rc = validateStreamRotations(streamList);
    }
    if (rc != NO_ERROR) {
        LOGE("Invalid stream configuration requested!");
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    camera3_stream_t *zslStream = NULL; //Only use this for size and not actual handle!
    for (size_t i = 0; i < streamList->num_streams; i++) {
        camera3_stream_t *newStream = streamList->streams[i];
        LOGH("newStream type = %d, stream format = %d "
                "stream size : %d x %d, stream rotation = %d",
                 newStream->stream_type, newStream->format,
                newStream->width, newStream->height, newStream->rotation);
        //if the stream is in the mStreamList validate it
        bool stream_exists = false;
        for (List<stream_info_t*>::iterator it=mStreamInfo.begin();
                it != mStreamInfo.end(); it++) {
            if ((*it)->stream == newStream) {
                QCamera3ProcessingChannel *channel =
                    (QCamera3ProcessingChannel*)(*it)->stream->priv;
                stream_exists = true;
                if (channel)
                    delete channel;
                (*it)->status = VALID;
                (*it)->stream->priv = NULL;
                (*it)->channel = NULL;
            }
        }
        if (!stream_exists && newStream->stream_type != CAMERA3_STREAM_INPUT) {
            //new stream
            stream_info_t* stream_info;
            stream_info = (stream_info_t* )malloc(sizeof(stream_info_t));
            if (!stream_info) {
               LOGE("Could not allocate stream info");
               rc = -ENOMEM;
               pthread_mutex_unlock(&mMutex);
               return rc;
            }
            stream_info->stream = newStream;
            stream_info->status = VALID;
            stream_info->channel = NULL;
            mStreamInfo.push_back(stream_info);
        }
        /* Covers Opaque ZSL and API1 F/W ZSL */
        if (IS_USAGE_ZSL(newStream->usage)
                || newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL ) {
            if (zslStream != NULL) {
                LOGE("Multiple input/reprocess streams requested!");
                pthread_mutex_unlock(&mMutex);
                return BAD_VALUE;
            }
            zslStream = newStream;
        }
        /* Covers YUV reprocess */
        if (inputStream != NULL) {
            if (newStream->stream_type == CAMERA3_STREAM_OUTPUT
                    && newStream->format == HAL_PIXEL_FORMAT_YCbCr_420_888
                    && inputStream->format == HAL_PIXEL_FORMAT_YCbCr_420_888
                    && inputStream->width == newStream->width
                    && inputStream->height == newStream->height) {
                if (zslStream != NULL) {
                    /* This scenario indicates multiple YUV streams with same size
                     * as input stream have been requested, since zsl stream handle
                     * is solely use for the purpose of overriding the size of streams
                     * which share h/w streams we will just make a guess here as to
                     * which of the stream is a ZSL stream, this will be refactored
                     * once we make generic logic for streams sharing encoder output
                     */
                    LOGH("Warning, Multiple ip/reprocess streams requested!");
                }
                zslStream = newStream;
            }
        }
    }

    /* If a zsl stream is set, we know that we have configured at least one input or
       bidirectional stream */
    if (NULL != zslStream) {
        mInputStreamInfo.dim.width = (int32_t)zslStream->width;
        mInputStreamInfo.dim.height = (int32_t)zslStream->height;
        mInputStreamInfo.format = zslStream->format;
        mInputStreamInfo.usage = zslStream->usage;
        LOGD("Input stream configured! %d x %d, format %d, usage %d",
                 mInputStreamInfo.dim.width,
                mInputStreamInfo.dim.height,
                mInputStreamInfo.format, mInputStreamInfo.usage);
    }

    cleanAndSortStreamInfo();
    if (mMetadataChannel) {
        delete mMetadataChannel;
        mMetadataChannel = NULL;
    }
    if (mSupportChannel) {
        delete mSupportChannel;
        mSupportChannel = NULL;
    }

    if (mAnalysisChannel) {
        delete mAnalysisChannel;
        mAnalysisChannel = NULL;
    }

    if (mDummyBatchChannel) {
        delete mDummyBatchChannel;
        mDummyBatchChannel = NULL;
    }

    if (mRawDumpChannel) {
        delete mRawDumpChannel;
        mRawDumpChannel = NULL;
    }

    mPictureChannel = NULL;
    mZSLChannel = NULL;

    mHALZSL = CAM_HAL3_ZSL_TYPE_NONE;
    if(mOpMode == QCAMERA3_VENDOR_STREAM_CONFIGURATION_YUV_ZSL_MODE)
    {
        if((isDualCamera() && (num_cb_stream >= 0 ) && (num_cb_stream <= 2))
            || (!isDualCamera() && (num_cb_stream > 0)))
        {
            mHALZSL = CAM_HAL3_ZSL_TYPE_CALLBACK;
        }
    }else if(isJpeg && !isZsl && !m_bIsVideo && (!numStreamsOnEncoder ||
              ((numStreamsOnEncoder == 1) && bJpegOnEncoder)))
    {
        mHALZSL = CAM_HAL3_ZSL_TYPE_SNAPSHOT;
    } else {
        mHALZSL = CAM_HAL3_ZSL_TYPE_NONE;
    }

    char is_type_value[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.is_type", is_type_value, "0");
    m_bEis3PropertyEnabled = (atoi(is_type_value) == IS_TYPE_EIS_3_0);

    /* get eis information for stream configuration */
    cam_is_type_t isTypeVideo, isTypePreview;
    isTypeVideo = static_cast<cam_is_type_t>(atoi(is_type_value));

    property_get("persist.vendor.camera.is_type_preview", is_type_value, "4");
    isTypePreview = static_cast<cam_is_type_t>(atoi(is_type_value));
    LOGD("isTypeVideo: %d isTypePreview: %d", isTypeVideo, isTypePreview);

    //Create metadata channel and initialize it
    cam_feature_mask_t metadataFeatureMask = CAM_QCOM_FEATURE_NONE;
    setPAAFSupport(metadataFeatureMask, CAM_STREAM_TYPE_METADATA,
            gCamCapability[mCameraId]->color_arrangement);
    mMetadataChannel = new QCamera3MetadataChannel(mCameraHandle->camera_handle,
                    mChannelHandle, mCameraHandle->ops, captureResultCb,
                    setBufferErrorStatus, &padding_info, metadataFeatureMask, this);
    if (mMetadataChannel == NULL) {
        LOGE("failed to allocate metadata channel");
        rc = -ENOMEM;
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    if(mHALZSL)
    {
        mMetadataChannel->setZSLMode(true);
    }

    rc = mMetadataChannel->initialize(IS_TYPE_NONE);
    if (rc < 0) {
        LOGE("metadata channel initialization failed");
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    cam_feature_mask_t zsl_ppmask = CAM_QCOM_FEATURE_NONE;
    bool isRawStreamRequested = false;
    bool isDepth = false;
    memset(mStreamConfigInfo, 0, sizeof(cam_stream_size_info_t)*CONFIG_INDEX_MAX);

    if (isSecureMode()) {
        LOGI("Configuring secure mode");
        mStreamConfigInfo[CONFIG_INDEX_MAIN].is_secure = SECURE;
    }

    /* Allocate channel objects for the requested streams */
    for (size_t i = 0; i < streamList->num_streams; i++) {
        camera3_stream_t *newStream = streamList->streams[i];
        uint32_t stream_usage = newStream->usage;
        int index = CONFIG_INDEX_MAIN;
        uint32_t camHdl = mCameraHandle->camera_handle;
        uint32_t channelHdl = mChannelHandle;
        bool is_logical_stream = false;
        cam_sync_type_t camConfigType = CAM_TYPE_MAIN;
        uint8_t skip_config = FALSE; //skip this config for yuv zsl
#ifdef USE_HAL_3_5
        if(isDualCamera())
        {
            uint32_t newStreamPhyId = UINT32_MAX;
            if(IS_YUV_ZSL)
            {
                is_logical_stream = true;
                is_logical_configured = true;
                newStreamPhyId = this->mCameraId;
                index = CONFIG_INDEX_MAIN;
                camHdl = mCameraHandle->camera_handle;
                channelHdl = mChannelHandle;
                camConfigType = CAM_TYPE_MAIN;
                if(IS_EQUAL(newStream->format, HAL_PIXEL_FORMAT_YCbCr_420_888))
                {
                    if(IS_VALID_PTR(mZSLChannel))
                    {
                        index = CONFIG_INDEX_AUX;
                        is_logical_stream = false;
                        camConfigType = CAM_TYPE_AUX;
                    }else {
                        index = CONFIG_INDEX_MAIN;
                        is_logical_stream = false;
                    }
                }
            } else  if((newStream->physical_camera_id != NULL)
                    && (newStream->physical_camera_id[0] != '\0'))
            {
                newStreamPhyId = atoi(newStream->physical_camera_id);
                if(newStreamPhyId == get_aux_camera_idx(this->mCameraId))
                {
                    index = CONFIG_INDEX_AUX;
                    is_aux_configured = true;
                } else if(newStreamPhyId == get_main_camera_idx(this->mCameraId)) {
                    index = CONFIG_INDEX_MAIN;
                    is_main_configured = true;
                } else {
                    LOGE("unknown camera id. should not be here !!!");
                }
                camHdl = get_phys_handle(newStreamPhyId, this->mCameraId, camHdl);
                channelHdl = get_phys_handle(newStreamPhyId, this->mCameraId, channelHdl);
            } else {
                is_logical_configured = true;
                is_logical_stream = true;
                index = CONFIG_INDEX_MAIN;
                newStreamPhyId = this->mCameraId;
                camHdl = mCameraHandle->camera_handle;
                channelHdl = mChannelHandle;
            }
        }
#endif

        if(IS_YUV_ZSL && newStream->format == HAL_PIXEL_FORMAT_BLOB)
        {
            if(isDualCamera())
            {
                is_logical_stream = false;
            }
            skip_config = TRUE;
        }

        uint32_t &stream_index = mStreamConfigInfo[index].num_streams;
        mStreamConfigInfo[index].stream_sizes[stream_index].width = (int32_t)newStream->width;
        mStreamConfigInfo[index].stream_sizes[stream_index].height = (int32_t)newStream->height;

        if (isDualCamera()) {
            mStreamConfigInfo[index].sync_type = CAM_TYPE_MAIN;
        } else {
            mStreamConfigInfo[index].sync_type = CAM_TYPE_STANDALONE;
        }
        if ((newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL
                || IS_USAGE_ZSL(newStream->usage)) &&
            newStream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED){
            mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_SNAPSHOT;
            if (isOnEncoder(maxViewfinderSize, newStream->width, newStream->height)) {
                if (bUseCommonFeatureMask)
                    zsl_ppmask = commonFeatureMask;
                else
                    zsl_ppmask = CAM_QCOM_FEATURE_NONE;
            } else {
                if (numStreamsOnEncoder > 0)
                    zsl_ppmask = CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                else
                    zsl_ppmask = CAM_QCOM_FEATURE_NONE;
            }
            mStreamConfigInfo[index].postprocess_mask[stream_index] = zsl_ppmask;
        } else if(newStream->stream_type == CAMERA3_STREAM_INPUT) {
                LOGH("Input stream configured, reprocess config");
        } else {
            //for non zsl streams find out the format
            switch (newStream->format) {
            case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED :
            {
                mStreamConfigInfo[index].postprocess_mask[stream_index] =
                        CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                /* add additional features to pp feature mask */
                addToPPFeatureMask(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
                        stream_index, &mStreamConfigInfo[index]);

                if (stream_usage & private_handle_t::PRIV_FLAGS_VIDEO_ENCODER) {
                        mStreamConfigInfo[index].type[stream_index] =
                                CAM_STREAM_TYPE_VIDEO;
                    if (m_bTnrEnabled && m_bTnrVideo) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] |=
                            CAM_QCOM_FEATURE_CPP_TNR;
                        //TNR and CDS are mutually exclusive. So reset CDS from feature mask
                        mStreamConfigInfo[index].postprocess_mask[stream_index] &=
                                ~CAM_QCOM_FEATURE_CDS;
                    }
                    if (isTypeVideo == IS_TYPE_EIS_3_0 /* hint for EIS 3 needed here */) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] |=
                            CAM_QTI_FEATURE_PPEISCORE;
                    } else if (isTypeVideo == IS_TYPE_VENDOR_EIS) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] |=
                            CAM_QTI_FEATURE_VENDOR_EIS;
                    }
                    if(IS_USAGE_HEIF(newStream->usage))
                    {
                        padding_info.width_padding = CAM_PAD_TO_512;
                        padding_info.height_padding = CAM_PAD_TO_512;
                        padding_info.usage = newStream->usage;
                    }

                } else {
                        mStreamConfigInfo[index].type[stream_index] =
                            CAM_STREAM_TYPE_PREVIEW;
                    if (m_bTnrEnabled && m_bTnrPreview) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] |=
                                CAM_QCOM_FEATURE_CPP_TNR;
                        //TNR and CDS are mutually exclusive. So reset CDS from feature mask
                        mStreamConfigInfo[index].postprocess_mask[stream_index] &=
                                ~CAM_QCOM_FEATURE_CDS;
                    }
                    if(!m_bSwTnrPreview) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] &=
                                ~CAM_QTI_FEATURE_SW_TNR;
                    }
                    padding_info.width_padding = mSurfaceStridePadding;
                    padding_info.height_padding = CAM_PAD_TO_2;
                    previewSize.width = (int32_t)newStream->width;
                    previewSize.height = (int32_t)newStream->height;
                    if (isTypePreview == IS_TYPE_VENDOR_EIS /* hint for VENDOR EIS needed here */) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] |=
                            CAM_QTI_FEATURE_VENDOR_EIS;
                    }
                }
                if ((newStream->rotation == CAMERA3_STREAM_ROTATION_90) ||
                        (newStream->rotation == CAMERA3_STREAM_ROTATION_270)) {
                    mStreamConfigInfo[index].stream_sizes[stream_index].width =
                            newStream->height;
                    mStreamConfigInfo[index].stream_sizes[stream_index].height =
                            newStream->width;
                }
            }
            break;
            case HAL_PIXEL_FORMAT_YCbCr_420_888:
                if(IS_YUV_ZSL)
                {
                    mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                   CAM_QCOM_FEATURE_NONE;
                    mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_CALLBACK;
                } else {
                    mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_CALLBACK;
                    if (isOnEncoder(maxViewfinderSize, newStream->width, newStream->height)) {
                        if (bUseCommonFeatureMask)
                            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                    commonFeatureMask;
                        else
                            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                    CAM_QCOM_FEATURE_NONE;
                    } else {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                    }
                    if ((isZsl) && (zslStream == newStream)) {
                        zsl_ppmask = mStreamConfigInfo[index].postprocess_mask[stream_index];
                    }
                }
            break;
            case HAL_PIXEL_FORMAT_BLOB:
                if (newStream->data_space !=  HAL_DATASPACE_DEPTH) {
                    mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_SNAPSHOT;
                    // No need to check bSmallJpegSize if ZSL is present
                    //since JPEG uses ZSL stream
                    if ((m_bIs4KVideo && !isZsl) || (bSmallJpegSize && !isZsl)) {
                        mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
                        /* Remove rotation if it is not supported
                                for 4K LiveVideo snapshot case (online processing) */
                        if (!(gCamCapability[mCameraId]->qcom_supported_feature_mask &
                                CAM_QCOM_FEATURE_ROTATION)) {
                            mStreamConfigInfo[index].postprocess_mask[stream_index]
                                    &= ~CAM_QCOM_FEATURE_ROTATION;
                        }
                    } else {
                        if (bUseCommonFeatureMask &&
                                isOnEncoder(maxViewfinderSize, newStream->width,
                                newStream->height)) {
                            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                    commonFeatureMask;
                        } else {
                            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                    CAM_QCOM_FEATURE_NONE;
                        }
                    }
                    if (isZsl) {
                        if (zslStream) {
                            mStreamConfigInfo[index].stream_sizes[stream_index].width =
                                    (int32_t)zslStream->width;
                            mStreamConfigInfo[index].stream_sizes[stream_index].height =
                                    (int32_t)zslStream->height;
                            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                    zsl_ppmask;
                        } else {
                            if (mQuadraCfaStage != QCFA_INACTIVE) {
                                mStreamConfigInfo[index].stream_sizes[stream_index].width
                                    = gCamCapability[mCameraId]->picture_sizes_tbl[0].width;
                                mStreamConfigInfo[index].stream_sizes[stream_index].height
                                    = gCamCapability[mCameraId]->picture_sizes_tbl[0].height;
                                mStreamConfigInfo[index].postprocess_mask[stream_index] =
                                    CAM_QCOM_FEATURE_NONE;
                            } else {
                                LOGE("Error, No ZSL stream identified");
                                pthread_mutex_unlock(&mMutex);
                                return -EINVAL;
                            }
                        }
                    } else if (m_bQuadraSizeConfigured) {
                        cam_dimension_t binning_dim =
                                 getQCFAComapitbleDim(newStream->width, newStream->height);
                        mStreamConfigInfo[index].stream_sizes[stream_index].width =
                            binning_dim.width;
                        mStreamConfigInfo[index].stream_sizes[stream_index].height =
                            binning_dim.height;
                        mStreamConfigInfo[index].postprocess_mask[stream_index] =
                            CAM_QCOM_FEATURE_NONE;
                    } else if (m_bIs4KVideo) {
                        mStreamConfigInfo[index].stream_sizes[stream_index].width =
                                (int32_t)videoWidth;
                        mStreamConfigInfo[index].stream_sizes[stream_index].height =
                                (int32_t)videoHeight;
                    } else if (bYuv888OverrideJpeg) {
                        mStreamConfigInfo[index].stream_sizes[stream_index].width =
                                (int32_t)largeYuv888Size.width;
                        mStreamConfigInfo[index].stream_sizes[stream_index].height =
                                (int32_t)largeYuv888Size.height;
                    }
                } else {
                    mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_DEPTH;
                    mStreamConfigInfo[index].postprocess_mask[stream_index] =
                            CAM_QTI_FEATURE_DEPTH_MAP;
                    isDepth = true;
                }
                break;
            case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            case HAL_PIXEL_FORMAT_RAW16:
            case HAL_PIXEL_FORMAT_RAW10:
            case HAL_PIXEL_FORMAT_RAW8:
            case HAL_PIXEL_FORMAT_Y16:
            case HAL_PIXEL_FORMAT_Y8:
                if (newStream->data_space !=  HAL_DATASPACE_DEPTH) {
                    mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_RAW;
                    mStreamConfigInfo[index].postprocess_mask[stream_index] =
                            CAM_QCOM_FEATURE_NONE;
                    isRawStreamRequested = true;
                } else {
                    mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_DEPTH;
                    mStreamConfigInfo[index].postprocess_mask[stream_index] =
                            CAM_QTI_FEATURE_DEPTH_MAP;
                    isDepth = true;
                }
                break;
            default:
                mStreamConfigInfo[index].type[stream_index] = CAM_STREAM_TYPE_DEFAULT;
                mStreamConfigInfo[index].postprocess_mask[stream_index] = CAM_QCOM_FEATURE_NONE;
                break;
            }
        }

        setPAAFSupport(mStreamConfigInfo[index].postprocess_mask[stream_index],
                (cam_stream_type_t) mStreamConfigInfo[index].type[stream_index],
                gCamCapability[mCameraId]->color_arrangement);

        if (newStream->priv == NULL) {
            //New stream, construct channel
            switch (newStream->stream_type) {
            case CAMERA3_STREAM_INPUT:
                newStream->usage |= GRALLOC_USAGE_HW_CAMERA_READ;
                newStream->usage |= GRALLOC_USAGE_HW_CAMERA_WRITE;//WR for inplace algo's
                break;
            case CAMERA3_STREAM_BIDIRECTIONAL:
                newStream->usage |= GRALLOC_USAGE_HW_CAMERA_READ |
                    GRALLOC_USAGE_HW_CAMERA_WRITE;
                break;
            case CAMERA3_STREAM_OUTPUT:
                /* For video encoding stream, set read/write rarely
                 * flag so that they may be set to un-cached*/
                if (newStream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)
                    newStream->usage |=
                         (GRALLOC_USAGE_SW_READ_RARELY |
                         GRALLOC_USAGE_SW_WRITE_RARELY |
                         GRALLOC_USAGE_HW_CAMERA_WRITE);
                else if (IS_USAGE_ZSL(newStream->usage))
                {
                    LOGD("ZSL usage flag skipping");
                }
                else if (newStream == zslStream
                        || newStream->format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
                    newStream->usage |= GRALLOC_USAGE_HW_CAMERA_ZSL;
                } else
                    newStream->usage |= GRALLOC_USAGE_HW_CAMERA_WRITE;
                break;
            default:
                LOGE("Invalid stream_type %d", newStream->stream_type);
                break;
            }

            if (newStream->stream_type == CAMERA3_STREAM_OUTPUT ||
                    newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL) {
                QCamera3ProcessingChannel *channel = NULL;
                int bufferCount = isSecureMode() ? MAX_SECURE_BUFFERS : MAX_INFLIGHT_REQUESTS;
                switch (newStream->format) {
                case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
                    if ((newStream->usage &
                            private_handle_t::PRIV_FLAGS_VIDEO_ENCODER) &&
                            (streamList->operation_mode ==
                            CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE)
                    ) {
                       cam_padding_info_t l_padding = gCamCapability[mCameraId]->padding_info;
                       if(IS_USAGE_HEIF(newStream->usage))
                        {
                             l_padding = padding_info;
                        }

                        channel = new QCamera3RegularChannel(camHdl,
                                channelHdl, mCameraHandle->ops, captureResultCb,
                                setBufferErrorStatus, &l_padding,
                                this,
                                newStream,
                                (cam_stream_type_t)
                                        mStreamConfigInfo[index].type[stream_index],
                                mStreamConfigInfo[index].postprocess_mask[stream_index],
                                mMetadataChannel,
                                0); //heap buffers are not required for HFR video channel
                        if (channel == NULL) {
                            LOGE("allocation of channel failed");
                            pthread_mutex_unlock(&mMutex);
                            return -ENOMEM;
                        }
                        //channel->getNumBuffers() will return 0 here so use
                        //MAX_INFLIGH_HFR_REQUESTS
                        newStream->max_buffers = MAX_INFLIGHT_HFR_REQUESTS;
                        newStream->priv = channel;
                        LOGI("num video buffers in HFR mode: %d",
                                 MAX_INFLIGHT_HFR_REQUESTS);
                    } else {
                        /* Copy stream contents in HFR preview only case to create
                         * dummy batch channel so that sensor streaming is in
                         * HFR mode */
                        if (!m_bIsVideo && (streamList->operation_mode ==
                                CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE)) {
                            mDummyBatchStream = *newStream;
                            mDummyBatchStream.usage |= GRALLOC_USAGE_HW_VIDEO_ENCODER;
                        }
                        bufferCount = MAX_INFLIGHT_REQUESTS;
                        if (mStreamConfigInfo[index].type[stream_index] ==
                                CAM_STREAM_TYPE_VIDEO) {
                            if (m_bEis3PropertyEnabled /* hint for EIS 3 needed here */)
                                bufferCount = MAX_VIDEO_BUFFERS;
                            else if (isTypeVideo == IS_TYPE_VENDOR_EIS)
                                bufferCount = MAX_VIDEO_BUFFERS;
                        }

                        if (isSecureMode()) {
                            bufferCount = MAX_SECURE_BUFFERS;
                            mStreamConfigInfo[index].is_secure = SECURE;
                        }

                       cam_padding_info_t l_padding = gCamCapability[mCameraId]->padding_info;
                       if(IS_USAGE_HEIF(newStream->usage))
                        {
                             l_padding = padding_info;
                        }

                        channel = new QCamera3RegularChannel(camHdl,
                                channelHdl, mCameraHandle->ops, captureResultCb,
                                setBufferErrorStatus, &l_padding,
                                this,
                                newStream,
                                (cam_stream_type_t)
                                        mStreamConfigInfo[index].type[stream_index],
                                mStreamConfigInfo[index].postprocess_mask[stream_index],
                                mMetadataChannel,
                                bufferCount);
                        if (channel == NULL) {
                            LOGE("allocation of channel failed");
                            pthread_mutex_unlock(&mMutex);
                            return -ENOMEM;
                        }
                        /* disable UBWC for preview, though supported,
                         * to take advantage of CPP duplication */
                        if ((m_bIsVideo && (!mCommon.isVideoUBWCEnabled()) &&
                                (previewSize.width == (int32_t)videoWidth)&&
                                (previewSize.height == (int32_t)videoHeight))||isDualCamera()){
                            channel->setUBWCEnabled(false);
                        }else {
                            channel->setUBWCEnabled(true);
                        }
                        newStream->max_buffers = channel->getNumBuffers();
                        newStream->priv = channel;
                    }
                    break;
                case HAL_PIXEL_FORMAT_YCbCr_420_888: {
                    bool skipAux = IS_YUV_ZSL && (num_cb_stream == 2);
                    uint32_t maxYUVBufs = bufferCount;
                    if(IS_YUV_ZSL && isDualCamera())
                    {
                        maxYUVBufs += MAX_DUAL_CAM_MUXER_BUF;
                    }
                    channel = new QCamera3YUVChannel(camHdl,
                            channelHdl,
                            mCameraHandle->ops, captureResultCb,
                            setBufferErrorStatus, &padding_info,
                            this,
                            newStream,
                            (cam_stream_type_t)
                                    mStreamConfigInfo[index].type[stream_index],
                            mStreamConfigInfo[index].postprocess_mask[stream_index],
                            mMetadataChannel, maxYUVBufs, skipAux,camConfigType);
                    if (channel == NULL) {
                        LOGE("allocation of YUV channel failed");
                        pthread_mutex_unlock(&mMutex);
                        return -ENOMEM;
                    }
                    newStream->max_buffers = channel->getNumBuffers();
                    newStream->priv = channel;
                    break;
                }
                case HAL_PIXEL_FORMAT_RAW_OPAQUE:
                case HAL_PIXEL_FORMAT_RAW16:
                case HAL_PIXEL_FORMAT_RAW10:
                case HAL_PIXEL_FORMAT_RAW8:
                    mRawChannel = new QCamera3RawChannel(
                            camHdl, channelHdl,
                            mCameraHandle->ops, captureResultCb,
                            setBufferErrorStatus, &padding_info,
                            this, newStream,
                            mStreamConfigInfo[index].postprocess_mask[stream_index],
                            mMetadataChannel,
                            (mQuadraCfaStage == QCFA_INACTIVE) &&
                            (newStream->format == HAL_PIXEL_FORMAT_RAW16),
                            bufferCount);
                    if (mRawChannel == NULL) {
                        LOGE("allocation of raw channel failed");
                        pthread_mutex_unlock(&mMutex);
                        return -ENOMEM;
                    }
                    newStream->max_buffers = mRawChannel->getNumBuffers();
                    newStream->priv = (QCamera3ProcessingChannel*)mRawChannel;
                    break;
                case HAL_PIXEL_FORMAT_BLOB:
                    bufferCount = isSecureMode() ? MAX_SECURE_BUFFERS :
                                  (m_bIsVideo ? 1 : MAX_INFLIGHT_BLOB);
                    if (newStream->data_space !=  HAL_DATASPACE_DEPTH) {
                        // Max live snapshot inflight buffer is 1. This is to mitigate
                        // frame drop issues for video snapshot. The more buffers being
                        // allocated, the more frame drops there are.
                        int maxZSLBuffers = MAX_INFLIGHT_REQUESTS;

                       if(mOpMode != QCAMERA3_VENDOR_STREAM_CONFIGURATION_YUV_ZSL_MODE)
                       {
                           char prop[PROPERTY_VALUE_MAX];
                           memset(prop, 0, sizeof(prop));
                           property_get("persist.vendor.camera.halzsl.enable", prop, "");
                           if (strlen(prop) > 0) {
                               mHALZSL = (atoi(prop) == 1)?
                                              CAM_HAL3_ZSL_TYPE_SNAPSHOT:
                                              CAM_HAL3_ZSL_TYPE_NONE;
                           }
                       }

                        LOGI("%s HAL ZSL : ",
                            (mHALZSL == CAM_HAL3_ZSL_TYPE_SNAPSHOT) ? "Enabling":"Disabling");
                        LOGH("isZsl %d, m_bIsVideo %d, numStreamsOnEncoder %d, bJpegOnEncoder %d",
                                isZsl, m_bIsVideo, numStreamsOnEncoder, bJpegOnEncoder);
                        int maxSnapshotBuffers = (m_bIsVideo ? 1 :
                            (mHALZSL == CAM_HAL3_ZSL_TYPE_SNAPSHOT) ?
                                    maxZSLBuffers : MAX_INFLIGHT_BLOB);
                        if (isDualCamera()) {
                            maxSnapshotBuffers += MAX_DUAL_CAM_MUXER_BUF;
                        }
                        mPictureChannel = new QCamera3PicChannel(
                                mCameraHandle->camera_handle, mChannelHandle,
                                mCameraHandle->ops, captureResultCb,
                                setBufferErrorStatus, &padding_info, this, newStream,
                                mStreamConfigInfo[index].postprocess_mask[stream_index],
                                m_bIs4KVideo, isZsl, mMetadataChannel,
                                maxSnapshotBuffers,
                                IS_SNAP_ZSL,
                                (m_bIsVideo && !m_bIs4KVideo));
                        if (mPictureChannel == NULL) {
                            LOGE("allocation of channel failed");
                            pthread_mutex_unlock(&mMutex);
                            return -ENOMEM;
                        }
                        newStream->priv = (QCamera3ProcessingChannel*)mPictureChannel;
                        newStream->max_buffers = mPictureChannel->getNumBuffers();
                        mPictureChannel->overrideYuvSize(
                                mStreamConfigInfo[index].stream_sizes[stream_index].width,
                                mStreamConfigInfo[index].stream_sizes[stream_index].height);
                        if(skip_config) LOGD("skip blob channel config in YUV ZSL mode");
                    } else {
                        mDepthChannel = new QCamera3DepthChannel(
                                camHdl, channelHdl,
                                mCameraHandle->ops, captureResultCb,
                                setBufferErrorStatus, &padding_info,
                                this, newStream,
                                mStreamConfigInfo[index].postprocess_mask[stream_index],
                                mMetadataChannel, gCamCapability[mCameraId]->max_depth_points);
                        if (mDepthChannel == NULL) {
                            LOGE("allocation of channel failed");
                            pthread_mutex_unlock(&mMutex);
                            return -ENOMEM;
                        }
                        newStream->max_buffers = mDepthChannel->getNumBuffers();
                        newStream->priv = (QCamera3ProcessingChannel*)mDepthChannel;
                    }
                    break;
                case HAL_PIXEL_FORMAT_Y16:
                case HAL_PIXEL_FORMAT_Y8:
                    mDepthChannel = new QCamera3DepthChannel(
                            camHdl, channelHdl,
                            mCameraHandle->ops, captureResultCb,
                            setBufferErrorStatus, &padding_info,
                            this, newStream,
                            mStreamConfigInfo[index].postprocess_mask[stream_index],
                            mMetadataChannel, gCamCapability[mCameraId]->max_depth_points,
                            bufferCount);
                    if (mDepthChannel == NULL) {
                        LOGE("allocation of channel failed");
                        pthread_mutex_unlock(&mMutex);
                        return -ENOMEM;
                    }
                    newStream->max_buffers = mDepthChannel->getNumBuffers();
                    newStream->priv = (QCamera3ProcessingChannel*)mDepthChannel;
                    break;
                default:
                    LOGE("not a supported format 0x%x", newStream->format);
                    pthread_mutex_unlock(&mMutex);
                    return -EINVAL;
                }
                if((IS_YUV_ZSL) && (newStream->format == HAL_PIXEL_FORMAT_YCbCr_420_888))
                {
                    if(mZSLChannel == NULL)
                    {
                        mZSLChannel = (QCamera3ProcessingChannel *)newStream->priv;
                        mZSLChannel->setZSLMode(true);
                    } else if(isDualCamera())
                    {
                        if(IS_VALID_PTR(mZSLChannel))
                        {
                            mZSLChannel->setAuxChannel((QCamera3Channel *)newStream->priv);
                            ((QCamera3ProcessingChannel *)(newStream->priv))->setZSLMode(true);
                        }
                    }
                }else if(IS_SNAP_ZSL
                          && IS_VALID_PTR(mPictureChannel)
                          && IS_EQUAL(mZSLChannel,NULL))
                {
                    mPictureChannel->setZSLMode(true);
                    mZSLChannel = mPictureChannel;
                }
            } else if (newStream->stream_type == CAMERA3_STREAM_INPUT) {
                newStream->max_buffers = MAX_INFLIGHT_REPROCESS_REQUESTS;
            } else {
                LOGE("Error, Unknown stream type");
                pthread_mutex_unlock(&mMutex);
                return -EINVAL;
            }

            QCamera3Channel *channel = (QCamera3Channel*) newStream->priv;
            if (channel != NULL && channel->isUBWCEnabled()) {
                cam_format_t fmt = channel->getStreamDefaultFormat(
                        mStreamConfigInfo[index].type[stream_index],
                        newStream->width, newStream->height, newStream->usage);
                if(fmt == CAM_FORMAT_YUV_420_NV12_UBWC) {
                    newStream->usage |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
                }
            }

            for (List<stream_info_t*>::iterator it=mStreamInfo.begin();
                    it != mStreamInfo.end(); it++) {
                if ((*it)->stream == newStream) {
                    (*it)->channel = (QCamera3ProcessingChannel*) newStream->priv;
                    break;
                }
            }
        } else {
            // Channel already exists for this stream
            // Do nothing for now
        }
        padding_info = gCamCapability[mCameraId]->padding_info;

        if(is_logical_stream)
        {
            //since logical with composit handle will always be added in main index,
            // so making a copy for aux.
            int aux_num_stream = mStreamConfigInfo[CONFIG_INDEX_AUX].num_streams;
            int main_num_stream = mStreamConfigInfo[CONFIG_INDEX_MAIN].num_streams;
            int aux_idx = CONFIG_INDEX_AUX;
            int main_idx = CONFIG_INDEX_MAIN;
            mStreamConfigInfo[aux_idx].stream_sizes[aux_num_stream] =
                    mStreamConfigInfo[main_idx].stream_sizes[main_num_stream];
            mStreamConfigInfo[aux_idx].type[aux_num_stream] =
                    mStreamConfigInfo[main_idx].type[main_num_stream];
            mStreamConfigInfo[aux_idx].postprocess_mask[aux_num_stream] =
                    mStreamConfigInfo[main_idx].postprocess_mask[main_num_stream];
            mStreamConfigInfo[aux_idx].is_type[aux_num_stream] =
                    mStreamConfigInfo[main_idx].is_type[main_num_stream];
            mStreamConfigInfo[aux_idx].format[aux_num_stream] =
                    mStreamConfigInfo[main_idx].format[main_num_stream];
            mStreamConfigInfo[aux_idx].rotation[aux_num_stream] =
                    mStreamConfigInfo[main_idx].rotation[main_num_stream];
                    mStreamConfigInfo[CONFIG_INDEX_AUX].num_streams++;
                    is_aux_configured = true;
        }

        /* Do not add entries for input stream in metastream info
         * since there is no real stream associated with it
         */

        if ((newStream->stream_type != CAMERA3_STREAM_INPUT) && !skip_config)
            stream_index++;

    }

    if(IS_YUV_ZSL && IS_VALID_PTR(mZSLChannel) && IS_VALID_PTR(mPictureChannel))
    {
        QCamera3ProcessingChannel * channel = mZSLChannel;
        mPictureChannel->setSourceZSLChannel(channel, mHALZSL,true);
        mPictureChannel->setZSLMode(true);
        if(isDualCamera() && (num_cb_stream == 2))
        {
            mPictureChannel->setAuxSourceZSLChannel(
                (QCamera3ProcessingChannel *)mZSLChannel->getAuxHandle(),mHALZSL,true);
        }
    }

    if(mHALZSL && IS_VALID_PTR(mZSLChannel))
    {
        mZSLChannel->setZSLStreamType(mHALZSL);
        if(IS_YUV_ZSL)
        {
            LOGI("Enabling YUV ZSL");
        }
    }

    bool onlyRaw = true;
    bool disableSupportStreams = false;
    switch (mOpMode){
    case QCAMERA3_VENDOR_STREAM_CONFIGURATION_RAW_ONLY_MODE:
        // Loop through all streams and check for raw only streams.
        for (size_t i = 0; i < streamList->num_streams; i++) {
            camera3_stream_t *newStream = streamList->streams[i];
            if ((newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL
                || IS_USAGE_ZSL(newStream->usage)) &&
                newStream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED){
                // Non raw stream exists.
                onlyRaw = false;
            }
            else if(newStream->stream_type == CAMERA3_STREAM_INPUT) {
                // Non raw stream exists.
                onlyRaw = false;
            }
            else {
                //for non zsl streams find out the format
                switch (newStream->format) {
                case HAL_PIXEL_FORMAT_RAW_OPAQUE:
                case HAL_PIXEL_FORMAT_RAW16:
                case HAL_PIXEL_FORMAT_RAW10:
                case HAL_PIXEL_FORMAT_RAW8:
                    break;
                default:
                    onlyRaw = false;
                    break;
                }
            }
        }
        break;
    case QCAMERA3_VENDOR_STREAM_CONFIGURATION_DISABLE_SUPPORT_STREAMS:
        disableSupportStreams = true;
        break;
    default:
        onlyRaw = false;
        disableSupportStreams = false;
        break;
    }

    // Only create analysis and callback streams if either the disable flag has
    // been set or if only RAW streams are present.
    bool createAnalysisAndCallbackStreams = true;
    if (onlyRaw || disableSupportStreams || isDepth) {
        createAnalysisAndCallbackStreams = false;
    }
    if (createAnalysisAndCallbackStreams && (mCommon.needAnalysisStream() || isDualCamera())) {
        cam_feature_mask_t analysisFeatureMask = CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
        setPAAFSupport(analysisFeatureMask, CAM_STREAM_TYPE_ANALYSIS,
                gCamCapability[mCameraId]->color_arrangement);
        cam_analysis_info_t analysisInfo;
        int32_t ret = NO_ERROR;
        ret = mCommon.getAnalysisInfo(
                FALSE,
                analysisFeatureMask,
                &analysisInfo);
        if (ret == NO_ERROR) {
            cam_dimension_t analysisDim;
            analysisDim = mCommon.getMatchingDimension(previewSize,
                    analysisInfo.analysis_recommended_res);
            uint32_t camHandle = mCameraHandle->camera_handle;
            uint32_t chHandle = mChannelHandle;
            if (isDualCamera() && !mCommon.needAnalysisStream()) {
                camHandle = get_main_camera_handle(mCameraHandle->camera_handle);
                chHandle = get_main_camera_handle(mChannelHandle);
            }
            mAnalysisChannel = new QCamera3SupportChannel(
                    mCameraHandle->camera_handle,
                    mChannelHandle,
                    mCameraHandle->ops,
                    &analysisInfo.analysis_padding_info,
                    analysisFeatureMask,
                    CAM_STREAM_TYPE_ANALYSIS,
                    &analysisDim,
                    analysisInfo.analysis_format,
                    gCamCapability[mCameraId]->color_arrangement,
                    this,
                    0); // force buffer count to 0
        } else {
            LOGW("getAnalysisInfo failed, ret = %d", ret);
        }
        if (!mAnalysisChannel) {
            LOGW("Analysis channel cannot be created");
        }
    } else {
        // If we need only RAW streams, mark the camera as STANDALONE
        int config_info_index = CONFIG_INDEX_MAIN;
        if(is_main_configured || is_logical_configured)
        {
            config_info_index = CONFIG_INDEX_MAIN;
        } else if(is_aux_configured) {
            config_info_index = CONFIG_INDEX_AUX;
        }

        do {
            mStreamConfigInfo[config_info_index].sync_type = CAM_TYPE_STANDALONE;
            config_info_index++;
        } while(isDualCamera() && is_aux_configured
                && (config_info_index < CONFIG_INDEX_MAX));
    }

    //RAW DUMP channel
    if (mEnableRawDump && isRawStreamRequested == false){
        cam_dimension_t rawDumpSize;
        rawDumpSize = getMaxRawSize(mCameraId);
        cam_feature_mask_t rawDumpFeatureMask = CAM_QCOM_FEATURE_NONE;
        setPAAFSupport(rawDumpFeatureMask,
                CAM_STREAM_TYPE_RAW,
                gCamCapability[mCameraId]->color_arrangement);
        mRawDumpChannel = new QCamera3RawDumpChannel(mCameraHandle->camera_handle,
                                  mChannelHandle,
                                  mCameraHandle->ops,
                                  rawDumpSize,
                                  &padding_info,
                                  this, rawDumpFeatureMask);
        if (!mRawDumpChannel) {
            LOGE("Raw Dump channel cannot be created");
            pthread_mutex_unlock(&mMutex);
            return -ENOMEM;
        }
    }


    if (mAnalysisChannel) {
        int index = CONFIG_INDEX_MAIN;
        if(is_main_configured || is_logical_configured)
        {
            index = CONFIG_INDEX_MAIN;
        } else if(is_aux_configured) {
            index = CONFIG_INDEX_AUX;
        }

        do
        {
            uint32_t &stream_index = mStreamConfigInfo[index].num_streams;
            cam_analysis_info_t analysisInfo;
            memset(&analysisInfo, 0, sizeof(cam_analysis_info_t));
            mStreamConfigInfo[index].type[stream_index] =
                    CAM_STREAM_TYPE_ANALYSIS;
            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                    CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
            setPAAFSupport(mStreamConfigInfo[index].postprocess_mask[stream_index],
                    mStreamConfigInfo[index].type[stream_index],
                    gCamCapability[mCameraId]->color_arrangement);
            rc = mCommon.getAnalysisInfo(FALSE,
                    mStreamConfigInfo[index].postprocess_mask[stream_index],
                    &analysisInfo);
            if (rc != NO_ERROR) {
                LOGE("getAnalysisInfo failed, ret = %d", rc);
                pthread_mutex_unlock(&mMutex);
                return rc;
            }
            mStreamConfigInfo[index].stream_sizes[stream_index] =
                    mCommon.getMatchingDimension(previewSize,
                    analysisInfo.analysis_recommended_res);
            stream_index++;
            index++;
        }while(isDualCamera() && is_aux_configured && (index < CONFIG_INDEX_MAX));
    }

    if (createAnalysisAndCallbackStreams &&
        isSupportChannelNeeded(streamList, mStreamConfigInfo[0])) {
        cam_analysis_info_t supportInfo;
        memset(&supportInfo, 0, sizeof(cam_analysis_info_t));
        cam_feature_mask_t callbackFeatureMask = CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
        setPAAFSupport(callbackFeatureMask,
                CAM_STREAM_TYPE_CALLBACK,
                gCamCapability[mCameraId]->color_arrangement);
        int32_t ret = NO_ERROR;
        ret = mCommon.getAnalysisInfo(FALSE, callbackFeatureMask, &supportInfo);
        if (ret != NO_ERROR) {
            /* Ignore the error for Mono camera
             * because the PAAF bit mask is only set
             * for CAM_STREAM_TYPE_ANALYSIS stream type
             */
            if (gCamCapability[mCameraId]->color_arrangement != CAM_FILTER_ARRANGEMENT_Y) {
                LOGW("getAnalysisInfo failed, ret = %d", ret);
            }
        }
        mSupportChannel = new QCamera3SupportChannel(
                mCameraHandle->camera_handle,
                mChannelHandle,
                mCameraHandle->ops,
                &gCamCapability[mCameraId]->padding_info,
                callbackFeatureMask,
                CAM_STREAM_TYPE_CALLBACK,
                &QCamera3SupportChannel::kDim,
                CAM_FORMAT_YUV_420_NV21,
                gCamCapability[mCameraId]->color_arrangement,
                this, 0);
        if (!mSupportChannel) {
            LOGE("dummy channel cannot be created");
            pthread_mutex_unlock(&mMutex);
            return -ENOMEM;
        }
    }

    if (mSupportChannel) {
        int index = CONFIG_INDEX_MAIN;
        if(is_main_configured || is_logical_configured)
        {
            index = CONFIG_INDEX_MAIN;
        } else if(is_aux_configured) {
            index = CONFIG_INDEX_AUX;
        }
        do {
            uint32_t &stream_index = mStreamConfigInfo[index].num_streams;
            mStreamConfigInfo[index].stream_sizes[stream_index] =
                    QCamera3SupportChannel::kDim;
            mStreamConfigInfo[index].type[stream_index] =
                    CAM_STREAM_TYPE_CALLBACK;
            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                    CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
            setPAAFSupport(mStreamConfigInfo[index].postprocess_mask[stream_index],
                    mStreamConfigInfo[index].type[stream_index],
                    gCamCapability[mCameraId]->color_arrangement);
            stream_index++;
            index++;
        }while(isDualCamera() && is_aux_configured && (index < CONFIG_INDEX_MAX));
    }

    if (mRawDumpChannel) {
        int index = CONFIG_INDEX_MAIN;
        if(is_main_configured || is_logical_configured)
        {
            index = CONFIG_INDEX_MAIN;
        } else if(is_aux_configured) {
            index = CONFIG_INDEX_AUX;
        }
        do {
            uint32_t &stream_index = mStreamConfigInfo[index].num_streams;
            cam_dimension_t rawSize;
            rawSize = getMaxRawSize(mCameraId);
            mStreamConfigInfo[index].stream_sizes[stream_index] =
                    rawSize;
            mStreamConfigInfo[index].type[stream_index] =
                    CAM_STREAM_TYPE_RAW;
            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                    CAM_QCOM_FEATURE_NONE;
            setPAAFSupport(mStreamConfigInfo[index].postprocess_mask[stream_index],
                    mStreamConfigInfo[index].type[stream_index],
                    gCamCapability[mCameraId]->color_arrangement);
            stream_index++;
            index++;
        }while(isDualCamera() && is_aux_configured && (index < CONFIG_INDEX_MAX));
    }
    /* In HFR mode, if video stream is not added, create a dummy channel so that
     * ISP can create a batch mode even for preview only case. This channel is
     * never 'start'ed (no stream-on), it is only 'initialized'  */
    if ((mOpMode == CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE) &&
            !m_bIsVideo) {
        cam_feature_mask_t dummyFeatureMask = CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
        setPAAFSupport(dummyFeatureMask,
                CAM_STREAM_TYPE_VIDEO,
                gCamCapability[mCameraId]->color_arrangement);
        mDummyBatchChannel = new QCamera3RegularChannel(mCameraHandle->camera_handle,
                mChannelHandle,
                mCameraHandle->ops, captureResultCb,
                setBufferErrorStatus, &gCamCapability[mCameraId]->padding_info,
                this,
                &mDummyBatchStream,
                CAM_STREAM_TYPE_VIDEO,
                dummyFeatureMask,
                mMetadataChannel);
        if (NULL == mDummyBatchChannel) {
            LOGE("creation of mDummyBatchChannel failed."
                    "Preview will use non-hfr sensor mode ");
        }
    }

    if (mDummyBatchChannel) {
        int index = CONFIG_INDEX_MAIN;
        if(is_main_configured || is_logical_configured)
        {
            index = CONFIG_INDEX_MAIN;
        } else if(is_aux_configured) {
            index = CONFIG_INDEX_AUX;
        }

       do {
           uint32_t &stream_index = mStreamConfigInfo[index].num_streams;
            mStreamConfigInfo[index].stream_sizes[stream_index].width =
                    mDummyBatchStream.width;
            mStreamConfigInfo[index].stream_sizes[stream_index].height =
                    mDummyBatchStream.height;
            mStreamConfigInfo[index].type[stream_index] =
                    CAM_STREAM_TYPE_VIDEO;
            mStreamConfigInfo[index].postprocess_mask[stream_index] =
                    CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
            setPAAFSupport(mStreamConfigInfo[index].postprocess_mask[stream_index],
                    mStreamConfigInfo[index].type[stream_index],
                    gCamCapability[mCameraId]->color_arrangement);
            stream_index++;
            index++;
        }while (isDualCamera() && is_aux_configured && (index < CONFIG_INDEX_MAX));
    }

    /* Initialize mPendingRequestInfo and mPendingBuffersMap */
    for (pendingRequestIterator i = mPendingRequestsList.begin();
            i != mPendingRequestsList.end();) {
        i = erasePendingRequest(i);
    }
    mPendingFrameDropList.clear();
    // Initialize/Reset the pending buffers list
    for (auto &req : mPendingBuffersMap.mPendingBuffersInRequest) {
        req.mPendingBufferList.clear();
    }
    mPendingBuffersMap.mPendingBuffersInRequest.clear();

    mPendingReprocessResultList.clear();

    mCurJpegMeta.clear();
    //Get min frame duration for this streams configuration
    deriveMinFrameDuration();

    if ((rc == NO_ERROR) && isDualCamera()) {
        bool syncCams = true;
        if (DUALCAM_SYNC_MECHANISM == CAM_SYNC_NO_SYNC) {
            syncCams = false;
        }
        bundleRelatedCameras(syncCams);
        ADD_GET_PARAM_ENTRY_TO_BATCH(mParameters,
            CAM_INTF_PARM_RELATED_SENSORS_CALIBRATION);
        rc = mCameraHandle->ops->get_parms(mCameraHandle->camera_handle,
                mParameters);
        if (rc != NO_ERROR) {
            LOGE("Failed to get CAM_INTF_PARM_RAW_DIMENSION");
            return rc;
        }
    }

    // Update state
    mState = CONFIGURED;

    pthread_mutex_unlock(&mMutex);

    return rc;
}

/*===========================================================================
 * FUNCTION   : validateCaptureRequest
 *
 * DESCRIPTION: validate a capture request from camera service
 *
 * PARAMETERS :
 *   @request : request from framework to process
 *
 * RETURN     :
 *
 *==========================================================================*/
int QCamera3HardwareInterface::validateCaptureRequest(
                    camera3_capture_request_t *request,
                    List<InternalRequest> &internallyRequestedStreams)
{
    ssize_t idx = 0;
    const camera3_stream_buffer_t *b;
    CameraMetadata meta;

    /* Sanity check the request */
    if (request == NULL) {
        LOGE("NULL capture request");
        return BAD_VALUE;
    }

    if ((request->settings == NULL) && (mState == CONFIGURED)) {
        /*settings cannot be null for the first request*/
        return BAD_VALUE;
    }

    uint32_t frameNumber = request->frame_number;
    if ((request->num_output_buffers < 1 || request->output_buffers == NULL)
            && (internallyRequestedStreams.size() == 0)) {
        LOGE("Request %d: No output buffers provided!",
                __FUNCTION__, frameNumber);
        return BAD_VALUE;
    }
    if (request->num_output_buffers >= MAX_NUM_STREAMS) {
        LOGE("Number of buffers %d equals or is greater than maximum number of streams!",
                 request->num_output_buffers, MAX_NUM_STREAMS);
        return BAD_VALUE;
    }
    if (request->input_buffer != NULL) {
        b = request->input_buffer;
        if (b->status != CAMERA3_BUFFER_STATUS_OK) {
            LOGE("Request %d: Buffer %ld: Status not OK!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        if (b->release_fence != -1) {
            LOGE("Request %d: Buffer %ld: Has a release fence!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        if (b->buffer == NULL) {
            LOGE("Request %d: Buffer %ld: NULL buffer handle!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
    }

    if(IS_MULTI_CAMERA)
    {
        if(request->num_physcam_settings > 0
              && ((request->physcam_id == NULL)
              || (request->physcam_settings == NULL)))
        {
            LOGE("Requested physcial settings or id is NULL");
            return BAD_VALUE;
        }
        uint32_t phy_settings = request->num_physcam_settings;
        Vector<uint32_t> phyIds = getPhyIdListForCameraId(mCameraId);
        for(uint32_t i = 0; i < phy_settings; i++)
        {
            if((request->physcam_id[i] != NULL )
                && (request->physcam_settings[i] != NULL))
            {
                bool found = false;
                if(request->physcam_id[i][0] == '\0')
                {
                    LOGE("invalid physical id: NULL");
                    return BAD_VALUE;
                }
                uint32_t id = atoi(request->physcam_id[i]);
                for(auto it = phyIds.begin(); it != phyIds.end(); it++)
                {
                   if(id == *it)
                   {
                       found = true;
                       break;
                   }
                }
                if(!found)
                {
                    LOGE("Invalid phys id in request");
                    return BAD_VALUE;
                }
            } else {
                LOGE("NULL pointer for physcam id %d", i);
                return BAD_VALUE;
            }
        }
    }

    // Validate all buffers
    b = request->output_buffers;
    if (b == NULL) {
       return BAD_VALUE;
    }
    while (idx < (ssize_t)request->num_output_buffers) {
        QCamera3ProcessingChannel *channel =
                static_cast<QCamera3ProcessingChannel*>(b->stream->priv);
        if (channel == NULL) {
            LOGE("Request %d: Buffer %ld: Unconfigured stream!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        if (b->status != CAMERA3_BUFFER_STATUS_OK) {
            LOGE("Request %d: Buffer %ld: Status not OK!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        if (b->release_fence != -1) {
            LOGE("Request %d: Buffer %ld: Has a release fence!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        if (b->buffer == NULL) {
            LOGE("Request %d: Buffer %ld: NULL buffer handle!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        if (*(b->buffer) == NULL) {
            LOGE("Request %d: Buffer %ld: NULL private handle!",
                     frameNumber, (long)idx);
            return BAD_VALUE;
        }
        idx++;
        b = request->output_buffers + idx;
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : deriveMinFrameDuration
 *
 * DESCRIPTION: derive mininum processed, jpeg, and raw frame durations based
 *              on currently configured streams.
 *
 * PARAMETERS : NONE
 *
 * RETURN     : NONE
 *
 *==========================================================================*/
void QCamera3HardwareInterface::deriveMinFrameDuration()
{
    int32_t maxJpegDim, maxProcessedDim, maxRawDim;

    maxJpegDim = 0;
    maxProcessedDim = 0;
    maxRawDim = 0;

    // Figure out maximum jpeg, processed, and raw dimensions
    for (List<stream_info_t*>::iterator it = mStreamInfo.begin();
        it != mStreamInfo.end(); it++) {

        // Input stream doesn't have valid stream_type
        if ((*it)->stream->stream_type == CAMERA3_STREAM_INPUT)
            continue;

        int32_t dimension = (int32_t)((*it)->stream->width * (*it)->stream->height);
        if ((*it)->stream->format == HAL_PIXEL_FORMAT_BLOB &&
                (*it)->stream->data_space != HAL_DATASPACE_DEPTH) {
            if (dimension > maxJpegDim)
                maxJpegDim = dimension;
        } else if ((*it)->stream->format == HAL_PIXEL_FORMAT_RAW_OPAQUE ||
                (*it)->stream->format == HAL_PIXEL_FORMAT_RAW10 ||
                (*it)->stream->format == HAL_PIXEL_FORMAT_RAW16 ||
                (*it)->stream->format == HAL_PIXEL_FORMAT_RAW8  ||
                (*it)->stream->data_space == HAL_DATASPACE_DEPTH) {
            if (dimension > maxRawDim)
                maxRawDim = dimension;
        } else {
            if (dimension > maxProcessedDim)
                maxProcessedDim = dimension;
        }
    }

    size_t count = MIN(gCamCapability[mCameraId]->supported_raw_dim_cnt,
            MAX_SIZES_CNT);

    //Assume all jpeg dimensions are in processed dimensions.
    if (maxJpegDim > maxProcessedDim)
        maxProcessedDim = maxJpegDim;
    //Find the smallest raw dimension that is greater or equal to jpeg dimension
    if (maxProcessedDim > maxRawDim) {
        maxRawDim = INT32_MAX;

        for (size_t i = 0; i < count; i++) {
            int32_t dimension = gCamCapability[mCameraId]->raw_dim[i].width *
                    gCamCapability[mCameraId]->raw_dim[i].height;
            if (dimension >= maxProcessedDim && dimension < maxRawDim)
                maxRawDim = dimension;
        }
    }

    //Find minimum durations for processed, jpeg, and raw
    for (size_t i = 0; i < count; i++) {
        if (maxRawDim == gCamCapability[mCameraId]->raw_dim[i].width *
                gCamCapability[mCameraId]->raw_dim[i].height) {
            mMinRawFrameDuration = gCamCapability[mCameraId]->raw_min_duration[i];
            break;
        }
    }
    count = MIN(gCamCapability[mCameraId]->picture_sizes_tbl_cnt, MAX_SIZES_CNT);
    for (size_t i = 0; i < count; i++) {
        if (maxProcessedDim ==
                gCamCapability[mCameraId]->picture_sizes_tbl[i].width *
                gCamCapability[mCameraId]->picture_sizes_tbl[i].height) {
            mMinProcessedFrameDuration = gCamCapability[mCameraId]->picture_min_duration[i];
            mMinJpegFrameDuration = gCamCapability[mCameraId]->picture_min_duration[i];
            break;
        }
    }
}

/*===========================================================================
 * FUNCTION   : getMinFrameDuration
 *
 * DESCRIPTION: get minimum frame draution based on the current maximum frame durations
 *              and current request configuration.
 *
 * PARAMETERS : @request: requset sent by the frameworks
 *
 * RETURN     : min farme duration for a particular request
 *
 *==========================================================================*/
int64_t QCamera3HardwareInterface::getMinFrameDuration(const camera3_capture_request_t *request)
{
    bool hasJpegStream = false;
    bool hasRawStream = false;
    int64_t mMinFrameDuration = mMinProcessedFrameDuration;
    for (uint32_t i = 0; i < request->num_output_buffers; i ++) {
        const camera3_stream_t *stream = request->output_buffers[i].stream;
        if (stream->format == HAL_PIXEL_FORMAT_BLOB && stream->data_space != HAL_DATASPACE_DEPTH)
            hasJpegStream = true;
        else if (stream->format == HAL_PIXEL_FORMAT_RAW_OPAQUE ||
                stream->format == HAL_PIXEL_FORMAT_RAW8 ||
                stream->format == HAL_PIXEL_FORMAT_RAW10 ||
                stream->format == HAL_PIXEL_FORMAT_RAW16 ||
                stream->data_space == HAL_DATASPACE_DEPTH)
            hasRawStream = true;
    }

    if (hasRawStream)
        mMinFrameDuration = MAX(mMinRawFrameDuration, mMinFrameDuration);
    if (hasJpegStream)
        mMinFrameDuration = MAX(mMinJpegFrameDuration, mMinFrameDuration);
    return mMinFrameDuration;
}

/*===========================================================================
 * FUNCTION   : handleBuffersDuringFlushLock
 *
 * DESCRIPTION: Account for buffers returned from back-end during flush
 *              This function is executed while mMutex is held by the caller.
 *
 * PARAMETERS :
 *   @buffer: image buffer for the callback
 *
 * RETURN     :
 *==========================================================================*/
void QCamera3HardwareInterface::handleBuffersDuringFlushLock(camera3_stream_buffer_t *buffer)
{
    bool buffer_found = false;
    for (List<PendingBuffersInRequest>::iterator req =
            mPendingBuffersMap.mPendingBuffersInRequest.begin();
            req != mPendingBuffersMap.mPendingBuffersInRequest.end(); req++) {
        for (List<PendingBufferInfo>::iterator i =
                req->mPendingBufferList.begin();
                i != req->mPendingBufferList.end(); i++) {
            if (i->buffer == buffer->buffer) {
                mPendingBuffersMap.numPendingBufsAtFlush--;
                LOGD("Found buffer %p for Frame %d, numPendingBufsAtFlush = %d",
                    buffer->buffer, req->frame_number,
                    mPendingBuffersMap.numPendingBufsAtFlush);
                buffer_found = true;
                break;
            }
        }
        if (buffer_found) {
            break;
        }
    }
    if (mPendingBuffersMap.numPendingBufsAtFlush == 0) {
        //signal the flush()
        LOGD("All buffers returned to HAL. Continue flush");
        pthread_cond_signal(&mBuffersCond);
    }
}


/*===========================================================================
 * FUNCTION   : handlePendingReprocResults
 *
 * DESCRIPTION: check and notify on any pending reprocess results
 *
 * PARAMETERS :
 *   @frame_number   : Pending request frame number
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::handlePendingReprocResults(uint32_t frame_number)
{
    for (List<PendingReprocessResult>::iterator j = mPendingReprocessResultList.begin();
            j != mPendingReprocessResultList.end(); j++) {
        if (j->frame_number == frame_number) {
            orchestrateNotify(&j->notify_msg);

            LOGD("Delayed reprocess notify %d",
                    frame_number);

            for (pendingRequestIterator k = mPendingRequestsList.begin();
                    k != mPendingRequestsList.end(); k++) {

                if (k->frame_number == j->frame_number) {
                    LOGD("Found reprocess frame number %d in pending reprocess List "
                            "Take it out!!",
                            k->frame_number);

                    camera3_capture_result result;
                    memset(&result, 0, sizeof(camera3_capture_result));
                    result.frame_number = frame_number;
                    result.num_output_buffers = 1;
                    result.output_buffers =  &j->buffer;
                    result.input_buffer = k->input_buffer;
                    result.result = k->settings;
                    result.partial_result = PARTIAL_RESULT_COUNT;
                    orchestrateResult(&result);

                    erasePendingRequest(k);
                    break;
                }
            }
            mPendingReprocessResultList.erase(j);
            break;
        }
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : checkFrameInPendingList
 *
 * DESCRIPTION: Check for the frame_number present in pending request list or not.
 *
 * PARAMETERS : @frame_number: frame_number
 *
 * RETURN     :@bool: true if frame present in list else false.
 *
 *==========================================================================*/
bool QCamera3HardwareInterface::checkFrameInPendingList(
        const uint32_t frame_number)
{
    bool ret = false;
    for(auto itr = mPendingRequestsList.begin(); itr != mPendingRequestsList.end(); itr++)
    {
        if(frame_number == itr->frame_number)
        {
            ret = true;
            break;
        }
    }
    return ret;
}

/*===========================================================================
 * FUNCTION   : handleBatchMetadata
 *
 * DESCRIPTION: Handles metadata buffer callback in batch mode
 *
 * PARAMETERS : @metadata_buf: metadata buffer
 *              @free_and_bufdone_meta_buf: Buf done on the meta buf and free
 *                 the meta buf in this method
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::handleBatchMetadata(
        mm_camera_super_buf_t *metadata_buf, bool free_and_bufdone_meta_buf)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_HANDLE_BATCH_METADATA);

    if (NULL == metadata_buf) {
        LOGE("metadata_buf is NULL");
        return;
    }
    /* In batch mode, the metdata will contain the frame number and timestamp of
     * the last frame in the batch. Eg: a batch containing buffers from request
     * 5,6,7 and 8 will have frame number and timestamp corresponding to 8.
     * multiple process_capture_requests => 1 set_param => 1 handleBatchMetata =>
     * multiple process_capture_results */
    metadata_buffer_t *metadata =
            (metadata_buffer_t *)metadata_buf->bufs[0]->buffer;
    int32_t frame_number_valid = 0, urgent_frame_number_valid = 0;
    uint32_t last_frame_number = 0, last_urgent_frame_number = 0;
    uint32_t first_frame_number = 0, first_urgent_frame_number = 0;
    uint32_t frame_number = 0, urgent_frame_number = 0;
    int64_t last_frame_capture_time = 0, first_frame_capture_time, capture_time;
    bool invalid_metadata = false;
    size_t urgentFrameNumDiff = 0, frameNumDiff = 0;
    size_t loopCount = 1;
    bool is_metabuf_queued = false;

    int32_t *p_frame_number_valid =
            POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER_VALID, metadata);
    uint32_t *p_frame_number =
            POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER, metadata);
    int64_t *p_capture_time =
            POINTER_OF_META(CAM_INTF_META_SENSOR_TIMESTAMP, metadata);
    int32_t *p_urgent_frame_number_valid =
            POINTER_OF_META(CAM_INTF_META_URGENT_FRAME_NUMBER_VALID, metadata);
    uint32_t *p_urgent_frame_number =
            POINTER_OF_META(CAM_INTF_META_URGENT_FRAME_NUMBER, metadata);

    if ((NULL == p_frame_number_valid) || (NULL == p_frame_number) ||
            (NULL == p_capture_time) || (NULL == p_urgent_frame_number_valid) ||
            (NULL == p_urgent_frame_number)) {
        LOGE("Invalid metadata");
        invalid_metadata = true;
    } else {
        frame_number_valid = *p_frame_number_valid;
        last_frame_number = *p_frame_number;
        last_frame_capture_time = *p_capture_time;
        urgent_frame_number_valid = *p_urgent_frame_number_valid;
        last_urgent_frame_number = *p_urgent_frame_number;
    }

    /* In batchmode, when no video buffers are requested, set_parms are sent
     * for every capture_request. The difference between consecutive urgent
     * frame numbers and frame numbers should be used to interpolate the
     * corresponding frame numbers and time stamps */
    pthread_mutex_lock(&mMutex);
    if (urgent_frame_number_valid) {
        ssize_t idx = mPendingBatchMap.indexOfKey(last_urgent_frame_number);
        if(idx < 0) {
            LOGE("Invalid urgent frame number received: %d.",
                last_urgent_frame_number);
            if(checkFrameInPendingList(last_urgent_frame_number))
            {
                LOGE("Irrecoverable Error: frame not batched");
                mState = ERROR;
            }
            pthread_mutex_unlock(&mMutex);
            goto BUFFER_NOT_BATCHED;
        }
        first_urgent_frame_number = mPendingBatchMap.valueAt(idx);
        urgentFrameNumDiff = last_urgent_frame_number + 1 -
                first_urgent_frame_number;

        LOGD("urgent_frm: valid: %d frm_num: %d - %d",
                 urgent_frame_number_valid,
                first_urgent_frame_number, last_urgent_frame_number);
    }

    if (frame_number_valid) {
        ssize_t idx = mPendingBatchMap.indexOfKey(last_frame_number);
        if(idx < 0) {
            LOGE("Invalid frame number received: %d.",
                last_frame_number);
            if(checkFrameInPendingList(last_frame_number))
            {
                LOGE("Irrecoverable Error: frame not batched");
                mState = ERROR;
            }
            pthread_mutex_unlock(&mMutex);
            goto BUFFER_NOT_BATCHED;
        }
        first_frame_number = mPendingBatchMap.valueAt(idx);
        frameNumDiff = last_frame_number + 1 -
                first_frame_number;
        mPendingBatchMap.removeItem(last_frame_number);

        LOGD("frm: valid: %d frm_num: %d - %d",
                 frame_number_valid,
                first_frame_number, last_frame_number);

    }
    pthread_mutex_unlock(&mMutex);

    if (urgent_frame_number_valid || frame_number_valid) {
        loopCount = MAX(urgentFrameNumDiff, frameNumDiff);
        if (urgentFrameNumDiff > MAX_HFR_BATCH_SIZE)
            LOGE("urgentFrameNumDiff: %d urgentFrameNum: %d",
                     urgentFrameNumDiff, last_urgent_frame_number);
        if (frameNumDiff > MAX_HFR_BATCH_SIZE)
            LOGE("frameNumDiff: %d frameNum: %d",
                     frameNumDiff, last_frame_number);
    }

    for (size_t i = 0; i < loopCount; i++) {
        /* handleMetadataWithLock is called even for invalid_metadata for
         * pipeline depth calculation */
        if (!invalid_metadata) {
            /* Infer frame number. Batch metadata contains frame number of the
             * last frame */
            if (urgent_frame_number_valid) {
                if (i < urgentFrameNumDiff) {
                    urgent_frame_number =
                            first_urgent_frame_number + i;
                    LOGD("inferred urgent frame_number: %d",
                             urgent_frame_number);
                    ADD_SET_PARAM_ENTRY_TO_BATCH(metadata,
                            CAM_INTF_META_URGENT_FRAME_NUMBER, urgent_frame_number);
                } else {
                    /* This is to handle when urgentFrameNumDiff < frameNumDiff */
                    ADD_SET_PARAM_ENTRY_TO_BATCH(metadata,
                            CAM_INTF_META_URGENT_FRAME_NUMBER_VALID, 0);
                }
            }

            /* Infer frame number. Batch metadata contains frame number of the
             * last frame */
            if (frame_number_valid) {
                if (i < frameNumDiff) {
                    frame_number = first_frame_number + i;
                    LOGD("inferred frame_number: %d", frame_number);
                    ADD_SET_PARAM_ENTRY_TO_BATCH(metadata,
                            CAM_INTF_META_FRAME_NUMBER, frame_number);
                } else {
                    /* This is to handle when urgentFrameNumDiff > frameNumDiff */
                    ADD_SET_PARAM_ENTRY_TO_BATCH(metadata,
                             CAM_INTF_META_FRAME_NUMBER_VALID, 0);
                }
            }

            if (last_frame_capture_time) {
                //Infer timestamp
                first_frame_capture_time = last_frame_capture_time -
                        (((loopCount - 1) * NSEC_PER_SEC) / (double) mHFRVideoFps);
                capture_time =
                        first_frame_capture_time + (i * NSEC_PER_SEC / (double) mHFRVideoFps);
                ADD_SET_PARAM_ENTRY_TO_BATCH(metadata,
                        CAM_INTF_META_SENSOR_TIMESTAMP, capture_time);
                LOGD("batch capture_time: %lld, capture_time: %lld",
                         last_frame_capture_time, capture_time);
            }
        }
        pthread_mutex_lock(&mMutex);
        handleMetadataWithLock(metadata_buf,
                false /* free_and_bufdone_meta_buf */,
                (i == 0) /* first metadata in the batch metadata */,
                &is_metabuf_queued /* if metabuf isqueued or not */);
        pthread_mutex_unlock(&mMutex);
    }

BUFFER_NOT_BATCHED:
    /* BufDone metadata buffer */
    if (free_and_bufdone_meta_buf && !is_metabuf_queued) {
        mMetadataChannel->bufDone(metadata_buf);
        free(metadata_buf);
        metadata_buf = NULL;
    }
}

void QCamera3HardwareInterface::notifyError(uint32_t frameNumber,
        camera3_error_msg_code_t errorCode)
{
    camera3_notify_msg_t notify_msg;
    memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
    notify_msg.type = CAMERA3_MSG_ERROR;
    notify_msg.message.error.error_code = errorCode;
    notify_msg.message.error.error_stream = NULL;
    notify_msg.message.error.frame_number = frameNumber;
    orchestrateNotify(&notify_msg);

    return;
}

bool QCamera3HardwareInterface::cacheMetaIfNeeded(
         mm_camera_super_buf_t *metadata_buf, uint32_t frame_number)
{
    bool cached = false;
    for (pendingRequestIterator i = mPendingRequestsList.begin();
            i != mPendingRequestsList.end(); i++) {

            if(!(i->frame_number == frame_number)) continue;

            if(i->received_main_meta && i->received_aux_meta)
            {
                LOGE("already received metadata for main and aux...");
                break;
            }

            if ((metadata_buf->camera_handle ==
                    get_main_camera_handle(mCameraHandle->camera_handle))
                    && (i->main_meta == NULL))
            {
                i->main_meta = metadata_buf;
                i->received_main_meta = true;
                cached = true;
                LOGI("cached main meta for frame_numer %d", frame_number);
                break;
            }

            if((metadata_buf->camera_handle ==
                     get_aux_camera_handle(mCameraHandle->camera_handle))
                     && (i->aux_meta == NULL))
            {
                i->aux_meta = metadata_buf;
                i->received_aux_meta = true;
                cached = true;
                LOGI("cached aux meta for frame_number %d",frame_number);
                break;
            }
    }
    return cached;
}

bool QCamera3HardwareInterface::isTotalMetaReceivedForFrame(
        __unused uint32_t frame_number, PendingRequestInfo *request)
{
    bool ret = true;

    if(request->requested_logical || request->requested_on_main)
    {
            ret &= request->received_main_meta;
    }

    if(request->requested_on_aux)
    {
        ret &= request->received_aux_meta;
    }

    return ret;
}

char *QCamera3HardwareInterface::getUINT8Ptr(uint32_t cameraId)
{
    char *id = (char *)malloc(sizeof(char)*2);
    if(id == NULL)
    {
        return NULL;
    }

    memset(id, 0, sizeof(char)*2);
    id[0] = cameraId + '0';
    return id;
}

void QCamera3HardwareInterface::releasePhysicalId(const char **physicalId, uint32_t size)
{
    if(physicalId == NULL)
        return;

    for(;size > 0; size--)
    {
        if(physicalId[size-1] != NULL)
        {
            free((void *)physicalId[size-1]);
            physicalId [size-1]= NULL;
        }

    }

    free((void *)physicalId);
}

void  QCamera3HardwareInterface::allocateAndinitializeMetadata(
        camera3_capture_result *result, PendingRequestInfo *request)
{
    uint32_t l_num_of_meta = 0;

    const camera_metadata_t **physical_meta = NULL;
    const char **physical_id = NULL;
    bool dummyMainMeta = false;
    bool dummyAuxMeta = false;

    if(request->requested_on_main)
    {
        l_num_of_meta++;
        dummyMainMeta = !request->received_main_meta;
    }

    if(request->requested_on_aux)
    {
        l_num_of_meta++;
        dummyAuxMeta = !request->received_aux_meta;
    }

    if(l_num_of_meta){
        physical_meta = (const camera_metadata_t **)malloc(
                                sizeof(camera_metadata_t*)*l_num_of_meta);
        if(NULL == physical_meta)
        {
            LOGE("could not allocate metadata");
        }

        physical_id = (const char **)malloc(sizeof(const char *)*l_num_of_meta);
        if(NULL == physical_id)
        {
            LOGE("could not allocate physical id");
        }


        int index = 0;
        if(request->requested_on_main)
        {
            physical_id[index] = getUINT8Ptr(get_main_camera_idx(mCameraId));
            physical_meta[index] = getPhysicalMeta(request->main_meta, request,
                                                    dummyMainMeta, CAM_TYPE_MAIN);
            index++;
        }

        if(request->requested_on_aux)
        {
            physical_id[index] = getUINT8Ptr(get_aux_camera_idx(mCameraId));
            physical_meta[index] = getPhysicalMeta(request->aux_meta, request,
                                                   dummyAuxMeta, CAM_TYPE_AUX);
        }
    }

    if(result->result == NULL)
    {
        if(request->main_meta != NULL)
        {
            result->result = getPhysicalMeta(request->main_meta, request,
                                              !request->received_main_meta, CAM_TYPE_MAIN);
        } else if(request->aux_meta != NULL)
        {
            result->result = getPhysicalMeta(request->aux_meta, request,
                                              !request->received_aux_meta, CAM_TYPE_AUX);
        } else {
            result->result=getPhysicalMeta(NULL, request, true);
        }
    }

    LOGH("num of physical meta %d", l_num_of_meta);
    result->physcam_metadata = physical_meta;
    result->physcam_ids = physical_id;
    result->num_physcam_metadata = l_num_of_meta;

}

void QCamera3HardwareInterface::releasePhysicalMetadata(
              const camera_metadata_t **meta, uint32_t num_of_meta)
{
    if(meta == NULL)
        return;
    for(uint32_t i = 0; i < num_of_meta; i++)
    {
        free_camera_metadata((camera_metadata_t *)meta[i]);
    }
    free(meta);
}

void QCamera3HardwareInterface::releaseCachedMeta(
       PendingRequestInfo *request, QCamera3Channel *meta_channel)
{
    if(request->main_meta != NULL)
    {
        meta_channel->bufDone(request->main_meta);
        free(request->main_meta);
        request->main_meta = NULL;
    }

    if(request->aux_meta != NULL)
    {
        meta_channel->bufDone(request->aux_meta);
        free(request->aux_meta);
        request->aux_meta = NULL;
    }
}

bool QCamera3HardwareInterface::shouldWaitForFrame(
        PendingRequestInfo *request, mm_camera_super_buf_t *cur_meta,
        uint32_t cur_frame_number, uint32_t &pendingFor)
{
    bool ret = false;

    if(!isDualCamera ()) return ret;

    if(request->frame_number == cur_frame_number)
    {
        if(!isTotalMetaReceivedForFrame(cur_frame_number,request))
        {
            ret = true;
        }
    } else if(request->frame_number < cur_frame_number) {
         if(cur_meta->camera_handle
             == get_main_camera_handle(mCameraHandle->camera_handle))
         {
             ret =request->received_main_meta &&
                 (!request->requested_on_aux  || !request->received_aux_meta);
         } else if(cur_meta->camera_handle
             == get_aux_camera_handle(mCameraHandle->camera_handle))
         {
             ret = request->received_aux_meta
                 && (!(request->requested_logical || request->requested_on_main)
                 || !request->received_main_meta);
         }
    } else {
        LOGH("current frame_number %d is less than request frame_number %d",
                cur_frame_number, request->frame_number);
        ret = true;
    }

if(request->requested_on_main && !request->received_main_meta)
{
    pendingFor = CAM_TYPE_MAIN;
}

if(request->requested_on_aux && !request->received_aux_meta)
{
    pendingFor |= CAM_TYPE_AUX;
}

    return ret;
}

bool QCamera3HardwareInterface::checkIfMetaDropped(PendingRequestInfo *request) {
    bool ret = false;
    if(request->requested_on_main || request->requested_logical)
        ret |=  !request->received_main_meta;

    if(request->requested_on_aux)
        ret |= !request->received_aux_meta;

    return ret;
}

/*===========================================================================
 * FUNCTION   : handleMetadataWithLock
 *
 * DESCRIPTION: Handles metadata buffer callback with mMutex lock held.
 *
 * PARAMETERS : @metadata_buf: metadata buffer
 *              @free_and_bufdone_meta_buf: Buf done on the meta buf and free
 *                 the meta buf in this method
 *              @firstMetadataInBatch: Boolean to indicate whether this is the
 *                  first metadata in a batch. Valid only for batch mode
 *              @p_is_metabuf_queued: Pointer to Boolean to check if metadata
 *                  buffer is enqueued or not.
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::handleMetadataWithLock(
    mm_camera_super_buf_t *metadata_buf, bool free_and_bufdone_meta_buf,
    bool firstMetadataInBatch, bool *p_is_metabuf_queued)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_HANDLE_METADATA_LKD);
    if ((mFlushPerf) || (ERROR == mState) || (DEINIT == mState)) {
        //during flush do not send metadata from this thread
        LOGD("not sending metadata during flush or when mState is error");
        if (free_and_bufdone_meta_buf) {
            mMetadataChannel->bufDone(metadata_buf);
            free(metadata_buf);
        }
        return;
    }

    //not in flush
    metadata_buffer_t *metadata = (metadata_buffer_t *)metadata_buf->bufs[0]->buffer;
    int32_t frame_number_valid, urgent_frame_number_valid;
    uint32_t frame_number, urgent_frame_number;
    int64_t capture_time;
    nsecs_t currentSysTime;
    bool meta_freed = false;

    int32_t *p_frame_number_valid =
            POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER_VALID, metadata);
    uint32_t *p_frame_number = POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER, metadata);
    int64_t *p_capture_time = POINTER_OF_META(CAM_INTF_META_SENSOR_TIMESTAMP, metadata);
    int32_t *p_urgent_frame_number_valid =
            POINTER_OF_META(CAM_INTF_META_URGENT_FRAME_NUMBER_VALID, metadata);
    uint32_t *p_urgent_frame_number =
            POINTER_OF_META(CAM_INTF_META_URGENT_FRAME_NUMBER, metadata);
    IF_META_AVAILABLE(cam_stream_ID_t, p_cam_frame_drop, CAM_INTF_META_FRAME_DROPPED,
            metadata) {
        LOGD("Dropped frame info for frame_number_valid %d, frame_number %d",
                 *p_frame_number_valid, *p_frame_number);
    }

    if ((NULL == p_frame_number_valid) || (NULL == p_frame_number) || (NULL == p_capture_time) ||
            (NULL == p_urgent_frame_number_valid) || (NULL == p_urgent_frame_number)) {
        LOGE("Invalid metadata");
        if (free_and_bufdone_meta_buf) {
            mMetadataChannel->bufDone(metadata_buf);
            free(metadata_buf);
            meta_freed = true;
        }
        goto done_metadata;
    }
    frame_number_valid =        *p_frame_number_valid;
    frame_number =              *p_frame_number;
    capture_time =              *p_capture_time;
    urgent_frame_number_valid = *p_urgent_frame_number_valid;
    urgent_frame_number =       *p_urgent_frame_number;
    currentSysTime =            systemTime(CLOCK_MONOTONIC);

    if (isDualCamera() && !IS_PP_TYPE_NONE) {
        metadata_buffer_t   *pMetaDataMain  = NULL;
        metadata_buffer_t   *pMetaDataAux   = NULL;
        metadata_buffer_t   *resultMetadata = NULL;
        if (metadata_buf->camera_handle ==
                get_main_camera_handle(mCameraHandle->camera_handle)) {
            pMetaDataMain = metadata;
            pMetaDataAux  = NULL;
        } else if (metadata_buf->camera_handle ==
                get_aux_camera_handle(mCameraHandle->camera_handle)) {
            pMetaDataMain = NULL;
            pMetaDataAux  = metadata;
        }
        resultMetadata = m_pFovControl->processResultMetadata(pMetaDataMain, pMetaDataAux);
        if ((frame_number == UINT32_MAX) || (mHALZSL && (resultMetadata == NULL))) {
            mMetadataChannel->bufDone(metadata_buf);
            free(metadata_buf);
            return;
        } else {
            if (pMetaDataAux && frame_number_valid
                && frame_number && this->needHALPP()
                && !mHALZSL) {
                LOGD("found valid metadata for aux %d", frame_number);
                for (auto req = mPendingBuffersMap.mPendingBuffersInRequest.begin();
                          req != mPendingBuffersMap.mPendingBuffersInRequest.end();
                          req++) {
                    if (req->frame_number == frame_number) {
                        for (auto k = req->mPendingBufferList.begin();
                                    k != req->mPendingBufferList.end(); k++ ) {
                            QCamera3PicChannel *channel = (QCamera3PicChannel *) (k->stream->priv);
                            if (k->stream->format == HAL_PIXEL_FORMAT_BLOB) {
                                LOGD("found snapshot stream in channel ");
                                channel->queueReprocMetadata(metadata_buf, frame_number, false);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    // Detect if buffers from any requests are overdue
    for (auto &req : mPendingBuffersMap.mPendingBuffersInRequest) {
        if ( (currentSysTime - req.timestamp) >
                           s2ns(getHalPPType() == CAM_HAL_PP_TYPE_BOKEH ?
                                       MISSING_BOKEH_REQUEST_BUF_TIMEOUT:
                                       MISSING_REQUEST_BUF_TIMEOUT) ) {
            for (auto &missed : req.mPendingBufferList) {
                assert(missed.stream->priv);
                if (missed.stream->priv) {
                    QCamera3Channel *ch = (QCamera3Channel *)(missed.stream->priv);
                    assert(ch->mStreams[0]);
                    if (ch->mStreams[0]) {
                        LOGE("Cancel missing frame = %d, buffer = %p,"
                            "stream type = %d, stream format = %d",
                            req.frame_number, missed.buffer,
                            ch->mStreams[0]->getMyType(), missed.stream->format);
                        /* TODO: timeout for post process */
                        ch->timeoutFrame(req.frame_number);
                    }
                }
            }
        }
    }
    //Partial result on process_capture_result for timestamp
    if (urgent_frame_number_valid) {
        LOGD("valid urgent frame_number = %u, capture_time = %lld",
           urgent_frame_number, capture_time);

        //Recieved an urgent Frame Number, handle it
        //using partial results
        for (pendingRequestIterator i =
                mPendingRequestsList.begin(); i != mPendingRequestsList.end(); i++) {
            LOGD("Iterator Frame = %d urgent frame = %d",
                 i->frame_number, urgent_frame_number);

            if ((!i->input_buffer) && (i->frame_number < urgent_frame_number) &&
                (i->partial_result_cnt == 0)) {
                LOGE("Error: HAL missed urgent metadata for frame number %d",
                         i->frame_number);
            }

            if (i->frame_number == urgent_frame_number &&
                     i->bUrgentReceived == 0) {

                camera3_capture_result_t result;
                memset(&result, 0, sizeof(camera3_capture_result_t));

                i->partial_result_cnt++;
                i->bUrgentReceived = 1;
                // Extract 3A metadata
                result.result =
                    translateCbUrgentMetadataToResultMetadata(metadata);
                // Populate metadata result
                result.frame_number = urgent_frame_number;
                result.num_output_buffers = 0;
                result.output_buffers = NULL;
#ifdef USE_HAL_3_5
                result.num_physcam_metadata = 0;
#endif //USE_HAL_3_5
                result.partial_result = i->partial_result_cnt;

                orchestrateResult(&result);
                LOGD("urgent frame_number = %u, capture_time = %lld",
                      result.frame_number, capture_time);
                if (mResetInstantAEC && mInstantAECSettledFrameNumber == 0) {
                    // Instant AEC settled for this frame.
                    LOGH("instant AEC settled for frame number %d", urgent_frame_number);
                    mInstantAECSettledFrameNumber = urgent_frame_number;
                }
                free_camera_metadata((camera_metadata_t *)result.result);
                break;
            }
        }
    }

    if (!frame_number_valid) {
        LOGD("Not a valid normal frame number, used as SOF only");
        if (free_and_bufdone_meta_buf) {
            mMetadataChannel->bufDone(metadata_buf);
            free(metadata_buf);
            meta_freed = true;
        }
        goto done_metadata;
    }
    LOGH("valid frame_number = %u, capture_time = %lld",
            frame_number, capture_time);

    if(IS_MULTI_CAMERA)
    {
        cacheMetaIfNeeded(metadata_buf, frame_number);
    }

    for (pendingRequestIterator i = mPendingRequestsList.begin();
            i != mPendingRequestsList.end() && i->frame_number <= frame_number;) {
        // Flush out all entries with less or equal frame numbers.

       if(IS_MULTI_CAMERA)
       {
           uint32_t pendingForCamType = CAM_TYPE_STANDALONE;
           if(shouldWaitForFrame(&(*i), metadata_buf, frame_number, pendingForCamType))
           {
               LOGH("pending meta of %x cam type for frame number %d, cur_frame %d",
                                   pendingForCamType, i->frame_number, frame_number);
               if(i->frame_number < frame_number)
               {
                   i++;
                   continue;
               } else  {
                   goto cached_metadata;
               }
           }else {
               LOGD("sending result for pending frame_number %d, cur_frame %d",
                   i->frame_number, frame_number);
           }
       }

        camera3_capture_result_t result;
        memset(&result, 0, sizeof(camera3_capture_result_t));

        LOGD("frame_number in the list is %u", i->frame_number);
        i->partial_result_cnt++;
        result.partial_result = i->partial_result_cnt;

        // Check whether any stream buffer corresponding to this is dropped or not
        // If dropped, then send the ERROR_BUFFER for the corresponding stream
        // OR check if instant AEC is enabled, then need to drop frames untill AEC is settled.
        bool dropFrame = false;
        if (p_cam_frame_drop ||
                (mInstantAEC || i->frame_number < mInstantAECSettledFrameNumber)) {
            /* Clear notify_msg structure */
            camera3_notify_msg_t notify_msg;
            memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
            for (List<RequestedBufferInfo>::iterator j = i->buffers.begin();
                    j != i->buffers.end(); j++) {
                dropFrame = false;
                QCamera3ProcessingChannel *channel = (QCamera3ProcessingChannel *)j->stream->priv;
                uint32_t streamID = channel->getStreamID(channel->getStreamTypeMask());
                if (p_cam_frame_drop) {
                    for (uint32_t k = 0; k < p_cam_frame_drop->num_streams; k++) {
                        if (streamID == p_cam_frame_drop->stream_request[k].streamID) {
                            // Got the stream ID for drop frame.
                            dropFrame = true;
                            break;
                        }
                    }
                } else {
                    // This is instant AEC case.
                    // For instant AEC drop the stream untill AEC is settled.
                    dropFrame = true;
                }
                if (dropFrame && !j->isZSL) {
                    // Send Error notify to frameworks with CAMERA3_MSG_ERROR_BUFFER
                    if (p_cam_frame_drop) {
                        // Treat msg as error for system buffer drops
                        LOGI("Start of reporting error frame#=%u,"
                                 "streamID=%u, mCameraId: %d",
                                 i->frame_number, streamID, mCameraId);
                    } else {
                        // For instant AEC, inform frame drop and frame number
                        LOGH("Start of reporting error frame#=%u for instant AEC, streamID=%u, "
                                "AEC settled frame number = %u mCameraId: %d",
                                i->frame_number, streamID,
                                mInstantAECSettledFrameNumber, mCameraId);
                    }
                    notify_msg.type = CAMERA3_MSG_ERROR;
                    notify_msg.message.error.frame_number = i->frame_number;
                    notify_msg.message.error.error_code = CAMERA3_MSG_ERROR_BUFFER ;
                    notify_msg.message.error.error_stream = j->stream;
                    orchestrateNotify(&notify_msg);
                    if (p_cam_frame_drop) {
                        // Treat msg as error for system buffer drops
                        LOGI("End of reporting error frame#=%u, streamID=%u mCameraId: %d",
                                i->frame_number, streamID, mCameraId);
                    } else {
                        // For instant AEC, inform frame drop and frame number
                        LOGH("End of reporting error frame#=%u"
                                "for instant AEC, streamID=%u, "
                                "AEC settled frame number = %u mCameraId: %d",
                                i->frame_number, streamID,
                                mInstantAECSettledFrameNumber, mCameraId);
                    }
                    PendingFrameDropInfo PendingFrameDrop;
                    PendingFrameDrop.frame_number=i->frame_number;
                    PendingFrameDrop.stream_ID = streamID;
                    // Add the Frame drop info to mPendingFrameDropList
                    mPendingFrameDropList.push_back(PendingFrameDrop);
               }
            }
        }

        // Send empty metadata with already filled buffers for dropped metadata
        // and send valid metadata with already filled buffers for current metadata
        /* we could hit this case when we either
         * 1. have a pending reprocess request or
         * 2. miss a metadata buffer callback */
         if(IS_MULTI_CAMERA && checkIfMetaDropped(&(*i))) {
            allocateAndinitializeMetadata(&result, &(*i));
            notifyError(i->frame_number, CAMERA3_MSG_ERROR_RESULT);
         }else if (!IS_MULTI_CAMERA && (i->frame_number < frame_number)) {
            if (i->input_buffer) {
                /* this will be handled in handleInputBufferWithLock */
                i++;
                continue;
            } else {
                if (i->internalRequestList.size() == 0) {
                    mPendingLiveRequest--;
                }
                CameraMetadata dummyMetadata;
                dummyMetadata.update(ANDROID_REQUEST_ID, &(i->request_id), 1);
                result.result = dummyMetadata.release();

                notifyError(i->frame_number, CAMERA3_MSG_ERROR_RESULT);
            }
        } else {
            if (i->internalRequestList.size() == 0) {
                mPendingLiveRequest--;
            }
            /* Clear notify_msg structure */
            camera3_notify_msg_t notify_msg;
            memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));

            // Send shutter notify to frameworks
            notify_msg.type = CAMERA3_MSG_SHUTTER;
            notify_msg.message.shutter.frame_number = i->frame_number;
            notify_msg.message.shutter.timestamp = (uint64_t)capture_time;
            orchestrateNotify(&notify_msg);

            i->timestamp = capture_time;

            /* Set the timestamp in display metadata so that clients aware of
               private_handle such as VT can use this un-modified timestamps.
               Camera framework is unaware of this timestamp and cannot change this */
            updateTimeStampInPendingBuffers(i->frame_number, i->timestamp);

            // Find channel requiring metadata, meaning internal offline postprocess
            // is needed.
            //TODO: for now, we don't support two streams requiring metadata at the same time.
            // (because we are not making copies, and metadata buffer is not reference counted.
            bool internalPproc = false;
            if (!IS_SNAP_ZSL) {
                for (pendingBufferIterator iter = i->buffers.begin();
                        iter != i->buffers.end(); iter++) {
                    if (iter->need_metadata) {
                        internalPproc = true;
                        QCamera3ProcessingChannel *channel =
                                (QCamera3ProcessingChannel *)iter->stream->priv;
                        channel->queueReprocMetadata(metadata_buf, i->frame_number, dropFrame);
                        if(p_is_metabuf_queued != NULL) {
                            *p_is_metabuf_queued = true;
                        }
                        meta_freed = true;
                        break;
                    }
                }
                for (auto itr = i->internalRequestList.begin();
                      itr != i->internalRequestList.end(); itr++) {
                    if (itr->need_metadata) {
                        internalPproc = true;
                        QCamera3ProcessingChannel *channel =
                                (QCamera3ProcessingChannel *)itr->stream->priv;
                        channel->queueReprocMetadata(metadata_buf, i->frame_number, dropFrame);
                        break;
                    }
                }
            }

            saveExifParams(metadata);
            if(IS_MULTI_CAMERA)
            {
                allocateAndinitializeMetadata(&result, &(*i));
            } else {
                result.result = translateFromHalMetadata(metadata,
                        i->timestamp, i->request_id, i->jpegMetadata, i->pipeline_depth,
                        i->capture_intent, internalPproc, i->fwkCacMode,
                        firstMetadataInBatch, i->enableZSL);
                result.result = restoreHdrScene(i->scene_mode, result.result);
            }

            if (i->blob_request) {
                {
                    //Dump tuning metadata if enabled and available
                    char prop[PROPERTY_VALUE_MAX];
                    memset(prop, 0, sizeof(prop));
                    property_get("persist.vendor.camera.dumpmetadata", prop, "0");
                    int32_t enabled = atoi(prop);
                    if (enabled) {
                        IF_META_AVAILABLE(tuning_params_t, tuning_ptr, CAM_INTF_META_TUNING_PARAMS,
                            metadata) {
                                dumpMetadataToFile(*tuning_ptr,mMetaFrameCount,enabled,
                                    "Snapshot",frame_number);
                        }
                    }

                }
            }

            if (!internalPproc) {
                LOGD("couldn't find need_metadata for this metadata");
                // Return metadata buffer
                if (free_and_bufdone_meta_buf) {
                   if(IS_MULTI_CAMERA)
                   {
                       releaseCachedMeta(&(*i), mMetadataChannel);
                   } else {
                        mMetadataChannel->bufDone(metadata_buf);
                        free(metadata_buf);
                   }
                   meta_freed = true;
                }
            }
        }
        if (!result.result) {
            LOGE("metadata is NULL");
        }
        result.frame_number = i->frame_number;
        result.input_buffer = i->input_buffer;
        result.num_output_buffers = 0;
        result.output_buffers = NULL;
        for (List<RequestedBufferInfo>::iterator j = i->buffers.begin();
                    j != i->buffers.end(); j++) {
            if (j->buffer) {
                result.num_output_buffers++;
            }
        }

        updateFpsInPreviewBuffer(metadata, i->frame_number);

        if (result.num_output_buffers > 0) {
            camera3_stream_buffer_t *result_buffers =
                new camera3_stream_buffer_t[result.num_output_buffers];
            if (result_buffers != NULL) {
                size_t result_buffers_idx = 0;
                for (List<RequestedBufferInfo>::iterator j = i->buffers.begin();
                        j != i->buffers.end(); j++) {
                    if (j->buffer) {
                        for (List<PendingFrameDropInfo>::iterator m = mPendingFrameDropList.begin();
                                m != mPendingFrameDropList.end(); m++) {
                            QCamera3Channel *channel = (QCamera3Channel *)j->buffer->stream->priv;
                            uint32_t streamID = channel->getStreamID(channel->getStreamTypeMask());
                            if((m->stream_ID == streamID) && (m->frame_number==frame_number)) {
                                j->buffer->status=CAMERA3_BUFFER_STATUS_ERROR;
                                LOGI("Stream STATUS_ERROR frame_number=%u,"
                                        "streamID=%u mCameraId: %d",
                                        frame_number, streamID, mCameraId);
                                m = mPendingFrameDropList.erase(m);
                                break;
                            }
                        }
                        j->buffer->status |= mPendingBuffersMap.getBufErrStatus(j->buffer->buffer);
                        if (j->buffer->status & CAMERA3_BUFFER_STATUS_ERROR) {
                            LOGI("CAMERA3_BUFFER_STATUS_ERROR frame_number=%u,"
                                    "buffer=%p mCameraId: %d",
                                    frame_number, j->buffer->buffer, mCameraId);
                            camera3_notify_msg_t notify_msg;
                            memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
                            notify_msg.type = CAMERA3_MSG_ERROR;
                            notify_msg.message.error.frame_number = frame_number;
                            notify_msg.message.error.error_code = CAMERA3_MSG_ERROR_BUFFER ;
                            notify_msg.message.error.error_stream = j->stream;
                            orchestrateNotify(&notify_msg);
                        }
                        mPendingBuffersMap.removeBuf(j->buffer->buffer);
                        result_buffers[result_buffers_idx++] = *(j->buffer);
                        free(j->buffer);
                        j->buffer = NULL;
                    }
                }

                if(i->bUrgentReceived == 0)
                {
                    LOGD("urgent metadata is dropped for frame number %d", frame_number);
                    i->partial_result_cnt++;
                    result.partial_result = i->partial_result_cnt;
                }
                result.output_buffers = result_buffers;
                orchestrateResult(&result);
                LOGD("Sending buffers with meta frame_number = %u, capture_time = %lld",
                        result.frame_number, i->timestamp);
                if(IS_MULTI_CAMERA)
                {
                    releasePhysicalMetadata(result.physcam_metadata, result.num_physcam_metadata);
                    releasePhysicalId(result.physcam_ids, result.num_physcam_metadata);
                    result.physcam_ids = NULL;
                    result.physcam_metadata = NULL;
                }
                free_camera_metadata((camera_metadata_t *)result.result);
                delete[] result_buffers;
            }else {
                LOGE("Fatal error: out of memory");
            }
        } else {
            if(i->bUrgentReceived == 0)
            {
                LOGD("urgent metadata is dropped for frame number %d", frame_number);
                i->partial_result_cnt++;
                result.partial_result = i->partial_result_cnt;
            }
            orchestrateResult(&result);
            LOGD("meta frame_number = %u, capture_time = %lld",
                    result.frame_number, i->timestamp);
            if(IS_MULTI_CAMERA)
            {
                releasePhysicalMetadata(result.physcam_metadata, result.num_physcam_metadata);
                releasePhysicalId(result.physcam_ids, result.num_physcam_metadata);
                result.physcam_ids = NULL;
                result.physcam_metadata = NULL;
            }
            free_camera_metadata((camera_metadata_t *)result.result);
        }

        i = erasePendingRequest(i);

        if (!mPendingReprocessResultList.empty()) {
            handlePendingReprocResults(frame_number + 1);
        }
    }

done_metadata:
    for (pendingRequestIterator i = mPendingRequestsList.begin();
            i != mPendingRequestsList.end() ;i++) {
        i->pipeline_depth++;
    }

    if(!meta_freed && free_and_bufdone_meta_buf)
    {
        mMetadataChannel->bufDone(metadata_buf);
        free(metadata_buf);
    }

    LOGD("mPendingLiveRequest = %d", mPendingLiveRequest);
cached_metadata:
    unblockRequestIfNecessary();
}

/*===========================================================================
 * FUNCTION   : restoreHdrScene
 *
 * DESCRIPTION: HAL internally removes HDR scene mode, need to restore when
 *              reporting metadata
 *
 * PARAMETERS : @result: Metadata to be reported in capture result
 *
 * RETURN     : camera_metadata_t*
 *
 *==========================================================================*/
camera_metadata_t* QCamera3HardwareInterface::restoreHdrScene(
        uint8_t sceneMode, const camera_metadata_t *result)
{
    CameraMetadata resultWrapper;

    resultWrapper.acquire((camera_metadata_t *)result);

    // If original scene mode was HDR, set it in result metadata
    if (sceneMode == ANDROID_CONTROL_SCENE_MODE_HDR) {
        LOGD("Restore HDR scene mode in result metadata");
        resultWrapper.update(ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);
    }

    return resultWrapper.release();
}

/*===========================================================================
 * FUNCTION   : hdrPlusPerfLock
 *
 * DESCRIPTION: perf lock for HDR+ using custom intent
 *
 * PARAMETERS : @metadata_buf: Metadata super_buf pointer
 *
 * RETURN     : None
 *
 *==========================================================================*/
void QCamera3HardwareInterface::hdrPlusPerfLock(
        mm_camera_super_buf_t *metadata_buf)
{
    if ((NULL == metadata_buf) || (ERROR == mState)) {
        LOGE("metadata_buf is NULL or return when mState is error");
        return;
    }
    metadata_buffer_t *metadata =
            (metadata_buffer_t *)metadata_buf->bufs[0]->buffer;
    int32_t *p_frame_number_valid =
            POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER_VALID, metadata);
    uint32_t *p_frame_number =
            POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER, metadata);

    if (p_frame_number_valid == NULL || p_frame_number == NULL) {
        LOGE("%s: Invalid metadata", __func__);
        return;
    }

    //acquire perf lock for 5 sec after the last HDR frame is captured
    if ((p_frame_number_valid != NULL) && *p_frame_number_valid) {
        if ((p_frame_number != NULL) &&
                (mLastCustIntentFrmNum == (int32_t)*p_frame_number)) {
            mPerfLockMgr.acquirePerfLock(PERF_LOCK_TAKE_SNAPSHOT, HDR_PLUS_PERF_TIME_OUT);
        }
    }
}

/*===========================================================================
 * FUNCTION   : handleInputBufferWithLock
 *
 * DESCRIPTION: Handles input buffer and shutter callback with mMutex lock held.
 *
 * PARAMETERS : @frame_number: frame number of the input buffer
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::handleInputBufferWithLock(uint32_t frame_number)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_HANDLE_IN_BUF_LKD);
    pendingRequestIterator i = mPendingRequestsList.begin();
    while (i != mPendingRequestsList.end() && i->frame_number != frame_number){
        i++;
    }
    if (i != mPendingRequestsList.end() && i->input_buffer) {
        //found the right request
        if (!i->shutter_notified) {
            CameraMetadata settings;
            camera3_notify_msg_t notify_msg;
            memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
            nsecs_t capture_time = systemTime(CLOCK_MONOTONIC);
            if(i->settings) {
                settings = i->settings;
                if (settings.exists(ANDROID_SENSOR_TIMESTAMP)) {
                    capture_time = settings.find(ANDROID_SENSOR_TIMESTAMP).data.i64[0];
                } else {
                    LOGE("No timestamp in input settings! Using current one.");
                }
            } else {
                LOGE("Input settings missing!");
            }

            notify_msg.type = CAMERA3_MSG_SHUTTER;
            notify_msg.message.shutter.frame_number = frame_number;
            notify_msg.message.shutter.timestamp = (uint64_t)capture_time;
            orchestrateNotify(&notify_msg);
            i->shutter_notified = true;
            LOGD("Input request metadata notify frame_number = %u, capture_time = %llu",
                        i->frame_number, notify_msg.message.shutter.timestamp);
        }

        if (i->input_buffer->release_fence != -1) {
           int32_t rc = sync_wait(i->input_buffer->release_fence, TIMEOUT_NEVER);
           close(i->input_buffer->release_fence);
           if (rc != OK) {
               LOGE("input buffer sync wait failed %d", rc);
           }
        }

        camera3_capture_result result;
        memset(&result, 0, sizeof(camera3_capture_result));
        result.frame_number = frame_number;
        result.result = i->settings;
        result.input_buffer = i->input_buffer;
        result.partial_result = PARTIAL_RESULT_COUNT;

        orchestrateResult(&result);
        LOGD("Input request metadata and input buffer frame_number = %u",
                        i->frame_number);
            mPendingLiveRequest--;
        i = erasePendingRequest(i);
    } else {
        LOGE("Could not find input request for frame number %d", frame_number);
    }
}

/*===========================================================================
 * FUNCTION   : handleBufferWithLock
 *
 * DESCRIPTION: Handles image buffer callback with mMutex lock held.
 *
 * PARAMETERS : @buffer: image buffer for the callback
 *              @frame_number: frame number of the image buffer
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::handleBufferWithLock(
    camera3_stream_buffer_t *buffer, uint32_t frame_number)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_HANDLE_BUF_LKD);

    if (buffer->stream->format == HAL_PIXEL_FORMAT_BLOB) {
        if (m_bIsVideo && !m_bIs4KVideo) {
            m_bStopPicChannel = true;
        }
        if (isDualCamera() && (getHalPPType() == CAM_HAL_PP_TYPE_BOKEH) && needHALPP()) {
            mPerfLockMgr.releasePerfLock(PERF_LOCK_BOKEH_SNAPSHOT);
        } else {
            mPerfLockMgr.releasePerfLock(PERF_LOCK_TAKE_SNAPSHOT);
        }
    }

    /* Nothing to be done during error state */
    if ((ERROR == mState) || (DEINIT == mState)) {
        return;
    }

    if (IS_USAGE_UBWC(buffer->stream->usage)) {
        QCamera3Channel *channel = (QCamera3Channel *)buffer->stream->priv;
        if ((1U << CAM_STREAM_TYPE_VIDEO) == channel->getStreamTypeMask()) {
            fillUBWCStats(buffer);
        }
    }

    if (mFlushPerf) {
        handleBuffersDuringFlushLock(buffer);
        return;
    }
    //not in flush
    // If the frame number doesn't exist in the pending request list,
    // directly send the buffer to the frameworks, and update pending buffers map
    // Otherwise, book-keep the buffer.
    if ((buffer->stream->format == HAL_PIXEL_FORMAT_BLOB) && (frame_number == mHdrFrameNum)) {
        mHdrFrameNum = 0;
        mHdrSnapshotRunning = false;
        pthread_cond_signal(&mHdrRequestCond);
    }

    if ((buffer->stream->format == HAL_PIXEL_FORMAT_BLOB) && (frame_number == mMultiFrameCaptureNumber)) {
        mMultiFrameCaptureNumber = 0;
        mMultiFrameSnapshotRunning = false;
    }

    pendingRequestIterator i = mPendingRequestsList.begin();
    while (i != mPendingRequestsList.end() && i->frame_number != frame_number){
        i++;
    }
    if (i == mPendingRequestsList.end() || (i->input_buffer != NULL)) {
        // Verify all pending requests frame_numbers are greater
        for (pendingRequestIterator j = mPendingRequestsList.begin();
                j != mPendingRequestsList.end(); j++) {
            if ((j->frame_number < frame_number) && !(j->input_buffer)) {
                LOGW("Error: pending live frame number %d is smaller than %d",
                         j->frame_number, frame_number);
            }
        }
        camera3_capture_result_t result;
        memset(&result, 0, sizeof(camera3_capture_result_t));
        result.result = NULL;
        result.frame_number = frame_number;
        result.num_output_buffers = 1;
        result.partial_result = 0;
        for (List<PendingFrameDropInfo>::iterator m = mPendingFrameDropList.begin();
                m != mPendingFrameDropList.end(); m++) {
            QCamera3Channel *channel = (QCamera3Channel *)buffer->stream->priv;
            uint32_t streamID = channel->getStreamID(channel->getStreamTypeMask());
            if((m->stream_ID == streamID) && (m->frame_number==frame_number) ) {
                buffer->status=CAMERA3_BUFFER_STATUS_ERROR;
                LOGI("Stream STATUS_ERROR frame_number=%d, streamID=%d, mCameraId: %d",
                         frame_number, streamID, mCameraId);
                m = mPendingFrameDropList.erase(m);
                break;
            }
        }
        buffer->status |= mPendingBuffersMap.getBufErrStatus(buffer->buffer);
        result.output_buffers = buffer;
        LOGH("result frame_number = %d, buffer = %p",
                 frame_number, buffer->buffer);

        mPendingBuffersMap.removeBuf(buffer->buffer);

        orchestrateResult(&result);
    } else {
       for (List<RequestedBufferInfo>::iterator j = i->buffers.begin();
           j != i->buffers.end(); j++) {
           if (j->stream == buffer->stream) {
               if (j->buffer != NULL) {
                   LOGE("Error: buffer is already set");
               } else {
                   j->buffer = (camera3_stream_buffer_t *)malloc(
                       sizeof(camera3_stream_buffer_t));
                   *(j->buffer) = *buffer;
                   LOGH("cache buffer %p at result frame_number %u",
                        buffer->buffer, frame_number);
               }
           }
       }
    }

    if (mPreviewStarted == false) {
        QCamera3Channel *channel = (QCamera3Channel *)buffer->stream->priv;
        if ((1U << CAM_STREAM_TYPE_PREVIEW) == channel->getStreamTypeMask()) {
            mPerfLockMgr.releasePerfLock(PERF_LOCK_START_PREVIEW);
            mPerfLockMgr.releasePerfLock(PERF_LOCK_OPEN_CAMERA);
            mPreviewStarted = true;

            // Set power hint for preview
            mPerfLockMgr.acquirePerfLock(PERF_LOCK_POWERHINT_ENCODE, 0);
        }
    }
}

/*===========================================================================
 * FUNCTION   : unblockRequestIfNecessary
 *
 * DESCRIPTION: Unblock capture_request if max_buffer hasn't been reached. Note
 *              that mMutex is held when this function is called.
 *
 * PARAMETERS :
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::unblockRequestIfNecessary()
{
   // Unblock process_capture_request
   pthread_cond_signal(&mRequestCond);
}

bool QCamera3HardwareInterface::IsQCFASelected(camera3_capture_request *request)
{
    if((request == NULL) || !m_bQuadraCfaSensor)
    {
        LOGE("Invalid check !!");
        return false;
    }
    bool qcfaReq = false;
    size_t i = 0;
    for(i = 0; i < request->num_output_buffers; i++)
    {
        const camera3_stream_buffer_t& output = request->output_buffers[i];
        if((output.stream->format == HAL_PIXEL_FORMAT_BLOB) &&
           ((output.stream->width ==
               (uint32_t)gCamCapability[mCameraId]->quadra_cfa_dim[0].width) &&
           (output.stream->height ==
              (uint32_t)gCamCapability[mCameraId]->quadra_cfa_dim[0].height))) {
            qcfaReq = true;
        }
    }
    char property[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.qcfa.select", property, "1");
    int selected = atoi(property);
    //ret: fasle for upscaling. true for remosaic
    return (qcfaReq && (selected > 0));
}

/*===========================================================================
 * FUNCTION   : isHdrSnapshotRequest
 *
 * DESCRIPTION: Function to determine if the request is for a HDR snapshot
 *
 * PARAMETERS : camera3 request structure
 *
 * RETURN     : boolean decision variable
 *
 *==========================================================================*/
bool QCamera3HardwareInterface::isHdrSnapshotRequest(camera3_capture_request *request)
{
    if (request == NULL) {
        LOGE("Invalid request handle");
        assert(0);
        return false;
    }

    char property[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.sensor.hdr", property, "0");
    int sensorHdr = atoi(property);
    if (sensorHdr)
        return false;

    if (!mForceHdrSnapshot) {
        CameraMetadata frame_settings;
        frame_settings = request->settings;

        if (frame_settings.exists(ANDROID_CONTROL_MODE)) {
            uint8_t metaMode = frame_settings.find(ANDROID_CONTROL_MODE).data.u8[0];
            if (metaMode != ANDROID_CONTROL_MODE_USE_SCENE_MODE) {
                return false;
            }
        } else {
            return false;
        }

        if (frame_settings.exists(ANDROID_CONTROL_SCENE_MODE)) {
            uint8_t fwk_sceneMode = frame_settings.find(ANDROID_CONTROL_SCENE_MODE).data.u8[0];
            if (fwk_sceneMode != ANDROID_CONTROL_SCENE_MODE_HDR) {
                return false;
            }
        } else {
            return false;
        }
    }

    for (uint32_t i = 0; i < request->num_output_buffers; i++) {
        if (request->output_buffers[i].stream->format
                == HAL_PIXEL_FORMAT_BLOB) {
            return true;
        }
    }

    return false;
}


/*===========================================================================
 * FUNCTION   : isMultiFrameSnapshotRequest
 *
 * DESCRIPTION: Function to determine if the request is for a Multiframe Process snapshot
 *
 * PARAMETERS : camera3 request structure
 *
 * RETURN     : boolean decision variable
 *
 *==========================================================================*/
bool QCamera3HardwareInterface::isMultiFrameSnapshotRequest(camera3_capture_request *request)
{
    if (request == NULL) {
        LOGE("Invalid request handle");
        assert(0);
        return false;
    }

    char prop[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.multiframe.capture.enable", prop, "0");
    mbIsMultiFrameCapture = atoi(prop) ? TRUE : FALSE;

    if (mbIsMultiFrameCapture) {
        property_get("persist.vendor.camera.multiframe.capture.count", prop, "3");
        mMultiFrameCaptureCount = (uint8_t)atoi(prop);
        if ((mMultiFrameCaptureCount == 0) || (mMultiFrameCaptureCount > MAX_MFPROC_FRAMECOUNT)) {
            LOGE("Supported Frame count range (1 - 5), set to max frame count %d",
                MAX_MFPROC_FRAMECOUNT);
            mMultiFrameCaptureCount = MAX_MFPROC_FRAMECOUNT;
        }

        for (uint32_t i = 0; i < request->num_output_buffers; i++) {
            if (request->output_buffers[i].stream->format == HAL_PIXEL_FORMAT_BLOB) {
                return true;
            }
        }
    }
    return false;
}


int32_t QCamera3HardwareInterface::orchestrateAdvancedCapture(
        camera3_capture_request_t *request, bool &isAdvancedCapture)
{
    isAdvancedCapture = false;
    bool blob_request = false;

    for (size_t i = 0; i < request->num_output_buffers; i++) {
        const camera3_stream_buffer_t& output = request->output_buffers[i];
        if (output.stream->format == HAL_PIXEL_FORMAT_BLOB) {
            //FIXME??:Call function to store local copy of jpeg data for encode params.
            blob_request = 1;
        }
    }

    if (!blob_request) {
        LOGD("Not a snapshot request, skip");
        return NO_ERROR;
    }

    if (isHdrSnapshotRequest(request)) {
        isAdvancedCapture = true;
        if(mPictureChannel != NULL)
            mPictureChannel->stopPostProc();
        return orchestrateHDRCapture(request);
    }

    if (isMultiFrameSnapshotRequest(request)) {
        isAdvancedCapture = true;
        return orchestrateMultiFrameCapture(request);
    }

    return NO_ERROR;
}


/*===========================================================================
 * FUNCTION   : orchestrateRequest
 *
 * DESCRIPTION: Orchestrates a capture request from camera service
 *
 * PARAMETERS :
 *   @request : request from framework to process
 *
 * RETURN     : Error status codes
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::orchestrateRequest(
        camera3_capture_request_t *request)
{
    int32_t ret = NO_ERROR;
    bool isAdvancedCapture = false;

    //Check if any advanced capture features are enabled
    ret = orchestrateAdvancedCapture(request, isAdvancedCapture);

    //If not, revert to regular capture flow
    if (!isAdvancedCapture) {
        uint32_t internalFrameNumber;
        List<InternalRequest> internallyRequestedStreams;
        _orchestrationDb.allocStoreInternalFrameNumber(request->frame_number, internalFrameNumber);
        request->frame_number = internalFrameNumber;
        ret = processCaptureRequest(request, internallyRequestedStreams);
    }

    return ret;
}

int32_t QCamera3HardwareInterface::orchestrateHDRCapture(
        camera3_capture_request_t *request)
{
    int32_t ret = NO_ERROR;
    uint32_t originalFrameNumber = request->frame_number;
    uint32_t originalOutputCount = request->num_output_buffers;
    const camera_metadata_t *original_settings = request->settings;
    List<InternalRequest> internallyRequestedStreams;
    List<InternalRequest> emptyInternalList;

    if (request->input_buffer == NULL) {
        mMultiFrameReqLock.lock();
        LOGD("Framework requested:%d buffers in HDR snapshot", request->num_output_buffers);
        uint32_t internalFrameNumber;
        CameraMetadata modified_meta;
        int8_t hdr_exp_values;
        cam_hdr_bracketing_info_t& hdrBracketingSetting =
                    gCamCapability[mCameraId]->hdr_bracketing_setting;
        uint32_t hdrFrameCount =
                hdrBracketingSetting.num_frames;
        LOGD("HDR values %d, %d , %d frame count: %u",
              (int8_t) hdrBracketingSetting.exp_val.values[0],
              (int8_t) hdrBracketingSetting.exp_val.values[1],
              (int8_t) hdrBracketingSetting.exp_val.values[2],
              hdrFrameCount);

        cam_exp_bracketing_t aeBracket;
        memset(&aeBracket, 0, sizeof(cam_exp_bracketing_t));
        aeBracket.mode =
            hdrBracketingSetting.exp_val.mode;

        if (aeBracket.mode == CAM_EXP_BRACKETING_OFF) {
            LOGD(" Bracketing is Off");
        }

        /* Add Blob channel to list of internally requested streams */
        for (uint32_t i = 0; i < request->num_output_buffers; i++) {
            if (request->output_buffers[i].stream->format
                    == HAL_PIXEL_FORMAT_BLOB) {
                InternalRequest streamRequested;
                streamRequested.meteringOnly = 1;
                streamRequested.need_metadata = 0;
                streamRequested.needPastFrame = 0;
                streamRequested.stream = request->output_buffers[i].stream;
                internallyRequestedStreams.push_back(streamRequested);
            }
        }
        request->num_output_buffers = 0;
        auto itr =  internallyRequestedStreams.begin();

        /* Capture Settling & -2x frame */

        /* Modify setting to set compensation */
        modified_meta = request->settings;
        hdr_exp_values = hdrBracketingSetting.exp_val.values[0];
        int32_t expCompensation = hdr_exp_values;
        uint8_t aeLock = 1;
        modified_meta.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &expCompensation, 1);
        modified_meta.update(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
        camera_metadata_t *modified_settings = modified_meta.release();
        request->settings = modified_settings;

        _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
        request->frame_number = internalFrameNumber;
        processCaptureRequest(request, internallyRequestedStreams);

        request->num_output_buffers = originalOutputCount;
        _orchestrationDb.allocStoreInternalFrameNumber(originalFrameNumber, internalFrameNumber);
        request->frame_number = internalFrameNumber;
        mHdrFrameNum = internalFrameNumber;
        processCaptureRequest(request, emptyInternalList);
        request->num_output_buffers = 0;

        /* Capture Settling & 0X frame */

        modified_meta = modified_settings;
        hdr_exp_values = hdrBracketingSetting.exp_val.values[1];
        expCompensation = hdr_exp_values;
        aeLock = 1;
        modified_meta.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &expCompensation, 1);
        modified_meta.update(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
        modified_settings = modified_meta.release();
        request->settings = modified_settings;

        itr =  internallyRequestedStreams.begin();
        if (itr == internallyRequestedStreams.end()) {
            LOGE("Error Internally Requested Stream list is empty");
            assert(0);
        } else {
            itr->need_metadata = 0;
            itr->meteringOnly = 1;
        }

        _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
        request->frame_number = internalFrameNumber;
        processCaptureRequest(request, internallyRequestedStreams);

        itr =  internallyRequestedStreams.begin();
        if (itr == internallyRequestedStreams.end()) {
            ALOGE("Error Internally Requested Stream list is empty");
            assert(0);
        } else {
            itr->need_metadata = 1;
            itr->meteringOnly = 0;
        }

        _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
        request->frame_number = internalFrameNumber;
        processCaptureRequest(request, internallyRequestedStreams);

        /* Capture 2X frame*/
        modified_meta = modified_settings;
        hdr_exp_values = hdrBracketingSetting.exp_val.values[2];
        expCompensation = hdr_exp_values;
        aeLock = 1;
        modified_meta.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &expCompensation, 1);
        modified_meta.update(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
        modified_settings = modified_meta.release();
        request->settings = modified_settings;

        itr =  internallyRequestedStreams.begin();
        if (itr == internallyRequestedStreams.end()) {
            ALOGE("Error Internally Requested Stream list is empty");
            assert(0);
        } else {
            itr->need_metadata = 0;
            itr->meteringOnly = 1;
        }
        _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
        request->frame_number = internalFrameNumber;
        processCaptureRequest(request, internallyRequestedStreams);

        itr =  internallyRequestedStreams.begin();
        if (itr == internallyRequestedStreams.end()) {
            ALOGE("Error Internally Requested Stream list is empty");
            assert(0);
        } else {
            itr->need_metadata = 1;
            itr->meteringOnly = 0;
        }

        _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
        request->frame_number = internalFrameNumber;
        ret = processCaptureRequest(request, internallyRequestedStreams);
        //Add extra EV 0 capture at the end to avoid preview flicker
        modified_meta = modified_settings;
        hdr_exp_values = hdrBracketingSetting.exp_val.values[0];
        expCompensation = hdr_exp_values;
        aeLock = 1;
        modified_meta.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &expCompensation, 1);
        modified_meta.update(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
        modified_settings = modified_meta.release();
        request->settings = modified_settings;

        itr =  internallyRequestedStreams.begin();
        if (itr == internallyRequestedStreams.end()) {
            LOGE("Error Internally Requested Stream list is empty");
            assert(0);
        } else {
            itr->need_metadata = 0;
            itr->meteringOnly = 1;
        }

        _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
        request->frame_number = internalFrameNumber;
        mHdrSnapshotRunning = true;
        processCaptureRequest(request, internallyRequestedStreams);


        /* Capture 2X on original streaming config*/
        internallyRequestedStreams.clear();

        /* Restore original settings pointer */
        request->settings = original_settings;
    }
    return ret;
}

int32_t QCamera3HardwareInterface::orchestrateMultiFrameCapture(
        camera3_capture_request_t *request)
{
    int32_t ret = NO_ERROR;
    uint32_t originalFrameNumber = request->frame_number;
    const camera_metadata_t *original_settings = request->settings;
    List<InternalRequest> internallyRequestedStreams;
    List<InternalRequest> emptyInternalList;

    if (request->input_buffer == NULL) {
        mMultiFrameReqLock.lock();
        LOGD("Framework requested:%d buffers in Multi Frame snapshot", request->num_output_buffers);
        uint32_t internalFrameNumber;

        /* Add Blob channel to list of internally requested streams */
        for (uint32_t i = 0; i < request->num_output_buffers; i++) {
            if (request->output_buffers[i].stream->format
                    == HAL_PIXEL_FORMAT_BLOB) {
                InternalRequest streamRequested;
                streamRequested.meteringOnly = 0;
                streamRequested.need_metadata = 1;
                streamRequested.needPastFrame = 1;
                streamRequested.stream = request->output_buffers[i].stream;
                internallyRequestedStreams.push_back(streamRequested);
            }
        }

        _orchestrationDb.allocStoreInternalFrameNumber(originalFrameNumber, internalFrameNumber);
        request->frame_number = internalFrameNumber;
        mMultiFrameCaptureNumber = internalFrameNumber;
        mMultiFrameSnapshotRunning = true;
        ret = processCaptureRequest(request, emptyInternalList);

        for (uint32_t j = 0; j < mMultiFrameCaptureCount-1; j++) {
            request->num_output_buffers = 0;
            _orchestrationDb.generateStoreInternalFrameNumber(internalFrameNumber);
            request->frame_number = internalFrameNumber;
            processCaptureRequest(request, internallyRequestedStreams);
        }

        internallyRequestedStreams.clear();

        /* Restore original settings pointer */
        request->settings = original_settings;
    }
    return ret;
}

/*===========================================================================
 * FUNCTION   : orchestrateResult
 *
 * DESCRIPTION: Orchestrates a capture result to camera service
 *
 * PARAMETERS :
 *   @request : request from framework to process
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::orchestrateResult(
                    camera3_capture_result_t *result)
{
    uint32_t frameworkFrameNumber;
    int32_t rc = _orchestrationDb.getFrameworkFrameNumber(result->frame_number,
            frameworkFrameNumber);
    if (rc != NO_ERROR) {
        LOGE("Cannot find translated frameworkFrameNumber");
        assert(0);
    } else {
        if (frameworkFrameNumber == EMPTY_FRAMEWORK_FRAME_NUMBER) {
            LOGD("Internal Request drop the result");
        } else {
            result->frame_number = frameworkFrameNumber;
            mCallbackOps->process_capture_result(mCallbackOps, result);
        }
    }
}

/*===========================================================================
 * FUNCTION   : orchestrateNotify
 *
 * DESCRIPTION: Orchestrates a notify to camera service
 *
 * PARAMETERS :
 *   @request : request from framework to process
 *
 * RETURN     :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::orchestrateNotify(camera3_notify_msg_t *notify_msg)
{
    uint32_t frameworkFrameNumber;
    uint32_t internalFrameNumber = notify_msg->message.shutter.frame_number;
    int32_t rc = NO_ERROR;

    rc = _orchestrationDb.getFrameworkFrameNumber(internalFrameNumber,
                                                          frameworkFrameNumber);

    if (rc != NO_ERROR) {
        if (notify_msg->message.error.error_code == CAMERA3_MSG_ERROR_DEVICE) {
            LOGD("Sending CAMERA3_MSG_ERROR_DEVICE to framework");
            frameworkFrameNumber = 0;
        } else {
            LOGE("Cannot find translated frameworkFrameNumber");
            assert(0);
            return;
        }
    }

    if (frameworkFrameNumber == EMPTY_FRAMEWORK_FRAME_NUMBER) {
        LOGD("Internal Request drop the notifyCb");
    } else {
        notify_msg->message.shutter.frame_number = frameworkFrameNumber;
        mCallbackOps->notify(mCallbackOps, notify_msg);
    }
}

int32_t QCamera3HardwareInterface::switchStreamConfigInternal(__unused uint32_t frame_number)
{
    pthread_mutex_unlock(&mMutex);
    //stop all channels and send error notification for pending requests
    flush(false);
    pthread_mutex_lock(&mMutex);

    //delete the channels
    cleanAndSortStreamInfo();

    if (mMetadataChannel) {
        delete mMetadataChannel;
        mMetadataChannel = NULL;
    }
    if (mAnalysisChannel) {
        delete mAnalysisChannel;
        mAnalysisChannel = NULL;
    }
    if (mQCFACaptureChannel) {
        delete mQCFACaptureChannel;
        mQCFACaptureChannel = NULL;
    }

    /* unconfigure and reset meta stream info */
    cam_stream_size_info_t stream_config_info;
    memset(&stream_config_info, 0, sizeof(cam_stream_size_info_t));
    clear_metadata_buffer(mParameters);
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_HAL_VERSION, CAM_HAL_V3);
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_STREAM_INFO, stream_config_info);
    mCameraHandle->ops->set_parms(mCameraHandle->camera_handle, mParameters);


    /* create meta and raw channel internally */
    cam_feature_mask_t metadataFeatureMask = CAM_QCOM_FEATURE_NONE;
    setPAAFSupport(metadataFeatureMask, CAM_STREAM_TYPE_METADATA,
            gCamCapability[mCameraId]->color_arrangement);
    mMetadataChannel = new QCamera3MetadataChannel(mCameraHandle->camera_handle, mChannelHandle,
                            mCameraHandle->ops, internalMetaCb, setBufferErrorStatus,
                            &gCamCapability[mCameraId]->padding_info, metadataFeatureMask, this);
    if (mMetadataChannel == NULL) {
        LOGE("failed to allocate metadata channel");
        return -1;
    }
    mMetadataChannel->initialize(IS_TYPE_NONE);

    cam_dimension_t raw_dim = getQuadraCfaDim();

    LOGH("quadra cfa raw dim: %dx%d", raw_dim.width, raw_dim.height);
    mQCFACaptureChannel = new QCamera3QCfaCaptureChannel(mCameraHandle->camera_handle, mChannelHandle,
                            mCameraHandle->ops, raw_dim, &gCamCapability[mCameraId]->padding_info,
                            this, CAM_QCOM_FEATURE_NONE,m_bInSensorQCFA);

    /* send meta stream info */
    cam_stream_size_info_t stream_sz_info;
    memset(&stream_sz_info, 0, sizeof(cam_stream_size_info_t));
    stream_sz_info.num_streams = 1;
    stream_sz_info.stream_sizes[0] = raw_dim;

    if(m_bInSensorQCFA)
        stream_sz_info.type[0] = CAM_STREAM_TYPE_SNAPSHOT;
    else
        stream_sz_info.type[0] = CAM_STREAM_TYPE_RAW;

    stream_sz_info.postprocess_mask[0] = CAM_QCOM_FEATURE_NONE;

    clear_metadata_buffer(mParameters);
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_HAL_VERSION, CAM_HAL_V3);
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_STREAM_INFO, stream_sz_info);
    bool enable = true;
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_QUADRA_CFA, enable);
    mCameraHandle->ops->set_parms(mCameraHandle->camera_handle, mParameters);

    /* initialize and start channel */
    mQCFACaptureChannel->initialize(IS_TYPE_NONE);


    int32_t rc = setBundleInfo();
    if (rc < 0) {
        LOGE("setBundleInfo failed %d", rc);
        return rc;
    }

    mMetadataChannel->start();

    mQCFACaptureChannel->start();

    if (mChannelHandle) {
        mCameraHandle->ops->start_channel(mCameraHandle->camera_handle, mChannelHandle);
    }

    LOGD("X");
    return 0;
}

int32_t QCamera3HardwareInterface::deleteQCFACaptureChannel()
{
    LOGD("E");

    if (mQCFACaptureChannel) {
        delete mQCFACaptureChannel;
        mQCFACaptureChannel = NULL;
    }

    return 0;
}

cam_dimension_t QCamera3HardwareInterface::getQCFAComapitbleDim(
            const uint32_t& width, const uint32_t& height)
{
    const cam_dimension_t& qcfa_dim = gCamCapability[mCameraId]->quadra_cfa_dim[0];
    const cam_dimension_t& raw_dim = gCamCapability[mCameraId]->raw_dim[0];
    if((0 == width) || (0 == height) ||
           ((uint32_t)qcfa_dim.width == width) ||
           ((uint32_t)qcfa_dim.height == height))
    {
        return raw_dim;
    }

    float aspectRatio = width/(float)height;
    float aspectRatioTolerence = 0.04;
    cam_dimension_t ret_dim = raw_dim;
    for(uint32_t i = 0; i < gCamCapability[mCameraId]->picture_sizes_tbl_cnt; i++)
    {
        const cam_dimension_t& dim = gCamCapability[mCameraId]->picture_sizes_tbl[i];
        if((dim.width <= raw_dim.width) && (dim.height <= raw_dim.height))
        {
            float ar = dim.width/(float)dim.height;
            if((ar >= (aspectRatio - aspectRatioTolerence)) &&
              (ar <= (aspectRatio + aspectRatioTolerence))){
              ret_dim = dim;
              break;
            }
        }
    }

    LOGI("changed dimentation to %dx%d", ret_dim.width, ret_dim.height);
    return ret_dim;
}

cam_dimension_t QCamera3HardwareInterface::getQuadraCfaDim()
{
    cam_dimension_t dim = {0,0};
    if (gCamCapability[mCameraId]->supported_quadra_cfa_dim_cnt > 0) {
        dim = gCamCapability[mCameraId]->quadra_cfa_dim[0];
        LOGD("dim: %dx%d", dim.width, dim.height);
    } else {
        LOGE("No quadra cfa dim available!");
    }

    return dim;
}


/*===========================================================================
 * FUNCTION   : captureQuadraCfaRawInternal
 *
 * DESCRIPTION: reconfig streams internally to capture quadra cfa raw frame
 *
 * PARAMETERS :
 *   @request : request from framework to process
 *
 * RETURN     :
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::captureQuadraCfaFrameInternal(camera3_capture_request_t *request)
{
    LOGD("E");
    int32_t rc = 0;

    /* 1. config streams internally and stream on */
    rc = switchStreamConfigInternal(request->frame_number);
    if (rc != NO_ERROR) {
        LOGE("fail to switch to internal stream configration");
        return rc;
    }
    assert(mMetadataChannel != NULL);
    assert(mQCFACaptureChannel  != NULL);


    /* 2. request new frame */
    int indexUsed;
    mMetadataChannel->request(NULL, request->frame_number, indexUsed);
    mQCFACaptureChannel->request(NULL, request->frame_number, indexUsed);

    cam_stream_ID_t streamsArray;
    memset(&streamsArray, 0, sizeof(cam_stream_ID_t));
    streamsArray.num_streams = 1;
    streamsArray.stream_request[0].streamID =
        mQCFACaptureChannel->getStreamID(mQCFACaptureChannel->getStreamTypeMask());
    streamsArray.stream_request[0].buf_index = CAM_FREERUN_IDX;
    setFrameParameters(request->settings, streamsArray, true, 0, mParameters, request);
    mCameraHandle->ops->set_parms(mCameraHandle->camera_handle, mParameters);


    /* 3. wait for raw capture done */
    mQCFACaptureChannel->waitCaptureDone();
    LOGH("quadra cfa raw capture done.");


    /* 4. stop streams and restore fwk stream configuration */
    mQCFACaptureChannel->stop();
    mMetadataChannel->stop();
    if (mChannelHandle) {
        mCameraHandle->ops->stop_channel(mCameraHandle->camera_handle,
                mChannelHandle);
    }

    // need keep channel context and frame buffer, so call destroy instead of del the channel.
    mQCFACaptureChannel->destroy();
    delete mMetadataChannel;
    mMetadataChannel = NULL;

    LOGD("reset quadra cfa mode after capture done.");
    bool enable = false;
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_QUADRA_CFA, enable);
    mCameraHandle->ops->set_parms(mCameraHandle->camera_handle, mParameters);

    mStreamInfo.clear();
    pthread_mutex_unlock(&mMutex);
    configureStreamsPerfLocked(&mStreamList);
    pthread_mutex_lock(&mMutex);

    /* 5. trigger 2 pass reprocess for cfa raw, within the same process_capture_request() call */

    LOGD("X");
    return rc;
}


/*===========================================================================
 * FUNCTION   : FrameNumberRegistry
 *
 * DESCRIPTION: Constructor
 *
 * PARAMETERS :
 *
 * RETURN     :
 *
 *==========================================================================*/
FrameNumberRegistry::FrameNumberRegistry()
{
    _nextFreeInternalNumber = INTERNAL_FRAME_STARTING_NUMBER;
}

/*===========================================================================
 * FUNCTION   : ~FrameNumberRegistry
 *
 * DESCRIPTION: Destructor
 *
 * PARAMETERS :
 *
 * RETURN     :
 *
 *==========================================================================*/
FrameNumberRegistry::~FrameNumberRegistry()
{
}

/*===========================================================================
 * FUNCTION   : PurgeOldEntriesLocked
 *
 * DESCRIPTION: Maintainance function to trigger LRU cleanup mechanism
 *
 * PARAMETERS :
 *
 * RETURN     : NONE
 *
 *==========================================================================*/
void FrameNumberRegistry::purgeOldEntriesLocked()
{
    while (_register.begin() != _register.end()) {
        auto itr = _register.begin();
        if (itr->first < (_nextFreeInternalNumber - FRAME_REGISTER_LRU_SIZE)) {
            _register.erase(itr);
        } else {
            return;
        }
    }
}

/*===========================================================================
 * FUNCTION   : allocStoreInternalFrameNumber
 *
 * DESCRIPTION: Method to note down a framework request and associate a new
 *              internal request number against it
 *
 * PARAMETERS :
 *   @fFrameNumber: Identifier given by framework
 *   @internalFN  : Output parameter which will have the newly generated internal
 *                  entry
 *
 * RETURN     : Error code
 *
 *==========================================================================*/
int32_t FrameNumberRegistry::allocStoreInternalFrameNumber(uint32_t frameworkFrameNumber,
                                                            uint32_t &internalFrameNumber)
{
    Mutex::Autolock lock(mRegistryLock);
    internalFrameNumber = _nextFreeInternalNumber++;
    LOGD("Storing ff#:%d, with internal:%d", frameworkFrameNumber, internalFrameNumber);
    _register.insert(std::pair<uint32_t,uint32_t>(internalFrameNumber, frameworkFrameNumber));
    purgeOldEntriesLocked();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : generateStoreInternalFrameNumber
 *
 * DESCRIPTION: Method to associate a new internal request number independent
 *              of any associate with framework requests
 *
 * PARAMETERS :
 *   @internalFrame#: Output parameter which will have the newly generated internal
 *
 *
 * RETURN     : Error code
 *
 *==========================================================================*/
int32_t FrameNumberRegistry::generateStoreInternalFrameNumber(uint32_t &internalFrameNumber)
{
    Mutex::Autolock lock(mRegistryLock);
    internalFrameNumber = _nextFreeInternalNumber++;
    LOGD("Generated internal framenumber:%d", internalFrameNumber);
    _register.insert(std::pair<uint32_t,uint32_t>(internalFrameNumber, EMPTY_FRAMEWORK_FRAME_NUMBER));
    purgeOldEntriesLocked();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getFrameworkFrameNumber
 *
 * DESCRIPTION: Method to query the framework framenumber given an internal #
 *
 * PARAMETERS :
 *   @internalFrame#: Internal reference
 *   @frameworkframenumber: Output parameter holding framework frame entry
 *
 * RETURN     : Error code
 *
 *==========================================================================*/
int32_t FrameNumberRegistry::getFrameworkFrameNumber(uint32_t internalFrameNumber,
                                                     uint32_t &frameworkFrameNumber)
{
    Mutex::Autolock lock(mRegistryLock);
    auto itr = _register.find(internalFrameNumber);
    if (itr == _register.end()) {
        LOGE("Cannot find internal#: %d", internalFrameNumber);
        return -ENOENT;
    }

    frameworkFrameNumber = itr->second;
    purgeOldEntriesLocked();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : processCaptureRequest
 *
 * DESCRIPTION: process a capture request from camera service
 *
 * PARAMETERS :
 *   @request : request from framework to process
 *
 * RETURN     :
 *
 *==========================================================================*/
int QCamera3HardwareInterface::processCaptureRequest(
                    camera3_capture_request_t *request,
                    List<InternalRequest> &internallyRequestedStreams)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_PROC_CAP_REQ);
    int rc = NO_ERROR;
    int32_t aux_request_id;
    int32_t request_id;
    CameraMetadata meta;
    CameraMetadata auxmeta;
    bool isVidBufRequested = false;
    camera3_stream_buffer_t *pInputBuffer = NULL;
#ifdef USE_HAL_3_5
    const camera_metadata_t *main_setting = NULL;
#endif //USE_HAL_3_5
    const camera_metadata_t *aux_setting = NULL;
    const camera_metadata_t *logical_setting = NULL;
    bool is_requested_on_main = false;
    bool is_requested_on_aux = false;
    bool is_logical_request = false;
    bool setEis = false;

    char prop[PROPERTY_VALUE_MAX];
#ifdef ENABLE_THROTTLE
    int32_t perfLevel = 0;
#endif


    pthread_mutex_lock(&mMutex);

    // Validate current state
    switch (mState) {
        case CONFIGURED:
        case STARTED:
            /* valid state */
            break;

        case ERROR:
            pthread_mutex_unlock(&mMutex);
            handleCameraDeviceError();
            return -ENODEV;

        default:
            LOGE("Invalid state %d", mState);
            pthread_mutex_unlock(&mMutex);
            return -ENODEV;
    }

    rc = validateCaptureRequest(request, internallyRequestedStreams);
    if (rc != NO_ERROR) {
        LOGE("incoming request is not valid");
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    logical_setting = request->settings;

    meta = request->settings;
#ifdef USE_HAL_3_5
    if(IS_MULTI_CAMERA)
    {
        if(request->num_physcam_settings > 0)
        {
            LOGD("num of physcam settings %d", request->num_physcam_settings);
            for(uint32_t i = 0; i < request->num_physcam_settings; i++)
            {
                if((request->physcam_id != NULL)
                     && (request->physcam_id[i] != NULL)
                     && (request->physcam_settings != NULL)
                     && (request->physcam_id[i][0] != '\0'))
                {
                    uint32_t id = atoi(request->physcam_id[i]);
                    if(id == get_main_camera_idx(this->mCameraId))
                    {
                        main_setting = request->physcam_settings[i];
                    }else if(id == get_aux_camera_idx(this->mCameraId)) {
                        aux_setting = request->physcam_settings[i];
                    }
                }
            }
        }

        for(uint32_t i = 0; i < request->num_output_buffers; i++)
        {
            camera3_stream_t *stream = request->output_buffers[i].stream;
            if((stream->physical_camera_id != NULL) && (stream->physical_camera_id[0] != 0))
            {
                int id = atoi(stream->physical_camera_id);
                if(id == (int)get_aux_camera_idx(this->mCameraId))
                {
                    LOGD("requested on aux");
                    is_requested_on_aux = true;
                }else if(id == (int)get_main_camera_idx(this->mCameraId)) {
                    LOGD("requested on main");
                    is_requested_on_main = true;
                }
            }else {
                LOGD("requested on logical");
                is_logical_request = true;
            }
        }

        if(!is_requested_on_main && (main_setting != NULL))
        {
            LOGD("request contain settings of main without output buffer");
        }

        if(!is_requested_on_aux && (aux_setting != NULL))
        {
            LOGD("request contain settings of main without output buffer");
        }

        if(logical_setting != NULL)
        {
            if(is_requested_on_main && (main_setting == NULL))
            {
               LOGD("setting logical as main settings");
                main_setting = logical_setting;
            }

            if(is_aux_configured && (aux_setting == NULL))
            {
                LOGD("setting logical as aux settings");
                aux_setting = logical_setting;
            }
        }

        if(is_requested_on_main)
        {
            meta = main_setting;
        } else {
            meta = logical_setting;
        }
        auxmeta = aux_setting;
    }
#endif

    /* check if need quadra cfa raw in each capture request */
    m_bQuadraCfaRequest = false;
    if (m_bQuadraCfaSensor && m_bQuadraSizeConfigured) {
        for (size_t i = 0; i < request->num_output_buffers; i++) {
            const camera3_stream_buffer_t& output = request->output_buffers[i];
            if (request->input_buffer == NULL && output.stream->format == HAL_PIXEL_FORMAT_BLOB) {
                if (IsQCFASelected(request)) {
                    /* only one output stream is supported for quadra cfa snapshot right now */
                    if (request->num_output_buffers > 1) {
                        LOGE("invalid num of streams requested for quadra cfa snapshot!");
                        pthread_mutex_unlock(&mMutex);
                        return BAD_VALUE;
                    }

                    LOGI("quadra cfa size request on blob stream");
                    m_bQuadraCfaRequest = true;
                    m_bPreSnapQuadraCfaRequest = true;
                    /* this will trigger internal stream reconfig and block until get raw output */
                    captureQuadraCfaFrameInternal(request);
                    //reset state to configured so that channel get initialized and streamed on again
                    mState = CONFIGURED;
                } else {
                    if (true == m_bPreSnapQuadraCfaRequest) {
                        m_bPreSnapQuadraCfaRequest = false;
                        mPictureChannel->stopPostProc();
                    }
                }
            }
        }
    }

    // For first capture request, send capture intent, and
    // stream on all streams
    uint32_t camHdl = mCameraHandle->camera_handle;
    uint32_t channelHdl = mChannelHandle;
    int config_index = CONFIG_INDEX_MAIN;
    CameraMetadata l_meta = meta;
    metadata_buffer_t *params = mParameters;
    uint8_t l_captureIntent = mCaptureIntent;

    if (mState == CONFIGURED) {
        // send an unconfigure to the backend so that the isp
        // resources are deallocated
        cam_is_type_t isTypeVideo, isTypePreview, is_type=IS_TYPE_NONE;
        if(isDualCamera()) {
            camHdl = get_main_camera_handle(mCameraHandle->camera_handle);
            channelHdl = mChannelHandle;

            //Set HAL pptype for dual camera.
            //For multicamera set PP type NONE.
            m_halPPType = CAM_HAL_PP_TYPE_SAT;
            char PP_prop[PROPERTY_VALUE_MAX];
            memset(PP_prop, 0, sizeof(PP_prop));
            property_get("persist.vendor.camera.halpp", PP_prop, "");
            if (strlen(PP_prop) > 0) {
                m_halPPType = (cam_hal_pp_type_t)atoi(PP_prop);
            }

            if (meta.exists(QCAMERA3_BOKEH_ENABLE)) {
                bool bokehEnable = l_meta.find(QCAMERA3_BOKEH_ENABLE).data.u8[0];
                LOGD("bokehEnable in vendor tag %d", bokehEnable);
                if (bokehEnable) {
                    m_halPPType = CAM_HAL_PP_TYPE_BOKEH;
                }
            }

            if (meta.exists(QCAMERA3_SAT_MODE_ON)) {
                bool satEnable = l_meta.find(QCAMERA3_SAT_MODE_ON).data.u8[0];
                LOGD("SAT in vendor tag %d", satEnable);
                if (satEnable) {
                    m_halPPType = CAM_HAL_PP_TYPE_SAT;
                }
            }
            if(IS_MULTI_CAMERA)
            {
                if(is_main_configured || is_logical_configured)
                {
                    camHdl = get_main_camera_handle(mCameraHandle->camera_handle);
                    params = mParameters;
                    l_meta = meta;
                    config_index = CONFIG_INDEX_MAIN;
                } else if(is_aux_configured) {
                    camHdl = get_aux_camera_handle(mCameraHandle->camera_handle);
                    params = mAuxParameters;
                    l_meta = auxmeta;
                    config_index = CONFIG_INDEX_AUX;
                }
            }
        }

        do {
            if (!mFirstConfiguration) {
                cam_stream_size_info_t stream_config_info;
                int32_t hal_version = CAM_HAL_V3;
                memset(&stream_config_info, 0, sizeof(cam_stream_size_info_t));
                clear_metadata_buffer(params);
                ADD_SET_PARAM_ENTRY_TO_BATCH(params,
                        CAM_INTF_PARM_HAL_VERSION, hal_version);
                ADD_SET_PARAM_ENTRY_TO_BATCH(params,
                        CAM_INTF_META_STREAM_INFO, stream_config_info);
                rc = mCameraHandle->ops->set_parms( camHdl, params);
                if (rc < 0) {
                    LOGE("set_parms for unconfigure failed");
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }

                if (isDualCamera() && !IS_PP_TYPE_NONE) {
                    clear_metadata_buffer(mAuxParameters);
                    memcpy(mAuxParameters, mParameters, sizeof(metadata_buffer_t));
                    if (m_pFovControl != NULL) {
                        m_pFovControl->translateInputParams(mParameters, mAuxParameters);
                    }

                    rc = mCameraHandle->ops->set_parms(
                            get_aux_camera_handle(mCameraHandle->camera_handle),
                            mAuxParameters);
                    if (rc < 0) {
                        LOGE("Aux: set_parms for unconfigure failed");
                        pthread_mutex_unlock(&mMutex);
                        return rc;
                    }
                }
            }
            mPerfLockMgr.acquirePerfLock(PERF_LOCK_START_PREVIEW);
            /* get eis information for stream configuration */
            char is_type_value[PROPERTY_VALUE_MAX];
            property_get("persist.vendor.camera.is_type", is_type_value, "0");
            isTypeVideo = static_cast<cam_is_type_t>(atoi(is_type_value));
            // Make default value for preview IS_TYPE as IS_TYPE_EIS_2_0
            property_get("persist.vendor.camera.is_type_preview", is_type_value, "4");
            isTypePreview = static_cast<cam_is_type_t>(atoi(is_type_value));
            LOGD("isTypeVideo: %d isTypePreview: %d", isTypeVideo, isTypePreview);

            int32_t vfe1_reserved_rdi = -1;
            if (l_meta.exists(QCAMERA3_SIMULTANEOUS_CAMERA_VFE1_RESERVED_RDI)) {
                vfe1_reserved_rdi =
                    l_meta.find(QCAMERA3_SIMULTANEOUS_CAMERA_VFE1_RESERVED_RDI).data.i32[0];
            } else {
                char value[PROPERTY_VALUE_MAX];
                property_get("persist.vendor.camera.vfe1.reservedrdi", value, "-1");
                vfe1_reserved_rdi = atoi(value);
            }
            if (vfe1_reserved_rdi < -1 && vfe1_reserved_rdi > 3)
                vfe1_reserved_rdi = -1;
            ADD_SET_PARAM_ENTRY_TO_BATCH(params,
                    CAM_INTF_PARM_VFE1_RESERVED_RDI, vfe1_reserved_rdi);
            rc = mCameraHandle->ops->set_parms( camHdl, params);

            if (l_meta.exists(ANDROID_CONTROL_CAPTURE_INTENT)) {
                int32_t hal_version = CAM_HAL_V3;
                uint8_t captureIntent =
                    l_meta.find(ANDROID_CONTROL_CAPTURE_INTENT).data.u8[0];
                l_captureIntent = captureIntent;
                clear_metadata_buffer(params);
                ADD_SET_PARAM_ENTRY_TO_BATCH(params, CAM_INTF_PARM_HAL_VERSION, hal_version);
                ADD_SET_PARAM_ENTRY_TO_BATCH(params, CAM_INTF_META_CAPTURE_INTENT, captureIntent);
            }

            if (mFirstConfiguration) {
                // configure instant AEC
                // Instant AEC is a session based parameter and it is needed only
                // once per complete session after open camera.
                // i.e. This is set only once for the first capture request, after open camera.
                setInstantAEC(l_meta);
            }
            uint8_t fwkVideoStabMode=0;
            if (l_meta.exists(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)) {
                fwkVideoStabMode = l_meta.find(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE).data.u8[0];
            }
 
            // If EIS setprop is enabled & if first capture setting has EIS enabled then only
            // turn it on for video/preview
            setEis = m_bEisEnable && fwkVideoStabMode && m_bEisSupportedSize &&
                    (isTypeVideo >= IS_TYPE_EIS_2_0);
            int32_t vsMode;
            vsMode = (setEis)? DIS_ENABLE: DIS_DISABLE;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(params, CAM_INTF_PARM_DIS_ENABLE, vsMode)) {
                rc = BAD_VALUE;
            }
            LOGD("setEis %d", setEis);
            bool eis3Supported = false;
            size_t count = IS_TYPE_MAX;
            count = MIN(gCamCapability[mCameraId]->supported_is_types_cnt, count);
            for (size_t i = 0; i < count; i++) {
                if (gCamCapability[mCameraId]->supported_is_types[i] == IS_TYPE_EIS_3_0) {
                    eis3Supported = true;
                    break;
                }
            }

            if (mQuadraCfaStage != QCFA_INACTIVE) {
                bool enable = (mQuadraCfaStage == QCFA_RAW_OUTPUT);
                if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_QUADRA_CFA, enable)) {
                    LOGE("Failed to update Quadra CFA mode");
                    rc = BAD_VALUE;
                }
            }

            //IS type will be 0 unless EIS is supported. If EIS is supported
            //it could either be 4 or 5 depending on the stream and video size
            for (uint32_t i = 0; i < mStreamConfigInfo[config_index].num_streams; i++) {
                if (setEis) {
                    if (mStreamConfigInfo[config_index].type[i] == CAM_STREAM_TYPE_PREVIEW) {
                        is_type = isTypePreview;
                    } else if (mStreamConfigInfo[config_index].type[i] == CAM_STREAM_TYPE_VIDEO ) {
                        if ( (isTypeVideo == IS_TYPE_EIS_3_0) && (eis3Supported == FALSE) ) {
                            LOGW(" EIS_3.0 is not supported and so setting EIS_2.0");
                            is_type = IS_TYPE_EIS_2_0;
                        } else {
                            is_type = isTypeVideo;
                        }
                    } else {
                        is_type = IS_TYPE_NONE;
                    }
                     mStreamConfigInfo[config_index].is_type[i] = is_type;
                } else {
                     mStreamConfigInfo[config_index].is_type[i] = IS_TYPE_NONE;
                }
            }

            if (setEis && eis3Supported && (isTypeVideo == IS_TYPE_EIS_3_0)) {
                mMaxInFlightRequests = MAX_INFLIGHT_EIS_REQUESTS;
            }
            else if (setEis && (isTypeVideo == IS_TYPE_VENDOR_EIS)) {
                mMaxInFlightRequests = MAX_INFLIGHT_EIS_REQUESTS;
            }

            // This DC info is required for setting the actual sync type instead of value
            // set in confgure streams
            if (l_meta.exists(QCAMERA3_DUALCAM_LINK_ENABLE)) {
                mIsDeviceLinked = l_meta.find(QCAMERA3_DUALCAM_LINK_ENABLE).data.u8[0];
                if (mIsDeviceLinked) {
                    mStreamConfigInfo[config_index].sync_type = get_cam_type(mCameraId);
                }
            }

            ADD_SET_PARAM_ENTRY_TO_BATCH(params,
                    CAM_INTF_META_STREAM_INFO, mStreamConfigInfo[config_index]);

            //Disable tintless only if the property is set to 0
            memset(prop, 0, sizeof(prop));
            property_get("persist.vendor.camera.tintless.enable", prop, "1");
            int32_t tintless_value = atoi(prop);

            ADD_SET_PARAM_ENTRY_TO_BATCH(params,
                    CAM_INTF_PARM_TINTLESS, tintless_value);

            //Disable CDS for HFR mode or if DIS/EIS is on.
            //CDS is a session parameter in the backend/ISP, so need to be set/reset
            //after every configure_stream
            if ((CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE == mOpMode) ||
                    (m_bIsVideo)) {
                int32_t cds = CAM_CDS_MODE_OFF;
                if (ADD_SET_PARAM_ENTRY_TO_BATCH(params,
                        CAM_INTF_PARM_CDS_MODE, cds))
                    LOGE("Failed to disable CDS for HFR mode");
            }

            if (m_debug_avtimer || l_meta.exists(QCAMERA3_USE_AV_TIMER)) {
                uint8_t* use_av_timer = NULL;
                if (m_debug_avtimer){
                    LOGI(" Enabling AV timer through setprop");
                    use_av_timer = &m_debug_avtimer;
                }
                else{
                    use_av_timer =
                        l_meta.find(QCAMERA3_USE_AV_TIMER).data.u8;
                    if (use_av_timer) {
                        LOGI("Enabling AV timer through Metadata: use_av_timer: %d", *use_av_timer);
                    }
                }

                if (ADD_SET_PARAM_ENTRY_TO_BATCH(params, CAM_INTF_META_USE_AV_TIMER, *use_av_timer)) {
                    rc = BAD_VALUE;
                }
            }

            setMobicat();

            /* Set fps and hfr mode while sending meta stream info so that sensor
             * can configure appropriate streaming mode */
            mHFRVideoFps = DEFAULT_VIDEO_FPS;
            mMinInFlightRequests = MIN_INFLIGHT_REQUESTS;
            mMaxInFlightRequests = MAX_INFLIGHT_REQUESTS;
            if (l_meta.exists(ANDROID_CONTROL_AE_TARGET_FPS_RANGE)) {
                rc = setHalFpsRange(l_meta, params);
                if (rc == NO_ERROR) {
                    int32_t max_fps =
                        (int32_t) l_meta.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE).data.i32[1];
                    if (max_fps == 60) {
                        mMinInFlightRequests = MIN_INFLIGHT_60FPS_REQUESTS;
                    }
                    /* For HFR, more buffers are dequeued upfront to improve the performance */
                    if (mBatchSize) {
                        mMinInFlightRequests = MIN_INFLIGHT_HFR_REQUESTS;
                        mMaxInFlightRequests = MAX_INFLIGHT_HFR_REQUESTS;
                    }
                    if (max_fps >= 60) {
                        mPerfLockMgr.releasePerfLock(PERF_LOCK_POWERHINT_ENCODE);
                        mPerfLockMgr.acquirePerfLock(PERF_LOCK_POWERHINT_HFR, 0);
                    } else {
                        mPerfLockMgr.releasePerfLock(PERF_LOCK_POWERHINT_HFR);
                        if (mPreviewStarted) {
                            mPerfLockMgr.acquirePerfLock(PERF_LOCK_POWERHINT_ENCODE, 0);
                        }
                    }
                }
                else {
                    LOGE("setHalFpsRange failed");
                }
            }
            mShouldSetSensorHdr = true;
            bool didSetSensorHdr = false;
            if (l_meta.exists(ANDROID_CONTROL_MODE)) {
                uint8_t metaMode = l_meta.find(ANDROID_CONTROL_MODE).data.u8[0];
                rc = extractSceneMode(l_meta, metaMode, params);
                if (rc != NO_ERROR) {
                    LOGE("extractSceneMode failed");
                }
            }

            if (m_halPPType == CAM_HAL_PP_TYPE_BOKEH) {
                LOGI("setting bokeh mode" );
                ADD_SET_PARAM_ENTRY_TO_BATCH(params, CAM_INTF_PARM_BOKEH_MODE, 1);
            }
            //Set feature masks based on dual cam feature enabled
            for (uint32_t i = 0; i < mStreamConfigInfo[config_index].num_streams; i++) {
                setDCFeature(mStreamConfigInfo[config_index].postprocess_mask[i],
                    (cam_stream_type_t) mStreamConfigInfo[config_index].type[i]);
            }

            if (l_meta.exists(QCAMERA3_VIDEO_HDR_MODE)) {
                cam_video_hdr_mode_t vhdr = (cam_video_hdr_mode_t)
                        l_meta.find(QCAMERA3_VIDEO_HDR_MODE).data.i32[0];
                rc = setVideoHdrMode(params, vhdr);
                if (rc != NO_ERROR) {
                    LOGE("setVideoHDR is failed");
                }
            }

            if(isDualCamera() && !IS_PP_TYPE_NONE)
            {
                ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
                        CAM_INTF_META_STREAM_INFO, mStreamConfigInfo[config_index]);
                if (m_pFovControl != NULL) {
                    m_pFovControl->setHalPPType(m_halPPType);
                    m_pFovControl->translateInputParams(mParameters, mAuxParameters);
                }
                if(mParameters->is_valid[CAM_INTF_META_STREAM_INFO])
                {
                    void *main_param = POINTER_OF_META(CAM_INTF_META_STREAM_INFO, mParameters);
                    if(main_param)
                    {
                        cam_stream_size_info_t *info = (cam_stream_size_info_t *)main_param;
                        rectifyStreamSizesByCamType(info, CAM_TYPE_MAIN);
                        mStreamConfigInfo[CONFIG_INDEX_MAIN] = *info;
                    }
                }
            } else if (IS_MULTI_CAMERA) {
                if(params->is_valid[CAM_INTF_META_STREAM_INFO])
                {
                    void *main_param = POINTER_OF_META(CAM_INTF_META_STREAM_INFO, params);
                    if(main_param)
                    {
                        cam_stream_size_info_t *info = (cam_stream_size_info_t *)main_param;
                        rectifyStreamSizesByCamType(info,
                            ((config_index == CONFIG_INDEX_AUX)? CAM_TYPE_AUX:CAM_TYPE_MAIN));
                        mStreamConfigInfo[config_index] = *info;
                    }
                }
            }

            // e.g. If we used video HDR in camcorder mode but are not using HDR in picture
            // mode, sensor HDR should be disabled here
            if (!didSetSensorHdr)
                setSensorHDR(params, false, false);
                mShouldSetSensorHdr = false;

            //TODO: validate the arguments, HSV scenemode should have only the
            //advertised fps ranges

            /*set the capture intent, hal version, tintless, stream info,
             *and disenable parameters to the backend*/
            LOGD("set_parms META_STREAM_INFO " );
            for (uint32_t i = 0; i < mStreamConfigInfo[config_index].num_streams; i++) {
                LOGI("STREAM INFO %d: type %d, wxh: %d x %d, pp_mask: 0x%" PRIx64
                        ", Format:%d is_type: %d sync_type %d", config_index,
                        mStreamConfigInfo[config_index].type[i],
                        mStreamConfigInfo[config_index].stream_sizes[i].width,
                        mStreamConfigInfo[config_index].stream_sizes[i].height,
                        mStreamConfigInfo[config_index].postprocess_mask[i],
                        mStreamConfigInfo[config_index].format[i],
                        mStreamConfigInfo[config_index].is_type[i],
                        mStreamConfigInfo[config_index].sync_type);
            }

            rc = mCameraHandle->ops->set_parms(camHdl, params);

            if (rc < 0) {
                LOGE("set_parms failed for hal version, stream info");
            }

            if (isDualCamera() && !IS_PP_TYPE_NONE)
            {
                //Set sync type for AUX camera.
                if(IS_YUV_ZSL)
                {
                    ADD_SET_PARAM_ENTRY_TO_BATCH(mAuxParameters,
                        CAM_INTF_META_STREAM_INFO, mStreamConfigInfo[CONFIG_INDEX_AUX]);
                }
                if (mAuxParameters->is_valid[CAM_INTF_META_STREAM_INFO]) {
                    void *aux_param = POINTER_OF_META(CAM_INTF_META_STREAM_INFO, mAuxParameters);
                    if (aux_param) {
                        cam_stream_size_info_t *info = (cam_stream_size_info_t *)aux_param;
                        info->sync_type = CAM_TYPE_AUX;
                        rectifyStreamSizesByCamType(info, CAM_TYPE_AUX);
                    }

                    cam_stream_size_info_t auxStreamInfo;
                    READ_PARAM_ENTRY(mAuxParameters, CAM_INTF_META_STREAM_INFO, auxStreamInfo);
                    mAuxStreamConfigInfo = auxStreamInfo;
                    for (uint32_t i = 0; i < auxStreamInfo.num_streams; i++) {
                        LOGI("AUX STREAM INFO : type %d, wxh: %d x %d, pp_mask: 0x%" PRIx64
                                ", Format:%d is_type: %d sync_type %d num stream %d",
                                auxStreamInfo.type[i],
                                auxStreamInfo.stream_sizes[i].width,
                                auxStreamInfo.stream_sizes[i].height,
                                auxStreamInfo.postprocess_mask[i],
                                auxStreamInfo.format[i],
                                auxStreamInfo.is_type[i],
                                auxStreamInfo.sync_type,
                                auxStreamInfo.num_streams);
                    }

                    rc = mCameraHandle->ops->set_parms(get_aux_camera_handle(mCameraHandle->camera_handle),
                            mAuxParameters);
                }
            }

            cam_sensor_config_t sensor_dim;
            memset(&sensor_dim, 0, sizeof(sensor_dim));
            if(config_index == CONFIG_INDEX_AUX)
            {
                rc = getSensorOutputSize(sensor_dim, CAM_TYPE_AUX);
            } else {
                rc = getSensorOutputSize(sensor_dim);
                mCropRegionMapper.update(gCamCapability[mCameraId]->active_array_size.width,
                                         gCamCapability[mCameraId]->active_array_size.height,
                                         sensor_dim.width, sensor_dim.height);
            }
            if (rc != NO_ERROR) {
                LOGE("Failed to get sensor output size");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
#ifdef ENABLE_THROTTLE
            mSettingInfo[0].sensorW = sensor_dim.width;
            mSettingInfo[0].sensorH = sensor_dim.height;
            mSettingInfo[0].sensorClk = sensor_dim.opClock;
            mSettingInfo[0].hfr = mHFRMode;
            mSettingInfo[0].tnr = m_bTnrEnabled;
            mSettingInfo[0].fd = false;
            if (meta.exists(ANDROID_STATISTICS_FACE_DETECT_MODE)) {
                mSettingInfo[0].fd =
                        meta.find(ANDROID_STATISTICS_FACE_DETECT_MODE).data.u8[0];
            }
            perfLevel = predictFSM(FSM, &mStreamConfigInfo, &mSettingInfo[0], mSessionId);
#endif
            if(isDualCamera()  && !IS_PP_TYPE_NONE) {
                cam_sensor_config_t sensor_dim_aux;
                memset(&sensor_dim_aux, 0, sizeof(sensor_dim_aux));
                rc = getSensorOutputSize(sensor_dim_aux, CAM_TYPE_AUX);
                if (rc != NO_ERROR) {
                    LOGE("Failed to get sensor output size");
                    pthread_mutex_unlock(&mMutex);
                    goto error_exit;
                }
                ADD_SET_PARAM_ENTRY_TO_BATCH(
                        mParameters, CAM_INTF_PARM_RAW_DIMENSION, sensor_dim);
                ADD_SET_PARAM_ENTRY_TO_BATCH(
                        mAuxParameters, CAM_INTF_PARM_RAW_DIMENSION, sensor_dim_aux);
                ADD_SET_PARAM_ENTRY_TO_BATCH(
                        mParameters, CAM_INTF_META_STREAM_INFO, mStreamConfigInfo[CONFIG_INDEX_MAIN]);
                ADD_SET_PARAM_ENTRY_TO_BATCH(
                        mAuxParameters, CAM_INTF_META_STREAM_INFO, mAuxStreamConfigInfo);
                // Update FOV-control config settings due to the change in the configuration
                rc = m_pFovControl->updateConfigSettings(mParameters, mAuxParameters);
                if (rc != NO_ERROR) {
                    LOGE("Failed to update FOV config settings");
                    pthread_mutex_unlock(&mMutex);
                    goto error_exit;
                } else if (isDualCamera())
                {
                    ADD_SET_PARAM_ENTRY_TO_BATCH(
                            params, CAM_INTF_PARM_RAW_DIMENSION, sensor_dim);
                    ADD_SET_PARAM_ENTRY_TO_BATCH(
                            params, CAM_INTF_PARM_RAW_DIMENSION, sensor_dim_aux);
                    ADD_SET_PARAM_ENTRY_TO_BATCH(
                            params, CAM_INTF_META_STREAM_INFO, mStreamConfigInfo[config_index]);
                }
#ifdef ENABLE_THROTTLE
                mSettingInfo[1].sensorW = sensor_dim_aux.width;
                mSettingInfo[1].sensorH = sensor_dim_aux.height;
                mSettingInfo[1].sensorClk = sensor_dim_aux.opClock;
                mSettingInfo[1].hfr = mSettingInfo[0].hfr;
                mSettingInfo[1].tnr = mSettingInfo[0].tnr;
                mSettingInfo[1].fd = mSettingInfo[0].fd;
                perfLevel = predictFSM(FSM, &mAuxStreamConfigInfo, &mSettingInfo[1], CONFIG_INDEX_AUX);
#endif
            }
#ifdef ENABLE_THROTTLE
            m_thermalAdapter.SetPerfLevel(perfLevel);
#endif

            if(config_index == CONFIG_INDEX_MAIN) {
                mCaptureIntent = l_captureIntent;
            } else if(config_index == CONFIG_INDEX_AUX){
                mAuxCaptureIntent = l_captureIntent;
            }

            if(IS_MULTI_CAMERA)
            {
                config_index++;
                if(camHdl == get_main_camera_handle(mCameraHandle->camera_handle))
                {
                    camHdl = get_aux_camera_handle(mCameraHandle->camera_handle);
                    params = mAuxParameters;
                    if(aux_setting != NULL)
                    {
                       l_meta = auxmeta;
                    } else {
                        aux_setting = logical_setting;
                        auxmeta = aux_setting;
                    }
                } else if(camHdl == get_aux_camera_handle(mCameraHandle->camera_handle))
                {
                    camHdl = get_main_camera_handle(mCameraHandle->camera_handle);
                    params = mParameters;
                    l_meta = meta;
                }
            }
        }while(IS_MULTI_CAMERA && is_aux_configured && (config_index < CONFIG_INDEX_MAX));

        memset(&mBatchedStreamsArray, 0, sizeof(cam_stream_ID_t));

        /* Set batchmode before initializing channel. Since registerBuffer
         * internally initializes some of the channels, better set batchmode
         * even before first register buffer */
        for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end(); it++) {
            QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
            if (((1U << CAM_STREAM_TYPE_VIDEO) == channel->getStreamTypeMask())
                    && mBatchSize) {
                rc = channel->setBatchSize(mBatchSize);
                //Disable per frame map unmap for HFR/batchmode case
                rc |= channel->setPerFrameMapUnmap(false);
                if (NO_ERROR != rc) {
                    LOGE("Channel init failed %d", rc);
                    pthread_mutex_unlock(&mMutex);
                    goto error_exit;
                }
            }
        }

        if (isDualCamera() && !IS_PP_TYPE_NONE) {
            for (auto it = mStreamInfo.begin(); it != mStreamInfo.end(); it++) {
                cam_feature_mask_t pp_mask = 0;
                QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
                for (size_t i = 0; i < mStreamConfigInfo[0].num_streams; i++) {
                    if ( (1U << mStreamConfigInfo[0].type[i]) == channel->getStreamTypeMask() ) {
                        pp_mask = mStreamConfigInfo[0].postprocess_mask[i];
                        channel->overridePPConfig(pp_mask);
                        break;
                    }
                }
            }

            initDCSettings();
            //Trigger deferred job slave session
            setDCDeferCamera(CAM_DEFER_START);
        }

        //First initialize all streams
        for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end(); it++) {
            QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
            if ((((1U << CAM_STREAM_TYPE_VIDEO) == channel->getStreamTypeMask()) ||
               ((1U << CAM_STREAM_TYPE_PREVIEW) == channel->getStreamTypeMask())) &&
               setEis) {
               int  config_index = CONFIG_INDEX_MAIN;
               if(IS_MULTI_CAMERA)
               {
                   if(is_main_configured || is_logical_configured)
                   {
                       config_index = CONFIG_INDEX_MAIN;
                   } else if(is_aux_configured)
                   {
                       config_index = CONFIG_INDEX_AUX;
                   }
               }
               do {
                    bool found = false;
                    for (size_t i = 0; i < mStreamConfigInfo[config_index].num_streams; i++) {
                        if ( (1U << mStreamConfigInfo[config_index].type[i])
                                    == channel->getStreamTypeMask() ) {
                            is_type = mStreamConfigInfo[config_index].is_type[i];
                            found = true;
                            break;
                        }
                    }
                if(found) break;
                if(is_aux_configured)
                    config_index++;
               }while (IS_MULTI_CAMERA && is_aux_configured && (config_index < CONFIG_INDEX_MAX));

                rc = channel->initialize(is_type);
            } else {
                rc = channel->initialize(IS_TYPE_NONE);
            }
            if (NO_ERROR != rc) {
                LOGE("Channel initialization failed %d", rc);
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }

        if (mRawDumpChannel) {
            rc = mRawDumpChannel->initialize(IS_TYPE_NONE);
            if (rc != NO_ERROR) {
                LOGE("Error: Raw Dump Channel init failed");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }
        if (mSupportChannel) {
            rc = mSupportChannel->initialize(IS_TYPE_NONE);
            if (rc < 0) {
                LOGE("Support channel initialization failed");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }
        if (mAnalysisChannel) {
            rc = mAnalysisChannel->initialize(IS_TYPE_NONE);
            if (rc < 0) {
                LOGE("Analysis channel initialization failed");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }
        if (mDummyBatchChannel) {
            rc = mDummyBatchChannel->setBatchSize(mBatchSize);
            if (rc < 0) {
                LOGE("mDummyBatchChannel setBatchSize failed");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
            rc = mDummyBatchChannel->initialize(IS_TYPE_NONE);
            if (rc < 0) {
                LOGE("mDummyBatchChannel initialization failed");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }

        // Set bundle info
        rc = setBundleInfo();
        if (rc < 0) {
            LOGE("setBundleInfo failed %d", rc);
            pthread_mutex_unlock(&mMutex);
            goto error_exit;
        }

        //update settings from app here
        if (meta.exists(QCAMERA3_DUALCAM_LINK_ENABLE)) {
            mIsDeviceLinked = meta.find(QCAMERA3_DUALCAM_LINK_ENABLE).data.u8[0];
            LOGH("Dualcam: setting On=%d id =%d", mIsDeviceLinked, mCameraId);
        }
        if (meta.exists(QCAMERA3_DUALCAM_LINK_IS_MAIN)) {
            mIsMainCamera = meta.find(QCAMERA3_DUALCAM_LINK_IS_MAIN).data.u8[0];
            LOGH("Dualcam: Is this main camera = %d id =%d", mIsMainCamera, mCameraId);
        }
        if (meta.exists(QCAMERA3_DUALCAM_LINK_RELATED_CAMERA_ID)) {
            mLinkedCameraId = meta.find(QCAMERA3_DUALCAM_LINK_RELATED_CAMERA_ID).data.u8[0];
            LOGH("Dualcam: Linked camera Id %d id =%d", mLinkedCameraId, mCameraId);

            if ( (mLinkedCameraId >= MM_CAMERA_MAX_NUM_SENSORS) &&
                (mLinkedCameraId != mCameraId) ) {
                LOGE("Dualcam: mLinkedCameraId %d is invalid, current cam id = %d",
                    mLinkedCameraId, mCameraId);
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }

        // add bundle related cameras
        LOGH("Dualcam: id =%d, mIsDeviceLinked=%d", mCameraId, mIsDeviceLinked);
        if (meta.exists(QCAMERA3_DUALCAM_LINK_ENABLE)) {
            cam_dual_camera_bundle_info_t *m_pRelCamSyncBuf =
                    &(m_pDualCamCmdPtr[0]->bundle_info);
            m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_BUNDLE_INFO;
            if (mIsDeviceLinked) {
                m_pRelCamSyncBuf->sync_control = CAM_SYNC_RELATED_SENSORS_ON;
                m_pRelCamSyncBuf->sync_mechanism = CAM_SYNC_HW_SYNC;
            } else {
                m_pRelCamSyncBuf->sync_control = CAM_SYNC_RELATED_SENSORS_OFF;
                m_pRelCamSyncBuf->sync_mechanism = CAM_SYNC_NO_SYNC;
            }

            pthread_mutex_lock(&gCamLock);

            if (sessionId[mLinkedCameraId] == 0xDEADBEEF) {
                LOGE("Dualcam: Invalid Session Id ");
                pthread_mutex_unlock(&gCamLock);
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }

            if (mIsMainCamera == 1) {
                m_pRelCamSyncBuf->mode = CAM_MODE_PRIMARY;
                m_pRelCamSyncBuf->type = CAM_TYPE_MAIN;
                m_pRelCamSyncBuf->sync_3a_config =
                        {CAM_3A_SYNC_FOLLOW, CAM_3A_SYNC_FOLLOW};
                m_pRelCamSyncBuf->cam_role = CAM_ROLE_BAYER;
                // related session id should be session id of linked session
                m_pRelCamSyncBuf->related_sensor_session_id = sessionId[mLinkedCameraId];
            } else {
                m_pRelCamSyncBuf->mode = CAM_MODE_SECONDARY;
                m_pRelCamSyncBuf->type = CAM_TYPE_AUX;
                m_pRelCamSyncBuf->sync_3a_config =
                        {CAM_3A_SYNC_FOLLOW, CAM_3A_SYNC_FOLLOW};
                m_pRelCamSyncBuf->cam_role = CAM_ROLE_MONO;
                m_pRelCamSyncBuf->related_sensor_session_id = sessionId[mLinkedCameraId];
            }
            pthread_mutex_unlock(&gCamLock);

            rc = mCameraHandle->ops->set_dual_cam_cmd(
                    mCameraHandle->camera_handle);
            if (rc < 0) {
                LOGE("Dualcam: link failed");
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }

       //Then start them.
       LOGH("Start META Channel");
       rc = mMetadataChannel->start();
       if (rc < 0) {
           LOGE("META channel start failed");
           pthread_mutex_unlock(&mMutex);
           goto error_exit;
       }

       if (mAnalysisChannel) {
           rc = mAnalysisChannel->start();
           if (rc < 0) {
               LOGE("Analysis channel start failed");
               mMetadataChannel->stop();
               pthread_mutex_unlock(&mMutex);
               goto error_exit;
           }
       }

        if (mSupportChannel) {
            rc = mSupportChannel->start();
            if (rc < 0) {
                LOGE("Support channel start failed");
                mMetadataChannel->stop();
                /* Although support and analysis are mutually exclusive today
                   adding it in anycase for future proofing */
                if (mAnalysisChannel) {
                    mAnalysisChannel->stop();
                }
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }
        for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end(); it++) {
            QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
            LOGH("Start Processing Channel mask=%d",
                     channel->getStreamTypeMask());
            if(channel)
            {
                rc = channel->start();
                if (rc < 0) {
                    LOGE("channel start failed");
                    pthread_mutex_unlock(&mMutex);
                    goto error_exit;
                }
            }
        }

        if (mRawDumpChannel) {
            LOGD("Starting raw dump stream");
            rc = mRawDumpChannel->start();
            if (rc != NO_ERROR) {
                LOGE("Error Starting Raw Dump Channel");
                for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
                      it != mStreamInfo.end(); it++) {
                    QCamera3Channel *channel =
                        (QCamera3Channel *)(*it)->stream->priv;
                    LOGH("Stopping Processing Channel mask=%d",
                        channel->getStreamTypeMask());
                    channel->stop();
                }
                if (mSupportChannel)
                    mSupportChannel->stop();
                if (mAnalysisChannel) {
                    mAnalysisChannel->stop();
                }
                mMetadataChannel->stop();
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }

        if (mChannelHandle) {

            rc = mCameraHandle->ops->start_channel(mCameraHandle->camera_handle,
                    mChannelHandle);
            if (rc != NO_ERROR) {
                LOGE("start_channel failed %d", rc);
                pthread_mutex_unlock(&mMutex);
                goto error_exit;
            }
        }

        if (isDualCamera()) {
            switchMaster(mMasterCamera);
            setDCDeferCamera(CAM_DEFER_PROCESS);
        }
        goto no_error;
error_exit:
        mPerfLockMgr.releasePerfLock(PERF_LOCK_START_PREVIEW);
        return rc;
no_error:
        mPendingLiveRequest = 0;
        mFirstConfiguration = false;
        if (mZSLChannel != NULL) {
            mZSLChannel->startDeferredAllocation();
        }
    }
    if ((mState == STARTED)&&(mStreamOnPending)){
        rc = startAllChannels();
        if (rc < 0) {
            LOGE("startAllChannels failed");
            pthread_mutex_unlock(&mMutex);
            return rc;
        }
        if (mChannelHandle) {
            mCameraHandle->ops->start_channel(mCameraHandle->camera_handle,
                        mChannelHandle);
            if (rc < 0) {
                LOGE("start_channel failed");
                pthread_mutex_unlock(&mMutex);
                return rc;
            }
        }
        mStreamOnPending = false;
    }

    uint32_t frameNumber = request->frame_number;
    cam_stream_ID_t streamsArray, streamsArraySlave;

    if (mFlushPerf) {
        //we cannot accept any requests during flush
        LOGE("process_capture_request cannot proceed during flush");
        pthread_mutex_unlock(&mMutex);
        return NO_ERROR; //should return an error
    }

    if (meta.exists(ANDROID_REQUEST_ID)) {
        request_id = meta.find(ANDROID_REQUEST_ID).data.i32[0];
        mCurrentRequestId = request_id;
        LOGD("Received request with id: %d", request_id);
    } else if (mState == CONFIGURED || mCurrentRequestId == -1){
        LOGE("Unable to find request id field, \
                & no previous id available");
        pthread_mutex_unlock(&mMutex);
        return NAME_NOT_FOUND;
    } else {
        LOGD("Re-using old request id");
        request_id = mCurrentRequestId;
    }

    LOGH("num_output_buffers = %d input_buffer = %p frame_number = %d",
                                    request->num_output_buffers,
                                    request->input_buffer,
                                    frameNumber);

    if (m_bStopPicChannel && mPictureChannel && !m_bIs4KVideo) {
        mPictureChannel->stopChannel();
        m_bStopPicChannel = false;
    }
    if (isDualCamera() && !IS_PP_TYPE_NONE) {
        fov_control_result_t fovControlResult = m_pFovControl->getFovControlResult();
        if (fovControlResult.isValid) {
            setDCControls(fovControlResult.camMasterPreview, fovControlResult.activeCameras,
                    fovControlResult.snapshotPostProcess, fovControlResult.fallback);
        }
    }else if(IS_MULTI_CAMERA && is_requested_on_aux) {
        if (auxmeta.exists(ANDROID_REQUEST_ID)) {
            aux_request_id = auxmeta.find(ANDROID_REQUEST_ID).data.i32[0];
            mAuxCurrentRequestId = aux_request_id;
            LOGD("Received request with id: %d", aux_request_id);
        } else if (mState == CONFIGURED || mAuxCurrentRequestId == -1){
            LOGE("Unable to find request id field, \
                    & no previous id available");
            pthread_mutex_unlock(&mMutex);
            return NAME_NOT_FOUND;
        } else {
            LOGD("Re-using old request id");
            aux_request_id = mAuxCurrentRequestId;
        }
    }

    // Acquire all request buffers first
    streamsArray.num_streams = 0;
    streamsArraySlave.num_streams = 0;
    int blob_request = 0;
    bool  is_blob_on_aux = 0;
    uint32_t snapshotStreamId = 0;
    uint32_t snapshotStreamIdSlave = 0;
    bool needSyncFrame = false;
    bool req_on_yuv_main = false;
    bool req_on_yuv_aux = false;
    uint8_t is_request_on_zsl = FALSE;
    for (size_t i = 0; i < request->num_output_buffers; i++) {
        cam_stream_ID_t *streamArray = &streamsArray;
        const camera3_stream_buffer_t& output = request->output_buffers[i];
        QCamera3Channel *channel = (QCamera3Channel *)output.stream->priv;

        if (channel == NULL) {
            ALOGE("%s: invalid channel pointer for stream", __func__);
            continue;
        }

        if(IS_MULTI_CAMERA && is_requested_on_aux)
        {
            if(channel->getMyHandle() == get_aux_camera_handle(mChannelHandle))
            {
                streamArray = &streamsArraySlave;
            }
        }

        if(IS_EQUAL(channel, mZSLChannel) || (IS_VALID_PTR(mZSLChannel) &&
            (((QCamera3Channel *)mZSLChannel)->getAuxHandle() == channel))
            || ((IS_YUV_ZSL) && IS_EQUAL(channel, mPictureChannel)))
        {
            is_request_on_zsl = TRUE;
            if((((QCamera3Channel *)mZSLChannel)->getAuxHandle() == channel))
            {
                streamArray = &streamsArraySlave;
            }

            if(IS_YUV_ZSL)
            {
                if(IS_EQUAL(channel, mZSLChannel)) {
                    req_on_yuv_main = true;
                } else if(IS_EQUAL(((QCamera3Channel *)mZSLChannel)->getAuxHandle(),channel))
                {
                    req_on_yuv_aux = true;
                }

                if(req_on_yuv_main && req_on_yuv_aux)
                {
                    needSyncFrame = true;
                }
            } else {
                needSyncFrame = false;
            }
        }

        if(IS_YUV_ZSL) {
            if(isDualCamera()
                && IS_EQUAL(channel, mPictureChannel)
                && IS_EQUAL(request->input_buffer,NULL)
                && (mMasterCamera == CAM_TYPE_AUX))
            {
                streamArray = &streamsArraySlave;
                is_request_on_zsl = TRUE;
            }
        }

        if (output.stream->format == HAL_PIXEL_FORMAT_BLOB) {
            //FIXME??:Call function to store local copy of jpeg data for encode params.
            if (m_bIsVideo && !m_bStopPicChannel && !m_bIs4KVideo) {
                rc = mPictureChannel->startChannel();
                if (rc != NO_ERROR) {
                    LOGE("startchannel is failed for Pic channel %d", rc);
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }
            }
            if(IS_MULTI_CAMERA &&
                (channel->getMyHandle() == get_aux_camera_handle(mChannelHandle)))
            {
                is_blob_on_aux = true;
            }
            blob_request = 1;
            snapshotStreamId = channel->getStreamID(channel->getStreamTypeMask());
        }

        if (output.acquire_fence != -1) {
           rc = sync_wait(output.acquire_fence, TIMEOUT_NEVER);
           close(output.acquire_fence);
           if (rc != OK) {
              LOGE("sync wait failed %d", rc);
              pthread_mutex_unlock(&mMutex);
              return rc;
           }
        }

        if (blob_request) {
            if(request->input_buffer != NULL)
            {
                configureHalPostProcess(true);
            } else {
                configureHalPostProcess(false);
            }
        }

        if (isDualCamera() && needHALPP() &&
                              (output.stream->format == HAL_PIXEL_FORMAT_BLOB)) {
            uint32_t snapshotStreamIdAux = channel->getAuxStreamID();
            if (mMasterCamera == CAM_TYPE_MAIN) {
                    snapshotStreamIdSlave = snapshotStreamIdAux;
            } else {
                    snapshotStreamIdSlave = snapshotStreamId;
                    snapshotStreamId = snapshotStreamIdAux;
            }
            LOGH("snapshotStreamId %d snapshotStreamIdAux %d", snapshotStreamId,
                                                               snapshotStreamIdAux);
            streamsArraySlave.stream_request[streamsArraySlave.num_streams].streamID =
                                                                    snapshotStreamIdSlave;
            streamsArraySlave.stream_request[streamsArraySlave.num_streams++].buf_index =
                                                                        CAM_FREERUN_IDX;
        }

        if (output.stream->format == HAL_PIXEL_FORMAT_BLOB) {
            if(request->input_buffer == NULL) {
                streamArray->stream_request[streamArray->num_streams++].streamID =
                                                                 snapshotStreamId;
            }
        } else {
            if( !(request->input_buffer != NULL) || (request->num_output_buffers > 1)) {
            streamArray->stream_request[streamArray->num_streams++].streamID =
                           channel->getStreamID(channel->getStreamTypeMask());
            }
        }

        if ((1U << CAM_STREAM_TYPE_VIDEO) == channel->getStreamTypeMask()) {
            isVidBufRequested = true;
        }
        if (((output.stream->format) == HAL_PIXEL_FORMAT_YCbCr_420_888) ||
                ((output.stream->format) == HAL_PIXEL_FORMAT_BLOB)) {
            if(request->input_buffer != NULL) {
                camera3_stream_buffer_t *inputbuf;
                inputbuf = request->input_buffer;
                if((inputbuf->stream->format == HAL_PIXEL_FORMAT_RAW_OPAQUE) ||
                        (inputbuf->stream->format == HAL_PIXEL_FORMAT_RAW16) ||
                        (inputbuf->stream->format == HAL_PIXEL_FORMAT_RAW10)) {
                    LOGH("Use offline ISP for input RAW format: %d", inputbuf->stream->format);
                    m_bOfflineIsp = true;
                } else {
                    m_bOfflineIsp = false;
                }
            } else if (m_bQuadraCfaRequest && !m_bInSensorQCFA) {
                m_bOfflineIsp = true;
                m_ppChannelCnt = 2;
            } else {
                m_bOfflineIsp = false;
                m_ppChannelCnt = 1;
            }
        }
    }

    //FIXME: Add checks to ensure to dups in validateCaptureRequest
    for (auto itr = internallyRequestedStreams.begin(); itr != internallyRequestedStreams.end();
          itr++) {
        QCamera3Channel *channel = (QCamera3Channel *)(*itr).stream->priv;
        cam_stream_ID_t *streamArray = &streamsArray;

        if(IS_YUV_ZSL) {
            if(isDualCamera()
                && IS_EQUAL(channel, mPictureChannel)
                && IS_EQUAL(request->input_buffer,NULL)
                && (mMasterCamera == CAM_TYPE_AUX))
            {
                streamArray = &streamsArraySlave;
            }
        }
        streamArray->stream_request[streamArray->num_streams++].streamID =
            channel->getStreamID(channel->getStreamTypeMask());

        if ((1U << CAM_STREAM_TYPE_VIDEO) == channel->getStreamTypeMask()) {
            isVidBufRequested = true;
        }
    }

    if (blob_request) {
        LOGI("[KPI Perf] : PROFILE_SNAPSHOT_REQUEST_RECEIVED");
        KPI_ATRACE_CAMSCOPE_INT("SNAPSHOT", CAMSCOPE_HAL3_SNAPSHOT, 1);

        if (isDualCamera() && (getHalPPType() == CAM_HAL_PP_TYPE_BOKEH) && needHALPP()) {
            mPerfLockMgr.acquirePerfLock(PERF_LOCK_BOKEH_SNAPSHOT,
                    PERF_LOCK_BOKEH_SNAP_TIMEOUT_MS);
        } else {
            mPerfLockMgr.acquirePerfLock(PERF_LOCK_TAKE_SNAPSHOT);
        }
    }
    if (blob_request && mRawDumpChannel) {
        LOGD("Trigger Raw based on blob request if Raw dump is enabled");
        streamsArray.stream_request[streamsArray.num_streams].streamID =
            mRawDumpChannel->getStreamID(mRawDumpChannel->getStreamTypeMask());
        streamsArray.stream_request[streamsArray.num_streams++].buf_index = CAM_FREERUN_IDX;
        if(needHALPP()){
            streamsArraySlave.stream_request[streamsArraySlave.num_streams].streamID =
                mRawDumpChannel->getStreamID(mRawDumpChannel->getStreamTypeMask());
            streamsArraySlave.stream_request[streamsArraySlave.num_streams++].buf_index =
                                                                            CAM_FREERUN_IDX;
       }
    }

    //Induce HAL ZSL request for every alternate frame (max 15fps)
    if (!is_request_on_zsl && (mZSLChannel != NULL) && (frameNumber % 2 == 0) &&
            (internallyRequestedStreams.size() == 0) && mZSLChannel->mAllocDone) {
        streamsArray.stream_request[streamsArray.num_streams].streamID =
            mZSLChannel->getStreamID(mZSLChannel->getStreamTypeMask());
        streamsArray.stream_request[streamsArray.num_streams++].buf_index = CAM_FREERUN_IDX;
        if(isDualCamera() && !IS_MULTI_CAMERA)
        {
            bool is_aux_needed = false;
            if (IS_HAL_PP_TYPE_BOKEH
                || (IS_YUV_ZSL
                && getHalPPType() == CAM_HAL_PP_TYPE_SAT))
            {
                is_aux_needed = true;
            }

            if(is_aux_needed)
            {
                streamsArraySlave.stream_request[streamsArraySlave.num_streams].streamID =
                    mZSLChannel->getStreamID(mZSLChannel->getStreamTypeMask());
                streamsArraySlave.stream_request[streamsArraySlave.num_streams++].buf_index =
                    CAM_FREERUN_IDX;
            }
        }
    }

    if(request->input_buffer == NULL) {
        /* Parse the settings:
         * - For every request in NORMAL MODE
         * - For every request in HFR mode during preview only case
         * - For first request of every batch in HFR mode during video
         * recording. In batchmode the same settings except frame number is
         * repeated in each request of the batch.
         */
        if (!mBatchSize ||
           (mBatchSize && !isVidBufRequested) ||
           (mBatchSize && isVidBufRequested && !mToBeQueuedVidBufs)) {
            cam_stream_ID_t *sArray = &streamsArray;
            int loop = CONFIG_INDEX_MAIN;
            metadata_buffer_t *params = mParameters;
            const camera_metadata_t *settings = request->settings;
            int blob = blob_request && !is_blob_on_aux;
            uint32_t blob_stream_id = snapshotStreamId;
            if(IS_MULTI_CAMERA)
            {
                if(!(is_main_configured || is_logical_configured))
                {
                    if(is_aux_configured)
                    {
                        sArray = &streamsArraySlave;
                        settings = aux_setting;
                        params = mAuxParameters;
                        blob = is_blob_on_aux;
                        loop = CONFIG_INDEX_AUX;
                    }
                }
            }
            do {
                rc = setFrameParameters(settings, *sArray, blob,
                    ((blob) ? blob_stream_id : 0), params, request);

                if (rc < 0) {
                    LOGE("fail to set frame parameters");
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }

                if(is_aux_configured && loop != CONFIG_INDEX_AUX)
                {
                    sArray = &streamsArraySlave;
                    settings = aux_setting;
                    params = mAuxParameters;
                    blob = is_blob_on_aux;
                }

                loop ++;
            } while(IS_MULTI_CAMERA && is_aux_configured && (loop < CONFIG_INDEX_MAX));
        }
        /* For batchMode HFR, setFrameParameters is not called for every
         * request. But only frame number of the latest request is parsed.
         * Keep track of first and last frame numbers in a batch so that
         * metadata for the frame numbers of batch can be duplicated in
         * handleBatchMetadta */
        if (mBatchSize) {
            if (!mToBeQueuedVidBufs) {
                //start of the batch
                mFirstFrameNumberInBatch = request->frame_number;
            }
            if(ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
                CAM_INTF_META_FRAME_NUMBER, request->frame_number)) {
                LOGE("Failed to set the frame number in the parameters");
                pthread_mutex_unlock(&mMutex);
                return BAD_VALUE;
            }
        }
        if (mNeedSensorRestart) {
            /* Unlock the mutex as restartSensor waits on the channels to be
             * stopped, which in turn calls stream callback functions -
             * handleBufferWithLock and handleMetadataWithLock */
            pthread_mutex_unlock(&mMutex);
            rc = dynamicUpdateMetaStreamInfo();
            if (rc != NO_ERROR) {
                LOGE("Restarting the sensor failed");
                return BAD_VALUE;
            }
            mNeedSensorRestart = false;
            pthread_mutex_lock(&mMutex);
        }
        if(mResetInstantAEC) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
                    CAM_INTF_PARM_INSTANT_AEC, (uint8_t)CAM_AEC_NORMAL_CONVERGENCE);
            mResetInstantAEC = false;
        }
    } else if (request->input_buffer != NULL) {

        if (request->input_buffer->acquire_fence != -1) {
           rc = sync_wait(request->input_buffer->acquire_fence, TIMEOUT_NEVER);
           close(request->input_buffer->acquire_fence);
           if (rc != OK) {
              LOGE("input buffer sync wait failed %d", rc);
              pthread_mutex_unlock(&mMutex);
              return rc;
           }
        }
    }

    if (mCaptureIntent == ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM) {
        mLastCustIntentFrmNum = frameNumber;
    }
    /* Update pending request list and pending buffers map */
    PendingRequestInfo pendingRequest;
    pendingRequestIterator latestRequest;
    pendingRequest.frame_number = frameNumber;
    pendingRequest.num_buffers = request->num_output_buffers;
    pendingRequest.request_id = request_id;
    pendingRequest.aux_request_id = aux_request_id;
    pendingRequest.blob_request = blob_request;
    pendingRequest.timestamp = 0;
    pendingRequest.bUrgentReceived = 0;
    if (request->input_buffer) {
        pendingRequest.input_buffer =
                (camera3_stream_buffer_t*)malloc(sizeof(camera3_stream_buffer_t));
        *(pendingRequest.input_buffer) = *(request->input_buffer);
        pInputBuffer = pendingRequest.input_buffer;
    } else {
       pendingRequest.input_buffer = NULL;
       pInputBuffer = NULL;
    }

    pendingRequest.pipeline_depth = 0;
    pendingRequest.partial_result_cnt = 0;
    extractJpegMetadata(mCurJpegMeta, request);
    pendingRequest.jpegMetadata = mCurJpegMeta;
    pendingRequest.settings = saveRequestSettings(mCurJpegMeta, request);
    pendingRequest.shutter_notified = false;
    pendingRequest.scene_mode = mCurrentSceneMode;
    pendingRequest.requested_on_main = is_requested_on_main;
    pendingRequest.requested_on_aux = is_requested_on_aux;
    pendingRequest.requested_logical = is_logical_request;
    pendingRequest.received_main_meta = false;
    pendingRequest.received_aux_meta = false;

    pendingRequest.main_meta = NULL;
    pendingRequest.aux_meta = NULL;

    //extract capture intent
    if (meta.exists(ANDROID_CONTROL_CAPTURE_INTENT)) {
        mCaptureIntent =
                meta.find(ANDROID_CONTROL_CAPTURE_INTENT).data.u8[0];
    }

    if(IS_MULTI_CAMERA && is_requested_on_aux)
    {
        if (auxmeta.exists(ANDROID_CONTROL_CAPTURE_INTENT)) {
        mAuxCaptureIntent =
                auxmeta.find(ANDROID_CONTROL_CAPTURE_INTENT).data.u8[0];
        }
    }

    pendingRequest.capture_intent = mCaptureIntent;
    pendingRequest.aux_capture_intent = mAuxCaptureIntent;

    pendingRequest.enableZSL = false;
    if (meta.exists(ANDROID_CONTROL_ENABLE_ZSL)) {
        pendingRequest.enableZSL = meta.find(ANDROID_CONTROL_ENABLE_ZSL).data.u8[0];
    }

    //extract CAC info
    if(IS_MULTI_CAMERA && is_requested_on_aux )
    {
        if (auxmeta.exists(ANDROID_COLOR_CORRECTION_ABERRATION_MODE)) {
            mAuxCacMode =
                    auxmeta.find(ANDROID_COLOR_CORRECTION_ABERRATION_MODE).data.u8[0];
        }
    }
    pendingRequest.fwkCacMode = mCacMode;
    pendingRequest.fwkAuxCacMode = mAuxCacMode;
    PendingBuffersInRequest bufsForCurRequest;
    bufsForCurRequest.frame_number = frameNumber;
    // Mark current timestamp for the new request
    List<PendingBuffersInRequest>::iterator bufsForPrevRequest;
    if ( mPendingBuffersMap.mPendingBuffersInRequest.size() > 0 ) {
        bufsForPrevRequest = mPendingBuffersMap.mPendingBuffersInRequest.end();
        bufsForPrevRequest --;
        if ( systemTime(CLOCK_MONOTONIC) > bufsForPrevRequest->timestamp ) {
           bufsForCurRequest.timestamp = systemTime(CLOCK_MONOTONIC);
        } else {
           bufsForCurRequest.timestamp = bufsForPrevRequest->timestamp;
        }
     } else {
        bufsForCurRequest.timestamp = systemTime(CLOCK_MONOTONIC);
    }

    if (meta.exists(ANDROID_SENSOR_EXPOSURE_TIME)) {
        int64_t sensorExpTime =
               meta.find(ANDROID_SENSOR_EXPOSURE_TIME).data.i64[0];
        bufsForCurRequest.timestamp += sensorExpTime;
    }

    for (size_t i = 0; i < request->num_output_buffers; i++) {
        RequestedBufferInfo requestedBuf;
        memset(&requestedBuf, 0, sizeof(requestedBuf));
        requestedBuf.stream = request->output_buffers[i].stream;
        requestedBuf.buffer = NULL;
        pendingRequest.buffers.push_back(requestedBuf);

        // Add to buffer handle the pending buffers list
        PendingBufferInfo bufferInfo;
        bufferInfo.buffer = request->output_buffers[i].buffer;
        bufferInfo.stream = request->output_buffers[i].stream;
        bufsForCurRequest.mPendingBufferList.push_back(bufferInfo);
        QCamera3Channel *channel = (QCamera3Channel *)bufferInfo.stream->priv;
        LOGD("frame = %d, buffer = %p, streamTypeMask = %d, stream format = %d",
            frameNumber, bufferInfo.buffer,
            channel->getStreamTypeMask(), bufferInfo.stream->format);
    }
    // Add this request packet into mPendingBuffersMap
    if(request->num_output_buffers > 0)
    {
        mPendingBuffersMap.mPendingBuffersInRequest.push_back(bufsForCurRequest);
        LOGD("mPendingBuffersMap.num_overall_buffers = %d",
            mPendingBuffersMap.get_num_overall_buffers());
    }

    latestRequest = mPendingRequestsList.insert(
            mPendingRequestsList.end(), pendingRequest);
    if(mFlush) {
        LOGI("mFlush is true");
        pthread_mutex_unlock(&mMutex);
        return NO_ERROR;
    }

    int indexUsed;
    // Notify metadata channel we receive a request
    mMetadataChannel->request(NULL, frameNumber, indexUsed);

    if(request->input_buffer != NULL){
        LOGD("Input request, frame_number %d", frameNumber);
        rc = setReprocParameters(request, &mReprocMeta, snapshotStreamId);
        if (NO_ERROR != rc) {
            LOGE("fail to set reproc parameters");
            pthread_mutex_unlock(&mMutex);
            return rc;
        }
    }

    // Call request on other streams
    uint32_t streams_need_metadata = 0;
    bool isZSLCapture = false;
    pendingBufferIterator pendingBufferIter = latestRequest->buffers.begin();
    for (size_t i = 0; i < request->num_output_buffers; i++) {
        metadata_buffer_t *m_params = mParameters;
        cam_stream_ID_t *streamArray = &streamsArray;
        const camera3_stream_buffer_t& output = request->output_buffers[i];
        QCamera3Channel *channel = (QCamera3Channel *)output.stream->priv;

        if (channel == NULL) {
            LOGW("invalid channel pointer for stream");
            continue;
        }

        if(IS_MULTI_CAMERA && is_aux_configured && is_requested_on_aux)
        {
            if(channel->getMyHandle() == get_aux_camera_handle(mChannelHandle))
            {
                m_params = mAuxParameters;
                streamArray = &streamsArraySlave;
            }
        } else if((mZSLChannel != NULL)
                && (((QCamera3Channel *)mZSLChannel)->getAuxHandle() == channel))
        {
                //request is on aux handle in non-multicamera
                streamArray = &streamsArraySlave;
        }

        if(IS_YUV_ZSL) {
            if(isDualCamera()
                && IS_EQUAL(channel, mPictureChannel)
                && IS_EQUAL(request->input_buffer,NULL)
                && (mMasterCamera == CAM_TYPE_AUX))
            {
                streamArray = &streamsArraySlave;
            }
        }

        if (output.stream->format == HAL_PIXEL_FORMAT_BLOB
                && output.stream->data_space != HAL_DATASPACE_DEPTH) {
            LOGD("snapshot request with output buffer %p, input buffer %p, frame_number %d",
                      output.buffer, request->input_buffer, frameNumber);
            if(request->input_buffer != NULL){
                rc = channel->request(output.buffer, frameNumber,
                        pInputBuffer, &mReprocMeta, indexUsed, false, false);
                if (rc < 0) {
                    LOGE("Fail to request on picture channel");
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }
            } else {
                if (channel->isZSLChannel()) {
                    isZSLCapture = needZSLCapture(request);
                    pendingBufferIter->isZSL = isZSLCapture;
                }
                LOGD("snapshot request with buffer %p, frame_number %d isZSLCapture %d",
                         output.buffer, frameNumber, isZSLCapture);
                if (m_bQuadraCfaRequest) {
                    LOGH("blob request for quadra cfa raw size");
                    rc = channel->request(output.buffer, frameNumber, NULL, m_params, indexUsed);
                    continue;
                }

                if (!request->settings) {
                    rc = channel->request(output.buffer, frameNumber,
                            NULL, mPrevParameters, indexUsed, false, false, isZSLCapture);
                } else {
                    rc = channel->request(output.buffer, frameNumber,
                            NULL, mParameters, indexUsed, false, false, isZSLCapture);
                }
                if (rc < 0) {
                    LOGE("Fail to request on picture channel");
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }

                uint32_t streamId = channel->getStreamID(channel->getStreamTypeMask());
                if (needHALPP())
                {
                    //stream id should be of (mMasterCam == CAM_TYPE_MAIN)
                    streamId = snapshotStreamId;
                }
                uint32_t j = 0;
                for (j = 0; j < streamArray->num_streams; j++) {
                    if (streamArray->stream_request[j].streamID == streamId) {
                      if (mOpMode == CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE)
                          streamArray->stream_request[j].buf_index = CAM_FREERUN_IDX;
                      else
                          streamArray->stream_request[j].buf_index = indexUsed;
                        break;
                    }
                }
                if (j == streamArray->num_streams) {
                    LOGE("Did not find matching stream to update index");
                    assert(0);
                }

                if (!channel->isZSLChannel()) {
                    pendingBufferIter->need_metadata = true;
                    streams_need_metadata++;
                }
            }
        } else if (output.stream->format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
            bool needMetadata = false;
            if (channel->isZSLChannel()) {
                isZSLCapture = needZSLCapture(request);
                pendingBufferIter->isZSL = isZSLCapture;
            }
            QCamera3YUVChannel *yuvChannel = (QCamera3YUVChannel *)channel;
            rc = yuvChannel->request(output.buffer, frameNumber,
                    pInputBuffer, (pInputBuffer ? &mReprocMeta : m_params),
                    needMetadata, indexUsed, false,false, isZSLCapture, needSyncFrame);
            if (rc < 0) {
                LOGE("Fail to request on YUV channel");
                pthread_mutex_unlock(&mMutex);
                return rc;
            }

            uint32_t streamId = channel->getStreamID(channel->getStreamTypeMask());
            uint32_t j = 0;
            for (j = 0; j < streamArray->num_streams; j++) {
                if (streamArray->stream_request[j].streamID == streamId) {
                    if (mOpMode == CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE)
                        streamArray->stream_request[j].buf_index = CAM_FREERUN_IDX;
                    else
                        streamArray->stream_request[j].buf_index = indexUsed;
                    break;
                }
            }
            if (j == streamArray->num_streams) {
                LOGE("Did not find matching stream to update index");
                assert(0);
            }

            pendingBufferIter->need_metadata = needMetadata;
            if (needMetadata)
                streams_need_metadata += 1;
            LOGD("calling YUV channel request, need_metadata is %d",
                     needMetadata);
        } else {
            LOGD("request with buffer %p, frame_number %d",
                  output.buffer, frameNumber);

            rc = channel->request(output.buffer, frameNumber, indexUsed);

            uint32_t streamId = channel->getStreamID(channel->getStreamTypeMask());
            uint32_t j = 0;
            for (j = 0; j < streamArray->num_streams; j++) {
                if (streamArray->stream_request[j].streamID == streamId) {
                    if (mOpMode == CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE)
                        streamArray->stream_request[j].buf_index = CAM_FREERUN_IDX;
                    else
                        streamArray->stream_request[j].buf_index = indexUsed;
                    break;
                }
            }
            if (j == streamArray->num_streams) {
                LOGE("Did not find matching stream to update index");
                assert(0);
            }

            if (((1U << CAM_STREAM_TYPE_VIDEO) == channel->getStreamTypeMask())
                    && mBatchSize) {
                mToBeQueuedVidBufs++;
                if (mToBeQueuedVidBufs == mBatchSize) {
                    channel->queueBatchBuf();
                }
            }
            if (rc < 0) {
                LOGE("request failed");
                pthread_mutex_unlock(&mMutex);
                return rc;
            }
        }
        pendingBufferIter++;
    }

    for (auto itr = internallyRequestedStreams.begin(); itr != internallyRequestedStreams.end();
          itr++) {
        QCamera3Channel *channel = (QCamera3Channel *)(*itr).stream->priv;

        if (channel == NULL) {
            LOGE("invalid channel pointer for stream");
            assert(0);
            return BAD_VALUE;
        }

        InternalRequest requestedStream;
        requestedStream = (*itr);
        cam_stream_ID_t *streamArray = &streamsArray;

        if(IS_YUV_ZSL) {
            if(isDualCamera()
                && IS_EQUAL(channel, mPictureChannel)
                && IS_EQUAL(request->input_buffer,NULL)
                && (mMasterCamera == CAM_TYPE_AUX))
            {
                streamArray = &streamsArraySlave;
            }
        }


        if ((*itr).stream->format == HAL_PIXEL_FORMAT_BLOB) {
            LOGD("snapshot request internally input buffer %p, frame_number %d",
                      request->input_buffer, frameNumber);
            if(request->input_buffer != NULL){
                rc = channel->request(NULL, frameNumber,
                        pInputBuffer, &mReprocMeta, indexUsed, true, requestedStream.meteringOnly);
                if (rc < 0) {
                    LOGE("Fail to request on picture channel");
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }
            } else {
                LOGD("snapshot request with frame_number %d", frameNumber);
                if (!request->settings) {
                    rc = channel->request(NULL, frameNumber,
                            NULL, mPrevParameters, indexUsed, true,
                            requestedStream.meteringOnly, requestedStream.needPastFrame);
                } else {
                    rc = channel->request(NULL, frameNumber,
                            NULL, mParameters, indexUsed, true,
                            requestedStream.meteringOnly, requestedStream.needPastFrame);
                }
                if (rc < 0) {
                    LOGE("Fail to request on picture channel");
                    pthread_mutex_unlock(&mMutex);
                    return rc;
                }

                if (((*itr).meteringOnly != 1) && !channel->isZSLChannel()) {
                    requestedStream.need_metadata = 1;
                    streams_need_metadata++;
                }
            }

            uint32_t streamId = channel->getStreamID(channel->getStreamTypeMask());
            if (needHALPP())
            {
                //stream id should be of (mMasterCam == CAM_TYPE_MAIN)
                streamId = snapshotStreamId;
            }
            uint32_t j = 0;
            for (j = 0; j < streamArray->num_streams; j++) {
                if (streamArray->stream_request[j].streamID == streamId) {
                  if (mOpMode == CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE)
                      streamArray->stream_request[j].buf_index = CAM_FREERUN_IDX;
                  else
                      streamArray->stream_request[j].buf_index = indexUsed;
                    break;
                }
            }
            if (j == streamArray->num_streams) {
                LOGE("Did not find matching stream to update index");
                assert(0);
            }

        } else {
            LOGE("Internal requests not supported on this stream type");
            assert(0);
            return INVALID_OPERATION;
        }
        latestRequest->internalRequestList.push_back(requestedStream);
    }

    //If 2 streams have need_metadata set to true, fail the request, unless
    //we copy/reference count the metadata buffer
    if (streams_need_metadata > 1) {
        LOGE("not supporting request in which two streams requires"
                " 2 HAL metadata for reprocessing");
        pthread_mutex_unlock(&mMutex);
        return -EINVAL;
    }

    //if one o/p and one i/p i.e. reprocess request.
    uint8_t reproc_req = ((request->num_output_buffers == 1) && (request->input_buffer != NULL));

    if(!reproc_req || (internallyRequestedStreams.size()))
    {
        /* Set the parameters to backend:
         * - For every request in NORMAL MODE
         * - For every request in HFR mode during preview only case
         * - Once every batch in HFR mode during video recording
         */
        if (!mBatchSize ||
           (mBatchSize && !isVidBufRequested) ||
           (mBatchSize && isVidBufRequested && (mToBeQueuedVidBufs == mBatchSize))) {
            LOGD("set_parms  batchSz: %d IsVidBufReq: %d vidBufTobeQd: %d ",
                     mBatchSize, isVidBufRequested,
                    mToBeQueuedVidBufs);

            if(mBatchSize && isVidBufRequested && (mToBeQueuedVidBufs == mBatchSize)) {
                for (uint32_t k = 0; k < streamsArray.num_streams; k++) {
                    uint32_t m = 0;
                    for (m = 0; m < mBatchedStreamsArray.num_streams; m++) {
                        if (streamsArray.stream_request[k].streamID ==
                                mBatchedStreamsArray.stream_request[m].streamID)
                            break;
                        }
                        if (m == mBatchedStreamsArray.num_streams) {
                            mBatchedStreamsArray.stream_request\
                                [mBatchedStreamsArray.num_streams].streamID =
                                streamsArray.stream_request[k].streamID;
                            mBatchedStreamsArray.stream_request\
                                [mBatchedStreamsArray.num_streams].buf_index =
                                streamsArray.stream_request[k].buf_index;
                            mBatchedStreamsArray.num_streams = mBatchedStreamsArray.num_streams + 1;
                        }
                }
                streamsArray = mBatchedStreamsArray;
            }

            /* Update stream id of all the requested buffers */
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_STREAM_ID, streamsArray)) {
                LOGE("Failed to set stream type mask in the parameters");
                return BAD_VALUE;
            }

            /* for quadra cfa request, dont' send request to back-end again,
             * as we already got raw frame  */
            if (!m_bQuadraCfaRequest) {
                /*While translating input params to Aux by FOV control, stream requests will also
                  get copied. Reset them on the slave session so that we request for frames only
                  on the master session.*/
                if (isDualCamera() && (getHalPPType() != CAM_HAL_PP_TYPE_NONE)) {
                    /* Update stream id of all the requested buffers in Aux session */
                    if (ADD_SET_PARAM_ENTRY_TO_BATCH( mAuxParameters,
                                             CAM_INTF_META_STREAM_ID,
                                                       streamsArray)) {
                        LOGE("Failed to set meta stream id in Aux session");
                        pthread_mutex_unlock(&mMutex);
                        return BAD_VALUE;
                    }
                    if(ADD_SET_PARAM_ENTRY_TO_BATCH(mAuxParameters,
                        CAM_INTF_META_FRAME_NUMBER, request->frame_number)) {
                        LOGE("Failed to set the frame number in the parameters");
                        pthread_mutex_unlock(&mMutex);
                        return BAD_VALUE;
                    }
                    metadata_buffer_t* params =
                            (mMasterCamera == CAM_TYPE_MAIN) ? mAuxParameters : mParameters;
                    if ((IS_YUV_ZSL && IS_VALID_PTR(mZSLChannel))
                        || (!(IS_YUV_ZSL) && blob_request && needHALPP())
                        ||(IS_SNAP_ZSL && IS_HAL_PP_TYPE_BOKEH)) {
                        LOGD("snapshot request on slave session");
                        ADD_SET_PARAM_ENTRY_TO_BATCH( params, CAM_INTF_META_STREAM_ID,
                                                                    streamsArraySlave);
                    } else {
                        LOGD("Requesting PCR on %s session", (mMasterCamera == CAM_TYPE_MAIN) ?
                                                                                "main" : "aux");
                        params->is_valid[CAM_INTF_META_STREAM_ID] = 0;
                        params->is_valid[CAM_INTF_META_FRAME_NUMBER] = 0;
                    }
                } else if(IS_MULTI_CAMERA && is_aux_configured) {
                    /* Update stream id of all the requested buffers in Aux session */
                    if(is_requested_on_aux){
                        if (ADD_SET_PARAM_ENTRY_TO_BATCH( mAuxParameters,
                                          CAM_INTF_META_STREAM_ID,
                                          streamsArraySlave)) {
                            LOGE("Failed to set meta stream id in Aux session");
                            pthread_mutex_unlock(&mMutex);
                            return BAD_VALUE;
                        }
                    } else {
                        if (ADD_SET_PARAM_ENTRY_TO_BATCH( mAuxParameters,
                                CAM_INTF_META_STREAM_ID,
                                streamsArray)) {
                            LOGE("Failed to set meta stream id in Aux session");
                            pthread_mutex_unlock(&mMutex);
                            return BAD_VALUE;
                        }
                        mAuxParameters->is_valid[CAM_INTF_META_FRAME_NUMBER] = 0;
                    }
                }
                if(!IS_MULTI_CAMERA)
                {
                    rc = mCameraHandle->ops->set_parms(
                            get_main_camera_handle(mCameraHandle->camera_handle), mParameters);
                    if (rc < 0) {
                        LOGE("set_parms failed");
                    }

                    if (isDualCamera() && !IS_PP_TYPE_NONE) {
                        rc = mCameraHandle->ops->set_parms(
                              get_aux_camera_handle(mCameraHandle->camera_handle), mAuxParameters);
                        if (rc < 0) {
                            LOGE("set_parms on aux failed");
                        }
                    }
                }else {
                    uint32_t camHandle = get_main_camera_handle(mCameraHandle->camera_handle);
                    metadata_buffer_t *params = mParameters;
                    uint32_t config_index = CONFIG_INDEX_MAIN;
                    if(!(is_main_configured || is_logical_configured))
                    {
                        if(is_aux_configured)
                        {
                            camHandle = get_aux_camera_handle(mCameraHandle->camera_handle);
                            params = mAuxParameters;
                            config_index = CONFIG_INDEX_AUX;
                        }
                    }
                    do {
                        rc = mCameraHandle->ops->set_parms(camHandle, params);
                        if (rc < 0) {
                            LOGE("set_parms on %s failed",
                                (config_index == CONFIG_INDEX_MAIN)?"main":"aux");
                        }

                        if(is_aux_configured && (config_index != CONFIG_INDEX_AUX))
                        {
                            camHandle = get_aux_camera_handle(mCameraHandle->camera_handle);
                            params = mAuxParameters;
                        }
                        config_index++;
                    } while(is_aux_configured && (config_index < CONFIG_INDEX_MAX));
                }
            }
            /* reset to zero coz, the batch is queued */
            mToBeQueuedVidBufs = 0;
            mPendingBatchMap.add(frameNumber, mFirstFrameNumberInBatch);
            memset(&mBatchedStreamsArray, 0, sizeof(cam_stream_ID_t));
        } else if (mBatchSize && isVidBufRequested && (mToBeQueuedVidBufs != mBatchSize)) {
            for (uint32_t k = 0; k < streamsArray.num_streams; k++) {
                uint32_t m = 0;
                for (m = 0; m < mBatchedStreamsArray.num_streams; m++) {
                    if (streamsArray.stream_request[k].streamID ==
                            mBatchedStreamsArray.stream_request[m].streamID)
                        break;
                }
                if (m == mBatchedStreamsArray.num_streams) {
                    mBatchedStreamsArray.stream_request[mBatchedStreamsArray.num_streams].streamID =
                        streamsArray.stream_request[k].streamID;
                    mBatchedStreamsArray.stream_request[mBatchedStreamsArray.num_streams].buf_index =
                        streamsArray.stream_request[k].buf_index;
                    mBatchedStreamsArray.num_streams = mBatchedStreamsArray.num_streams + 1;
                }
            }
        }
    }
    if (internallyRequestedStreams.size() == 0) {
        mPendingLiveRequest++;
    }

    LOGD("mPendingLiveRequest = %d", mPendingLiveRequest);
    mState = STARTED;
    if(mHdrSnapshotRunning) {
        LOGD("blocked for HDR snapshot completion");
        mMultiFrameReqLock.unlock();
        pthread_cond_wait(&mHdrRequestCond, &mMutex);
        mHdrSnapshotRunning = false;
        LOGD("unblocked ");
    }
    if (mMultiFrameSnapshotRunning) {
        mMultiFrameReqLock.unlock();
        mMultiFrameSnapshotRunning = false;
    }

    // Added a timed condition wait
    struct timespec ts;
    uint8_t isValidTimeout = 1;
    rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (rc < 0) {
      isValidTimeout = 0;
      LOGE("Error reading the real time clock!!");
    }
    else {
      // Make timeout as 5 sec for request to be honored
      ts.tv_sec += 5;
    }
      ts.tv_sec += ((bufsForCurRequest.timestamp - systemTime(CLOCK_MONOTONIC))/1000000000);
    //Block on conditional variable
    while ((mPendingLiveRequest > mMaxInFlightRequests) && !pInputBuffer &&
            (mState != ERROR) && (mState != DEINIT)) {
        if (!isValidTimeout) {
            LOGD("Blocking on conditional wait");
            pthread_cond_wait(&mRequestCond, &mMutex);
        }
        else {
            LOGD("Blocking on timed conditional wait");
            rc = pthread_cond_timedwait(&mRequestCond, &mMutex, &ts);
            if (rc == ETIMEDOUT) {
                rc = -ENODEV;
                LOGE("Unblocked on timeout!!!!");
                break;
            }
        }
        LOGD("Unblocked");
    }
    pthread_mutex_unlock(&mMutex);

    if (m_bQuadraCfaRequest) {
        pthread_mutex_lock(&mMutex);
        mm_camera_buf_def dummy_meta_buf;
        mm_camera_super_buf_t dummy_super_buf;
        memset(&dummy_meta_buf, 0, sizeof(mm_camera_buf_def));
        memset(&dummy_super_buf, 0, sizeof(mm_camera_super_buf_t));
        dummy_super_buf.num_bufs = 1;
        dummy_super_buf.bufs[0] = &dummy_meta_buf;

        dummy_meta_buf.buffer = &(mQCFACaptureChannel->urgent_meta);
        handleMetadataWithLock(&dummy_super_buf,
                false /* free_and_bufdone_meta_buf */,
                false /* first frame of batch metadata */ ,
                NULL);

        handleMetadataWithLock(&(mQCFACaptureChannel->meta_frame), false, false, NULL);

        pthread_mutex_unlock(&mMutex);
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : dump
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *
 *
 * RETURN     :
 *==========================================================================*/
void QCamera3HardwareInterface::dump(int fd)
{
    pthread_mutex_lock(&mMutex);
    dprintf(fd, "\n Camera HAL3 information Begin \n");

    dprintf(fd, "\nNumber of pending requests: %zu \n",
        mPendingRequestsList.size());
    dprintf(fd, "-------+-------------------+-------------+----------+---------------------\n");
    dprintf(fd, " Frame | Number of Buffers |   Req Id:   | Blob Req | Input buffer present\n");
    dprintf(fd, "-------+-------------------+-------------+----------+---------------------\n");
    for(pendingRequestIterator i = mPendingRequestsList.begin();
            i != mPendingRequestsList.end(); i++) {
        dprintf(fd, " %5d | %17d | %11d | %8d | %p \n",
        i->frame_number, i->num_buffers, i->request_id, i->blob_request,
        i->input_buffer);
    }
    dprintf(fd, "\nPending buffer map: Number of buffers: %u\n",
                mPendingBuffersMap.get_num_overall_buffers());
    dprintf(fd, "-------+------------------\n");
    dprintf(fd, " Frame | Stream type mask \n");
    dprintf(fd, "-------+------------------\n");
    for(auto &req : mPendingBuffersMap.mPendingBuffersInRequest) {
        for(auto &j : req.mPendingBufferList) {
            QCamera3Channel *channel = (QCamera3Channel *)(j.stream->priv);
            dprintf(fd, " %5d | %11d \n",
                    req.frame_number, channel->getStreamTypeMask());
        }
    }
    dprintf(fd, "-------+------------------\n");

    dprintf(fd, "\nPending frame drop list: %zu\n",
        mPendingFrameDropList.size());
    dprintf(fd, "-------+-----------\n");
    dprintf(fd, " Frame | Stream ID \n");
    dprintf(fd, "-------+-----------\n");
    for(List<PendingFrameDropInfo>::iterator i = mPendingFrameDropList.begin();
        i != mPendingFrameDropList.end(); i++) {
        dprintf(fd, " %5d | %9d \n",
            i->frame_number, i->stream_ID);
    }
    dprintf(fd, "-------+-----------\n");

    dprintf(fd, "\n Camera HAL3 information End \n");

    /* use dumpsys media.camera as trigger to send update debug level event */
    mUpdateDebugLevel = true;
    pthread_mutex_unlock(&mMutex);
    return;
}

/*===========================================================================
 * FUNCTION   : flush
 *
 * DESCRIPTION: Calls stopAllChannels, notifyErrorForPendingRequests and
 *              conditionally restarts channels
 *
 * PARAMETERS :
 *  @ restartChannels: re-start all channels
 *
 *
 * RETURN     :
 *          0 on success
 *          Error code on failure
 *==========================================================================*/
int QCamera3HardwareInterface::flush(bool restartChannels)
{
    mPerfLockMgr.acquirePerfLock(PERF_LOCK_FLUSH, DEFAULT_PERF_LOCK_TIMEOUT_MS);
    KPI_ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_STOP_PREVIEW);
    int32_t rc = NO_ERROR;
    Mutex::Autolock lock(mMultiFrameReqLock);
    LOGD("Unblocking Process Capture Request");
    pthread_mutex_lock(&mMutex);
    mFlush = true;
    pthread_mutex_unlock(&mMutex);

    if (isDualCamera()) {
        setDCLowPowerMode(MM_CAMERA_DUAL_CAM);
    }

    rc = stopAllChannels();

    // Reset bundle info
    rc = setBundleInfo();
    if (rc < 0) {
        LOGE("setBundleInfo failed %d", rc);
        return rc;
    }

    // Mutex Lock
    pthread_mutex_lock(&mMutex);

    if(mHdrSnapshotRunning)
    {
        mHdrFrameNum = 0;
        mHdrSnapshotRunning = false;
        pthread_cond_signal(&mHdrRequestCond);
    }

    if(mMultiFrameSnapshotRunning)
    {
        mMultiFrameCaptureNumber = 0;
        mMultiFrameSnapshotRunning = false;
    }
    // Unblock process_capture_request
    mPendingLiveRequest = 0;
    pthread_cond_signal(&mRequestCond);

    rc = notifyErrorForPendingRequests();
    if (rc < 0) {
        LOGE("notifyErrorForPendingRequests failed");
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    mFlush = false;
    if (mState == STARTED)
    {
        mStreamOnPending = restartChannels;
    }
    pthread_mutex_unlock(&mMutex);
    mPerfLockMgr.releasePerfLock(PERF_LOCK_FLUSH);

    return 0;
}

/*===========================================================================
 * FUNCTION   : flushPerf
 *
 * DESCRIPTION: This is the performance optimization version of flush that does
 *              not use stream off, rather flushes the system
 *
 * PARAMETERS :
 *
 *
 * RETURN     : 0 : success
 *              -EINVAL: input is malformed (device is not valid)
 *              -ENODEV: if the device has encountered a serious error
 *==========================================================================*/
int QCamera3HardwareInterface::flushPerf()
{
    KPI_ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_STOP_PREVIEW);
    int32_t rc = 0;
    struct timespec timeout;
    bool timed_wait = false;

    pthread_mutex_lock(&mMutex);
    mFlushPerf = true;
    mPendingBuffersMap.numPendingBufsAtFlush =
        mPendingBuffersMap.get_num_overall_buffers();
    LOGD("Calling flush. Wait for %d buffers to return",
        mPendingBuffersMap.numPendingBufsAtFlush);

    /* send the flush event to the backend */
    rc = mCameraHandle->ops->flush(mCameraHandle->camera_handle);
    if (rc < 0) {
        LOGE("Error in flush: IOCTL failure");
        mFlushPerf = false;
        pthread_mutex_unlock(&mMutex);
        return -ENODEV;
    }

    if (mPendingBuffersMap.numPendingBufsAtFlush == 0) {
        LOGD("No pending buffers in HAL, return flush");
        mFlushPerf = false;
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    /* wait on a signal that buffers were received */
    rc = clock_gettime(CLOCK_MONOTONIC, &timeout);
    if (rc < 0) {
        LOGE("Error reading the real time clock, cannot use timed wait");
    } else {
        timeout.tv_sec += FLUSH_TIMEOUT;
        timed_wait = true;
    }

    //Block on conditional variable
    while (mPendingBuffersMap.numPendingBufsAtFlush != 0) {
        LOGD("Waiting on mBuffersCond");
        if (!timed_wait) {
            rc = pthread_cond_wait(&mBuffersCond, &mMutex);
            if (rc != 0) {
                 LOGE("pthread_cond_wait failed due to rc = %s",
                        strerror(rc));
                 break;
            }
        } else {
            rc = pthread_cond_timedwait(&mBuffersCond, &mMutex, &timeout);
            if (rc != 0) {
                LOGE("pthread_cond_timedwait failed due to rc = %s",
                            strerror(rc));
                break;
            }
        }
    }
    if (rc != 0) {
        mFlushPerf = false;
        pthread_mutex_unlock(&mMutex);
        return -ENODEV;
    }

    LOGD("Received buffers, now safe to return them");

    //make sure the channels handle flush
    //currently only required for the picture channel to release snapshot resources
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end(); it++) {
        QCamera3Channel *channel = (*it)->channel;
        if (channel) {
            rc = channel->flush();
            if (rc) {
               LOGE("Flushing the channels failed with error %d", rc);
               // even though the channel flush failed we need to continue and
               // return the buffers we have to the framework, however the return
               // value will be an error
               rc = -ENODEV;
            }
        }
    }

    /* notify the frameworks and send errored results */
    rc = notifyErrorForPendingRequests();
    if (rc < 0) {
        LOGE("notifyErrorForPendingRequests failed");
        pthread_mutex_unlock(&mMutex);
        return rc;
    }

    //unblock process_capture_request
    mPendingLiveRequest = 0;
    unblockRequestIfNecessary();

    mFlushPerf = false;
    pthread_mutex_unlock(&mMutex);
    LOGD ("Flush Operation complete. rc = %d", rc);
    return rc;
}

/*===========================================================================
 * FUNCTION   : handleCameraDeviceError
 *
 * DESCRIPTION: This function calls internal flush and notifies the error to
 *              framework and updates the state variable.
 *
 * PARAMETERS : None
 *
 * RETURN     : NO_ERROR on Success
 *              Error code on failure
 *==========================================================================*/
int32_t QCamera3HardwareInterface::handleCameraDeviceError()
{
    int32_t rc = NO_ERROR;

    {
        Mutex::Autolock lock(mFlushLock);
        pthread_mutex_lock(&mMutex);
        if (mState != ERROR) {
            //if mState != ERROR, nothing to be done
            pthread_mutex_unlock(&mMutex);
            return NO_ERROR;
        }
        pthread_mutex_unlock(&mMutex);

        rc = flush(false /* restart channels */);
        if (NO_ERROR != rc) {
            LOGE("internal flush to handle mState = ERROR failed");
        }

        pthread_mutex_lock(&mMutex);
        mState = DEINIT;
        pthread_mutex_unlock(&mMutex);
    }

    camera3_notify_msg_t notify_msg;
    memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
    notify_msg.type = CAMERA3_MSG_ERROR;
    notify_msg.message.error.error_code = CAMERA3_MSG_ERROR_DEVICE;
    notify_msg.message.error.error_stream = NULL;
    notify_msg.message.error.frame_number = 0;
    orchestrateNotify(&notify_msg);

    return rc;
}

/*===========================================================================
 * FUNCTION   : captureResultCb
 *
 * DESCRIPTION: Callback handler for all capture result
 *              (streams, as well as metadata)
 *
 * PARAMETERS :
 *   @metadata : metadata information
 *   @buffer   : actual gralloc buffer to be returned to frameworks.
 *               NULL if metadata.
 *
 * RETURN     : NONE
 *==========================================================================*/
void QCamera3HardwareInterface::captureResultCb(mm_camera_super_buf_t *metadata_buf,
                camera3_stream_buffer_t *buffer, uint32_t frame_number, bool isInputBuffer)
{
    if (metadata_buf) {
        pthread_mutex_lock(&mMutex);
        uint8_t batchSize = mBatchSize;
        pthread_mutex_unlock(&mMutex);
        if (batchSize) {
            handleBatchMetadata(metadata_buf,
                    true /* free_and_bufdone_meta_buf */);
        } else { /* mBatchSize = 0 */
            pthread_mutex_lock(&mMutex);
            hdrPlusPerfLock(metadata_buf);
            handleMetadataWithLock(metadata_buf,
                    true /* free_and_bufdone_meta_buf */,
                    false /* first frame of batch metadata */ ,
                    NULL);
            pthread_mutex_unlock(&mMutex);
        }
    } else if (isInputBuffer) {
        pthread_mutex_lock(&mMutex);
        handleInputBufferWithLock(frame_number);
        pthread_mutex_unlock(&mMutex);
    } else {
        pthread_mutex_lock(&mMutex);
        handleBufferWithLock(buffer, frame_number);
        pthread_mutex_unlock(&mMutex);
    }
    return;
}

void QCamera3HardwareInterface::internalMetaCb(mm_camera_super_buf_t *metadata){
    metadata_buffer_t *p_metadata = (metadata_buffer_t *)metadata->bufs[0]->buffer;
    int32_t *p_frame_number_valid = POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER_VALID, p_metadata);
    uint32_t *p_frame_number = POINTER_OF_META(CAM_INTF_META_FRAME_NUMBER, p_metadata);
    int32_t *p_urgent_frame_number_valid =
            POINTER_OF_META(CAM_INTF_META_URGENT_FRAME_NUMBER_VALID, p_metadata);
    uint32_t *p_urgent_frame_number =
            POINTER_OF_META(CAM_INTF_META_URGENT_FRAME_NUMBER, p_metadata);

    if ((NULL == p_frame_number_valid) || (NULL == p_frame_number) ||
            (NULL == p_urgent_frame_number_valid) || (NULL == p_urgent_frame_number)) {
        LOGE("Invalid metadata");
        mMetadataChannel->bufDone(metadata);
        free(metadata);
        return;
    }

    if (*p_urgent_frame_number_valid) {
        LOGD("valid urgent meta for frame number:%d", *p_urgent_frame_number);

        if (mQCFACaptureChannel != NULL) {
            cam_frame_len_offset_t meta_offset;
            mMetadataChannel->getFrameOffset(meta_offset);
            mQCFACaptureChannel->queueReprocMetadata(metadata, meta_offset, true);
        }
    } else if (*p_frame_number_valid) {
        LOGD("valid meta for frame number:%d", *p_frame_number);
        if (mQCFACaptureChannel != NULL) {
            cam_frame_len_offset_t meta_offset;
            mMetadataChannel->getFrameOffset(meta_offset);
            mQCFACaptureChannel->queueReprocMetadata(metadata, meta_offset);
        }
    }

    mMetadataChannel->bufDone(metadata);
    free(metadata);
}

/*===========================================================================
 * FUNCTION   : getReprocessibleOutputStreamId
 *
 * DESCRIPTION: Get source output stream id for the input reprocess stream
 *              based on size and format, which would be the largest
 *              output stream if an input stream exists.
 *
 * PARAMETERS :
 *   @id      : return the stream id if found
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::getReprocessibleOutputStreamId(uint32_t &id)
{
    /* check if any output or bidirectional stream with the same size and format
       and return that stream */
    if ((mInputStreamInfo.dim.width > 0) &&
            (mInputStreamInfo.dim.height > 0)) {
        for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
                it != mStreamInfo.end(); it++) {

            camera3_stream_t *stream = (*it)->stream;
            if ((stream->width == (uint32_t)mInputStreamInfo.dim.width) &&
                    (stream->height == (uint32_t)mInputStreamInfo.dim.height) &&
                    (stream->format == mInputStreamInfo.format)) {
                // Usage flag for an input stream and the source output stream
                // may be different.
                LOGD("Found reprocessible output stream! %p", *it);
                LOGD("input stream usage 0x%x, current stream usage 0x%x",
                         stream->usage, mInputStreamInfo.usage);
                if (stream->usage & (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER)) {
                    LOGD("stream with HW_TEXTURE/HW_COMPOSER usage (preview stream), skip it.");
                    continue;
                }

                QCamera3Channel *channel = (QCamera3Channel *)stream->priv;
                if (channel != NULL && channel->mStreams[0]) {
                    id = channel->mStreams[0]->getMyServerID();
                    return NO_ERROR;
                }
            }
        }
    } else {
        LOGD("No input stream, so no reprocessible output stream");
    }
    return NAME_NOT_FOUND;
}

/*===========================================================================
 * FUNCTION   : lookupFwkName
 *
 * DESCRIPTION: In case the enum is not same in fwk and backend
 *              make sure the parameter is correctly propogated
 *
 * PARAMETERS  :
 *   @arr      : map between the two enums
 *   @len      : len of the map
 *   @hal_name : name of the hal_parm to map
 *
 * RETURN     : int type of status
 *              fwk_name  -- success
 *              none-zero failure code
 *==========================================================================*/
template <typename halType, class mapType> int lookupFwkName(const mapType *arr,
        size_t len, halType hal_name)
{

    for (size_t i = 0; i < len; i++) {
        if (arr[i].hal_name == hal_name) {
            return arr[i].fwk_name;
        }
    }

    /* Not able to find matching framework type is not necessarily
     * an error case. This happens when mm-camera supports more attributes
     * than the frameworks do */
    LOGH("Cannot find matching framework type");
    return NAME_NOT_FOUND;
}

/*===========================================================================
 * FUNCTION   : lookupHalName
 *
 * DESCRIPTION: In case the enum is not same in fwk and backend
 *              make sure the parameter is correctly propogated
 *
 * PARAMETERS  :
 *   @arr      : map between the two enums
 *   @len      : len of the map
 *   @fwk_name : name of the hal_parm to map
 *
 * RETURN     : int32_t type of status
 *              hal_name  -- success
 *              none-zero failure code
 *==========================================================================*/
template <typename fwkType, class mapType> int lookupHalName(const mapType *arr,
        size_t len, fwkType fwk_name)
{
    for (size_t i = 0; i < len; i++) {
        if (arr[i].fwk_name == fwk_name) {
            return arr[i].hal_name;
        }
    }

    LOGE("Cannot find matching hal type fwk_name=%d", fwk_name);
    return NAME_NOT_FOUND;
}

/*===========================================================================
 * FUNCTION   : lookupProp
 *
 * DESCRIPTION: lookup a value by its name
 *
 * PARAMETERS :
 *   @arr     : map between the two enums
 *   @len     : size of the map
 *   @name    : name to be looked up
 *
 * RETURN     : Value if found
 *              CAM_CDS_MODE_MAX if not found
 *==========================================================================*/
template <class mapType> cam_cds_mode_type_t lookupProp(const mapType *arr,
        size_t len, const char *name)
{
    if (name) {
        for (size_t i = 0; i < len; i++) {
            if (!strcmp(arr[i].desc, name)) {
                return arr[i].val;
            }
        }
    }
    return CAM_CDS_MODE_MAX;
}

camera_metadata_t * QCamera3HardwareInterface::getPhysicalMeta(
        const mm_camera_super_buf_t *metadata, PendingRequestInfo *request,
        bool dummyMeta, cam_sync_type_t sync_type)
{

    if(metadata == NULL && !dummyMeta)
    {
       return NULL;
    }

    int32_t request_id = request->request_id;
    int32_t capture_intent = request->capture_intent;
    uint8_t fwkCacMode = request->fwkCacMode;

    if(sync_type == CAM_TYPE_AUX)
    {
        request_id = request->aux_request_id;
        capture_intent = request->aux_capture_intent;
        fwkCacMode = request->fwkAuxCacMode;
    }

    if(dummyMeta)
    {
        CameraMetadata *meta = new CameraMetadata();
        meta->update(ANDROID_REQUEST_ID, &(request_id), 1);
        return meta->release();
    }

     camera_metadata_t *cam_meta = NULL;
     CameraMetadata resultWrapper;
     metadata_buffer_t *meta = (metadata_buffer_t *)metadata->bufs[0]->buffer;
     cam_meta = translateFromHalMetadata(meta, request->timestamp, request_id,
                    request->jpegMetadata, request->pipeline_depth, capture_intent,
                    false, fwkCacMode, false, request->enableZSL);
     resultWrapper.acquire(cam_meta);

    if(IS_MULTI_CAMERA)
    {
        //physical meta requires fields of urget metadata as well so updating all fields.
        IF_META_AVAILABLE(uint32_t, whiteBalanceState, CAM_INTF_META_AWB_STATE, meta) {
            uint8_t fwk_whiteBalanceState = (uint8_t) *whiteBalanceState;
            resultWrapper.update(ANDROID_CONTROL_AWB_STATE, &fwk_whiteBalanceState, 1);
            LOGD("urgent meta : ANDROID_CONTROL_AWB_STATE %u", *whiteBalanceState);
        }

        IF_META_AVAILABLE(cam_trigger_t, aecTrigger, CAM_INTF_META_AEC_PRECAPTURE_TRIGGER, meta) {
            resultWrapper.update(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
                    &aecTrigger->trigger, 1);
            resultWrapper.update(ANDROID_CONTROL_AE_PRECAPTURE_ID,
                    &aecTrigger->trigger_id, 1);
            LOGD("urgent Metadata : CAM_INTF_META_AEC_PRECAPTURE_TRIGGER: %d",
                     aecTrigger->trigger);
            LOGD("urgent Metadata : ANDROID_CONTROL_AE_PRECAPTURE_ID: %d",
                    aecTrigger->trigger_id);
        }

        IF_META_AVAILABLE(uint32_t, ae_state, CAM_INTF_META_AEC_STATE, meta) {
            uint8_t fwk_ae_state = (uint8_t) *ae_state;
            resultWrapper.update(ANDROID_CONTROL_AE_STATE, &fwk_ae_state, 1);
            LOGD("urgent Metadata : ANDROID_CONTROL_AE_STATE %u", *ae_state);
        }

        IF_META_AVAILABLE(uint32_t, afState, CAM_INTF_META_AF_STATE, meta) {
            uint8_t fwk_afState = (uint8_t) *afState;
            resultWrapper.update(ANDROID_CONTROL_AF_STATE, &fwk_afState, 1);
            LOGD("urgent Metadata : ANDROID_CONTROL_AF_STATE %u", *afState);
        }

        IF_META_AVAILABLE(uint32_t, focusMode, CAM_INTF_PARM_FOCUS_MODE, meta) {
            int val = lookupFwkName(FOCUS_MODES_MAP, METADATA_MAP_SIZE(FOCUS_MODES_MAP), *focusMode);
            if (NAME_NOT_FOUND != val) {
                uint8_t fwkAfMode = (uint8_t)val;
                resultWrapper.update(ANDROID_CONTROL_AF_MODE, &fwkAfMode, 1);
                LOGD("urgent Metadata : ANDROID_CONTROL_AF_MODE %d", val);
            } else {
                LOGH("urgent Metadata not found : ANDROID_CONTROL_AF_MODE %d",
                        val);
            }
        }

        IF_META_AVAILABLE(cam_trigger_t, af_trigger, CAM_INTF_META_AF_TRIGGER, meta) {
            resultWrapper.update(ANDROID_CONTROL_AF_TRIGGER,
                    &af_trigger->trigger, 1);
            LOGD("urgent Metadata : CAM_INTF_META_AF_TRIGGER = %d",
                     af_trigger->trigger);
            resultWrapper.update(ANDROID_CONTROL_AF_TRIGGER_ID, &af_trigger->trigger_id, 1);
            LOGD("urgent Metadata : ANDROID_CONTROL_AF_TRIGGER_ID = %d",
                    af_trigger->trigger_id);
        }

        IF_META_AVAILABLE(int32_t, whiteBalance, CAM_INTF_PARM_WHITE_BALANCE, meta) {
            int val = lookupFwkName(WHITE_BALANCE_MODES_MAP,
                    METADATA_MAP_SIZE(WHITE_BALANCE_MODES_MAP), *whiteBalance);
            if (NAME_NOT_FOUND != val) {
                uint8_t fwkWhiteBalanceMode = (uint8_t)val;
                resultWrapper.update(ANDROID_CONTROL_AWB_MODE, &fwkWhiteBalanceMode, 1);
                LOGD("urgent Metadata : ANDROID_CONTROL_AWB_MODE %d", val);
            } else {
                LOGH("urgent Metadata not found : ANDROID_CONTROL_AWB_MODE");
            }
        }

        uint8_t fwk_aeMode = ANDROID_CONTROL_AE_MODE_OFF;
        uint32_t aeMode = CAM_AE_MODE_MAX;
        int32_t flashMode = CAM_FLASH_MODE_MAX;
        int32_t redeye = -1;
        IF_META_AVAILABLE(uint32_t, pAeMode, CAM_INTF_META_AEC_MODE, meta) {
            aeMode = *pAeMode;
        }
        IF_META_AVAILABLE(int32_t, pFlashMode, CAM_INTF_PARM_LED_MODE, meta) {
            flashMode = *pFlashMode;
        }
        IF_META_AVAILABLE(int32_t, pRedeye, CAM_INTF_PARM_REDEYE_REDUCTION, meta) {
            redeye = *pRedeye;
        }

        if (1 == redeye) {
            fwk_aeMode = ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE;
            resultWrapper.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
        } else if ((CAM_FLASH_MODE_AUTO == flashMode) || (CAM_FLASH_MODE_ON == flashMode)) {
            int val = lookupFwkName(AE_FLASH_MODE_MAP, METADATA_MAP_SIZE(AE_FLASH_MODE_MAP),
                    flashMode);
            if (NAME_NOT_FOUND != val) {
                fwk_aeMode = (uint8_t)val;
                resultWrapper.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
            } else {
                LOGE("Unsupported flash mode %d", flashMode);
            }
        } else if (aeMode == CAM_AE_MODE_ON) {
            fwk_aeMode = ANDROID_CONTROL_AE_MODE_ON;
            resultWrapper.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
        } else if (aeMode == CAM_AE_MODE_OFF) {
            fwk_aeMode = ANDROID_CONTROL_AE_MODE_OFF;
            resultWrapper.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
        } else {
            LOGE("Not enough info to deduce ANDROID_CONTROL_AE_MODE redeye:%d, "
                  "flashMode:%d, aeMode:%u!!!",
                     redeye, flashMode, aeMode);
        }
    }

    return resultWrapper.release();
}

/*===========================================================================
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *   @metadata : metadata information from callback
 *   @timestamp: metadata buffer timestamp
 *   @request_id: request id
 *   @jpegMetadata: additional jpeg metadata
 *   @pprocDone: whether internal offline postprocsesing is done
 *
 * RETURN     : camera_metadata_t*
 *              metadata in a format specified by fwk
 *==========================================================================*/
camera_metadata_t*
QCamera3HardwareInterface::translateFromHalMetadata(
                                 metadata_buffer_t *metadata,
                                 nsecs_t timestamp,
                                 int32_t request_id,
                                 const CameraMetadata& jpegMetadata,
                                 uint8_t pipeline_depth,
                                 uint8_t capture_intent,
                                 bool pprocDone,
                                 uint8_t fwk_cacMode,
                                 bool firstMetadataInBatch,
                                 bool enableZSL)
{
    CameraMetadata camMetadata;
    camera_metadata_t *resultMetadata;

    if (mBatchSize && !firstMetadataInBatch) {
        /* In batch mode, use cached metadata from the first metadata
            in the batch */
        camMetadata.clear();
        camMetadata = mCachedMetadata;
    }

    if (jpegMetadata.entryCount())
        camMetadata.append(jpegMetadata);

    camMetadata.update(ANDROID_SENSOR_TIMESTAMP, &timestamp, 1);
    camMetadata.update(ANDROID_REQUEST_ID, &request_id, 1);
    camMetadata.update(ANDROID_REQUEST_PIPELINE_DEPTH, &pipeline_depth, 1);
    camMetadata.update(ANDROID_CONTROL_CAPTURE_INTENT, &capture_intent, 1);

    if (mBatchSize && !firstMetadataInBatch) {
        /* In batch mode, use cached metadata instead of parsing metadata buffer again */
        resultMetadata = camMetadata.release();
        return resultMetadata;
    }
    IF_META_AVAILABLE(uint32_t, frame_number, CAM_INTF_META_FRAME_NUMBER, metadata) {
        int64_t fwk_frame_number = *frame_number;
        camMetadata.update(ANDROID_SYNC_FRAME_NUMBER, &fwk_frame_number, 1);
    }

    IF_META_AVAILABLE(cam_fps_range_t, float_range, CAM_INTF_PARM_FPS_RANGE, metadata) {
        int32_t fps_range[2];
        fps_range[0] = (int32_t)float_range->min_fps;
        fps_range[1] = (int32_t)float_range->max_fps;
        camMetadata.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
                                      fps_range, 2);
        LOGD("urgent Metadata : ANDROID_CONTROL_AE_TARGET_FPS_RANGE [%d, %d]",
             fps_range[0], fps_range[1]);
    }

    IF_META_AVAILABLE(int32_t, expCompensation, CAM_INTF_PARM_EXPOSURE_COMPENSATION, metadata) {
        camMetadata.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, expCompensation, 1);
    }

    IF_META_AVAILABLE(uint32_t, sceneMode, CAM_INTF_PARM_BESTSHOT_MODE, metadata) {
        int val = lookupFwkName(SCENE_MODES_MAP,
                METADATA_MAP_SIZE(SCENE_MODES_MAP),
                *sceneMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwkSceneMode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_SCENE_MODE, &fwkSceneMode, 1);
            LOGD("urgent Metadata : ANDROID_CONTROL_SCENE_MODE: %d",
                     fwkSceneMode);
        }
    }

    IF_META_AVAILABLE(uint32_t, ae_lock, CAM_INTF_PARM_AEC_LOCK, metadata) {
        uint8_t fwk_ae_lock = (uint8_t) *ae_lock;
        camMetadata.update(ANDROID_CONTROL_AE_LOCK, &fwk_ae_lock, 1);
    }

    IF_META_AVAILABLE(uint32_t, awb_lock, CAM_INTF_PARM_AWB_LOCK, metadata) {
        uint8_t fwk_awb_lock = (uint8_t) *awb_lock;
        camMetadata.update(ANDROID_CONTROL_AWB_LOCK, &fwk_awb_lock, 1);
    }

    IF_META_AVAILABLE(uint32_t, color_correct_mode, CAM_INTF_META_COLOR_CORRECT_MODE, metadata) {
        uint8_t fwk_color_correct_mode = (uint8_t) *color_correct_mode;
        camMetadata.update(ANDROID_COLOR_CORRECTION_MODE, &fwk_color_correct_mode, 1);
    }

    IF_META_AVAILABLE(cam_edge_application_t, edgeApplication,
            CAM_INTF_META_EDGE_MODE, metadata) {
        camMetadata.update(ANDROID_EDGE_MODE, &(edgeApplication->edge_mode), 1);
        camMetadata.update(QCAMERA3_SHARPNESS_STRENGTH, &(edgeApplication->sharpness), 1);
    }

    IF_META_AVAILABLE(uint32_t, flashPower, CAM_INTF_META_FLASH_POWER, metadata) {
        uint8_t fwk_flashPower = (uint8_t) *flashPower;
        camMetadata.update(ANDROID_FLASH_FIRING_POWER, &fwk_flashPower, 1);
    }

    IF_META_AVAILABLE(int64_t, flashFiringTime, CAM_INTF_META_FLASH_FIRING_TIME, metadata) {
        camMetadata.update(ANDROID_FLASH_FIRING_TIME, flashFiringTime, 1);
    }

    IF_META_AVAILABLE(int32_t, flashState, CAM_INTF_META_FLASH_STATE, metadata) {
        if (0 <= *flashState) {
            uint8_t fwk_flashState = (uint8_t) *flashState;
            if (!gCamCapability[mCameraId]->flash_available) {
                fwk_flashState = ANDROID_FLASH_STATE_UNAVAILABLE;
            }
            camMetadata.update(ANDROID_FLASH_STATE, &fwk_flashState, 1);
        }
    }

    IF_META_AVAILABLE(uint32_t, flashMode, CAM_INTF_META_FLASH_MODE, metadata) {
        int val = lookupFwkName(FLASH_MODES_MAP, METADATA_MAP_SIZE(FLASH_MODES_MAP), *flashMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwk_flashMode = (uint8_t)val;
            camMetadata.update(ANDROID_FLASH_MODE, &fwk_flashMode, 1);
        }
    }

    IF_META_AVAILABLE(uint32_t, hotPixelMode, CAM_INTF_META_HOTPIXEL_MODE, metadata) {
        uint8_t fwk_hotPixelMode = (uint8_t) *hotPixelMode;
        camMetadata.update(ANDROID_HOT_PIXEL_MODE, &fwk_hotPixelMode, 1);
    }

    IF_META_AVAILABLE(float, lensAperture, CAM_INTF_META_LENS_APERTURE, metadata) {
        camMetadata.update(ANDROID_LENS_APERTURE , lensAperture, 1);
    }

    IF_META_AVAILABLE(float, filterDensity, CAM_INTF_META_LENS_FILTERDENSITY, metadata) {
        camMetadata.update(ANDROID_LENS_FILTER_DENSITY , filterDensity, 1);
    }

    IF_META_AVAILABLE(float, focalLength, CAM_INTF_META_LENS_FOCAL_LENGTH, metadata) {
        camMetadata.update(ANDROID_LENS_FOCAL_LENGTH, focalLength, 1);
    }

    IF_META_AVAILABLE(cam_ois_mode_t, opticalStab, CAM_INTF_META_LENS_OPT_STAB_MODE, metadata) {
        uint8_t fwk_opticalStab = 0;
        if ((*opticalStab) == OIS_MODE_ACTIVE) {
            fwk_opticalStab = 1;
        }
        camMetadata.update(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &fwk_opticalStab, 1);
    }

    IF_META_AVAILABLE(uint32_t, videoStab, CAM_INTF_META_VIDEO_STAB_MODE, metadata) {
        uint8_t fwk_videoStab = (uint8_t) *videoStab;
        LOGD("fwk_videoStab = %d", fwk_videoStab);
        camMetadata.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &fwk_videoStab, 1);
    } else {
        // Regardless of Video stab supports or not, CTS is expecting the EIS result to be non NULL
        // and so hardcoding the Video Stab result to OFF mode.
        uint8_t fwkVideoStabMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
        camMetadata.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &fwkVideoStabMode, 1);
        LOGD("EIS result default to OFF mode");
    }

    IF_META_AVAILABLE(uint32_t, noiseRedMode, CAM_INTF_META_NOISE_REDUCTION_MODE, metadata) {
        uint8_t fwk_noiseRedMode = (uint8_t) *noiseRedMode;
        camMetadata.update(ANDROID_NOISE_REDUCTION_MODE, &fwk_noiseRedMode, 1);
    }

    IF_META_AVAILABLE(float, effectiveExposureFactor, CAM_INTF_META_EFFECTIVE_EXPOSURE_FACTOR, metadata) {
        camMetadata.update(ANDROID_REPROCESS_EFFECTIVE_EXPOSURE_FACTOR, effectiveExposureFactor, 1);
    }

    IF_META_AVAILABLE(cam_black_level_metadata_t, blackLevelSourcePattern,
        CAM_INTF_META_BLACK_LEVEL_SOURCE_PATTERN, metadata) {

        LOGD("dynamicblackLevel = %f %f %f %f",
          blackLevelSourcePattern->cam_black_level[0],
          blackLevelSourcePattern->cam_black_level[1],
          blackLevelSourcePattern->cam_black_level[2],
          blackLevelSourcePattern->cam_black_level[3]);
    }

    IF_META_AVAILABLE(cam_black_level_metadata_t, blackLevelAppliedPattern,
        CAM_INTF_META_BLACK_LEVEL_APPLIED_PATTERN, metadata) {
        float fwk_blackLevelInd[4];

        fwk_blackLevelInd[0] = blackLevelAppliedPattern->cam_black_level[0];
        fwk_blackLevelInd[1] = blackLevelAppliedPattern->cam_black_level[1];
        fwk_blackLevelInd[2] = blackLevelAppliedPattern->cam_black_level[2];
        fwk_blackLevelInd[3] = blackLevelAppliedPattern->cam_black_level[3];

        LOGD("applied dynamicblackLevel = %f %f %f %f",
          blackLevelAppliedPattern->cam_black_level[0],
          blackLevelAppliedPattern->cam_black_level[1],
          blackLevelAppliedPattern->cam_black_level[2],
          blackLevelAppliedPattern->cam_black_level[3]);
        camMetadata.update(QCAMERA3_SENSOR_DYNAMIC_BLACK_LEVEL_PATTERN, fwk_blackLevelInd, 4);

#ifndef USE_HAL_3_3
        // Update the ANDROID_SENSOR_DYNAMIC_BLACK_LEVEL
        // Need convert the internal 14 bit depth to sensor 10 bit sensor raw
        // depth space.
        fwk_blackLevelInd[0] /= 16.0;
        fwk_blackLevelInd[1] /= 16.0;
        fwk_blackLevelInd[2] /= 16.0;
        fwk_blackLevelInd[3] /= 16.0;
        camMetadata.update(ANDROID_SENSOR_DYNAMIC_BLACK_LEVEL, fwk_blackLevelInd, 4);
#endif
    }

#ifndef USE_HAL_3_3
    // Fixed whitelevel is used by ISP/Sensor
    camMetadata.update(ANDROID_SENSOR_DYNAMIC_WHITE_LEVEL,
            &gCamCapability[mCameraId]->white_level, 1);
#endif

    IF_META_AVAILABLE(cam_crop_region_t, hScalerCropRegion,
            CAM_INTF_META_SCALER_CROP_REGION, metadata) {
        int32_t scalerCropRegion[4];
        scalerCropRegion[0] = hScalerCropRegion->left;
        scalerCropRegion[1] = hScalerCropRegion->top;
        scalerCropRegion[2] = hScalerCropRegion->width;
        scalerCropRegion[3] = hScalerCropRegion->height;

        // Adjust crop region from sensor output coordinate system to active
        // array coordinate system.
        mCropRegionMapper.toActiveArray(scalerCropRegion[0], scalerCropRegion[1],
                scalerCropRegion[2], scalerCropRegion[3]);

        camMetadata.update(ANDROID_SCALER_CROP_REGION, scalerCropRegion, 4);
    }

    IF_META_AVAILABLE(int64_t, sensorExpTime, CAM_INTF_META_SENSOR_EXPOSURE_TIME, metadata) {
        LOGD("sensorExpTime = %lld", *sensorExpTime);
        camMetadata.update(ANDROID_SENSOR_EXPOSURE_TIME , sensorExpTime, 1);
    }

    IF_META_AVAILABLE(int64_t, sensorFameDuration,
            CAM_INTF_META_SENSOR_FRAME_DURATION, metadata) {
        LOGD("sensorFameDuration = %lld", *sensorFameDuration);
        camMetadata.update(ANDROID_SENSOR_FRAME_DURATION, sensorFameDuration, 1);
    }

    IF_META_AVAILABLE(int64_t, sensorRollingShutterSkew,
            CAM_INTF_META_SENSOR_ROLLING_SHUTTER_SKEW, metadata) {
        LOGD("sensorRollingShutterSkew = %lld", *sensorRollingShutterSkew);
        camMetadata.update(ANDROID_SENSOR_ROLLING_SHUTTER_SKEW,
                sensorRollingShutterSkew, 1);
    }

    IF_META_AVAILABLE(int32_t, sensorSensitivity, CAM_INTF_META_SENSOR_SENSITIVITY, metadata) {
        LOGD("sensorSensitivity = %d", *sensorSensitivity);
        camMetadata.update(ANDROID_SENSOR_SENSITIVITY, sensorSensitivity, 1);

        //calculate the noise profile based on sensitivity
        double noise_profile_S = computeNoiseModelEntryS(*sensorSensitivity);
        double noise_profile_O = computeNoiseModelEntryO(*sensorSensitivity);
        double noise_profile[2 * gCamCapability[mCameraId]->num_color_channels];
        for (int i = 0; i < 2 * gCamCapability[mCameraId]->num_color_channels; i += 2) {
            noise_profile[i]   = noise_profile_S;
            noise_profile[i+1] = noise_profile_O;
        }
        LOGD("noise model entry (S, O) is (%f, %f)",
                noise_profile_S, noise_profile_O);
        camMetadata.update(ANDROID_SENSOR_NOISE_PROFILE, noise_profile,
                (size_t) (2 * gCamCapability[mCameraId]->num_color_channels));
    }

#ifndef USE_HAL_3_3
    IF_META_AVAILABLE(int32_t, ispSensitivity, CAM_INTF_META_ISP_SENSITIVITY, metadata) {
        int32_t fwk_ispSensitivity = (int32_t) *ispSensitivity;
        camMetadata.update(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST, &fwk_ispSensitivity, 1);
    }
#endif

    IF_META_AVAILABLE(uint32_t, shadingMode, CAM_INTF_META_SHADING_MODE, metadata) {
        uint8_t fwk_shadingMode = (uint8_t) *shadingMode;
        camMetadata.update(ANDROID_SHADING_MODE, &fwk_shadingMode, 1);
    }

    IF_META_AVAILABLE(uint32_t, faceDetectMode, CAM_INTF_META_STATS_FACEDETECT_MODE, metadata) {
        int val = lookupFwkName(FACEDETECT_MODES_MAP, METADATA_MAP_SIZE(FACEDETECT_MODES_MAP),
                *faceDetectMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwk_faceDetectMode = (uint8_t)val;
            camMetadata.update(ANDROID_STATISTICS_FACE_DETECT_MODE, &fwk_faceDetectMode, 1);

            if (fwk_faceDetectMode != ANDROID_STATISTICS_FACE_DETECT_MODE_OFF) {
                IF_META_AVAILABLE(cam_face_detection_data_t, faceDetectionInfo,
                        CAM_INTF_META_FACE_DETECTION, metadata) {
                    uint8_t numFaces = MIN(
                            faceDetectionInfo->num_faces_detected, MAX_ROI);
                    int32_t faceIds[MAX_ROI];
                    uint8_t faceScores[MAX_ROI];
                    int32_t faceRectangles[MAX_ROI * 4];
                    int32_t faceLandmarks[MAX_ROI * 6];
                    size_t j = 0, k = 0;

                    for (size_t i = 0; i < numFaces; i++) {
                        faceScores[i] = (uint8_t)faceDetectionInfo->faces[i].score;
                        // Adjust crop region from sensor output coordinate system to active
                        // array coordinate system.
                        cam_rect_t& rect = faceDetectionInfo->faces[i].face_boundary;
                        mCropRegionMapper.convertFDROI(rect.left, rect.top,
                                rect.width, rect.height);

                        convertToRegions(faceDetectionInfo->faces[i].face_boundary,
                                faceRectangles+j, -1);

                        LOGL("FD_DEBUG : Frame[%d] Face[%d] : top-left (%d, %d), "
                                "bottom-right (%d, %d)",
                                faceDetectionInfo->frame_id, i,
                                faceRectangles[j + FACE_LEFT], faceRectangles[j + FACE_TOP],
                                faceRectangles[j + FACE_RIGHT], faceRectangles[j + FACE_BOTTOM]);

                        j+= 4;
                    }
                    if (numFaces <= 0) {
                        memset(faceIds, 0, sizeof(int32_t) * MAX_ROI);
                        memset(faceScores, 0, sizeof(uint8_t) * MAX_ROI);
                        memset(faceRectangles, 0, sizeof(int32_t) * MAX_ROI * 4);
                        memset(faceLandmarks, 0, sizeof(int32_t) * MAX_ROI * 6);
                    }

                    camMetadata.update(ANDROID_STATISTICS_FACE_SCORES, faceScores,
                            numFaces);
                    camMetadata.update(ANDROID_STATISTICS_FACE_RECTANGLES,
                            faceRectangles, numFaces * 4U);
                    if (fwk_faceDetectMode ==
                            ANDROID_STATISTICS_FACE_DETECT_MODE_FULL) {
                        IF_META_AVAILABLE(cam_face_landmarks_data_t, landmarks,
                                CAM_INTF_META_FACE_LANDMARK, metadata) {

                            for (size_t i = 0; i < numFaces; i++) {
                                // Map the co-ordinate sensor output coordinate system to active
                                // array coordinate system.
                                mCropRegionMapper.toActiveArray(
                                        landmarks->face_landmarks[i].left_eye_center.x,
                                        landmarks->face_landmarks[i].left_eye_center.y);
                                mCropRegionMapper.toActiveArray(
                                        landmarks->face_landmarks[i].right_eye_center.x,
                                        landmarks->face_landmarks[i].right_eye_center.y);
                                mCropRegionMapper.toActiveArray(
                                        landmarks->face_landmarks[i].mouth_center.x,
                                        landmarks->face_landmarks[i].mouth_center.y);

                                convertLandmarks(landmarks->face_landmarks[i], faceLandmarks+k);

                                LOGL("FD_DEBUG LANDMARK : Frame[%d] Face[%d] : "
                                        "left-eye (%d, %d), right-eye (%d, %d), mouth (%d, %d)",
                                        faceDetectionInfo->frame_id, i,
                                        faceLandmarks[k + LEFT_EYE_X],
                                        faceLandmarks[k + LEFT_EYE_Y],
                                        faceLandmarks[k + RIGHT_EYE_X],
                                        faceLandmarks[k + RIGHT_EYE_Y],
                                        faceLandmarks[k + MOUTH_X],
                                        faceLandmarks[k + MOUTH_Y]);

                                k+= TOTAL_LANDMARK_INDICES;
                            }
                        } else {
                            for (size_t i = 0; i < numFaces; i++) {
                                setInvalidLandmarks(faceLandmarks+k);
                                k+= TOTAL_LANDMARK_INDICES;
                            }
                        }

                        camMetadata.update(ANDROID_STATISTICS_FACE_IDS, faceIds, numFaces);
                        camMetadata.update(ANDROID_STATISTICS_FACE_LANDMARKS,
                                faceLandmarks, numFaces * 6U);
                   }
                    IF_META_AVAILABLE(cam_face_blink_data_t, blinks,
                            CAM_INTF_META_FACE_BLINK, metadata) {
                        uint8_t detected[MAX_ROI];
                        uint8_t degree[MAX_ROI * 2];
                        for (size_t i = 0; i < numFaces; i++) {
                            detected[i] = blinks->blink[i].blink_detected;
                            degree[2 * i] = blinks->blink[i].left_blink;
                            degree[2 * i + 1] = blinks->blink[i].right_blink;

                        LOGL("FD_DEBUG LANDMARK : Frame[%d] : Face[%d] : "
                                "blink_detected=%d, leye_blink=%d, reye_blink=%d",
                                faceDetectionInfo->frame_id, i, detected[i], degree[2 * i],
                                degree[2 * i + 1]);
                        }
                        camMetadata.update(QCAMERA3_STATS_BLINK_DETECTED,
                                detected, numFaces);
                        camMetadata.update(QCAMERA3_STATS_BLINK_DEGREE,
                                degree, numFaces * 2);
                    }
                    IF_META_AVAILABLE(cam_face_smile_data_t, smiles,
                            CAM_INTF_META_FACE_SMILE, metadata) {
                        uint8_t degree[MAX_ROI];
                        uint8_t confidence[MAX_ROI];
                        for (size_t i = 0; i < numFaces; i++) {
                            degree[i] = smiles->smile[i].smile_degree;
                            confidence[i] = smiles->smile[i].smile_confidence;

                        LOGL("FD_DEBUG LANDMARK : Frame[%d] : Face[%d] : "
                                "smile_degree=%d, smile_score=%d",
                                faceDetectionInfo->frame_id, i, degree[i], confidence[i]);
                        }
                        camMetadata.update(QCAMERA3_STATS_SMILE_DEGREE,
                                degree, numFaces);
                        camMetadata.update(QCAMERA3_STATS_SMILE_CONFIDENCE,
                                confidence, numFaces);
                    }
                    IF_META_AVAILABLE(cam_face_gaze_data_t, gazes,
                            CAM_INTF_META_FACE_GAZE, metadata) {
                        int8_t angle[MAX_ROI];
                        int32_t direction[MAX_ROI * 3];
                        int8_t degree[MAX_ROI * 2];
                        for (size_t i = 0; i < numFaces; i++) {
                            angle[i] = gazes->gaze[i].gaze_angle;
                            direction[3 * i] = gazes->gaze[i].updown_dir;
                            direction[3 * i + 1] = gazes->gaze[i].leftright_dir;
                            direction[3 * i + 2] = gazes->gaze[i].roll_dir;
                            degree[2 * i] = gazes->gaze[i].left_right_gaze;
                            degree[2 * i + 1] = gazes->gaze[i].top_bottom_gaze;

                            LOGL("FD_DEBUG LANDMARK : Frame[%d] : Face[%d] : gaze_angle=%d, "
                                    "updown_dir=%d, leftright_dir=%d,, roll_dir=%d, "
                                    "left_right_gaze=%d, top_bottom_gaze=%d",
                                    faceDetectionInfo->frame_id, i, angle[i],
                                    direction[3 * i], direction[3 * i + 1],
                                    direction[3 * i + 2],
                                    degree[2 * i], degree[2 * i + 1]);
                        }
                        camMetadata.update(QCAMERA3_STATS_GAZE_ANGLE,
                                (uint8_t *)angle, numFaces);
                        camMetadata.update(QCAMERA3_STATS_GAZE_DIRECTION,
                                direction, numFaces * 3);
                        camMetadata.update(QCAMERA3_STATS_GAZE_DEGREE,
                                (uint8_t *)degree, numFaces * 2);
                    }
                }
            }
        }
    }

    IF_META_AVAILABLE(uint32_t, histogramMode, CAM_INTF_META_STATS_HISTOGRAM_MODE, metadata) {
        uint8_t fwk_histogramMode = (uint8_t) *histogramMode;
        camMetadata.update(QCAMERA3_HISTOGRAM_MODE, &fwk_histogramMode, 1);

        if (fwk_histogramMode == QCAMERA3_HISTOGRAM_MODE_ON) {
            IF_META_AVAILABLE(cam_hist_stats_t, stats_data, CAM_INTF_META_HISTOGRAM, metadata) {
                // process histogram statistics info
                uint32_t hist_buf[4][CAM_HISTOGRAM_STATS_SIZE];
                uint32_t hist_size = sizeof(cam_histogram_data_t::hist_buf);
                cam_histogram_data_t rHistData, grHistData, gbHistData, bHistData;
                memset(&rHistData, 0, sizeof(rHistData));
                memset(&grHistData, 0, sizeof(grHistData));
                memset(&gbHistData, 0, sizeof(gbHistData));
                memset(&bHistData, 0, sizeof(bHistData));

                switch (stats_data->type) {
                case CAM_HISTOGRAM_TYPE_BAYER:
                    switch (stats_data->bayer_stats.data_type) {
                        case CAM_STATS_CHANNEL_GR:
                            rHistData = grHistData = gbHistData = bHistData =
                                    stats_data->bayer_stats.gr_stats;
                            break;
                        case CAM_STATS_CHANNEL_GB:
                            rHistData = grHistData = gbHistData = bHistData =
                                    stats_data->bayer_stats.gb_stats;
                            break;
                        case CAM_STATS_CHANNEL_B:
                            rHistData = grHistData = gbHistData = bHistData =
                                    stats_data->bayer_stats.b_stats;
                            break;
                        case CAM_STATS_CHANNEL_ALL:
                            rHistData = stats_data->bayer_stats.r_stats;
                            gbHistData = stats_data->bayer_stats.gb_stats;
                            grHistData = stats_data->bayer_stats.gr_stats;
                            bHistData = stats_data->bayer_stats.b_stats;
                            break;
                        case CAM_STATS_CHANNEL_Y:
                        case CAM_STATS_CHANNEL_R:
                        default:
                            rHistData = grHistData = gbHistData = bHistData =
                                    stats_data->bayer_stats.r_stats;
                            break;
                    }
                    break;
                case CAM_HISTOGRAM_TYPE_YUV:
                    rHistData = grHistData = gbHistData = bHistData =
                            stats_data->yuv_stats;
                    break;
                }

                memcpy(hist_buf, rHistData.hist_buf, hist_size);
                memcpy(hist_buf[1], gbHistData.hist_buf, hist_size);
                memcpy(hist_buf[2], grHistData.hist_buf, hist_size);
                memcpy(hist_buf[3], bHistData.hist_buf, hist_size);

                camMetadata.update(QCAMERA3_HISTOGRAM_STATS, (int32_t*)hist_buf, hist_size*4);
            }
        }
    }

    IF_META_AVAILABLE(uint32_t, sharpnessMapMode,
            CAM_INTF_META_STATS_SHARPNESS_MAP_MODE, metadata) {
        uint8_t fwk_sharpnessMapMode = (uint8_t) *sharpnessMapMode;
        camMetadata.update(ANDROID_STATISTICS_SHARPNESS_MAP_MODE, &fwk_sharpnessMapMode, 1);
    }

    IF_META_AVAILABLE(cam_sharpness_map_t, sharpnessMap,
            CAM_INTF_META_STATS_SHARPNESS_MAP, metadata) {
        camMetadata.update(ANDROID_STATISTICS_SHARPNESS_MAP, (int32_t *)sharpnessMap->sharpness,
                CAM_MAX_MAP_WIDTH * CAM_MAX_MAP_HEIGHT * 3);
    }

    IF_META_AVAILABLE(cam_lens_shading_map_t, lensShadingMap,
            CAM_INTF_META_LENS_SHADING_MAP, metadata) {
        size_t map_height = MIN((size_t)gCamCapability[mCameraId]->lens_shading_map_size.height,
                CAM_MAX_SHADING_MAP_HEIGHT);
        size_t map_width = MIN((size_t)gCamCapability[mCameraId]->lens_shading_map_size.width,
                CAM_MAX_SHADING_MAP_WIDTH);
        camMetadata.update(ANDROID_STATISTICS_LENS_SHADING_MAP,
                lensShadingMap->lens_shading, 4U * map_width * map_height);
    }

    IF_META_AVAILABLE(uint32_t, toneMapMode, CAM_INTF_META_TONEMAP_MODE, metadata) {
        uint8_t fwk_toneMapMode = (uint8_t) *toneMapMode;
        camMetadata.update(ANDROID_TONEMAP_MODE, &fwk_toneMapMode, 1);
    }

    IF_META_AVAILABLE(cam_rgb_tonemap_curves, tonemap, CAM_INTF_META_TONEMAP_CURVES, metadata) {
        //Populate CAM_INTF_META_TONEMAP_CURVES
        /* ch0 = G, ch 1 = B, ch 2 = R*/
        if (tonemap->tonemap_points_cnt > CAM_MAX_TONEMAP_CURVE_SIZE) {
            LOGE("Fatal: tonemap_points_cnt %d exceeds max value of %d",
                     tonemap->tonemap_points_cnt,
                    CAM_MAX_TONEMAP_CURVE_SIZE);
            tonemap->tonemap_points_cnt = CAM_MAX_TONEMAP_CURVE_SIZE;
        }

        camMetadata.update(ANDROID_TONEMAP_CURVE_GREEN,
                        &tonemap->curves[0].tonemap_points[0][0],
                        tonemap->tonemap_points_cnt * 2);

        camMetadata.update(ANDROID_TONEMAP_CURVE_BLUE,
                        &tonemap->curves[1].tonemap_points[0][0],
                        tonemap->tonemap_points_cnt * 2);

        camMetadata.update(ANDROID_TONEMAP_CURVE_RED,
                        &tonemap->curves[2].tonemap_points[0][0],
                        tonemap->tonemap_points_cnt * 2);
    }

    IF_META_AVAILABLE(cam_color_correct_gains_t, colorCorrectionGains,
            CAM_INTF_META_COLOR_CORRECT_GAINS, metadata) {
        camMetadata.update(ANDROID_COLOR_CORRECTION_GAINS, colorCorrectionGains->gains,
                CC_GAIN_MAX);
    }

    IF_META_AVAILABLE(cam_color_correct_matrix_t, colorCorrectionMatrix,
            CAM_INTF_META_COLOR_CORRECT_TRANSFORM, metadata) {
        camMetadata.update(ANDROID_COLOR_CORRECTION_TRANSFORM,
                (camera_metadata_rational_t *)(void *)colorCorrectionMatrix->transform_matrix,
                CC_MATRIX_COLS * CC_MATRIX_ROWS);
    }

    IF_META_AVAILABLE(cam_profile_tone_curve, toneCurve,
            CAM_INTF_META_PROFILE_TONE_CURVE, metadata) {
        if (toneCurve->tonemap_points_cnt > CAM_MAX_TONEMAP_CURVE_SIZE) {
            LOGE("Fatal: tonemap_points_cnt %d exceeds max value of %d",
                     toneCurve->tonemap_points_cnt,
                    CAM_MAX_TONEMAP_CURVE_SIZE);
            toneCurve->tonemap_points_cnt = CAM_MAX_TONEMAP_CURVE_SIZE;
        }
        camMetadata.update(ANDROID_SENSOR_PROFILE_TONE_CURVE,
                (float*)toneCurve->curve.tonemap_points,
                toneCurve->tonemap_points_cnt * 2);
    }

    IF_META_AVAILABLE(cam_color_correct_gains_t, predColorCorrectionGains,
            CAM_INTF_META_PRED_COLOR_CORRECT_GAINS, metadata) {
        camMetadata.update(ANDROID_STATISTICS_PREDICTED_COLOR_GAINS,
                predColorCorrectionGains->gains, 4);
    }

    IF_META_AVAILABLE(cam_color_correct_matrix_t, predColorCorrectionMatrix,
            CAM_INTF_META_PRED_COLOR_CORRECT_TRANSFORM, metadata) {
        camMetadata.update(ANDROID_STATISTICS_PREDICTED_COLOR_TRANSFORM,
                (camera_metadata_rational_t *)(void *)predColorCorrectionMatrix->transform_matrix,
                CC_MATRIX_ROWS * CC_MATRIX_COLS);
    }

    IF_META_AVAILABLE(float, otpWbGrGb, CAM_INTF_META_OTP_WB_GRGB, metadata) {
        camMetadata.update(ANDROID_SENSOR_GREEN_SPLIT, otpWbGrGb, 1);
    }

    IF_META_AVAILABLE(uint32_t, blackLevelLock, CAM_INTF_META_BLACK_LEVEL_LOCK, metadata) {
        uint8_t fwk_blackLevelLock = (uint8_t) *blackLevelLock;
        camMetadata.update(ANDROID_BLACK_LEVEL_LOCK, &fwk_blackLevelLock, 1);
    }

    IF_META_AVAILABLE(uint32_t, sceneFlicker, CAM_INTF_META_SCENE_FLICKER, metadata) {
        uint8_t fwk_sceneFlicker = (uint8_t) *sceneFlicker;
        camMetadata.update(ANDROID_STATISTICS_SCENE_FLICKER, &fwk_sceneFlicker, 1);
    }

    IF_META_AVAILABLE(uint32_t, effectMode, CAM_INTF_PARM_EFFECT, metadata) {
        int val = lookupFwkName(EFFECT_MODES_MAP, METADATA_MAP_SIZE(EFFECT_MODES_MAP),
                *effectMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwk_effectMode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_EFFECT_MODE, &fwk_effectMode, 1);
        }
    }

    IF_META_AVAILABLE(cam_test_pattern_data_t, testPatternData,
            CAM_INTF_META_TEST_PATTERN_DATA, metadata) {
        int32_t fwk_testPatternMode = lookupFwkName(TEST_PATTERN_MAP,
                METADATA_MAP_SIZE(TEST_PATTERN_MAP), testPatternData->mode);
        if (NAME_NOT_FOUND != fwk_testPatternMode) {
            camMetadata.update(ANDROID_SENSOR_TEST_PATTERN_MODE, &fwk_testPatternMode, 1);
        }
        int32_t fwk_testPatternData[4];
        fwk_testPatternData[0] = testPatternData->r;
        fwk_testPatternData[3] = testPatternData->b;
        switch (gCamCapability[mCameraId]->color_arrangement) {
        case CAM_FILTER_ARRANGEMENT_RGGB:
        case CAM_FILTER_ARRANGEMENT_GRBG:
            fwk_testPatternData[1] = testPatternData->gr;
            fwk_testPatternData[2] = testPatternData->gb;
            break;
        case CAM_FILTER_ARRANGEMENT_GBRG:
        case CAM_FILTER_ARRANGEMENT_BGGR:
            fwk_testPatternData[2] = testPatternData->gr;
            fwk_testPatternData[1] = testPatternData->gb;
            break;
        default:
            LOGE("color arrangement %d is not supported",
                gCamCapability[mCameraId]->color_arrangement);
            break;
        }
        camMetadata.update(ANDROID_SENSOR_TEST_PATTERN_DATA, fwk_testPatternData, 4);
    }

    IF_META_AVAILABLE(double, gps_coords, CAM_INTF_META_JPEG_GPS_COORDINATES, metadata) {
        camMetadata.update(ANDROID_JPEG_GPS_COORDINATES, gps_coords, 3);
    }

    IF_META_AVAILABLE(uint8_t, gps_methods, CAM_INTF_META_JPEG_GPS_PROC_METHODS, metadata) {
        String8 str((const char *)gps_methods);
        camMetadata.update(ANDROID_JPEG_GPS_PROCESSING_METHOD, str);
    }

    IF_META_AVAILABLE(int64_t, gps_timestamp, CAM_INTF_META_JPEG_GPS_TIMESTAMP, metadata) {
        camMetadata.update(ANDROID_JPEG_GPS_TIMESTAMP, gps_timestamp, 1);
    }

    IF_META_AVAILABLE(int32_t, jpeg_orientation, CAM_INTF_META_JPEG_ORIENTATION, metadata) {
        camMetadata.update(ANDROID_JPEG_ORIENTATION, jpeg_orientation, 1);
    }

    IF_META_AVAILABLE(uint32_t, jpeg_quality, CAM_INTF_META_JPEG_QUALITY, metadata) {
        uint8_t fwk_jpeg_quality = (uint8_t) *jpeg_quality;
        camMetadata.update(ANDROID_JPEG_QUALITY, &fwk_jpeg_quality, 1);
    }

    IF_META_AVAILABLE(uint32_t, thumb_quality, CAM_INTF_META_JPEG_THUMB_QUALITY, metadata) {
        uint8_t fwk_thumb_quality = (uint8_t) *thumb_quality;
        camMetadata.update(ANDROID_JPEG_THUMBNAIL_QUALITY, &fwk_thumb_quality, 1);
    }

    IF_META_AVAILABLE(cam_dimension_t, thumb_size, CAM_INTF_META_JPEG_THUMB_SIZE, metadata) {
        int32_t fwk_thumb_size[2];
        fwk_thumb_size[0] = thumb_size->width;
        fwk_thumb_size[1] = thumb_size->height;
        camMetadata.update(ANDROID_JPEG_THUMBNAIL_SIZE, fwk_thumb_size, 2);
    }

    IF_META_AVAILABLE(int32_t, privateData, CAM_INTF_META_PRIVATE_DATA, metadata) {
        camMetadata.update(QCAMERA3_PRIVATEDATA_REPROCESS,
                privateData,
                MAX_METADATA_PRIVATE_PAYLOAD_SIZE_IN_BYTES / sizeof(int32_t));
    }

    IF_META_AVAILABLE(int32_t, meteringMode, CAM_INTF_PARM_AEC_ALGO_TYPE, metadata) {
        camMetadata.update(QCAMERA3_EXPOSURE_METER,
                meteringMode, 1);
    }

    IF_META_AVAILABLE(cam_asd_hdr_scene_data_t, hdr_scene_data,
            CAM_INTF_META_ASD_HDR_SCENE_DATA, metadata) {
        LOGD("hdr_scene_data: %d %f\n",
                hdr_scene_data->is_hdr_scene, hdr_scene_data->hdr_confidence);
        uint8_t isHdr = hdr_scene_data->is_hdr_scene;
        float isHdrConfidence = hdr_scene_data->hdr_confidence;
        camMetadata.update(QCAMERA3_STATS_IS_HDR_SCENE,
                           &isHdr, 1);
        camMetadata.update(QCAMERA3_STATS_IS_HDR_SCENE_CONFIDENCE,
                           &isHdrConfidence, 1);
    }



    IF_META_AVAILABLE(tuning_params_t, tuning_ptr, CAM_INTF_META_TUNING_PARAMS, metadata) {
        uint8_t tuning_meta_data_blob[sizeof(tuning_params_t)];
        uint8_t *data = (uint8_t *)&tuning_meta_data_blob[0];
        tuning_ptr->tuning_data_version = TUNING_DATA_VERSION;


        memcpy(data, ((uint8_t *)&tuning_ptr->tuning_data_version),
                sizeof(uint32_t));
        data += sizeof(uint32_t);

        memcpy(data, ((uint8_t *)&tuning_ptr->tuning_sensor_data_size),
                sizeof(uint32_t));
        LOGD("tuning_sensor_data_size %d",(int)(*(int *)data));
        data += sizeof(uint32_t);

        memcpy(data, ((uint8_t *)&tuning_ptr->tuning_vfe_data_size),
                sizeof(uint32_t));
        LOGD("tuning_vfe_data_size %d",(int)(*(int *)data));
        data += sizeof(uint32_t);

        memcpy(data, ((uint8_t *)&tuning_ptr->tuning_cpp_data_size),
                sizeof(uint32_t));
        LOGD("tuning_cpp_data_size %d",(int)(*(int *)data));
        data += sizeof(uint32_t);

        memcpy(data, ((uint8_t *)&tuning_ptr->tuning_cac_data_size),
                sizeof(uint32_t));
        LOGD("tuning_cac_data_size %d",(int)(*(int *)data));
        data += sizeof(uint32_t);

        tuning_ptr->tuning_mod3_data_size = 0;
        memcpy(data, ((uint8_t *)&tuning_ptr->tuning_mod3_data_size),
                sizeof(uint32_t));
        LOGD("tuning_mod3_data_size %d",(int)(*(int *)data));
        data += sizeof(uint32_t);

        size_t count = MIN(tuning_ptr->tuning_sensor_data_size,
                TUNING_SENSOR_DATA_MAX);
        memcpy(data, ((uint8_t *)&tuning_ptr->data),
                count);
        data += count;

        count = MIN(tuning_ptr->tuning_vfe_data_size,
                TUNING_VFE_DATA_MAX);
        memcpy(data, ((uint8_t *)&tuning_ptr->data[TUNING_VFE_DATA_OFFSET]),
                count);
        data += count;

        count = MIN(tuning_ptr->tuning_cpp_data_size,
                TUNING_CPP_DATA_MAX);
        memcpy(data, ((uint8_t *)&tuning_ptr->data[TUNING_CPP_DATA_OFFSET]),
                count);
        data += count;

        count = MIN(tuning_ptr->tuning_cac_data_size,
                TUNING_CAC_DATA_MAX);
        memcpy(data, ((uint8_t *)&tuning_ptr->data[TUNING_CAC_DATA_OFFSET]),
                count);
        data += count;

        camMetadata.update(QCAMERA3_TUNING_META_DATA_BLOB,
                (int32_t *)(void *)tuning_meta_data_blob,
                (size_t)(data-tuning_meta_data_blob) / sizeof(uint32_t));
    }

    IF_META_AVAILABLE(cam_neutral_col_point_t, neuColPoint,
            CAM_INTF_META_NEUTRAL_COL_POINT, metadata) {
        camMetadata.update(ANDROID_SENSOR_NEUTRAL_COLOR_POINT,
                (camera_metadata_rational_t *)(void *)neuColPoint->neutral_col_point,
                NEUTRAL_COL_POINTS);
    }

    IF_META_AVAILABLE(uint32_t, shadingMapMode, CAM_INTF_META_LENS_SHADING_MAP_MODE, metadata) {
        uint8_t fwk_shadingMapMode = (uint8_t) *shadingMapMode;
        camMetadata.update(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &fwk_shadingMapMode, 1);
    }

    IF_META_AVAILABLE(cam_area_t, hAeRegions, CAM_INTF_META_AEC_ROI, metadata) {
        int32_t aeRegions[REGIONS_TUPLE_COUNT];
        // Adjust crop region from sensor output coordinate system to active
        // array coordinate system.
        mCropRegionMapper.toActiveArray(hAeRegions->rect.left, hAeRegions->rect.top,
                hAeRegions->rect.width, hAeRegions->rect.height);

        convertToRegions(hAeRegions->rect, aeRegions, hAeRegions->weight);
        camMetadata.update(ANDROID_CONTROL_AE_REGIONS, aeRegions,
                REGIONS_TUPLE_COUNT);
        LOGD("Metadata : ANDROID_CONTROL_AE_REGIONS: FWK: [%d,%d,%d,%d] HAL: [%d,%d,%d,%d]",
                 aeRegions[0], aeRegions[1], aeRegions[2], aeRegions[3],
                hAeRegions->rect.left, hAeRegions->rect.top, hAeRegions->rect.width,
                hAeRegions->rect.height);
    }

    IF_META_AVAILABLE(float, focusDistance, CAM_INTF_META_LENS_FOCUS_DISTANCE, metadata) {
        camMetadata.update(ANDROID_LENS_FOCUS_DISTANCE , focusDistance, 1);
    }

    IF_META_AVAILABLE(float, focusRange, CAM_INTF_META_LENS_FOCUS_RANGE, metadata) {
        camMetadata.update(ANDROID_LENS_FOCUS_RANGE , focusRange, 2);
    }

    IF_META_AVAILABLE(cam_af_lens_state_t, lensState, CAM_INTF_META_LENS_STATE, metadata) {
        uint8_t fwk_lensState = *lensState;
        camMetadata.update(ANDROID_LENS_STATE , &fwk_lensState, 1);
    }

    IF_META_AVAILABLE(cam_area_t, hAfRegions, CAM_INTF_META_AF_ROI, metadata) {
        /*af regions*/
        int32_t afRegions[REGIONS_TUPLE_COUNT];
        // Adjust crop region from sensor output coordinate system to active
        // array coordinate system.
        mCropRegionMapper.toActiveArray(hAfRegions->rect.left, hAfRegions->rect.top,
                hAfRegions->rect.width, hAfRegions->rect.height);

        convertToRegions(hAfRegions->rect, afRegions, hAfRegions->weight);
        camMetadata.update(ANDROID_CONTROL_AF_REGIONS, afRegions,
                REGIONS_TUPLE_COUNT);
        LOGD("Metadata : ANDROID_CONTROL_AF_REGIONS: FWK: [%d,%d,%d,%d] HAL: [%d,%d,%d,%d]",
                 afRegions[0], afRegions[1], afRegions[2], afRegions[3],
                hAfRegions->rect.left, hAfRegions->rect.top, hAfRegions->rect.width,
                hAfRegions->rect.height);
    }

    IF_META_AVAILABLE(uint32_t, hal_ab_mode, CAM_INTF_PARM_ANTIBANDING, metadata) {
        if (*hal_ab_mode == CAM_ANTIBANDING_MODE_AUTO_50HZ ||
              *hal_ab_mode == CAM_ANTIBANDING_MODE_AUTO_60HZ){
             //CAM_ANTIBANDING_MODE_AUTO_50HZ/CAM_ANTIBANDING_MODE_AUTO_60HZ
             *hal_ab_mode = CAM_ANTIBANDING_MODE_AUTO;
        }
        int val = lookupFwkName(ANTIBANDING_MODES_MAP, METADATA_MAP_SIZE(ANTIBANDING_MODES_MAP),
                *hal_ab_mode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwk_ab_mode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_AE_ANTIBANDING_MODE, &fwk_ab_mode, 1);
        }
    }

    IF_META_AVAILABLE(uint32_t, bestshotMode, CAM_INTF_PARM_BESTSHOT_MODE, metadata) {
        int val = lookupFwkName(SCENE_MODES_MAP,
                METADATA_MAP_SIZE(SCENE_MODES_MAP), *bestshotMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwkBestshotMode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_SCENE_MODE, &fwkBestshotMode, 1);
            LOGD("Metadata : ANDROID_CONTROL_SCENE_MODE");
        } else {
            LOGH("Metadata not found : ANDROID_CONTROL_SCENE_MODE");
        }
    }

    IF_META_AVAILABLE(uint32_t, mode, CAM_INTF_META_MODE, metadata) {
         uint8_t fwk_mode = (uint8_t) *mode;
         camMetadata.update(ANDROID_CONTROL_MODE, &fwk_mode, 1);
    }


    uint8_t hotPixelMapMode = ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF;
    camMetadata.update(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE, &hotPixelMapMode, 1);

    int32_t hotPixelMap[2];
    camMetadata.update(ANDROID_STATISTICS_HOT_PIXEL_MAP, &hotPixelMap[0], 0);

    // CDS
    IF_META_AVAILABLE(int32_t, cds, CAM_INTF_PARM_CDS_MODE, metadata) {
        camMetadata.update(QCAMERA3_CDS_MODE, cds, 1);
    }

    IF_META_AVAILABLE(cam_sensor_hdr_type_t, vhdr, CAM_INTF_PARM_SENSOR_HDR, metadata) {
        int32_t fwk_hdr;
        int8_t curr_hdr_state = ((mCurrFeatureState & CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR) != 0);
        if(*vhdr == CAM_SENSOR_HDR_OFF) {
            fwk_hdr = QCAMERA3_VIDEO_HDR_MODE_OFF;
        } else {
            fwk_hdr = QCAMERA3_VIDEO_HDR_MODE_ON;
        }

        if(fwk_hdr != curr_hdr_state) {
           LOGH("PROFILE_META_HDR_TOGGLED value=%d", fwk_hdr);
           if(fwk_hdr)
              mCurrFeatureState |= CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR;
           else
              mCurrFeatureState &= ~CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR;
        }
        camMetadata.update(QCAMERA3_VIDEO_HDR_MODE, &fwk_hdr, 1);
    }

    //binning correction
    IF_META_AVAILABLE(cam_binning_correction_mode_t, bin_correction,
            CAM_INTF_META_BINNING_CORRECTION_MODE, metadata) {
        int32_t fwk_bin_mode = (int32_t) *bin_correction;
        camMetadata.update(QCAMERA3_BINNING_CORRECTION_MODE, &fwk_bin_mode, 1);
    }

    IF_META_AVAILABLE(cam_ir_mode_type_t, ir, CAM_INTF_META_IR_MODE, metadata) {
        int32_t fwk_ir = (int32_t) *ir;
        int8_t curr_ir_state = ((mCurrFeatureState & CAM_QCOM_FEATURE_IR ) != 0);
        int8_t is_ir_on = 0;

        (fwk_ir > 0) ? (is_ir_on = 1) : (is_ir_on = 0) ;
        if(is_ir_on != curr_ir_state) {
           LOGH("PROFILE_META_IR_TOGGLED value=%d", fwk_ir);
           if(is_ir_on)
              mCurrFeatureState |= CAM_QCOM_FEATURE_IR;
           else
              mCurrFeatureState &= ~CAM_QCOM_FEATURE_IR;
        }
        camMetadata.update(QCAMERA3_IR_MODE, &fwk_ir, 1);
    }

    // AEC SPEED
    IF_META_AVAILABLE(float, aec, CAM_INTF_META_AEC_CONVERGENCE_SPEED, metadata) {
        camMetadata.update(QCAMERA3_AEC_CONVERGENCE_SPEED, aec, 1);
    }

    // AWB SPEED
    IF_META_AVAILABLE(float, awb, CAM_INTF_META_AWB_CONVERGENCE_SPEED, metadata) {
        camMetadata.update(QCAMERA3_AWB_CONVERGENCE_SPEED, awb, 1);
    }

    // TNR
    IF_META_AVAILABLE(cam_denoise_param_t, tnr, CAM_INTF_PARM_TEMPORAL_DENOISE, metadata) {
        uint8_t tnr_enable       = tnr->denoise_enable;
        int32_t tnr_process_type = (int32_t)tnr->process_plates;
        int8_t curr_tnr_state = ((mCurrFeatureState & CAM_QTI_FEATURE_SW_TNR) != 0) ;
        int8_t is_tnr_on = 0;

        (tnr_enable > 0) ? (is_tnr_on = 1) : (is_tnr_on = 0);
        if(is_tnr_on != curr_tnr_state) {
           LOGH("PROFILE_META_TNR_TOGGLED value=%d", tnr_enable);
           if(is_tnr_on)
              mCurrFeatureState |= CAM_QTI_FEATURE_SW_TNR;
           else
              mCurrFeatureState &= ~CAM_QTI_FEATURE_SW_TNR;
        }

        camMetadata.update(QCAMERA3_TEMPORAL_DENOISE_ENABLE, &tnr_enable, 1);
        camMetadata.update(QCAMERA3_TEMPORAL_DENOISE_PROCESS_TYPE, &tnr_process_type, 1);
    }

    // Reprocess crop data
    IF_META_AVAILABLE(cam_crop_data_t, crop_data, CAM_INTF_META_CROP_DATA, metadata) {
        uint8_t cnt = crop_data->num_of_streams;
        if ( (0 >= cnt) || (cnt > MAX_NUM_STREAMS)) {
            // mm-qcamera-daemon only posts crop_data for streams
            // not linked to pproc. So no valid crop metadata is not
            // necessarily an error case.
            LOGD("No valid crop metadata entries");
        } else {
            uint32_t reproc_stream_id;
            if ( NO_ERROR != getReprocessibleOutputStreamId(reproc_stream_id)) {
                LOGD("No reprocessible stream found, ignore crop data");
            } else {
                int rc = NO_ERROR;
                Vector<int32_t> roi_map;
                int32_t *crop = new int32_t[cnt*4];
                if (NULL == crop) {
                   rc = NO_MEMORY;
                }
                if (NO_ERROR == rc) {
                    int32_t streams_found = 0;
                    for (size_t i = 0; i < cnt; i++) {
                        if (crop_data->crop_info[i].stream_id == reproc_stream_id) {
                            if (pprocDone) {
                                // HAL already does internal reprocessing,
                                // either via reprocessing before JPEG encoding,
                                // or offline postprocessing for pproc bypass case.
                                crop[0] = 0;
                                crop[1] = 0;
                                crop[2] = mInputStreamInfo.dim.width;
                                crop[3] = mInputStreamInfo.dim.height;
                            } else {
                                crop[0] = crop_data->crop_info[i].crop.left;
                                crop[1] = crop_data->crop_info[i].crop.top;
                                crop[2] = crop_data->crop_info[i].crop.width;
                                crop[3] = crop_data->crop_info[i].crop.height;
                            }
                            roi_map.add(crop_data->crop_info[i].roi_map.left);
                            roi_map.add(crop_data->crop_info[i].roi_map.top);
                            roi_map.add(crop_data->crop_info[i].roi_map.width);
                            roi_map.add(crop_data->crop_info[i].roi_map.height);
                            streams_found++;
                            LOGD("Adding reprocess crop data for stream %dx%d, %dx%d",
                                    crop[0], crop[1], crop[2], crop[3]);
                            LOGD("Adding reprocess crop roi map for stream %dx%d, %dx%d",
                                    crop_data->crop_info[i].roi_map.left,
                                    crop_data->crop_info[i].roi_map.top,
                                    crop_data->crop_info[i].roi_map.width,
                                    crop_data->crop_info[i].roi_map.height);
                            break;

                       }
                    }
                    camMetadata.update(QCAMERA3_CROP_COUNT_REPROCESS,
                            &streams_found, 1);
                    camMetadata.update(QCAMERA3_CROP_REPROCESS,
                            crop, (size_t)(streams_found * 4));
                    if (roi_map.array()) {
                        camMetadata.update(QCAMERA3_CROP_ROI_MAP_REPROCESS,
                                roi_map.array(), roi_map.size());
                    }
               }
               if (crop) {
                   delete [] crop;
               }
            }
        }
    }

    if (gCamCapability[mCameraId]->aberration_modes_count == 0) {
        // Regardless of CAC supports or not, CTS is expecting the CAC result to be non NULL and
        // so hardcoding the CAC result to OFF mode.
        uint8_t fwkCacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF;
        camMetadata.update(ANDROID_COLOR_CORRECTION_ABERRATION_MODE, &fwkCacMode, 1);
    } else {
        IF_META_AVAILABLE(cam_aberration_mode_t, cacMode, CAM_INTF_PARM_CAC, metadata) {
            int val = lookupFwkName(COLOR_ABERRATION_MAP, METADATA_MAP_SIZE(COLOR_ABERRATION_MAP),
                    *cacMode);
            if (NAME_NOT_FOUND != val) {
                uint8_t resultCacMode = (uint8_t)val;
                // check whether CAC result from CB is equal to Framework set CAC mode
                // If not equal then set the CAC mode came in corresponding request
                if (fwk_cacMode != resultCacMode) {
                    resultCacMode = fwk_cacMode;
                }
                //Check if CAC is disabled by property
                if (m_cacModeDisabled) {
                    resultCacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF;
                }

                LOGD("fwk_cacMode=%d resultCacMode=%d", fwk_cacMode, resultCacMode);
                camMetadata.update(ANDROID_COLOR_CORRECTION_ABERRATION_MODE, &resultCacMode, 1);
            } else {
                LOGE("Invalid CAC camera parameter: %d", *cacMode);
            }
        }
    }

    // Post blob of cam_cds_data through vendor tag.
    IF_META_AVAILABLE(cam_cds_data_t, cdsInfo, CAM_INTF_META_CDS_DATA, metadata) {
        uint8_t cnt = cdsInfo->num_of_streams;
        cam_cds_data_t cdsDataOverride;
        memset(&cdsDataOverride, 0, sizeof(cdsDataOverride));
        cdsDataOverride.session_cds_enable = cdsInfo->session_cds_enable;
        cdsDataOverride.num_of_streams = 1;
        if ((0 < cnt) && (cnt <= MAX_NUM_STREAMS)) {
            uint32_t reproc_stream_id;
            if ( NO_ERROR != getReprocessibleOutputStreamId(reproc_stream_id)) {
                LOGD("No reprocessible stream found, ignore cds data");
            } else {
                for (size_t i = 0; i < cnt; i++) {
                    if (cdsInfo->cds_info[i].stream_id ==
                            reproc_stream_id) {
                        cdsDataOverride.cds_info[0].cds_enable =
                                cdsInfo->cds_info[i].cds_enable;
                        break;
                    }
                }
            }
        } else {
            LOGD("Invalid stream count %d in CDS_DATA", cnt);
        }
        camMetadata.update(QCAMERA3_CDS_INFO,
                (uint8_t *)&cdsDataOverride,
                sizeof(cam_cds_data_t));
    }

    // Ldaf calibration data
    if (!mLdafCalibExist) {
        IF_META_AVAILABLE(uint32_t, ldafCalib,
                CAM_INTF_META_LDAF_EXIF, metadata) {
            mLdafCalibExist = true;
            mLdafCalib[0] = ldafCalib[0];
            mLdafCalib[1] = ldafCalib[1];
            LOGD("ldafCalib[0] is %d, ldafCalib[1] is %d",
                    ldafCalib[0], ldafCalib[1]);
        }
    }

    // EXIF debug data through vendor tag
    /*
     * Mobicat Mask can assume 3 values:
     * 1 refers to Mobicat data,
     * 2 refers to Stats Debug and Exif Debug Data
     * 3 refers to Mobicat and Stats Debug Data
     * We want to make sure that we are sending Exif debug data
     * only when Mobicat Mask is 2.
     */
    if ((mExifParams.debug_params != NULL) && (getMobicatMask() == 2)) {
        camMetadata.update(QCAMERA3_HAL_PRIVATEDATA_EXIF_DEBUG_DATA_BLOB,
                (uint8_t *)(void *)mExifParams.debug_params,
                sizeof(mm_jpeg_debug_exif_params_t));
    }

    // Reprocess and DDM debug data through vendor tag
    cam_reprocess_info_t repro_info;
    memset(&repro_info, 0, sizeof(cam_reprocess_info_t));
    IF_META_AVAILABLE(cam_stream_crop_info_t, sensorCropInfo,
            CAM_INTF_META_SNAP_CROP_INFO_SENSOR, metadata) {
        memcpy(&(repro_info.sensor_crop_info), sensorCropInfo, sizeof(cam_stream_crop_info_t));
    }
    IF_META_AVAILABLE(cam_stream_crop_info_t, camifCropInfo,
            CAM_INTF_META_SNAP_CROP_INFO_CAMIF, metadata) {
        memcpy(&(repro_info.camif_crop_info), camifCropInfo, sizeof(cam_stream_crop_info_t));
    }
    IF_META_AVAILABLE(cam_stream_crop_info_t, ispCropInfo,
            CAM_INTF_META_SNAP_CROP_INFO_ISP, metadata) {
        memcpy(&(repro_info.isp_crop_info), ispCropInfo, sizeof(cam_stream_crop_info_t));
    }
    IF_META_AVAILABLE(cam_stream_crop_info_t, cppCropInfo,
            CAM_INTF_META_SNAP_CROP_INFO_CPP, metadata) {
        memcpy(&(repro_info.cpp_crop_info), cppCropInfo, sizeof(cam_stream_crop_info_t));
    }
    IF_META_AVAILABLE(cam_focal_length_ratio_t, ratio,
            CAM_INTF_META_AF_FOCAL_LENGTH_RATIO, metadata) {
        memcpy(&(repro_info.af_focal_length_ratio), ratio, sizeof(cam_focal_length_ratio_t));
    }
    IF_META_AVAILABLE(int32_t, flip, CAM_INTF_PARM_FLIP, metadata) {
        memcpy(&(repro_info.pipeline_flip), flip, sizeof(int32_t));
    } else {
        memcpy(&(repro_info.pipeline_flip), &gCamCapability[mCameraId]->sensor_rotation, sizeof(int32_t));
    }
    IF_META_AVAILABLE(cam_rotation_info_t, rotationInfo,
            CAM_INTF_PARM_ROTATION, metadata) {
        memcpy(&(repro_info.rotation_info), rotationInfo, sizeof(cam_rotation_info_t));
    }
    IF_META_AVAILABLE(cam_area_t, afRoi, CAM_INTF_META_AF_ROI, metadata) {
        memcpy(&(repro_info.af_roi), afRoi, sizeof(cam_area_t));
    }
    IF_META_AVAILABLE(cam_dyn_img_data_t, dynMask, CAM_INTF_META_IMG_DYN_FEAT, metadata) {
        memcpy(&(repro_info.dyn_mask), dynMask, sizeof(cam_dyn_img_data_t));
    }
    camMetadata.update(QCAMERA3_HAL_PRIVATEDATA_REPROCESS_DATA_BLOB,
        (uint8_t *)&repro_info, sizeof(cam_reprocess_info_t));

    // INSTANT AEC MODE
    IF_META_AVAILABLE(uint8_t, instant_aec_mode,
            CAM_INTF_PARM_INSTANT_AEC, metadata) {
        camMetadata.update(QCAMERA3_INSTANT_AEC_MODE, instant_aec_mode, 1);
    }

    //Bokeh status
    IF_META_AVAILABLE(cam_rtb_msg_type_t, rtbStatus,
            CAM_INTF_META_RTB_DATA, metadata) {
        LOGD("Bokeh status %d", *rtbStatus);
        mRTBStatus = *rtbStatus;
        camMetadata.update(QCAMERA3_BOKEH_STATUS, (int32_t*)rtbStatus, 1);
    }

    if(isDualCamera() && (getHalPPType() == CAM_HAL_PP_TYPE_DUAL_FOV) )
    {
        uint8_t status = mBundledSnapshot;
        camMetadata.update(QCAMERA3_FUSION_STATUS, (uint8_t *)&status, 1);
    }

    camMetadata.update(ANDROID_CONTROL_ENABLE_ZSL, (uint8_t*)&enableZSL, 1);
    /* In batch mode, cache the first metadata in the batch */
    if (mBatchSize && firstMetadataInBatch) {
        mCachedMetadata.clear();
        mCachedMetadata = camMetadata;
    }

    resultMetadata = camMetadata.release();
    return resultMetadata;
}

/*===========================================================================
 * FUNCTION   : saveExifParams
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *   @metadata : metadata information from callback
 *
 * RETURN     : none
 *
 *==========================================================================*/
void QCamera3HardwareInterface::saveExifParams(metadata_buffer_t *metadata)
{
    IF_META_AVAILABLE(cam_ae_exif_debug_t, ae_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_AE, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->ae_debug_params = *ae_exif_debug_params;
            mExifParams.debug_params->ae_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_awb_exif_debug_t,awb_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_AWB, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->awb_debug_params = *awb_exif_debug_params;
            mExifParams.debug_params->awb_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_af_exif_debug_t,af_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_AF, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->af_debug_params = *af_exif_debug_params;
            mExifParams.debug_params->af_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_asd_exif_debug_t, asd_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_ASD, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->asd_debug_params = *asd_exif_debug_params;
            mExifParams.debug_params->asd_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_stats_buffer_exif_debug_t,stats_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_STATS, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->stats_debug_params = *stats_exif_debug_params;
            mExifParams.debug_params->stats_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_bestats_buffer_exif_debug_t,bestats_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_BESTATS, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->bestats_debug_params = *bestats_exif_debug_params;
            mExifParams.debug_params->bestats_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_bhist_buffer_exif_debug_t, bhist_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_BHIST, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->bhist_debug_params = *bhist_exif_debug_params;
            mExifParams.debug_params->bhist_debug_params_valid = TRUE;
        }
    }
    IF_META_AVAILABLE(cam_q3a_tuning_info_t, q3a_tuning_exif_debug_params,
            CAM_INTF_META_EXIF_DEBUG_3A_TUNING, metadata) {
        if (mExifParams.debug_params) {
            mExifParams.debug_params->q3a_tuning_debug_params = *q3a_tuning_exif_debug_params;
            mExifParams.debug_params->q3a_tuning_debug_params_valid = TRUE;
        }
    }

    IF_META_AVAILABLE(cam_3a_params_t,ae_debug,
            CAM_INTF_META_AEC_INFO,metadata) {
       mExifParams.cam_3a_params = *ae_debug;
       mExifParams.cam_3a_params_valid = TRUE;
    }
}

/*===========================================================================
 * FUNCTION   : get3AExifParams
 *
 * DESCRIPTION:
 *
 * PARAMETERS : none
 *
 *
 * RETURN     : mm_jpeg_exif_params_t
 *
 *==========================================================================*/
mm_jpeg_exif_params_t QCamera3HardwareInterface::get3AExifParams()
{
    return mExifParams;
}

/*===========================================================================
 * FUNCTION   : translateCbUrgentMetadataToResultMetadata
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *   @metadata : metadata information from callback
 *
 * RETURN     : camera_metadata_t*
 *              metadata in a format specified by fwk
 *==========================================================================*/
camera_metadata_t*
QCamera3HardwareInterface::translateCbUrgentMetadataToResultMetadata
                                (metadata_buffer_t *metadata)
{
    CameraMetadata camMetadata;
    camera_metadata_t *resultMetadata;


    IF_META_AVAILABLE(uint32_t, whiteBalanceState, CAM_INTF_META_AWB_STATE, metadata) {
        uint8_t fwk_whiteBalanceState = (uint8_t) *whiteBalanceState;
        camMetadata.update(ANDROID_CONTROL_AWB_STATE, &fwk_whiteBalanceState, 1);
        LOGD("urgent Metadata : ANDROID_CONTROL_AWB_STATE %u", *whiteBalanceState);
    }

    IF_META_AVAILABLE(cam_trigger_t, aecTrigger, CAM_INTF_META_AEC_PRECAPTURE_TRIGGER, metadata) {
        camMetadata.update(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
                &aecTrigger->trigger, 1);
        camMetadata.update(ANDROID_CONTROL_AE_PRECAPTURE_ID,
                &aecTrigger->trigger_id, 1);
        LOGD("urgent Metadata : CAM_INTF_META_AEC_PRECAPTURE_TRIGGER: %d",
                 aecTrigger->trigger);
        LOGD("urgent Metadata : ANDROID_CONTROL_AE_PRECAPTURE_ID: %d",
                aecTrigger->trigger_id);
    }

    IF_META_AVAILABLE(uint32_t, ae_state, CAM_INTF_META_AEC_STATE, metadata) {
        uint8_t fwk_ae_state = (uint8_t) *ae_state;
        camMetadata.update(ANDROID_CONTROL_AE_STATE, &fwk_ae_state, 1);
        LOGD("urgent Metadata : ANDROID_CONTROL_AE_STATE %u", *ae_state);
    }

    IF_META_AVAILABLE(cam_3a_params_t,ae_debug, CAM_INTF_META_AEC_INFO,metadata) {
        mFlashNeeded = ae_debug->flash_needed;
        LOGD("mFlashNeeded %d", mFlashNeeded);
    }

    IF_META_AVAILABLE(uint32_t, afState, CAM_INTF_META_AF_STATE, metadata) {
        uint8_t fwk_afState = (uint8_t) *afState;
        camMetadata.update(ANDROID_CONTROL_AF_STATE, &fwk_afState, 1);
        LOGD("urgent Metadata : ANDROID_CONTROL_AF_STATE %u", *afState);
    }

    IF_META_AVAILABLE(uint32_t, focusMode, CAM_INTF_PARM_FOCUS_MODE, metadata) {
        int val = lookupFwkName(FOCUS_MODES_MAP, METADATA_MAP_SIZE(FOCUS_MODES_MAP), *focusMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwkAfMode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_AF_MODE, &fwkAfMode, 1);
            LOGD("urgent Metadata : ANDROID_CONTROL_AF_MODE %d", val);
        } else {
            LOGH("urgent Metadata not found : ANDROID_CONTROL_AF_MODE %d",
                    val);
        }
    }

    IF_META_AVAILABLE(cam_trigger_t, af_trigger, CAM_INTF_META_AF_TRIGGER, metadata) {
        camMetadata.update(ANDROID_CONTROL_AF_TRIGGER,
                &af_trigger->trigger, 1);
        LOGD("urgent Metadata : CAM_INTF_META_AF_TRIGGER = %d",
                 af_trigger->trigger);
        camMetadata.update(ANDROID_CONTROL_AF_TRIGGER_ID, &af_trigger->trigger_id, 1);
        LOGD("urgent Metadata : ANDROID_CONTROL_AF_TRIGGER_ID = %d",
                af_trigger->trigger_id);
    }

    IF_META_AVAILABLE(int32_t, whiteBalance, CAM_INTF_PARM_WHITE_BALANCE, metadata) {
        int val = lookupFwkName(WHITE_BALANCE_MODES_MAP,
                METADATA_MAP_SIZE(WHITE_BALANCE_MODES_MAP), *whiteBalance);
        if (NAME_NOT_FOUND != val) {
            uint8_t fwkWhiteBalanceMode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_AWB_MODE, &fwkWhiteBalanceMode, 1);
            LOGD("urgent Metadata : ANDROID_CONTROL_AWB_MODE %d", val);
        } else {
            LOGH("urgent Metadata not found : ANDROID_CONTROL_AWB_MODE");
        }
    }

    uint8_t fwk_aeMode = ANDROID_CONTROL_AE_MODE_OFF;
    uint32_t aeMode = CAM_AE_MODE_MAX;
    int32_t flashMode = CAM_FLASH_MODE_MAX;
    int32_t redeye = -1;
    IF_META_AVAILABLE(uint32_t, pAeMode, CAM_INTF_META_AEC_MODE, metadata) {
        aeMode = *pAeMode;
    }
    IF_META_AVAILABLE(int32_t, pFlashMode, CAM_INTF_PARM_LED_MODE, metadata) {
        flashMode = *pFlashMode;
    }
    IF_META_AVAILABLE(int32_t, pRedeye, CAM_INTF_PARM_REDEYE_REDUCTION, metadata) {
        redeye = *pRedeye;
    }

    if (1 == redeye) {
        fwk_aeMode = ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE;
        camMetadata.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
    } else if ((CAM_FLASH_MODE_AUTO == flashMode) || (CAM_FLASH_MODE_ON == flashMode)) {
        int val = lookupFwkName(AE_FLASH_MODE_MAP, METADATA_MAP_SIZE(AE_FLASH_MODE_MAP),
                flashMode);
        if (NAME_NOT_FOUND != val) {
            fwk_aeMode = (uint8_t)val;
            camMetadata.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
        } else {
            LOGE("Unsupported flash mode %d", flashMode);
        }
    } else if (aeMode == CAM_AE_MODE_ON) {
        fwk_aeMode = ANDROID_CONTROL_AE_MODE_ON;
        camMetadata.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
    } else if (aeMode == CAM_AE_MODE_OFF) {
        fwk_aeMode = ANDROID_CONTROL_AE_MODE_OFF;
        camMetadata.update(ANDROID_CONTROL_AE_MODE, &fwk_aeMode, 1);
    } else {
        LOGE("Not enough info to deduce ANDROID_CONTROL_AE_MODE redeye:%d, "
              "flashMode:%d, aeMode:%u!!!",
                 redeye, flashMode, aeMode);
    }
    if (mInstantAEC) {
        // Increment frame Idx count untill a bound reached for instant AEC.
        mInstantAecFrameIdxCount++;
        IF_META_AVAILABLE(cam_3a_params_t, ae_params,
                CAM_INTF_META_AEC_INFO, metadata) {
            LOGH("ae_params->settled = %d",ae_params->settled);
            // If AEC settled, or if number of frames reached bound value,
            // should reset instant AEC.
            if (ae_params->settled ||
                    (mInstantAecFrameIdxCount > mAecSkipDisplayFrameBound)) {
                LOGH("AEC settled or Frames reached instantAEC bound, resetting instantAEC");
                mInstantAEC = false;
                mResetInstantAEC = true;
                mInstantAecFrameIdxCount = 0;
            }
        }
    }
    resultMetadata = camMetadata.release();
    return resultMetadata;
}

/*===========================================================================
 * FUNCTION   : dumpMetadataToFile
 *
 * DESCRIPTION: Dumps tuning metadata to file system
 *
 * PARAMETERS :
 *   @meta           : tuning metadata
 *   @dumpFrameCount : current dump frame count
 *   @enabled        : Enable mask
 *
 *==========================================================================*/
void QCamera3HardwareInterface::dumpMetadataToFile(tuning_params_t &meta,
                                                   uint32_t &dumpFrameCount,
                                                   bool enabled,
                                                   const char *type,
                                                   uint32_t frameNumber)
{
    //Some sanity checks
    if (meta.tuning_sensor_data_size > TUNING_SENSOR_DATA_MAX) {
        LOGE("Tuning sensor data size bigger than expected %d: %d",
              meta.tuning_sensor_data_size,
              TUNING_SENSOR_DATA_MAX);
        return;
    }

    if (meta.tuning_vfe_data_size > TUNING_VFE_DATA_MAX) {
        LOGE("Tuning VFE data size bigger than expected %d: %d",
              meta.tuning_vfe_data_size,
              TUNING_VFE_DATA_MAX);
        return;
    }

    if (meta.tuning_cpp_data_size > TUNING_CPP_DATA_MAX) {
        LOGE("Tuning CPP data size bigger than expected %d: %d",
              meta.tuning_cpp_data_size,
              TUNING_CPP_DATA_MAX);
        return;
    }

    if (meta.tuning_cac_data_size > TUNING_CAC_DATA_MAX) {
        LOGE("Tuning CAC data size bigger than expected %d: %d",
              meta.tuning_cac_data_size,
              TUNING_CAC_DATA_MAX);
        return;
    }
    //

    if(enabled){
        char timeBuf[FILENAME_MAX];
        char buf[FILENAME_MAX];
        memset(buf, 0, sizeof(buf));
        memset(timeBuf, 0, sizeof(timeBuf));
        time_t current_time;
        struct tm * timeinfo;
        time (&current_time);
        timeinfo = localtime (&current_time);
        if (timeinfo != NULL) {
            strftime (timeBuf, sizeof(timeBuf),
                    QCAMERA_DUMP_FRM_LOCATION"%Y%m%d%H%M%S", timeinfo);
        }
        String8 filePath(timeBuf);
        snprintf(buf,
                sizeof(buf),
                "%dm_%s_%d.bin",
                dumpFrameCount,
                type,
                frameNumber);
        filePath.append(buf);
        int file_fd = open(filePath.string(), O_RDWR | O_CREAT, 0777);
        if (file_fd >= 0) {
            ssize_t written_len = 0;
            meta.tuning_data_version = TUNING_DATA_VERSION;
            void *data = (void *)((uint8_t *)&meta.tuning_data_version);
            written_len += write(file_fd, data, sizeof(uint32_t));
            data = (void *)((uint8_t *)&meta.tuning_sensor_data_size);
            LOGD("tuning_sensor_data_size %d",(int)(*(int *)data));
            written_len += write(file_fd, data, sizeof(uint32_t));
            data = (void *)((uint8_t *)&meta.tuning_vfe_data_size);
            LOGD("tuning_vfe_data_size %d",(int)(*(int *)data));
            written_len += write(file_fd, data, sizeof(uint32_t));
            data = (void *)((uint8_t *)&meta.tuning_cpp_data_size);
            LOGD("tuning_cpp_data_size %d",(int)(*(int *)data));
            written_len += write(file_fd, data, sizeof(uint32_t));
            data = (void *)((uint8_t *)&meta.tuning_cac_data_size);
            LOGD("tuning_cac_data_size %d",(int)(*(int *)data));
            written_len += write(file_fd, data, sizeof(uint32_t));
            meta.tuning_mod3_data_size = 0;
            data = (void *)((uint8_t *)&meta.tuning_mod3_data_size);
            LOGD("tuning_mod3_data_size %d",(int)(*(int *)data));
            written_len += write(file_fd, data, sizeof(uint32_t));
            size_t total_size = meta.tuning_sensor_data_size;
            data = (void *)((uint8_t *)&meta.data);
            written_len += write(file_fd, data, total_size);
            total_size = meta.tuning_vfe_data_size;
            data = (void *)((uint8_t *)&meta.data[TUNING_VFE_DATA_OFFSET]);
            written_len += write(file_fd, data, total_size);
            total_size = meta.tuning_cpp_data_size;
            data = (void *)((uint8_t *)&meta.data[TUNING_CPP_DATA_OFFSET]);
            written_len += write(file_fd, data, total_size);
            total_size = meta.tuning_cac_data_size;
            data = (void *)((uint8_t *)&meta.data[TUNING_CAC_DATA_OFFSET]);
            written_len += write(file_fd, data, total_size);
            close(file_fd);
        }else {
            LOGE("fail to open file for metadata dumping");
        }
    }
}

/*===========================================================================
 * FUNCTION   : cleanAndSortStreamInfo
 *
 * DESCRIPTION: helper method to clean up invalid streams in stream_info,
 *              and sort them such that raw stream is at the end of the list
 *              This is a workaround for camera daemon constraint.
 *
 * PARAMETERS : None
 *
 *==========================================================================*/
void QCamera3HardwareInterface::cleanAndSortStreamInfo()
{
    List<stream_info_t *> newStreamInfo;

    /*clean up invalid streams*/
    for (List<stream_info_t*>::iterator it=mStreamInfo.begin();
            it != mStreamInfo.end();) {
        if(((*it)->status) == INVALID){
            QCamera3Channel *channel = (QCamera3Channel*)(*it)->stream->priv;
            delete channel;
            free(*it);
            it = mStreamInfo.erase(it);
        } else {
            it++;
        }
    }

    // Move preview/video/callback/snapshot streams into newList
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end();) {
        if ((*it)->stream->format != HAL_PIXEL_FORMAT_RAW_OPAQUE &&
                (*it)->stream->format != HAL_PIXEL_FORMAT_RAW8 &&
                (*it)->stream->format != HAL_PIXEL_FORMAT_RAW10 &&
                (*it)->stream->format != HAL_PIXEL_FORMAT_RAW16 &&
                (*it)->stream->data_space != HAL_DATASPACE_DEPTH) {
            newStreamInfo.push_back(*it);
            it = mStreamInfo.erase(it);
        } else
            it++;
    }
    // Move raw streams into newList
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
            it != mStreamInfo.end();) {
        newStreamInfo.push_back(*it);
        it = mStreamInfo.erase(it);
    }

    mStreamInfo = newStreamInfo;
}

/*===========================================================================
 * FUNCTION   : extractJpegMetadata
 *
 * DESCRIPTION: helper method to extract Jpeg metadata from capture request.
 *              JPEG metadata is cached in HAL, and return as part of capture
 *              result when metadata is returned from camera daemon.
 *
 * PARAMETERS : @jpegMetadata: jpeg metadata to be extracted
 *              @request:      capture request
 *
 *==========================================================================*/
void QCamera3HardwareInterface::extractJpegMetadata(
        CameraMetadata& jpegMetadata,
        const camera3_capture_request_t *request)
{
    CameraMetadata frame_settings;
    frame_settings = request->settings;

    if (frame_settings.exists(ANDROID_JPEG_GPS_COORDINATES))
        jpegMetadata.update(ANDROID_JPEG_GPS_COORDINATES,
                frame_settings.find(ANDROID_JPEG_GPS_COORDINATES).data.d,
                frame_settings.find(ANDROID_JPEG_GPS_COORDINATES).count);

    if (frame_settings.exists(ANDROID_JPEG_GPS_PROCESSING_METHOD))
        jpegMetadata.update(ANDROID_JPEG_GPS_PROCESSING_METHOD,
                frame_settings.find(ANDROID_JPEG_GPS_PROCESSING_METHOD).data.u8,
                frame_settings.find(ANDROID_JPEG_GPS_PROCESSING_METHOD).count);

    if (frame_settings.exists(ANDROID_JPEG_GPS_TIMESTAMP))
        jpegMetadata.update(ANDROID_JPEG_GPS_TIMESTAMP,
                frame_settings.find(ANDROID_JPEG_GPS_TIMESTAMP).data.i64,
                frame_settings.find(ANDROID_JPEG_GPS_TIMESTAMP).count);

    if (frame_settings.exists(ANDROID_JPEG_ORIENTATION))
        jpegMetadata.update(ANDROID_JPEG_ORIENTATION,
                frame_settings.find(ANDROID_JPEG_ORIENTATION).data.i32,
                frame_settings.find(ANDROID_JPEG_ORIENTATION).count);

    if (frame_settings.exists(ANDROID_JPEG_QUALITY))
        jpegMetadata.update(ANDROID_JPEG_QUALITY,
                frame_settings.find(ANDROID_JPEG_QUALITY).data.u8,
                frame_settings.find(ANDROID_JPEG_QUALITY).count);

    if (frame_settings.exists(ANDROID_JPEG_THUMBNAIL_QUALITY))
        jpegMetadata.update(ANDROID_JPEG_THUMBNAIL_QUALITY,
                frame_settings.find(ANDROID_JPEG_THUMBNAIL_QUALITY).data.u8,
                frame_settings.find(ANDROID_JPEG_THUMBNAIL_QUALITY).count);

    if (frame_settings.exists(ANDROID_JPEG_THUMBNAIL_SIZE)) {
        int32_t thumbnail_size[2];
        thumbnail_size[0] = frame_settings.find(ANDROID_JPEG_THUMBNAIL_SIZE).data.i32[0];
        thumbnail_size[1] = frame_settings.find(ANDROID_JPEG_THUMBNAIL_SIZE).data.i32[1];
        if (frame_settings.exists(ANDROID_JPEG_ORIENTATION)) {
            int32_t orientation =
                  frame_settings.find(ANDROID_JPEG_ORIENTATION).data.i32[0];
            if ((!needJpegExifRotation() || (needJpegExifRotation() && !useExifRotation()))
                    && ((orientation == 90) || (orientation == 270))) {
               //swap thumbnail dimensions for rotations 90 and 270 in jpeg metadata if CPP
               //or JPEG is doing the rotation
               int32_t temp;
               temp = thumbnail_size[0];
               thumbnail_size[0] = thumbnail_size[1];
               thumbnail_size[1] = temp;
            }
         }
         LOGD("Thumbnail wxh %dx%d", thumbnail_size[0], thumbnail_size[1]);
         jpegMetadata.update(ANDROID_JPEG_THUMBNAIL_SIZE,
                thumbnail_size,
                frame_settings.find(ANDROID_JPEG_THUMBNAIL_SIZE).count);
    }

}

/*===========================================================================
 * FUNCTION   : convertToRegions
 *
 * DESCRIPTION: helper method to convert from cam_rect_t into int32_t array
 *
 * PARAMETERS :
 *   @rect   : cam_rect_t struct to convert
 *   @region : int32_t destination array
 *   @weight : if we are converting from cam_area_t, weight is valid
 *             else weight = -1
 *
 *==========================================================================*/
void QCamera3HardwareInterface::convertToRegions(cam_rect_t rect,
        int32_t *region, int weight)
{
    region[FACE_LEFT] = rect.left;
    region[FACE_TOP] = rect.top;
    region[FACE_RIGHT] = rect.left + rect.width;
    region[FACE_BOTTOM] = rect.top + rect.height;
    if (weight > -1) {
        region[FACE_WEIGHT] = weight;
    }
}

/*===========================================================================
 * FUNCTION   : convertFromRegions
 *
 * DESCRIPTION: helper method to convert from array to cam_rect_t
 *
 * PARAMETERS :
 *   @rect   : cam_rect_t struct to convert
 *   @region : int32_t destination array
 *   @weight : if we are converting from cam_area_t, weight is valid
 *             else weight = -1
 *
 *==========================================================================*/
void QCamera3HardwareInterface::convertFromRegions(cam_area_t &roi,
        const camera_metadata_t *settings, uint32_t tag)
{
    CameraMetadata frame_settings;
    frame_settings = settings;
    int32_t x_min = frame_settings.find(tag).data.i32[0];
    int32_t y_min = frame_settings.find(tag).data.i32[1];
    int32_t x_max = frame_settings.find(tag).data.i32[2];
    int32_t y_max = frame_settings.find(tag).data.i32[3];
    roi.weight = frame_settings.find(tag).data.i32[4];
    roi.rect.left = x_min;
    roi.rect.top = y_min;
    roi.rect.width = x_max - x_min;
    roi.rect.height = y_max - y_min;
}

/*===========================================================================
 * FUNCTION   : resetIfNeededROI
 *
 * DESCRIPTION: helper method to reset the roi if it is greater than scaler
 *              crop region
 *
 * PARAMETERS :
 *   @roi       : cam_area_t struct to resize
 *   @scalerCropRegion : cam_crop_region_t region to compare against
 *
 *
 *==========================================================================*/
bool QCamera3HardwareInterface::resetIfNeededROI(cam_area_t* roi,
                                                 const cam_crop_region_t* scalerCropRegion)
{
    int32_t roi_x_max = roi->rect.width + roi->rect.left;
    int32_t roi_y_max = roi->rect.height + roi->rect.top;
    int32_t crop_x_max = scalerCropRegion->width + scalerCropRegion->left;
    int32_t crop_y_max = scalerCropRegion->height + scalerCropRegion->top;

    /* According to spec weight = 0 is used to indicate roi needs to be disabled
     * without having this check the calculations below to validate if the roi
     * is inside scalar crop region will fail resulting in the roi not being
     * reset causing algorithm to continue to use stale roi window
     */
    if (roi->weight == 0) {
        return true;
    }

    if ((roi_x_max < scalerCropRegion->left) ||
        // right edge of roi window is left of scalar crop's left edge
        (roi_y_max < scalerCropRegion->top)  ||
        // bottom edge of roi window is above scalar crop's top edge
        (roi->rect.left > crop_x_max) ||
        // left edge of roi window is beyond(right) of scalar crop's right edge
        (roi->rect.top > crop_y_max)){
        // top edge of roi windo is above scalar crop's top edge
        return false;
    }
    if (roi->rect.left < scalerCropRegion->left) {
        roi->rect.left = scalerCropRegion->left;
    }
    if (roi->rect.top < scalerCropRegion->top) {
        roi->rect.top = scalerCropRegion->top;
    }
    if (roi_x_max > crop_x_max) {
        roi_x_max = crop_x_max;
    }
    if (roi_y_max > crop_y_max) {
        roi_y_max = crop_y_max;
    }
    roi->rect.width = roi_x_max - roi->rect.left;
    roi->rect.height = roi_y_max - roi->rect.top;
    return true;
}

/*===========================================================================
 * FUNCTION   : convertLandmarks
 *
 * DESCRIPTION: helper method to extract the landmarks from face detection info
 *
 * PARAMETERS :
 *   @landmark_data : input landmark data to be converted
 *   @landmarks : int32_t destination array
 *
 *
 *==========================================================================*/
void QCamera3HardwareInterface::convertLandmarks(
        cam_face_landmarks_info_t landmark_data,
        int32_t *landmarks)
{
    if (landmark_data.is_left_eye_valid) {
        landmarks[LEFT_EYE_X] = (int32_t)landmark_data.left_eye_center.x;
        landmarks[LEFT_EYE_Y] = (int32_t)landmark_data.left_eye_center.y;
    } else {
        landmarks[LEFT_EYE_X] = FACE_INVALID_POINT;
        landmarks[LEFT_EYE_Y] = FACE_INVALID_POINT;
    }

    if (landmark_data.is_right_eye_valid) {
        landmarks[RIGHT_EYE_X] = (int32_t)landmark_data.right_eye_center.x;
        landmarks[RIGHT_EYE_Y] = (int32_t)landmark_data.right_eye_center.y;
    } else {
        landmarks[RIGHT_EYE_X] = FACE_INVALID_POINT;
        landmarks[RIGHT_EYE_Y] = FACE_INVALID_POINT;
    }

    if (landmark_data.is_mouth_valid) {
        landmarks[MOUTH_X] = (int32_t)landmark_data.mouth_center.x;
        landmarks[MOUTH_Y] = (int32_t)landmark_data.mouth_center.y;
    } else {
        landmarks[MOUTH_X] = FACE_INVALID_POINT;
        landmarks[MOUTH_Y] = FACE_INVALID_POINT;
    }
}

/*===========================================================================
 * FUNCTION   : setInvalidLandmarks
 *
 * DESCRIPTION: helper method to set invalid landmarks
 *
 * PARAMETERS :
 *   @landmarks : int32_t destination array
 *
 *
 *==========================================================================*/
void QCamera3HardwareInterface::setInvalidLandmarks(
        int32_t *landmarks)
{
    landmarks[LEFT_EYE_X] = FACE_INVALID_POINT;
    landmarks[LEFT_EYE_Y] = FACE_INVALID_POINT;
    landmarks[RIGHT_EYE_X] = FACE_INVALID_POINT;
    landmarks[RIGHT_EYE_Y] = FACE_INVALID_POINT;
    landmarks[MOUTH_X] = FACE_INVALID_POINT;
    landmarks[MOUTH_Y] = FACE_INVALID_POINT;
}

#define DATA_PTR(MEM_OBJ,INDEX) MEM_OBJ->getPtr( INDEX )

/*===========================================================================
 * FUNCTION   : getCapabilities
 *
 * DESCRIPTION: query camera capability from back-end
 *
 * PARAMETERS :
 *   @ops  : mm-interface ops structure
 *   @cam_handle  : camera handle for which we need capability
 *
 * RETURN     : ptr type of capability structure
 *              capability for success
 *              NULL for failure
 *==========================================================================*/
cam_capability_t *QCamera3HardwareInterface::getCapabilities(mm_camera_ops_t *ops,
        uint32_t cam_handle)
{
    int rc = NO_ERROR;
    QCamera3HeapMemory *capabilityHeap = NULL;
    cam_capability_t *cap_ptr = NULL;

    if (ops == NULL) {
        LOGE("Invalid arguments");
        return NULL;
    }

    capabilityHeap = new QCamera3HeapMemory(1);
    if (capabilityHeap == NULL) {
        LOGE("creation of capabilityHeap failed");
        return NULL;
    }

    /* Allocate memory for capability buffer */
    rc = capabilityHeap->allocate(sizeof(cam_capability_t));
    if(rc != OK) {
        LOGE("No memory for cappability");
        goto allocate_failed;
    }

    /* Map memory for capability buffer */
    memset(DATA_PTR(capabilityHeap,0), 0, sizeof(cam_capability_t));

    rc = ops->map_buf(cam_handle,
            CAM_MAPPING_BUF_TYPE_CAPABILITY, capabilityHeap->getFd(0),
            sizeof(cam_capability_t), capabilityHeap->getPtr(0));
    if(rc < 0) {
        LOGE("failed to map capability buffer");
        rc = FAILED_TRANSACTION;
        goto map_failed;
    }

    /* Query Capability */
    rc = ops->query_capability(cam_handle);
    if(rc < 0) {
        LOGE("failed to query capability");
        rc = FAILED_TRANSACTION;
        goto query_failed;
    }

    cap_ptr = (cam_capability_t *)malloc(sizeof(cam_capability_t));
    if (cap_ptr == NULL) {
        LOGE("out of memory");
        rc = NO_MEMORY;
        goto query_failed;
    }

    memset(cap_ptr, 0, sizeof(cam_capability_t));
    memcpy(cap_ptr, DATA_PTR(capabilityHeap, 0), sizeof(cam_capability_t));

    int index;
    for (index = 0; index < CAM_ANALYSIS_INFO_MAX; index++) {
        cam_analysis_info_t *p_analysis_info = &cap_ptr->analysis_info[index];
        p_analysis_info->analysis_padding_info.offset_info.offset_x = 0;
        p_analysis_info->analysis_padding_info.offset_info.offset_y = 0;
    }

query_failed:
    ops->unmap_buf(cam_handle, CAM_MAPPING_BUF_TYPE_CAPABILITY);
map_failed:
    capabilityHeap->deallocate();
allocate_failed:
    delete capabilityHeap;

    if (rc != NO_ERROR) {
        return NULL;
    } else {
        return cap_ptr;
    }
}

/*===========================================================================
 * FUNCTION   : initCapabilities
 *
 * DESCRIPTION: initialize camera capabilities in static data struct
 *
 * PARAMETERS :
 *   @cameraId  : camera Id
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::initCapabilities(uint32_t cameraId)
{
    int rc = 0;
    mm_camera_vtbl_t *cameraHandle = NULL;
    uint32_t handle = 0;

    rc = camera_open((uint8_t)cameraId, &cameraHandle);
    if (rc) {
        LOGE("camera_open failed. rc = %d", rc);
        goto open_failed;
    }
    if (!cameraHandle) {
        LOGE("camera_open failed. cameraHandle = %p", cameraHandle);
        goto open_failed;
    }

    handle = get_main_camera_handle(cameraHandle->camera_handle);
    gCamCapability[cameraId] = getCapabilities(cameraHandle->ops, handle);
    if (gCamCapability[cameraId] == NULL) {
        rc = FAILED_TRANSACTION;
        goto failed_op;
    }

    gCamCapability[cameraId]->camera_index = cameraId;
    if (is_dual_camera_by_idx(cameraId)) {
        handle = get_aux_camera_handle(cameraHandle->camera_handle);
        gCamCapability[cameraId]->aux_cam_cap =
                getCapabilities(cameraHandle->ops, handle);
        if (gCamCapability[cameraId]->aux_cam_cap == NULL) {
            rc = FAILED_TRANSACTION;
            free(gCamCapability[cameraId]);
            goto failed_op;
        }

        // Copy the main camera capability to main_cam_cap struct
        gCamCapability[cameraId]->main_cam_cap =
                        (cam_capability_t *)malloc(sizeof(cam_capability_t));
        if (gCamCapability[cameraId]->main_cam_cap == NULL) {
            LOGE("out of memory");
            rc = NO_MEMORY;
            goto failed_op;
        }
        memcpy(gCamCapability[cameraId]->main_cam_cap, gCamCapability[cameraId],
                sizeof(cam_capability_t));
    }

    if (gCamCapability[cameraId]->is_remosaic_lib_present ||
            gCamCapability[cameraId]->is_quadracfa_insensor) {
        gCamCapability[cameraId]->is_quadracfa_sensor = TRUE;
    }

    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.quadcfa.id", prop, "");
    if (strlen(prop) > 0) {
        uint8_t camId = atoi(prop);
        if (camId == cameraId) {
            gCamCapability[cameraId]->is_quadracfa_sensor = TRUE;
        }
    }

    if (gCamCapability[cameraId]->is_quadracfa_sensor) {
        LOGI("camera id:%d, quadra cfa sensor.", cameraId);
        property_get("persist.vendor.camera.quadcfa.pic_size", prop, "quarter");
        if (strlen(prop) > 0 && !strcmp(prop, "quarter")) {
            // overide supported picture size, min_duration, and stall_duration,
            // move to filter_supported_snapshot_dimesion()
            cam_dimension_t temp_tbl[MAX_SIZES_CNT];
            int64_t min_duration[MAX_SIZES_CNT];
            int64_t stall_durations[MAX_SIZES_CNT];
            size_t tbl_cnt = 0;

            memset(temp_tbl, 0, sizeof(temp_tbl));
            memset(min_duration, 0, sizeof(min_duration));
            memset(stall_durations, 0, sizeof(stall_durations));
            for (size_t i = 0; i < gCamCapability[cameraId]->picture_sizes_tbl_cnt; i++) {
                cam_dimension_t dim = gCamCapability[cameraId]->picture_sizes_tbl[i];
                if (dim.width  <= gCamCapability[cameraId]->raw_dim[0].width &&
                    dim.height <= gCamCapability[cameraId]->raw_dim[0].height) {
                    temp_tbl[tbl_cnt] = dim;
                    min_duration[tbl_cnt]    = gCamCapability[cameraId]->picture_min_duration[i];
                    stall_durations[tbl_cnt] = gCamCapability[cameraId]->jpeg_stall_durations[i];
                    tbl_cnt++;
                }
            }
            gCamCapability[cameraId]->picture_sizes_tbl_cnt = tbl_cnt;
            memcpy(gCamCapability[cameraId]->picture_sizes_tbl, temp_tbl, sizeof(temp_tbl));
            memcpy(gCamCapability[cameraId]->picture_min_duration,
                    min_duration, sizeof(min_duration));
            memcpy(gCamCapability[cameraId]->jpeg_stall_durations,
                    stall_durations, sizeof(stall_durations));
            LOGD("raw_dim[0]:%dx%d, pic dim[0]:%dx%d", gCamCapability[cameraId]->raw_dim[0].width,
                gCamCapability[cameraId]->raw_dim[0].height,
                gCamCapability[cameraId]->picture_sizes_tbl[0].width,
                gCamCapability[cameraId]->picture_sizes_tbl[0].height);
        }

        cam_dimension_t raw_dim = gCamCapability[cameraId]->raw_dim[0];
        gCamCapability[cameraId]->pixel_array_size = raw_dim;

        gCamCapability[cameraId]->active_array_size.left   = 0;
        gCamCapability[cameraId]->active_array_size.top    = 0;
        gCamCapability[cameraId]->active_array_size.width  = raw_dim.width;
        gCamCapability[cameraId]->active_array_size.height = raw_dim.height;

        LOGD("override active array size to (%d, %d).", raw_dim.width, raw_dim.height);
    }

failed_op:
    cameraHandle->ops->close_camera(cameraHandle->camera_handle);
    cameraHandle = NULL;
open_failed:
    return rc;
}

/*==========================================================================
 * FUNCTION   : get3Aversion
 *
 * DESCRIPTION: get the Q3A S/W version
 *
 * PARAMETERS :
 *  @sw_version: Reference of Q3A structure which will hold version info upon
 *               return
 *
 * RETURN     : None
 *
 *==========================================================================*/
void QCamera3HardwareInterface::get3AVersion(cam_q3a_version_t &sw_version)
{
    if(gCamCapability[mCameraId])
        sw_version = gCamCapability[mCameraId]->q3a_version;
    else
        LOGE("Capability structure NULL!");
}


/*===========================================================================
 * FUNCTION   : initParameters
 *
 * DESCRIPTION: initialize camera parameters
 *
 * PARAMETERS :
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::initParameters()
{
    int rc = 0;

    //Allocate Set Param Buffer
    int count = isDualCamera() ? MM_CAMERA_MAX_CAM_CNT : 1;
    mParamHeap = new QCamera3HeapMemory(count);
    rc = mParamHeap->allocate(sizeof(metadata_buffer_t));
    if(rc != OK) {
        rc = NO_MEMORY;
        LOGE("Failed to allocate SETPARM Heap memory");
        delete mParamHeap;
        mParamHeap = NULL;
        return rc;
    }

    //Map memory for parameters buffer
    rc = mCameraHandle->ops->map_buf(mCameraHandle->camera_handle,
            CAM_MAPPING_BUF_TYPE_PARM_BUF,
            mParamHeap->getFd(0),
            sizeof(metadata_buffer_t),
            (metadata_buffer_t *) DATA_PTR(mParamHeap,0));
    if(rc < 0) {
        LOGE("failed to map SETPARM buffer");
        rc = FAILED_TRANSACTION;
        mParamHeap->deallocate();
        delete mParamHeap;
        mParamHeap = NULL;
        return rc;
    }

    if (isDualCamera()) {
        //Map memory for parameters buffer
        rc = mCameraHandle->ops->map_buf(get_aux_camera_handle(mCameraHandle->camera_handle),
                CAM_MAPPING_BUF_TYPE_PARM_BUF,
                mParamHeap->getFd(1),
                sizeof(metadata_buffer_t),
                (metadata_buffer_t *) DATA_PTR(mParamHeap,1));
        if(rc < 0) {
            LOGE("failed to map SETPARM buffer");
            rc = FAILED_TRANSACTION;
            mParamHeap->deallocate();
            delete mParamHeap;
            mParamHeap = NULL;
            return rc;
        }
        mAuxParameters = (metadata_buffer_t *) DATA_PTR(mParamHeap,1);
    }

    mParameters = (metadata_buffer_t *) DATA_PTR(mParamHeap,0);

    mPrevParameters = (metadata_buffer_t *)malloc(sizeof(metadata_buffer_t));
    return rc;
}

/*===========================================================================
 * FUNCTION   : deinitParameters
 *
 * DESCRIPTION: de-initialize camera parameters
 *
 * PARAMETERS :
 *
 * RETURN     : NONE
 *==========================================================================*/
void QCamera3HardwareInterface::deinitParameters()
{
    mCameraHandle->ops->unmap_buf(mCameraHandle->camera_handle,
            CAM_MAPPING_BUF_TYPE_PARM_BUF);

    mParamHeap->deallocate();
    delete mParamHeap;
    mParamHeap = NULL;

    mParameters = NULL;
    mAuxParameters = NULL;

    free(mPrevParameters);
    mPrevParameters = NULL;
}

/*===========================================================================
 * FUNCTION   : calcMaxJpegSize
 *
 * DESCRIPTION: Calculates maximum jpeg size supported by the cameraId
 *
 * PARAMETERS :
 *
 * RETURN     : max_jpeg_size
 *==========================================================================*/
size_t QCamera3HardwareInterface::calcMaxJpegSize(uint32_t camera_id)
{
    size_t max_jpeg_size = 0;
    size_t temp_width, temp_height;
    size_t count = MIN(gCamCapability[camera_id]->picture_sizes_tbl_cnt,
            MAX_SIZES_CNT);
    for (size_t i = 0; i < count; i++) {
        temp_width = (size_t)gCamCapability[camera_id]->picture_sizes_tbl[i].width;
        temp_height = (size_t)gCamCapability[camera_id]->picture_sizes_tbl[i].height;
        if (temp_width * temp_height > max_jpeg_size ) {
            max_jpeg_size = temp_width * temp_height;
        }
    }

    max_jpeg_size = max_jpeg_size * 3/2 + sizeof(camera3_jpeg_blob_t);

    if(is_dual_camera_by_idx(camera_id))
    {
        return max_jpeg_size * MM_JPEG_MAX_MPO_IMAGES;
    } else {
        return max_jpeg_size;
    }
}

/*===========================================================================
 * FUNCTION   : getMaxRawSize
 *
 * DESCRIPTION: Fetches maximum raw size supported by the cameraId
 *
 * PARAMETERS :
 *
 * RETURN     : Largest supported Raw Dimension
 *==========================================================================*/
cam_dimension_t QCamera3HardwareInterface::getMaxRawSize(uint32_t camera_id)
{
    int max_width = 0;
    cam_dimension_t maxRawSize;

    memset(&maxRawSize, 0, sizeof(cam_dimension_t));
    for (size_t i = 0; i < gCamCapability[camera_id]->supported_raw_dim_cnt; i++) {
        if (max_width < gCamCapability[camera_id]->raw_dim[i].width) {
            max_width = gCamCapability[camera_id]->raw_dim[i].width;
            maxRawSize = gCamCapability[camera_id]->raw_dim[i];
        }
    }
    return maxRawSize;
}


/*===========================================================================
 * FUNCTION   : calcMaxJpegDim
 *
 * DESCRIPTION: Calculates maximum jpeg dimension supported by the cameraId
 *
 * PARAMETERS :
 *
 * RETURN     : max_jpeg_dim
 *==========================================================================*/
cam_dimension_t QCamera3HardwareInterface::calcMaxJpegDim()
{
    cam_dimension_t max_jpeg_dim;
    cam_dimension_t curr_jpeg_dim;
    max_jpeg_dim.width = 0;
    max_jpeg_dim.height = 0;
    curr_jpeg_dim.width = 0;
    curr_jpeg_dim.height = 0;
    for (size_t i = 0; i < gCamCapability[mCameraId]->picture_sizes_tbl_cnt; i++) {
        curr_jpeg_dim.width = gCamCapability[mCameraId]->picture_sizes_tbl[i].width;
        curr_jpeg_dim.height = gCamCapability[mCameraId]->picture_sizes_tbl[i].height;
        if (curr_jpeg_dim.width * curr_jpeg_dim.height >
            max_jpeg_dim.width * max_jpeg_dim.height ) {
            max_jpeg_dim.width = curr_jpeg_dim.width;
            max_jpeg_dim.height = curr_jpeg_dim.height;
        }
    }

    // adjust for quadra cfa
    if (m_bQuadraCfaSensor &&
            gCamCapability[mCameraId]->supported_quadra_cfa_dim_cnt > 0) {
        curr_jpeg_dim = gCamCapability[mCameraId]->quadra_cfa_dim[0];
        if (curr_jpeg_dim.width * curr_jpeg_dim.height >
            max_jpeg_dim.width * max_jpeg_dim.height ) {
            max_jpeg_dim = curr_jpeg_dim;
        }
    }

    return max_jpeg_dim;
}

/*===========================================================================
 * FUNCTION   : addStreamConfig
 *
 * DESCRIPTION: adds the stream configuration to the array
 *
 * PARAMETERS :
 * @available_stream_configs : pointer to stream configuration array
 * @scalar_format            : scalar format
 * @dim                      : configuration dimension
 * @config_type              : input or output configuration type
 *
 * RETURN     : NONE
 *==========================================================================*/
void QCamera3HardwareInterface::addStreamConfig(Vector<int32_t> &available_stream_configs,
        int32_t scalar_format, const cam_dimension_t &dim, int32_t config_type)
{
    available_stream_configs.add(scalar_format);
    available_stream_configs.add(dim.width);
    available_stream_configs.add(dim.height);
    available_stream_configs.add(config_type);
}

/*===========================================================================
 * FUNCTION   : suppportBurstCapture
 *
 * DESCRIPTION: Whether a particular camera supports BURST_CAPTURE
 *
 * PARAMETERS :
 *   @cameraId  : camera Id
 *
 * RETURN     : true if camera supports BURST_CAPTURE
 *              false otherwise
 *==========================================================================*/
bool QCamera3HardwareInterface::supportBurstCapture(uint32_t cameraId)
{
    const int64_t highResDurationBound = 50000000; // 50 ms, 20 fps
    const int64_t fullResDurationBound = 100000000; // 100 ms, 10 fps
    const int32_t highResWidth = 3264;
    const int32_t highResHeight = 2448;

    if (gCamCapability[cameraId]->picture_min_duration[0] > fullResDurationBound) {
        // Maximum resolution images cannot be captured at >= 10fps
        // -> not supporting BURST_CAPTURE
        return false;
    }

    if (gCamCapability[cameraId]->picture_min_duration[0] <= highResDurationBound) {
        // Maximum resolution images can be captured at >= 20fps
        // --> supporting BURST_CAPTURE
        return true;
    }

    // Find the smallest highRes resolution, or largest resolution if there is none
    size_t totalCnt = MIN(gCamCapability[cameraId]->picture_sizes_tbl_cnt,
            MAX_SIZES_CNT);
    size_t highRes = 0;
    while ((highRes + 1 < totalCnt) &&
            (gCamCapability[cameraId]->picture_sizes_tbl[highRes+1].width *
            gCamCapability[cameraId]->picture_sizes_tbl[highRes+1].height >=
            highResWidth * highResHeight)) {
        highRes++;
    }
    if (gCamCapability[cameraId]->picture_min_duration[highRes] <= highResDurationBound) {
        return true;
    } else {
        return false;
    }
}

/*===========================================================================
 * FUNCTION   : initStaticMetadata
 *
 * DESCRIPTION: initialize the static metadata
 *
 * PARAMETERS :
 *   @cameraId  : camera Id
 *
 * RETURN     : int32_t type of status
 *              0  -- success
 *              non-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::initStaticMetadata(uint32_t cameraId)
{
    int rc = 0;
    CameraMetadata staticInfo;
    size_t count = 0;
    bool limitedDevice = false;
    char prop[PROPERTY_VALUE_MAX];
    bool supportBurst = false;

    supportBurst = supportBurstCapture(cameraId);

    /* If sensor is YUV sensor (no raw support) or if per-frame control is not
     * guaranteed or if min fps of max resolution is less than 20 fps, its
     * advertised as limited device*/
    limitedDevice = gCamCapability[cameraId]->no_per_frame_control_support ||
            (CAM_SENSOR_YUV == gCamCapability[cameraId]->sensor_type.sens_type) ||
            (CAM_SENSOR_MONO == gCamCapability[cameraId]->sensor_type.sens_type) ||
            !supportBurst;

    uint8_t supportedHwLvl = limitedDevice ?
            ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED :
#ifndef USE_HAL_3_3
            // LEVEL_3 - This device will support level 3.
            ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_3;
#else
            ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL;
#endif

    staticInfo.update(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
            &supportedHwLvl, 1);

    bool facingBack = false;
    if ((gCamCapability[cameraId]->position == CAM_POSITION_BACK) ||
            (gCamCapability[cameraId]->position == CAM_POSITION_BACK_AUX)) {
        facingBack = true;
    }
    /*HAL 3 only*/
    staticInfo.update(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
                    &gCamCapability[cameraId]->min_focus_distance, 1);

    staticInfo.update(ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
                    &gCamCapability[cameraId]->hyper_focal_distance, 1);

    /*should be using focal lengths but sensor doesn't provide that info now*/
    staticInfo.update(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
                      &gCamCapability[cameraId]->focal_length,
                      1);

    staticInfo.update(ANDROID_LENS_INFO_AVAILABLE_APERTURES,
            gCamCapability[cameraId]->apertures,
            MIN(CAM_APERTURES_MAX, gCamCapability[cameraId]->apertures_count));

    staticInfo.update(ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
            gCamCapability[cameraId]->filter_densities,
            MIN(CAM_FILTER_DENSITIES_MAX, gCamCapability[cameraId]->filter_densities_count));


    uint8_t available_opt_stab_modes[CAM_OPT_STAB_MAX];
    size_t mode_count =
        MIN((size_t)CAM_OPT_STAB_MAX, gCamCapability[cameraId]->optical_stab_modes_count);
    for (size_t i = 0; i < mode_count; i++) {
      available_opt_stab_modes[i] = gCamCapability[cameraId]->optical_stab_modes[i];
    }
    staticInfo.update(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
            available_opt_stab_modes, mode_count);

    int32_t lens_shading_map_size[] = {
           MIN(CAM_MAX_SHADING_MAP_WIDTH, gCamCapability[cameraId]->lens_shading_map_size.width),
           MIN(CAM_MAX_SHADING_MAP_HEIGHT, gCamCapability[cameraId]->lens_shading_map_size.height)};
    staticInfo.update(ANDROID_LENS_INFO_SHADING_MAP_SIZE,
                      lens_shading_map_size,
                      sizeof(lens_shading_map_size)/sizeof(int32_t));

    staticInfo.update(ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
            gCamCapability[cameraId]->sensor_physical_size, SENSOR_PHYSICAL_SIZE_CNT);

    staticInfo.update(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
            gCamCapability[cameraId]->exposure_time_range, EXPOSURE_TIME_RANGE_CNT);

    staticInfo.update(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
            &gCamCapability[cameraId]->max_frame_duration, 1);

    camera_metadata_rational baseGainFactor = {
            gCamCapability[cameraId]->base_gain_factor.numerator,
            gCamCapability[cameraId]->base_gain_factor.denominator};
    staticInfo.update(ANDROID_SENSOR_BASE_GAIN_FACTOR,
                      &baseGainFactor, 1);

    staticInfo.update(ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
                     (uint8_t *)&gCamCapability[cameraId]->color_arrangement, 1);

    int32_t pixel_array_size[] = {gCamCapability[cameraId]->pixel_array_size.width,
            gCamCapability[cameraId]->pixel_array_size.height};
    staticInfo.update(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
                      pixel_array_size, sizeof(pixel_array_size)/sizeof(pixel_array_size[0]));

    int32_t active_array_size[] = {gCamCapability[cameraId]->active_array_size.left,
            gCamCapability[cameraId]->active_array_size.top,
            gCamCapability[cameraId]->active_array_size.width,
            gCamCapability[cameraId]->active_array_size.height};
    staticInfo.update(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
            active_array_size, sizeof(active_array_size)/sizeof(active_array_size[0]));

    staticInfo.update(ANDROID_SENSOR_INFO_WHITE_LEVEL,
            &gCamCapability[cameraId]->white_level, 1);

    staticInfo.update(ANDROID_SENSOR_BLACK_LEVEL_PATTERN,
            gCamCapability[cameraId]->black_level_pattern, BLACK_LEVEL_PATTERN_CNT);

#ifndef USE_HAL_3_3
    bool hasBlackRegions = false;
    if (gCamCapability[cameraId]->optical_black_region_count > MAX_OPTICAL_BLACK_REGIONS) {
        LOGW("black_region_count: %d is bounded to %d",
            gCamCapability[cameraId]->optical_black_region_count, MAX_OPTICAL_BLACK_REGIONS);
        gCamCapability[cameraId]->optical_black_region_count = MAX_OPTICAL_BLACK_REGIONS;
    }
    if (gCamCapability[cameraId]->optical_black_region_count != 0) {
        int32_t opticalBlackRegions[MAX_OPTICAL_BLACK_REGIONS * 4];
        for (size_t i = 0; i < gCamCapability[cameraId]->optical_black_region_count * 4; i++) {
            opticalBlackRegions[i] = gCamCapability[cameraId]->optical_black_regions[i];
        }
        staticInfo.update(ANDROID_SENSOR_OPTICAL_BLACK_REGIONS,
                opticalBlackRegions, gCamCapability[cameraId]->optical_black_region_count * 4);
        hasBlackRegions = true;
    }
#endif
    staticInfo.update(ANDROID_FLASH_INFO_CHARGE_DURATION,
            &gCamCapability[cameraId]->flash_charge_duration, 1);

    staticInfo.update(ANDROID_TONEMAP_MAX_CURVE_POINTS,
            &gCamCapability[cameraId]->max_tone_map_curve_points, 1);

    // SOF timestamp is based on monotonic_boottime. So advertize REALTIME timesource
    // REALTIME defined in HAL3 API is same as linux's CLOCK_BOOTTIME
    // Ref: kernel/...../msm_isp_util.c: msm_isp_get_timestamp: get_monotonic_boottime
    uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    staticInfo.update(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
            &timestampSource, 1);

    //update histogram vendor data
    staticInfo.update(QCAMERA3_HISTOGRAM_BUCKETS,
            &gCamCapability[cameraId]->histogram_size, 1);

    staticInfo.update(QCAMERA3_HISTOGRAM_MAX_COUNT,
            &gCamCapability[cameraId]->max_histogram_count, 1);

    int32_t sharpness_map_size[] = {
            gCamCapability[cameraId]->sharpness_map_size.width,
            gCamCapability[cameraId]->sharpness_map_size.height};

    staticInfo.update(ANDROID_STATISTICS_INFO_SHARPNESS_MAP_SIZE,
            sharpness_map_size, sizeof(sharpness_map_size)/sizeof(int32_t));

    staticInfo.update(ANDROID_STATISTICS_INFO_MAX_SHARPNESS_MAP_VALUE,
            &gCamCapability[cameraId]->max_sharpness_map_value, 1);

    int32_t bayer_formats[] = {
            ANDROID_SCALER_AVAILABLE_FORMATS_RAW_OPAQUE,
            ANDROID_SCALER_AVAILABLE_FORMATS_RAW16,
            ANDROID_SCALER_AVAILABLE_FORMATS_YCbCr_420_888,
            ANDROID_SCALER_AVAILABLE_FORMATS_BLOB,
            HAL_PIXEL_FORMAT_RAW10,
            HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
            };
    size_t bayer_formats_count = sizeof(bayer_formats) / sizeof(int32_t);
    int32_t depth_formats[] = {
            ANDROID_SCALER_AVAILABLE_FORMATS_BLOB,
            HAL_PIXEL_FORMAT_Y16,
            //HAL_PIXEL_FORMAT_Y8, This is not yet a public format
            HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
            };
    size_t depth_formats_count = sizeof(depth_formats) / sizeof(int32_t);
    int32_t* scalar_formats = NULL;
    size_t scalar_formats_count = 0;
    if (gCamCapability[cameraId]->is_depth_sensor) {
        scalar_formats = depth_formats;
        scalar_formats_count = depth_formats_count;
    } else {
        scalar_formats = bayer_formats;
        scalar_formats_count = bayer_formats_count;
    }
    staticInfo.update(ANDROID_SCALER_AVAILABLE_FORMATS,
              scalar_formats,
              scalar_formats_count);


    int32_t available_processed_sizes[MAX_SIZES_CNT * 2];
    count = MIN(gCamCapability[cameraId]->picture_sizes_tbl_cnt, MAX_SIZES_CNT);
    makeTable(gCamCapability[cameraId]->picture_sizes_tbl,
            count, MAX_SIZES_CNT, available_processed_sizes);
    staticInfo.update(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
            available_processed_sizes, count * 2);

    int32_t available_raw_sizes[MAX_SIZES_CNT * 2];
    count = MIN(gCamCapability[cameraId]->supported_raw_dim_cnt, MAX_SIZES_CNT);
    makeTable(gCamCapability[cameraId]->raw_dim,
            count, MAX_SIZES_CNT, available_raw_sizes);
    staticInfo.update(ANDROID_SCALER_AVAILABLE_RAW_SIZES,
            available_raw_sizes, count * 2);

    int32_t available_fps_ranges[MAX_SIZES_CNT * 2];
    count = MIN(gCamCapability[cameraId]->fps_ranges_tbl_cnt, MAX_SIZES_CNT);
    makeFPSTable(gCamCapability[cameraId]->fps_ranges_tbl,
            count, MAX_SIZES_CNT, available_fps_ranges);
    staticInfo.update(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
            available_fps_ranges, count * 2);

    camera_metadata_rational exposureCompensationStep = {
            gCamCapability[cameraId]->exp_compensation_step.numerator,
            gCamCapability[cameraId]->exp_compensation_step.denominator};
    staticInfo.update(ANDROID_CONTROL_AE_COMPENSATION_STEP,
                      &exposureCompensationStep, 1);

    Vector<uint8_t> availableVstabModes;
    availableVstabModes.add(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF);
    char eis_prop[PROPERTY_VALUE_MAX];
    bool eisSupported = false;
    memset(eis_prop, 0, sizeof(eis_prop));
    property_get("persist.vendor.camera.eis.enable", eis_prop, "1");
    uint8_t eis_prop_set = (uint8_t)atoi(eis_prop);
    count = IS_TYPE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_is_types_cnt, count);
    for (size_t i = 0; i < count; i++) {
        if ((gCamCapability[cameraId]->supported_is_types[i] == IS_TYPE_EIS_2_0) ||
            (gCamCapability[cameraId]->supported_is_types[i] == IS_TYPE_EIS_3_0) ||
            (gCamCapability[cameraId]->supported_is_types[i] == IS_TYPE_VENDOR_EIS)) {
            eisSupported = true;
            break;
        }
    }
    if (eis_prop_set && eisSupported) {
        availableVstabModes.add(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_ON);
    }
    staticInfo.update(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
                      availableVstabModes.array(), availableVstabModes.size());

    /*HAL 1 and HAL 3 common*/
    uint32_t zoomSteps = gCamCapability[cameraId]->zoom_ratio_tbl_cnt;
    uint32_t maxZoomStep = gCamCapability[cameraId]->zoom_ratio_tbl[zoomSteps - 1];
    uint32_t minZoomStep = 100; //as per HAL1/API1 spec
    float maxZoom = maxZoomStep/minZoomStep;
    staticInfo.update(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
            &maxZoom, 1);

    uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
    staticInfo.update(ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

    int32_t max3aRegions[3] = {/*AE*/1,/*AWB*/ 0,/*AF*/ 1};
    if (gCamCapability[cameraId]->supported_focus_modes_cnt == 1)
        max3aRegions[2] = 0; /* AF not supported */
    staticInfo.update(ANDROID_CONTROL_MAX_REGIONS,
            max3aRegions, 3);

    /* 0: OFF, 1: OFF+SIMPLE, 2: OFF+FULL, 3: OFF+SIMPLE+FULL */
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.facedetect", prop, "1");
    uint8_t supportedFaceDetectMode = (uint8_t)atoi(prop);
    LOGD("Support face detection mode: %d",
             supportedFaceDetectMode);

    int32_t maxFaces = gCamCapability[cameraId]->max_num_roi;
    /* support mode should be OFF if max number of face is 0 */
    if (maxFaces <= 0) {
        supportedFaceDetectMode = 0;
    }
    Vector<uint8_t> availableFaceDetectModes;
    availableFaceDetectModes.add(ANDROID_STATISTICS_FACE_DETECT_MODE_OFF);
    if (supportedFaceDetectMode == 1) {
        availableFaceDetectModes.add(ANDROID_STATISTICS_FACE_DETECT_MODE_SIMPLE);
    } else if (supportedFaceDetectMode == 2) {
        availableFaceDetectModes.add(ANDROID_STATISTICS_FACE_DETECT_MODE_FULL);
    } else if (supportedFaceDetectMode == 3) {
        availableFaceDetectModes.add(ANDROID_STATISTICS_FACE_DETECT_MODE_SIMPLE);
        availableFaceDetectModes.add(ANDROID_STATISTICS_FACE_DETECT_MODE_FULL);
    } else {
        maxFaces = 0;
    }
    staticInfo.update(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
            availableFaceDetectModes.array(),
            availableFaceDetectModes.size());
    staticInfo.update(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
            (int32_t *)&maxFaces, 1);
    uint8_t face_bsgc = gCamCapability[cameraId]->face_bsgc;
    staticInfo.update(QCAMERA3_STATS_BSGC_AVAILABLE,
            &face_bsgc, 1);

    int32_t exposureCompensationRange[] = {
            gCamCapability[cameraId]->exposure_compensation_min,
            gCamCapability[cameraId]->exposure_compensation_max};
    staticInfo.update(ANDROID_CONTROL_AE_COMPENSATION_RANGE,
            exposureCompensationRange,
            sizeof(exposureCompensationRange)/sizeof(int32_t));

    uint8_t lensFacing = (facingBack) ?
            ANDROID_LENS_FACING_BACK : ANDROID_LENS_FACING_FRONT;
    staticInfo.update(ANDROID_LENS_FACING, &lensFacing, 1);

    staticInfo.update(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
                      available_thumbnail_sizes,
                      sizeof(available_thumbnail_sizes)/sizeof(int32_t));

    /*all sizes will be clubbed into this tag*/
    count = MIN(gCamCapability[cameraId]->picture_sizes_tbl_cnt, MAX_SIZES_CNT);
    /*android.scaler.availableStreamConfigurations*/
    Vector<int32_t> available_stream_configs;
    cam_dimension_t active_array_dim;
    active_array_dim.width = gCamCapability[cameraId]->active_array_size.width;
    active_array_dim.height = gCamCapability[cameraId]->active_array_size.height;

    /*advertise list of input dimensions supported based on below property.
    By default all sizes upto 5MP will be advertised.
    Note that the setprop resolution format should be WxH.
    e.g: adb shell setprop persist.vendor.camera.input.minsize 1280x720
    To list all supported sizes, setprop needs to be set with "0x0" */
    cam_dimension_t minInputSize = {2592,1944}; //5MP
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.input.minsize", prop, "2592x1944");
    if (strlen(prop) > 0) {
        char *saveptr = NULL;
        char *token = strtok_r(prop, "x", &saveptr);
        if (token != NULL) {
            minInputSize.width = atoi(token);
        }
        token = strtok_r(NULL, "x", &saveptr);
        if (token != NULL) {
            minInputSize.height = atoi(token);
        }
    }

    if (minInputSize.width > gCamCapability[cameraId]->picture_sizes_tbl[0].width ||
        minInputSize.height > gCamCapability[cameraId]->picture_sizes_tbl[0].height) {
        minInputSize = gCamCapability[cameraId]->picture_sizes_tbl[0];
    }

    /* Add input/output stream configurations for each scalar formats*/
    for (size_t j = 0; j < scalar_formats_count; j++) {
        switch (scalar_formats[j]) {
        case ANDROID_SCALER_AVAILABLE_FORMATS_RAW16:
        case ANDROID_SCALER_AVAILABLE_FORMATS_RAW_OPAQUE:
        case HAL_PIXEL_FORMAT_RAW10:
            for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
                addStreamConfig(available_stream_configs, scalar_formats[j],
                        gCamCapability[cameraId]->raw_dim[i],
                        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
            }
            break;
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_Y8:
            for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
                addStreamConfig(available_stream_configs, scalar_formats[j],
                        gCamCapability[cameraId]->raw_dim[i],
                        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
            }
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            if (!gCamCapability[cameraId]->is_depth_sensor) {
                for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                        gCamCapability[cameraId]->picture_sizes_tbl_cnt); i++) {
                    addStreamConfig(available_stream_configs, scalar_formats[j],
                            gCamCapability[cameraId]->picture_sizes_tbl[i],
                            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
                }
            } else {
                for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                        gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
                    addStreamConfig(available_stream_configs, scalar_formats[j],
                            gCamCapability[cameraId]->raw_dim[i],
                            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
                }
            }
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
        case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
        default:
            cam_dimension_t largest_picture_size;
            memset(&largest_picture_size, 0, sizeof(cam_dimension_t));
            for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->picture_sizes_tbl_cnt); i++) {
                addStreamConfig(available_stream_configs, scalar_formats[j],
                        gCamCapability[cameraId]->picture_sizes_tbl[i],
                        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
                /*For below 2 formats we also support i/p streams for reprocessing advertise those*/
                if (scalar_formats[j] == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED ||
                        scalar_formats[j] == HAL_PIXEL_FORMAT_YCbCr_420_888) {
                     if ((gCamCapability[cameraId]->picture_sizes_tbl[i].width
                            >= minInputSize.width) || (gCamCapability[cameraId]->
                            picture_sizes_tbl[i].height >= minInputSize.height)) {
                         addStreamConfig(available_stream_configs, scalar_formats[j],
                                 gCamCapability[cameraId]->picture_sizes_tbl[i],
                                 ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_INPUT);
                     }
                }
            }

            break;
        }
    }

    staticInfo.update(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                      available_stream_configs.array(), available_stream_configs.size());
    static const uint8_t hotpixelMode = ANDROID_HOT_PIXEL_MODE_FAST;
    staticInfo.update(ANDROID_HOT_PIXEL_MODE, &hotpixelMode, 1);

    static const uint8_t hotPixelMapMode = ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF;
    staticInfo.update(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE, &hotPixelMapMode, 1);

    /* android.scaler.availableMinFrameDurations */
    Vector<int64_t> available_min_durations;
    for (size_t j = 0; j < scalar_formats_count; j++) {
        switch (scalar_formats[j]) {
        case ANDROID_SCALER_AVAILABLE_FORMATS_RAW16:
        case ANDROID_SCALER_AVAILABLE_FORMATS_RAW_OPAQUE:
        case HAL_PIXEL_FORMAT_RAW10:
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_Y8:
            for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
                available_min_durations.add(scalar_formats[j]);
                available_min_durations.add(gCamCapability[cameraId]->raw_dim[i].width);
                available_min_durations.add(gCamCapability[cameraId]->raw_dim[i].height);
                available_min_durations.add(gCamCapability[cameraId]->raw_min_duration[i]);
            }
            break;
        default:
            for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->picture_sizes_tbl_cnt); i++) {
                available_min_durations.add(scalar_formats[j]);
                available_min_durations.add(gCamCapability[cameraId]->picture_sizes_tbl[i].width);
                available_min_durations.add(gCamCapability[cameraId]->picture_sizes_tbl[i].height);
                available_min_durations.add(gCamCapability[cameraId]->picture_min_duration[i]);
            }
            break;
        }
    }
    staticInfo.update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                      available_min_durations.array(), available_min_durations.size());

    Vector<int32_t> available_hfr_configs;
    for (size_t i = 0; i < gCamCapability[cameraId]->hfr_tbl_cnt; i++) {
        int32_t fps = 0;
        switch (gCamCapability[cameraId]->hfr_tbl[i].mode) {
        case CAM_HFR_MODE_60FPS:
            fps = 60;
            break;
        case CAM_HFR_MODE_90FPS:
            fps = 90;
            break;
        case CAM_HFR_MODE_120FPS:
            fps = 120;
            break;
        case CAM_HFR_MODE_150FPS:
            fps = 150;
            break;
        case CAM_HFR_MODE_180FPS:
            fps = 180;
            break;
        case CAM_HFR_MODE_210FPS:
            fps = 210;
            break;
        case CAM_HFR_MODE_240FPS:
            fps = 240;
            break;
        case CAM_HFR_MODE_480FPS:
            fps = 480;
            break;
        case CAM_HFR_MODE_OFF:
        case CAM_HFR_MODE_MAX:
        default:
            break;
        }

        /* Advertise only MIN_FPS_FOR_BATCH_MODE or above as HIGH_SPEED_CONFIGS */
        if (fps >= MIN_FPS_FOR_BATCH_MODE) {
            /* For each HFR frame rate, need to advertise one variable fps range
             * and one fixed fps range per dimension. Eg: for 120 FPS, advertise [30, 120]
             * and [120, 120]. While camcorder preview alone is running [30, 120] is
             * set by the app. When video recording is started, [120, 120] is
             * set. This way sensor configuration does not change when recording
             * is started */

            /* (width, height, fps_min, fps_max, batch_size_max) */
            for (size_t j = 0; j < gCamCapability[cameraId]->hfr_tbl[i].dim_cnt &&
                j < MAX_SIZES_CNT; j++) {
                available_hfr_configs.add(
                        gCamCapability[cameraId]->hfr_tbl[i].dim[j].width);
                available_hfr_configs.add(
                        gCamCapability[cameraId]->hfr_tbl[i].dim[j].height);
                available_hfr_configs.add(PREVIEW_FPS_FOR_HFR);
                available_hfr_configs.add(fps);
                available_hfr_configs.add(fps / PREVIEW_FPS_FOR_HFR);

                /* (width, height, fps_min, fps_max, batch_size_max) */
                available_hfr_configs.add(
                        gCamCapability[cameraId]->hfr_tbl[i].dim[j].width);
                available_hfr_configs.add(
                        gCamCapability[cameraId]->hfr_tbl[i].dim[j].height);
                available_hfr_configs.add(fps);
                available_hfr_configs.add(fps);
                available_hfr_configs.add(fps / PREVIEW_FPS_FOR_HFR);
            }
       }
    }
    //Advertise HFR capability only if the property is set
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.hal3hfr.enable", prop, "1");
    uint8_t hfrEnable = (uint8_t)atoi(prop);

    if(hfrEnable && available_hfr_configs.array()) {
        staticInfo.update(
                ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS,
                available_hfr_configs.array(), available_hfr_configs.size());
    }

    int32_t max_jpeg_size = (int32_t)calcMaxJpegSize(cameraId);
    staticInfo.update(ANDROID_JPEG_MAX_SIZE,
                      &max_jpeg_size, 1);

    uint8_t avail_effects[CAM_EFFECT_MODE_MAX];
    size_t size = 0;
    count = CAM_EFFECT_MODE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_effects_cnt, count);
    for (size_t i = 0; i < count; i++) {
        int val = lookupFwkName(EFFECT_MODES_MAP, METADATA_MAP_SIZE(EFFECT_MODES_MAP),
                gCamCapability[cameraId]->supported_effects[i]);
        if (NAME_NOT_FOUND != val) {
            avail_effects[size] = (uint8_t)val;
            size++;
        }
    }
    staticInfo.update(ANDROID_CONTROL_AVAILABLE_EFFECTS,
                      avail_effects,
                      size);

    uint8_t avail_scene_modes[CAM_SCENE_MODE_MAX];
    uint8_t supported_indexes[CAM_SCENE_MODE_MAX];
    size_t supported_scene_modes_cnt = 0;
    count = CAM_SCENE_MODE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_scene_modes_cnt, count);
    for (size_t i = 0; i < count; i++) {
        if (gCamCapability[cameraId]->supported_scene_modes[i] !=
                CAM_SCENE_MODE_OFF) {
            int val = lookupFwkName(SCENE_MODES_MAP,
                    METADATA_MAP_SIZE(SCENE_MODES_MAP),
                    gCamCapability[cameraId]->supported_scene_modes[i]);

            if (NAME_NOT_FOUND != val) {
                avail_scene_modes[supported_scene_modes_cnt] = (uint8_t)val;
                supported_indexes[supported_scene_modes_cnt] = (uint8_t)i;
                supported_scene_modes_cnt++;
            }
        }
    }
    staticInfo.update(ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
                      avail_scene_modes,
                      supported_scene_modes_cnt);

    uint8_t scene_mode_overrides[CAM_SCENE_MODE_MAX  * 3];
    makeOverridesList(gCamCapability[cameraId]->scene_mode_overrides,
                      supported_scene_modes_cnt,
                      CAM_SCENE_MODE_MAX,
                      scene_mode_overrides,
                      supported_indexes,
                      cameraId);

    if (supported_scene_modes_cnt == 0) {
        supported_scene_modes_cnt = 1;
        avail_scene_modes[0] = ANDROID_CONTROL_SCENE_MODE_DISABLED;
    }

    staticInfo.update(ANDROID_CONTROL_SCENE_MODE_OVERRIDES,
            scene_mode_overrides, supported_scene_modes_cnt * 3);

    uint8_t available_control_modes[] = {ANDROID_CONTROL_MODE_OFF,
                                         ANDROID_CONTROL_MODE_AUTO,
                                         ANDROID_CONTROL_MODE_USE_SCENE_MODE};
    staticInfo.update(ANDROID_CONTROL_AVAILABLE_MODES,
            available_control_modes,
            3);

    uint8_t avail_antibanding_modes[CAM_ANTIBANDING_MODE_MAX];
    size = 0;
    count = CAM_ANTIBANDING_MODE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_antibandings_cnt, count);
    for (size_t i = 0; i < count; i++) {
        int val = lookupFwkName(ANTIBANDING_MODES_MAP, METADATA_MAP_SIZE(ANTIBANDING_MODES_MAP),
                gCamCapability[cameraId]->supported_antibandings[i]);
        if (NAME_NOT_FOUND != val) {
            avail_antibanding_modes[size] = (uint8_t)val;
            size++;
        }

    }
    staticInfo.update(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
                      avail_antibanding_modes,
                      size);

    uint8_t avail_abberation_modes[] = {
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF,
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST,
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY};
    count = CAM_COLOR_CORRECTION_ABERRATION_MAX;
    count = MIN(gCamCapability[cameraId]->aberration_modes_count, count);
    if (0 == count) {
        //  If no aberration correction modes are available for a device, this advertise OFF mode
        size = 1;
    } else {
        // If count is not zero then atleast one among the FAST or HIGH quality is supported
        // So, advertize all 3 modes if atleast any one mode is supported as per the
        // new M requirement
        size = 3;
    }
    staticInfo.update(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
            avail_abberation_modes,
            size);

    uint8_t avail_af_modes[CAM_FOCUS_MODE_MAX];
    size = 0;
    count = CAM_FOCUS_MODE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_focus_modes_cnt, count);
    for (size_t i = 0; i < count; i++) {
        int val = lookupFwkName(FOCUS_MODES_MAP, METADATA_MAP_SIZE(FOCUS_MODES_MAP),
                gCamCapability[cameraId]->supported_focus_modes[i]);
        if (NAME_NOT_FOUND != val) {
            avail_af_modes[size] = (uint8_t)val;
            size++;
        }
    }
    staticInfo.update(ANDROID_CONTROL_AF_AVAILABLE_MODES,
                      avail_af_modes,
                      size);

    uint8_t avail_awb_modes[CAM_WB_MODE_MAX];
    size = 0;
    count = CAM_WB_MODE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_white_balances_cnt, count);
    for (size_t i = 0; i < count; i++) {
        int val = lookupFwkName(WHITE_BALANCE_MODES_MAP,
                METADATA_MAP_SIZE(WHITE_BALANCE_MODES_MAP),
                gCamCapability[cameraId]->supported_white_balances[i]);
        if (NAME_NOT_FOUND != val) {
            avail_awb_modes[size] = (uint8_t)val;
            size++;
        }
    }
    staticInfo.update(ANDROID_CONTROL_AWB_AVAILABLE_MODES,
                      avail_awb_modes,
                      size);

    uint8_t available_flash_levels[CAM_FLASH_FIRING_LEVEL_MAX];
    count = CAM_FLASH_FIRING_LEVEL_MAX;
    count = MIN(gCamCapability[cameraId]->supported_flash_firing_level_cnt,
            count);
    for (size_t i = 0; i < count; i++) {
        available_flash_levels[i] =
                gCamCapability[cameraId]->supported_firing_levels[i];
    }
    staticInfo.update(ANDROID_FLASH_FIRING_POWER,
            available_flash_levels, count);

    uint8_t flashAvailable;
    if (gCamCapability[cameraId]->flash_available)
        flashAvailable = ANDROID_FLASH_INFO_AVAILABLE_TRUE;
    else
        flashAvailable = ANDROID_FLASH_INFO_AVAILABLE_FALSE;
    staticInfo.update(ANDROID_FLASH_INFO_AVAILABLE,
            &flashAvailable, 1);

    Vector<uint8_t> avail_ae_modes;
    count = CAM_AE_MODE_MAX;
    count = MIN(gCamCapability[cameraId]->supported_ae_modes_cnt, count);
    for (size_t i = 0; i < count; i++) {
        avail_ae_modes.add(gCamCapability[cameraId]->supported_ae_modes[i]);
    }
    if (flashAvailable) {
        avail_ae_modes.add(ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH);
        avail_ae_modes.add(ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH);
        avail_ae_modes.add(ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE);
    }
    staticInfo.update(ANDROID_CONTROL_AE_AVAILABLE_MODES,
                      avail_ae_modes.array(),
                      avail_ae_modes.size());

    int32_t sensitivity_range[2];
    sensitivity_range[0] = gCamCapability[cameraId]->sensitivity_range.min_sensitivity;
    sensitivity_range[1] = gCamCapability[cameraId]->sensitivity_range.max_sensitivity;
    staticInfo.update(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
                      sensitivity_range,
                      sizeof(sensitivity_range) / sizeof(int32_t));

    staticInfo.update(ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY,
                      &gCamCapability[cameraId]->max_analog_sensitivity,
                      1);

    int32_t sensor_orientation = (int32_t)gCamCapability[cameraId]->sensor_mount_angle;
    staticInfo.update(ANDROID_SENSOR_ORIENTATION,
                      &sensor_orientation,
                      1);

    int32_t max_output_streams[] = {
            MAX_STALLING_STREAMS,
            MAX_PROCESSED_STREAMS,
            MAX_RAW_STREAMS};
    staticInfo.update(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
            max_output_streams,
            sizeof(max_output_streams)/sizeof(max_output_streams[0]));

    uint8_t avail_leds = 0;
    staticInfo.update(ANDROID_LED_AVAILABLE_LEDS,
                      &avail_leds, 0);

    uint8_t focus_dist_calibrated;
    int val = lookupFwkName(FOCUS_CALIBRATION_MAP, METADATA_MAP_SIZE(FOCUS_CALIBRATION_MAP),
            gCamCapability[cameraId]->focus_dist_calibrated);
    if (NAME_NOT_FOUND != val) {
        focus_dist_calibrated = (uint8_t)val;
        staticInfo.update(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
                     &focus_dist_calibrated, 1);
    }

    int32_t avail_testpattern_modes[MAX_TEST_PATTERN_CNT];
    size = 0;
    count = MIN(gCamCapability[cameraId]->supported_test_pattern_modes_cnt,
            MAX_TEST_PATTERN_CNT);
    for (size_t i = 0; i < count; i++) {
        int testpatternMode = lookupFwkName(TEST_PATTERN_MAP, METADATA_MAP_SIZE(TEST_PATTERN_MAP),
                gCamCapability[cameraId]->supported_test_pattern_modes[i]);
        if (NAME_NOT_FOUND != testpatternMode) {
            avail_testpattern_modes[size] = testpatternMode;
            size++;
        }
    }
    staticInfo.update(ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
                      avail_testpattern_modes,
                      size);

    uint8_t max_pipeline_depth =
        (uint8_t)(MAX_INFLIGHT_REQUESTS + EMPTY_PIPELINE_DELAY + FRAME_SKIP_DELAY);
    staticInfo.update(ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
                      &max_pipeline_depth,
                      1);

    int32_t partial_result_count = PARTIAL_RESULT_COUNT;
    staticInfo.update(ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
                      &partial_result_count,
                       1);

    int32_t max_stall_duration = MAX_REPROCESS_STALL;
    staticInfo.update(ANDROID_REPROCESS_MAX_CAPTURE_STALL, &max_stall_duration, 1);

    Vector<uint8_t> available_capabilities;
    available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
    available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
    available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
    available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS);
    if (supportBurst) {
        available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE);
    }
    available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING);
    available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_YUV_REPROCESSING);
    if (hfrEnable && available_hfr_configs.array()) {
        available_capabilities.add(
                ANDROID_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO);
    }
    if (gCamCapability[cameraId]->is_depth_sensor) {
        available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT);
    }

    if (CAM_SENSOR_YUV != gCamCapability[cameraId]->sensor_type.sens_type) {
        available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW);
    }
#ifdef USE_HAL_3_5
    if(is_dual_camera_by_idx(cameraId))
    {
        available_capabilities.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA);
    }
#endif //USE_HAL_3_5
    staticInfo.update(ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
            available_capabilities.array(),
            available_capabilities.size());

    //aeLockAvailable to be set to true if capabilities has MANUAL_SENSOR or BURST_CAPTURE
    //Assumption is that all bayer cameras support MANUAL_SENSOR.
    uint8_t aeLockAvailable = (gCamCapability[cameraId]->sensor_type.sens_type == CAM_SENSOR_RAW) ?
            ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE : ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE;

    staticInfo.update(ANDROID_CONTROL_AE_LOCK_AVAILABLE,
            &aeLockAvailable, 1);

    //awbLockAvailable to be set to true if capabilities has MANUAL_POST_PROCESSING or
    //BURST_CAPTURE. Assumption is that all bayer cameras support MANUAL_POST_PROCESSING.
    uint8_t awbLockAvailable = (gCamCapability[cameraId]->sensor_type.sens_type == CAM_SENSOR_RAW) ?
            ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE : ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE;

    staticInfo.update(ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
            &awbLockAvailable, 1);

    int32_t max_input_streams = 1;
    staticInfo.update(ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
                      &max_input_streams,
                      1);

    /* format of the map is : input format, num_output_formats, outputFormat1,..,outputFormatN */
    int32_t io_format_map[] = {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 2,
            HAL_PIXEL_FORMAT_BLOB, HAL_PIXEL_FORMAT_YCbCr_420_888,
            HAL_PIXEL_FORMAT_YCbCr_420_888, 2, HAL_PIXEL_FORMAT_BLOB,
            HAL_PIXEL_FORMAT_YCbCr_420_888};
    staticInfo.update(ANDROID_SCALER_AVAILABLE_INPUT_OUTPUT_FORMATS_MAP,
                      io_format_map, sizeof(io_format_map)/sizeof(io_format_map[0]));

    int32_t max_latency = ANDROID_SYNC_MAX_LATENCY_PER_FRAME_CONTROL;
    staticInfo.update(ANDROID_SYNC_MAX_LATENCY,
                      &max_latency,
                      1);

#ifndef USE_HAL_3_3
    int32_t isp_sensitivity_range[2];
    isp_sensitivity_range[0] =
        gCamCapability[cameraId]->isp_sensitivity_range.min_sensitivity;
    isp_sensitivity_range[1] =
        gCamCapability[cameraId]->isp_sensitivity_range.max_sensitivity;
    staticInfo.update(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE,
                      isp_sensitivity_range,
                      sizeof(isp_sensitivity_range) / sizeof(isp_sensitivity_range[0]));
#endif

    uint8_t available_hot_pixel_modes[] = {ANDROID_HOT_PIXEL_MODE_FAST,
                                           ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY};
    staticInfo.update(ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
            available_hot_pixel_modes,
            sizeof(available_hot_pixel_modes)/sizeof(available_hot_pixel_modes[0]));

    uint8_t available_shading_modes[] = {ANDROID_SHADING_MODE_OFF,
                                         ANDROID_SHADING_MODE_FAST,
                                         ANDROID_SHADING_MODE_HIGH_QUALITY};
    staticInfo.update(ANDROID_SHADING_AVAILABLE_MODES,
                      available_shading_modes,
                      3);

    uint8_t available_lens_shading_map_modes[] = {ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF,
                                                  ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_ON};
    staticInfo.update(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
                      available_lens_shading_map_modes,
                      2);

    uint8_t available_edge_modes[] = {ANDROID_EDGE_MODE_OFF,
                                      ANDROID_EDGE_MODE_FAST,
                                      ANDROID_EDGE_MODE_HIGH_QUALITY,
                                      ANDROID_EDGE_MODE_ZERO_SHUTTER_LAG};
    staticInfo.update(ANDROID_EDGE_AVAILABLE_EDGE_MODES,
            available_edge_modes,
            sizeof(available_edge_modes)/sizeof(available_edge_modes[0]));

    uint8_t available_noise_red_modes[] = {ANDROID_NOISE_REDUCTION_MODE_OFF,
                                           ANDROID_NOISE_REDUCTION_MODE_FAST,
                                           ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY,
                                           ANDROID_NOISE_REDUCTION_MODE_MINIMAL,
                                           ANDROID_NOISE_REDUCTION_MODE_ZERO_SHUTTER_LAG};
    staticInfo.update(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
            available_noise_red_modes,
            sizeof(available_noise_red_modes)/sizeof(available_noise_red_modes[0]));

    uint8_t available_tonemap_modes[] = {ANDROID_TONEMAP_MODE_CONTRAST_CURVE,
                                         ANDROID_TONEMAP_MODE_FAST,
                                         ANDROID_TONEMAP_MODE_HIGH_QUALITY};
    staticInfo.update(ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
            available_tonemap_modes,
            sizeof(available_tonemap_modes)/sizeof(available_tonemap_modes[0]));

    uint8_t available_hot_pixel_map_modes[] = {ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF};
    staticInfo.update(ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
            available_hot_pixel_map_modes,
            sizeof(available_hot_pixel_map_modes)/sizeof(available_hot_pixel_map_modes[0]));

    val = lookupFwkName(REFERENCE_ILLUMINANT_MAP, METADATA_MAP_SIZE(REFERENCE_ILLUMINANT_MAP),
            gCamCapability[cameraId]->reference_illuminant1);
    if (NAME_NOT_FOUND != val) {
        uint8_t fwkReferenceIlluminant = (uint8_t)val;
        staticInfo.update(ANDROID_SENSOR_REFERENCE_ILLUMINANT1, &fwkReferenceIlluminant, 1);
    }

    val = lookupFwkName(REFERENCE_ILLUMINANT_MAP, METADATA_MAP_SIZE(REFERENCE_ILLUMINANT_MAP),
            gCamCapability[cameraId]->reference_illuminant2);
    if (NAME_NOT_FOUND != val) {
        uint8_t fwkReferenceIlluminant = (uint8_t)val;
        staticInfo.update(ANDROID_SENSOR_REFERENCE_ILLUMINANT2, &fwkReferenceIlluminant, 1);
    }

    staticInfo.update(ANDROID_SENSOR_FORWARD_MATRIX1, (camera_metadata_rational_t *)
            (void *)gCamCapability[cameraId]->forward_matrix1,
            FORWARD_MATRIX_COLS * FORWARD_MATRIX_ROWS);

    staticInfo.update(ANDROID_SENSOR_FORWARD_MATRIX2, (camera_metadata_rational_t *)
            (void *)gCamCapability[cameraId]->forward_matrix2,
            FORWARD_MATRIX_COLS * FORWARD_MATRIX_ROWS);

    staticInfo.update(ANDROID_SENSOR_COLOR_TRANSFORM1, (camera_metadata_rational_t *)
            (void *)gCamCapability[cameraId]->color_transform1,
            COLOR_TRANSFORM_COLS * COLOR_TRANSFORM_ROWS);

    staticInfo.update(ANDROID_SENSOR_COLOR_TRANSFORM2, (camera_metadata_rational_t *)
            (void *)gCamCapability[cameraId]->color_transform2,
            COLOR_TRANSFORM_COLS * COLOR_TRANSFORM_ROWS);

    staticInfo.update(ANDROID_SENSOR_CALIBRATION_TRANSFORM1, (camera_metadata_rational_t *)
            (void *)gCamCapability[cameraId]->calibration_transform1,
            CAL_TRANSFORM_COLS * CAL_TRANSFORM_ROWS);

    staticInfo.update(ANDROID_SENSOR_CALIBRATION_TRANSFORM2, (camera_metadata_rational_t *)
            (void *)gCamCapability[cameraId]->calibration_transform2,
            CAL_TRANSFORM_COLS * CAL_TRANSFORM_ROWS);

    int32_t request_keys_basic[] = {ANDROID_COLOR_CORRECTION_MODE,
       ANDROID_COLOR_CORRECTION_TRANSFORM, ANDROID_COLOR_CORRECTION_GAINS,
       ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
       ANDROID_CONTROL_AE_ANTIBANDING_MODE, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
       ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_MODE,
       ANDROID_CONTROL_AE_REGIONS, ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
       ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, ANDROID_CONTROL_AF_MODE,
       ANDROID_CONTROL_AF_TRIGGER, ANDROID_CONTROL_AWB_LOCK,
       ANDROID_CONTROL_AWB_MODE, ANDROID_CONTROL_CAPTURE_INTENT,
       ANDROID_CONTROL_EFFECT_MODE, ANDROID_CONTROL_MODE,
       ANDROID_CONTROL_SCENE_MODE, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
       ANDROID_DEMOSAIC_MODE, ANDROID_EDGE_MODE,
       ANDROID_FLASH_FIRING_POWER, ANDROID_FLASH_FIRING_TIME, ANDROID_FLASH_MODE,
       ANDROID_JPEG_GPS_COORDINATES,
       ANDROID_JPEG_GPS_PROCESSING_METHOD, ANDROID_JPEG_GPS_TIMESTAMP,
       ANDROID_JPEG_ORIENTATION, ANDROID_JPEG_QUALITY, ANDROID_JPEG_THUMBNAIL_QUALITY,
       ANDROID_JPEG_THUMBNAIL_SIZE, ANDROID_LENS_APERTURE, ANDROID_LENS_FILTER_DENSITY,
       ANDROID_LENS_FOCAL_LENGTH, ANDROID_LENS_FOCUS_DISTANCE,
       ANDROID_LENS_OPTICAL_STABILIZATION_MODE, ANDROID_NOISE_REDUCTION_MODE,
       ANDROID_REQUEST_ID, ANDROID_REQUEST_TYPE,
       ANDROID_SCALER_CROP_REGION, ANDROID_SENSOR_EXPOSURE_TIME,
       ANDROID_SENSOR_FRAME_DURATION, ANDROID_HOT_PIXEL_MODE,
       ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE,
       ANDROID_SENSOR_SENSITIVITY, ANDROID_SHADING_MODE,
#ifndef USE_HAL_3_3
       ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST,
#endif
       ANDROID_STATISTICS_FACE_DETECT_MODE,
       ANDROID_STATISTICS_SHARPNESS_MAP_MODE,
       ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, ANDROID_TONEMAP_CURVE_BLUE,
       ANDROID_TONEMAP_CURVE_GREEN, ANDROID_TONEMAP_CURVE_RED, ANDROID_TONEMAP_MODE,
       ANDROID_BLACK_LEVEL_LOCK, ANDROID_CONTROL_ENABLE_ZSL };

    size_t request_keys_cnt =
            sizeof(request_keys_basic)/sizeof(request_keys_basic[0]);
    Vector<int32_t> available_request_keys;
    available_request_keys.appendArray(request_keys_basic, request_keys_cnt);
    if (gCamCapability[cameraId]->supported_focus_modes_cnt > 1) {
        available_request_keys.add(ANDROID_CONTROL_AF_REGIONS);
    }

    staticInfo.update(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
            available_request_keys.array(), available_request_keys.size());

    int32_t result_keys_basic[] = {ANDROID_COLOR_CORRECTION_TRANSFORM,
       ANDROID_COLOR_CORRECTION_GAINS, ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_REGIONS,
       ANDROID_CONTROL_AE_STATE, ANDROID_CONTROL_AF_MODE,
       ANDROID_CONTROL_AF_STATE, ANDROID_CONTROL_AWB_MODE,
       ANDROID_CONTROL_AWB_STATE, ANDROID_CONTROL_MODE, ANDROID_EDGE_MODE,
       ANDROID_FLASH_FIRING_POWER, ANDROID_FLASH_FIRING_TIME, ANDROID_FLASH_MODE,
       ANDROID_FLASH_STATE, ANDROID_JPEG_GPS_COORDINATES, ANDROID_JPEG_GPS_PROCESSING_METHOD,
       ANDROID_JPEG_GPS_TIMESTAMP, ANDROID_JPEG_ORIENTATION, ANDROID_JPEG_QUALITY,
       ANDROID_JPEG_THUMBNAIL_QUALITY, ANDROID_JPEG_THUMBNAIL_SIZE, ANDROID_LENS_APERTURE,
       ANDROID_LENS_FILTER_DENSITY, ANDROID_LENS_FOCAL_LENGTH, ANDROID_LENS_FOCUS_DISTANCE,
       ANDROID_LENS_FOCUS_RANGE, ANDROID_LENS_STATE, ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
       ANDROID_NOISE_REDUCTION_MODE, ANDROID_REQUEST_ID,
       ANDROID_SCALER_CROP_REGION, ANDROID_SHADING_MODE, ANDROID_SENSOR_EXPOSURE_TIME,
       ANDROID_SENSOR_FRAME_DURATION, ANDROID_SENSOR_SENSITIVITY,
       ANDROID_SENSOR_TIMESTAMP, ANDROID_SENSOR_NEUTRAL_COLOR_POINT,
       ANDROID_SENSOR_PROFILE_TONE_CURVE, ANDROID_BLACK_LEVEL_LOCK, ANDROID_TONEMAP_CURVE_BLUE,
       ANDROID_TONEMAP_CURVE_GREEN, ANDROID_TONEMAP_CURVE_RED, ANDROID_TONEMAP_MODE,
       ANDROID_STATISTICS_FACE_DETECT_MODE,
       ANDROID_STATISTICS_SHARPNESS_MAP, ANDROID_STATISTICS_SHARPNESS_MAP_MODE,
       ANDROID_STATISTICS_PREDICTED_COLOR_GAINS, ANDROID_STATISTICS_PREDICTED_COLOR_TRANSFORM,
       ANDROID_STATISTICS_SCENE_FLICKER, ANDROID_STATISTICS_FACE_RECTANGLES,
       ANDROID_STATISTICS_FACE_SCORES,
#ifndef USE_HAL_3_3
       ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST,
#endif
       ANDROID_CONTROL_ENABLE_ZSL};

    size_t result_keys_cnt =
            sizeof(result_keys_basic)/sizeof(result_keys_basic[0]);

    Vector<int32_t> available_result_keys;
    available_result_keys.appendArray(result_keys_basic, result_keys_cnt);
    if (gCamCapability[cameraId]->supported_focus_modes_cnt > 1) {
        available_result_keys.add(ANDROID_CONTROL_AF_REGIONS);
    }
    if (CAM_SENSOR_RAW == gCamCapability[cameraId]->sensor_type.sens_type) {
        available_result_keys.add(ANDROID_SENSOR_NOISE_PROFILE);
        available_result_keys.add(ANDROID_SENSOR_GREEN_SPLIT);
    }
    if (supportedFaceDetectMode == 1) {
        available_result_keys.add(ANDROID_STATISTICS_FACE_RECTANGLES);
        available_result_keys.add(ANDROID_STATISTICS_FACE_SCORES);
    } else if ((supportedFaceDetectMode == 2) ||
            (supportedFaceDetectMode == 3)) {
        available_result_keys.add(ANDROID_STATISTICS_FACE_IDS);
        available_result_keys.add(ANDROID_STATISTICS_FACE_LANDMARKS);
    }
#ifndef USE_HAL_3_3
    if (hasBlackRegions) {
        available_result_keys.add(ANDROID_SENSOR_DYNAMIC_BLACK_LEVEL);
        available_result_keys.add(ANDROID_SENSOR_DYNAMIC_WHITE_LEVEL);
    }
#endif
    staticInfo.update(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
            available_result_keys.array(), available_result_keys.size());

    int32_t characteristics_keys_basic[] = {ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
       ANDROID_CONTROL_AE_AVAILABLE_MODES, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
       ANDROID_CONTROL_AE_COMPENSATION_RANGE, ANDROID_CONTROL_AE_COMPENSATION_STEP,
       ANDROID_CONTROL_AF_AVAILABLE_MODES, ANDROID_CONTROL_AVAILABLE_EFFECTS,
       ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
       ANDROID_SCALER_CROPPING_TYPE,
       ANDROID_SYNC_MAX_LATENCY,
       ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
       ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
       ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
       ANDROID_CONTROL_AWB_AVAILABLE_MODES, ANDROID_CONTROL_MAX_REGIONS,
       ANDROID_CONTROL_SCENE_MODE_OVERRIDES,ANDROID_FLASH_INFO_AVAILABLE,
       ANDROID_FLASH_INFO_CHARGE_DURATION, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
       ANDROID_JPEG_MAX_SIZE, ANDROID_LENS_INFO_AVAILABLE_APERTURES,
       ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
       ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
       ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
       ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
       ANDROID_LENS_INFO_SHADING_MAP_SIZE, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
       ANDROID_LENS_FACING,
       ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
       ANDROID_REQUEST_PIPELINE_MAX_DEPTH, ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
       ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
       ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
       ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
       ANDROID_SCALER_AVAILABLE_INPUT_OUTPUT_FORMATS_MAP,
       ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
       /*ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,*/
       ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, ANDROID_SENSOR_FORWARD_MATRIX1,
       ANDROID_SENSOR_REFERENCE_ILLUMINANT1, ANDROID_SENSOR_REFERENCE_ILLUMINANT2,
       ANDROID_SENSOR_FORWARD_MATRIX2, ANDROID_SENSOR_COLOR_TRANSFORM1,
       ANDROID_SENSOR_COLOR_TRANSFORM2, ANDROID_SENSOR_CALIBRATION_TRANSFORM1,
       ANDROID_SENSOR_CALIBRATION_TRANSFORM2, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
       ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
       ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
       ANDROID_SENSOR_INFO_PHYSICAL_SIZE, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
       ANDROID_SENSOR_INFO_WHITE_LEVEL, ANDROID_SENSOR_BASE_GAIN_FACTOR,
       ANDROID_SENSOR_BLACK_LEVEL_PATTERN, ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY,
       ANDROID_SENSOR_ORIENTATION, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
       ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
       ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
       ANDROID_STATISTICS_INFO_MAX_SHARPNESS_MAP_VALUE,
       ANDROID_STATISTICS_INFO_SHARPNESS_MAP_SIZE, ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
       ANDROID_EDGE_AVAILABLE_EDGE_MODES,
       ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
       ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
       ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
       ANDROID_TONEMAP_MAX_CURVE_POINTS,
       ANDROID_CONTROL_AVAILABLE_MODES,
       ANDROID_CONTROL_AE_LOCK_AVAILABLE,
       ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
       ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
       ANDROID_SHADING_AVAILABLE_MODES,
       ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
#ifndef USE_HAL_3_3
       ANDROID_SENSOR_OPAQUE_RAW_SIZE,
       ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE,
#endif
       };

    Vector<int32_t> available_characteristics_keys;
    available_characteristics_keys.appendArray(characteristics_keys_basic,
            sizeof(characteristics_keys_basic)/sizeof(int32_t));
#ifndef USE_HAL_3_3
    if (hasBlackRegions) {
        available_characteristics_keys.add(ANDROID_SENSOR_OPTICAL_BLACK_REGIONS);
    }
#endif
    staticInfo.update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                      available_characteristics_keys.array(),
                      available_characteristics_keys.size());

    /*available stall durations depend on the hw + sw and will be different for different devices */
    /*have to add for raw after implementation*/
    int32_t stall_formats[] = {HAL_PIXEL_FORMAT_BLOB, ANDROID_SCALER_AVAILABLE_FORMATS_RAW16};
    size_t stall_formats_count = sizeof(stall_formats)/sizeof(int32_t);

    Vector<int64_t> available_stall_durations;
    for (uint32_t j = 0; j < stall_formats_count; j++) {
        if (stall_formats[j] == HAL_PIXEL_FORMAT_BLOB) {
            for (uint32_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->picture_sizes_tbl_cnt); i++) {
                available_stall_durations.add(stall_formats[j]);
                available_stall_durations.add(gCamCapability[cameraId]->picture_sizes_tbl[i].width);
                available_stall_durations.add(gCamCapability[cameraId]->picture_sizes_tbl[i].height);
                available_stall_durations.add(gCamCapability[cameraId]->jpeg_stall_durations[i]);
          }
        } else {
            for (uint32_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
                available_stall_durations.add(stall_formats[j]);
                available_stall_durations.add(gCamCapability[cameraId]->raw_dim[i].width);
                available_stall_durations.add(gCamCapability[cameraId]->raw_dim[i].height);
                available_stall_durations.add(gCamCapability[cameraId]->raw16_stall_durations[i]);
            }
        }
    }
    staticInfo.update(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                      available_stall_durations.array(),
                      available_stall_durations.size());

    //QCAMERA3_OPAQUE_RAW
    uint8_t raw_format = QCAMERA3_OPAQUE_RAW_FORMAT_LEGACY;
    cam_format_t fmt = CAM_FORMAT_BAYER_QCOM_RAW_10BPP_GBRG;
    switch (gCamCapability[cameraId]->opaque_raw_fmt) {
    case LEGACY_RAW:
        if (gCamCapability[cameraId]->white_level == MAX_VALUE_8BIT)
            fmt = CAM_FORMAT_BAYER_QCOM_RAW_8BPP_GBRG;
        else if (gCamCapability[cameraId]->white_level == MAX_VALUE_10BIT)
            fmt = CAM_FORMAT_BAYER_QCOM_RAW_10BPP_GBRG;
        else if (gCamCapability[cameraId]->white_level == MAX_VALUE_12BIT)
            fmt = CAM_FORMAT_BAYER_QCOM_RAW_12BPP_GBRG;
        raw_format = QCAMERA3_OPAQUE_RAW_FORMAT_LEGACY;
        break;
    case MIPI_RAW:
        if (gCamCapability[cameraId]->white_level == MAX_VALUE_8BIT)
            fmt = CAM_FORMAT_BAYER_MIPI_RAW_8BPP_GBRG;
        else if (gCamCapability[cameraId]->white_level == MAX_VALUE_10BIT)
            fmt = CAM_FORMAT_BAYER_MIPI_RAW_10BPP_GBRG;
        else if (gCamCapability[cameraId]->white_level == MAX_VALUE_12BIT)
            fmt = CAM_FORMAT_BAYER_MIPI_RAW_12BPP_GBRG;
        raw_format = QCAMERA3_OPAQUE_RAW_FORMAT_MIPI;
        break;
    default:
        LOGE("unknown opaque_raw_format %d",
                gCamCapability[cameraId]->opaque_raw_fmt);
        break;
    }
    staticInfo.update(QCAMERA3_OPAQUE_RAW_FORMAT, &raw_format, 1);

    Vector<int32_t> strides;
    for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
            gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
        cam_stream_buf_plane_info_t buf_planes;
        strides.add(gCamCapability[cameraId]->raw_dim[i].width);
        strides.add(gCamCapability[cameraId]->raw_dim[i].height);
        mm_stream_calc_offset_raw(fmt, &gCamCapability[cameraId]->raw_dim[i],
            &gCamCapability[cameraId]->padding_info, &buf_planes);
        strides.add(buf_planes.plane_info.mp[0].stride);
    }
    staticInfo.update(QCAMERA3_OPAQUE_RAW_STRIDES, strides.array(),
            strides.size());

    //Video HDR default
    if ((gCamCapability[cameraId]->qcom_supported_feature_mask) &
            (CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR |
            CAM_QCOM_FEATURE_ZIGZAG_HDR | CAM_QCOM_FEATURE_SENSOR_HDR)) {
        int32_t vhdr_mode[] = {
                QCAMERA3_VIDEO_HDR_MODE_OFF,
                QCAMERA3_VIDEO_HDR_MODE_ON};

        size_t vhdr_mode_count = sizeof(vhdr_mode) / sizeof(int32_t);
        staticInfo.update(QCAMERA3_AVAILABLE_VIDEO_HDR_MODES,
                    vhdr_mode, vhdr_mode_count);
    }

    staticInfo.update(QCAMERA3_DUALCAM_CALIB_META_DATA_BLOB,
            (const uint8_t*)&gCamCapability[cameraId]->related_cam_calibration,
            sizeof(gCamCapability[cameraId]->related_cam_calibration));

    uint8_t isMonoOnly =
            (gCamCapability[cameraId]->color_arrangement == CAM_FILTER_ARRANGEMENT_Y);
    staticInfo.update(QCAMERA3_SENSOR_IS_MONO_ONLY,
            &isMonoOnly, 1);

#ifndef USE_HAL_3_3
    Vector<int32_t> opaque_size;
    for (size_t j = 0; j < scalar_formats_count; j++) {
        if (scalar_formats[j] == ANDROID_SCALER_AVAILABLE_FORMATS_RAW_OPAQUE) {
            for (size_t i = 0; i < MIN(MAX_SIZES_CNT,
                    gCamCapability[cameraId]->supported_raw_dim_cnt); i++) {
                cam_stream_buf_plane_info_t buf_planes;

                rc = mm_stream_calc_offset_raw(fmt, &gCamCapability[cameraId]->raw_dim[i],
                         &gCamCapability[cameraId]->padding_info, &buf_planes);

                if (rc == 0) {
                    opaque_size.add(gCamCapability[cameraId]->raw_dim[i].width);
                    opaque_size.add(gCamCapability[cameraId]->raw_dim[i].height);
                    opaque_size.add(buf_planes.plane_info.frame_len);
                }else {
                    LOGE("raw frame calculation failed!");
                }
            }
        }
    }

    if ((opaque_size.size() > 0) &&
            (opaque_size.size() % PER_CONFIGURATION_SIZE_3 == 0))
        staticInfo.update(ANDROID_SENSOR_OPAQUE_RAW_SIZE, opaque_size.array(), opaque_size.size());
    else
        LOGW("Warning: ANDROID_SENSOR_OPAQUE_RAW_SIZE is using rough estimation(2 bytes/pixel)");
#endif

    if (gCamCapability[cameraId]->supported_ir_mode_cnt > 0) {
        int32_t avail_ir_modes[CAM_IR_MODE_MAX];
        size = 0;
        count = CAM_IR_MODE_MAX;
        count = MIN(gCamCapability[cameraId]->supported_ir_mode_cnt, count);
        for (size_t i = 0; i < count; i++) {
            int val = lookupFwkName(IR_MODES_MAP, METADATA_MAP_SIZE(IR_MODES_MAP),
                    gCamCapability[cameraId]->supported_ir_modes[i]);
            if (NAME_NOT_FOUND != val) {
                avail_ir_modes[size] = (int32_t)val;
                size++;
            }
        }
        staticInfo.update(QCAMERA3_IR_AVAILABLE_MODES,
                avail_ir_modes, size);
    }

    if (gCamCapability[cameraId]->supported_instant_aec_modes_cnt > 0) {
        int32_t available_instant_aec_modes[CAM_AEC_CONVERGENCE_MAX];
        size = 0;
        count = CAM_AEC_CONVERGENCE_MAX;
        count = MIN(gCamCapability[cameraId]->supported_instant_aec_modes_cnt, count);
        for (size_t i = 0; i < count; i++) {
            int val = lookupFwkName(INSTANT_AEC_MODES_MAP, METADATA_MAP_SIZE(INSTANT_AEC_MODES_MAP),
                    gCamCapability[cameraId]->supported_instant_aec_modes[i]);
            if (NAME_NOT_FOUND != val) {
                available_instant_aec_modes[size] = (int32_t)val;
                size++;
            }
        }
        staticInfo.update(QCAMERA3_INSTANT_AEC_AVAILABLE_MODES,
                available_instant_aec_modes, size);
    }

    int32_t sharpness_range[] = {
            gCamCapability[cameraId]->sharpness_ctrl.min_value,
            gCamCapability[cameraId]->sharpness_ctrl.max_value};
    staticInfo.update(QCAMERA3_SHARPNESS_RANGE, sharpness_range, 2);

    if (gCamCapability[cameraId]->supported_binning_correction_mode_cnt > 0) {
        int32_t avail_binning_modes[CAM_BINNING_CORRECTION_MODE_MAX];
        size = 0;
        count = CAM_BINNING_CORRECTION_MODE_MAX;
        count = MIN(gCamCapability[cameraId]->supported_binning_correction_mode_cnt, count);
        for (size_t i = 0; i < count; i++) {
            int val = lookupFwkName(BINNING_CORRECTION_MODES_MAP,
                    METADATA_MAP_SIZE(BINNING_CORRECTION_MODES_MAP),
                    gCamCapability[cameraId]->supported_binning_modes[i]);
            if (NAME_NOT_FOUND != val) {
                avail_binning_modes[size] = (int32_t)val;
                size++;
            }
        }
        staticInfo.update(QCAMERA3_AVAILABLE_BINNING_CORRECTION_MODES,
                avail_binning_modes, size);
    }

    if (gCamCapability[cameraId]->supported_aec_modes_cnt > 0) {
        int32_t available_aec_modes[CAM_AEC_MODE_MAX];
        size = 0;
        count = MIN(gCamCapability[cameraId]->supported_aec_modes_cnt, CAM_AEC_MODE_MAX);
        for (size_t i = 0; i < count; i++) {
            int32_t val = lookupFwkName(AEC_MODES_MAP, METADATA_MAP_SIZE(AEC_MODES_MAP),
                    gCamCapability[cameraId]->supported_aec_modes[i]);
            if (NAME_NOT_FOUND != val)
                available_aec_modes[size++] = val;
        }
        staticInfo.update(QCAMERA3_EXPOSURE_METER_AVAILABLE_MODES,
                available_aec_modes, size);
    }

    if (gCamCapability[cameraId]->supported_iso_modes_cnt > 0) {
        int32_t available_iso_modes[CAM_ISO_MODE_MAX];
        size = 0;
        count = MIN(gCamCapability[cameraId]->supported_iso_modes_cnt, CAM_ISO_MODE_MAX);
        uint8_t max_analog_mode = getIsoMode(gCamCapability[cameraId]->max_analog_sensitivity);
        count = MIN(count, max_analog_mode + 1);
        for (size_t i = 0; i < count; i++) {
            int32_t val = lookupFwkName(ISO_MODES_MAP, METADATA_MAP_SIZE(ISO_MODES_MAP),
                    gCamCapability[cameraId]->supported_iso_modes[i]);
            if (NAME_NOT_FOUND != val)
                available_iso_modes[size++] = val;
        }
        staticInfo.update(QCAMERA3_ISO_AVAILABLE_MODES,
                available_iso_modes, size);
    }

    int64_t available_exp_time_range[EXPOSURE_TIME_RANGE_CNT];
    for (size_t i = 0; i < EXPOSURE_TIME_RANGE_CNT; i++)
        available_exp_time_range[i] = gCamCapability[cameraId]->exposure_time_range[i];
    staticInfo.update(QCAMERA3_EXP_TIME_RANGE,
            available_exp_time_range, EXPOSURE_TIME_RANGE_CNT);

    int32_t available_saturation_range[4];
    available_saturation_range[0] = gCamCapability[cameraId]->saturation_ctrl.min_value;
    available_saturation_range[1] = gCamCapability[cameraId]->saturation_ctrl.max_value;
    available_saturation_range[2] = gCamCapability[cameraId]->saturation_ctrl.def_value;
    available_saturation_range[3] = gCamCapability[cameraId]->saturation_ctrl.step;
    staticInfo.update(QCAMERA3_SATURATION_RANGE,
            available_saturation_range, 4);

    uint8_t is_hdr_values[2];
    is_hdr_values[0] = 0;
    is_hdr_values[1] = 1;
    staticInfo.update(QCAMERA3_STATS_IS_HDR_SCENE_VALUES,
            is_hdr_values, 2);

    float is_hdr_confidence_range[2];
    is_hdr_confidence_range[0] = 0.0;
    is_hdr_confidence_range[1] = 1.0;
    staticInfo.update(QCAMERA3_STATS_IS_HDR_SCENE_CONFIDENCE_RANGE,
            is_hdr_confidence_range, 2);
    if (gCamCapability[cameraId]->is_depth_sensor) {
        staticInfo.update(ANDROID_DEPTH_MAX_DEPTH_SAMPLES,
                &gCamCapability[cameraId]->max_depth_points,1);
        staticInfo.update(ANDROID_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS,
                available_stream_configs.array(), available_stream_configs.size());
        staticInfo.update(ANDROID_DEPTH_AVAILABLE_DEPTH_STALL_DURATIONS,
                available_stall_durations.array(),
                available_stall_durations.size());
        staticInfo.update(ANDROID_DEPTH_AVAILABLE_DEPTH_MIN_FRAME_DURATIONS,
                available_min_durations.array(), available_min_durations.size());
        uint8_t isDepthOnly = ANDROID_DEPTH_DEPTH_IS_EXCLUSIVE_TRUE;
        staticInfo.update(ANDROID_DEPTH_DEPTH_IS_EXCLUSIVE, &isDepthOnly, 1);
    }

    if (gCamCapability[cameraId]->is_quadracfa_sensor) {
        uint8_t is_qcfa_sensor = 1;
        int32_t dim[2];
        dim[0] = gCamCapability[cameraId]->quadra_cfa_dim[0].width;
        dim[1] = gCamCapability[cameraId]->quadra_cfa_dim[0].height;
        LOGD("vendor tag for quadra cfa, dim:%dx%d", dim[0], dim[1]);
        staticInfo.update(QCAMERA3_IS_QUADRA_CFA_SENSOR, &is_qcfa_sensor, 1);
        staticInfo.update(QCAMERA3_SUPPORT_QUADRA_CFA_DIM, dim, 2);
    }

    //HFR configs for 60 and 90
    Vector<int32_t> custom_hfr_configs;
    for (size_t i = 0; i < gCamCapability[cameraId]->hfr_tbl_cnt; i++) {
        int32_t fps = 0;
        switch (gCamCapability[cameraId]->hfr_tbl[i].mode) {
        case CAM_HFR_MODE_60FPS:
            fps = 60;
            break;
        case CAM_HFR_MODE_90FPS:
            fps = 90;
            break;
        default:
            break;
        }

        if (fps > 0) {
            /* (fps, max width, max height) */
            custom_hfr_configs.add(fps);
            custom_hfr_configs.add(
                    gCamCapability[cameraId]->hfr_tbl[i].dim[0].width);
            custom_hfr_configs.add(
                    gCamCapability[cameraId]->hfr_tbl[i].dim[0].height);
        }
    }

    /*HFR configs of 60 and 90fps are not supported as changes are not completely implemented
    end to end. Once changes are implemented, changes can be uncommented to support it. Define
    macro "SUPPORT_HFR_CONFIG_60_90_FPS" to enable HFR 60 and 90 fps in the app setting*/
    if (custom_hfr_configs.size() > 0) {
        staticInfo.update(
            QCAMERA3_HFR_SIZES,
            custom_hfr_configs.array(), custom_hfr_configs.size());
    }

    uint8_t cam_mode = is_dual_camera_by_idx(cameraId);
    staticInfo.update(QCAMERA3_LOGICAL_CAM_MODE, &cam_mode, 1);

    // set supported wb cct, we should get them from m_pCapabilit
    gCamCapability[cameraId]->min_wb_cct = 2000;
    gCamCapability[cameraId]->max_wb_cct = 8000;
    // set supported wb rgb gains, ideally we should get them from m_pCapability
    //but for now hardcode.
    gCamCapability[cameraId]->min_wb_gain = 1.0;
    gCamCapability[cameraId]->max_wb_gain = 4.0;

    int32_t cct_range[2];
    cct_range[0] = gCamCapability[cameraId]->min_wb_cct;
    cct_range[1] = gCamCapability[cameraId]->max_wb_cct;
    staticInfo.update(QCAMERA3_MANUAL_WB_CCT_RANGE,
            cct_range, 2);

    float wb_gains_range[2];
    wb_gains_range[0] = gCamCapability[cameraId]->min_wb_gain;
    wb_gains_range[1] = gCamCapability[cameraId]->max_wb_gain;
    staticInfo.update(QCAMERA3_MANUAL_WB_GAINS_RANGE,
            wb_gains_range, 2);

#ifdef USE_HAL_3_5
    if(is_dual_camera_by_idx(cameraId))
    {
        uint8_t physical_camera_ids[4];
        memset(physical_camera_ids, 0, sizeof(physical_camera_ids));
        physical_camera_ids[0] = convertIdToUTF8(get_main_camera_idx(cameraId));
        physical_camera_ids[2] = convertIdToUTF8(get_aux_camera_idx(cameraId));
        staticInfo.update(ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS,
                                  (uint8_t *)physical_camera_ids, 4);
        int sync_type = ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE_CALIBRATED;
        staticInfo.update(ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE, &sync_type, 1);
    }
#endif //USE_HAL_3_5
    staticInfo.update(ANDROID_REQUEST_AVAILABLE_PHYSICAL_CAMERA_REQUEST_KEYS,
            available_request_keys.array(), available_request_keys.size());
    gStaticMetadata[cameraId] = staticInfo.release();
    return rc;
}

/*===========================================================================
 * FUNCTION   : convertIdToUTF8
 *
 * DESCRIPTION: Method to convert camera id to UTF8
 *
 * PARAMETERS :
 *   @(uint32_t) id: camera id need to be converted.
 *
 * RETURN     :
 * @(uint8_t) : utf8 converted value.
 *==========================================================================*/
uint8_t QCamera3HardwareInterface::convertIdToUTF8(uint32_t id)
{
    return id + '0';
}

/*===========================================================================
 * FUNCTION   : makeTable
 *
 * DESCRIPTION: make a table of sizes
 *
 * PARAMETERS :
 *
 *
 *==========================================================================*/
void QCamera3HardwareInterface::makeTable(cam_dimension_t* dimTable, size_t size,
        size_t max_size, int32_t *sizeTable)
{
    size_t j = 0;
    if (size > max_size) {
       size = max_size;
    }
    for (size_t i = 0; i < size; i++) {
        sizeTable[j] = dimTable[i].width;
        sizeTable[j+1] = dimTable[i].height;
        j+=2;
    }
}

/*===========================================================================
 * FUNCTION   : makeFPSTable
 *
 * DESCRIPTION: make a table of fps ranges
 *
 * PARAMETERS :
 *
 *==========================================================================*/
void QCamera3HardwareInterface::makeFPSTable(cam_fps_range_t* fpsTable, size_t size,
        size_t max_size, int32_t *fpsRangesTable)
{
    size_t j = 0;
    if (size > max_size) {
       size = max_size;
    }
    for (size_t i = 0; i < size; i++) {
        fpsRangesTable[j] = (int32_t)fpsTable[i].min_fps;
        fpsRangesTable[j+1] = (int32_t)fpsTable[i].max_fps;
        j+=2;
    }
}

/*===========================================================================
 * FUNCTION   : makeOverridesList
 *
 * DESCRIPTION: make a list of scene mode overrides
 *
 * PARAMETERS :
 *
 *
 *==========================================================================*/
void QCamera3HardwareInterface::makeOverridesList(
        cam_scene_mode_overrides_t* overridesTable, size_t size, size_t max_size,
        uint8_t *overridesList, uint8_t *supported_indexes, uint32_t camera_id)
{
    /*daemon will give a list of overrides for all scene modes.
      However we should send the fwk only the overrides for the scene modes
      supported by the framework*/
    size_t j = 0;
    if (size > max_size) {
       size = max_size;
    }
    size_t focus_count = CAM_FOCUS_MODE_MAX;
    focus_count = MIN(gCamCapability[camera_id]->supported_focus_modes_cnt,
            focus_count);
    for (size_t i = 0; i < size; i++) {
        bool supt = false;
        size_t index = supported_indexes[i];
        overridesList[j] = gCamCapability[camera_id]->flash_available ?
                ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH : ANDROID_CONTROL_AE_MODE_ON;
        int val = lookupFwkName(WHITE_BALANCE_MODES_MAP,
                METADATA_MAP_SIZE(WHITE_BALANCE_MODES_MAP),
                overridesTable[index].awb_mode);
        if (NAME_NOT_FOUND != val) {
            overridesList[j+1] = (uint8_t)val;
        }
        uint8_t focus_override = overridesTable[index].af_mode;
        for (size_t k = 0; k < focus_count; k++) {
           if (gCamCapability[camera_id]->supported_focus_modes[k] == focus_override) {
              supt = true;
              break;
           }
        }
        if (supt) {
            val = lookupFwkName(FOCUS_MODES_MAP, METADATA_MAP_SIZE(FOCUS_MODES_MAP),
                    focus_override);
            if (NAME_NOT_FOUND != val) {
                overridesList[j+2] = (uint8_t)val;
            }
        } else {
           overridesList[j+2] = ANDROID_CONTROL_AF_MODE_OFF;
        }
        j+=3;
    }
}

/*===========================================================================
 * FUNCTION   : filterJpegSizes
 *
 * DESCRIPTION: Returns the supported jpeg sizes based on the max dimension that
 *              could be downscaled to
 *
 * PARAMETERS :
 *
 * RETURN     : length of jpegSizes array
 *==========================================================================*/

size_t QCamera3HardwareInterface::filterJpegSizes(int32_t *jpegSizes, int32_t *processedSizes,
        size_t processedSizesCnt, size_t maxCount, cam_rect_t active_array_size,
        uint8_t downscale_factor)
{
    if (0 == downscale_factor) {
        downscale_factor = 1;
    }

    int32_t min_width = active_array_size.width / downscale_factor;
    int32_t min_height = active_array_size.height / downscale_factor;
    size_t jpegSizesCnt = 0;
    if (processedSizesCnt > maxCount) {
        processedSizesCnt = maxCount;
    }
    for (size_t i = 0; i < processedSizesCnt; i+=2) {
        if (processedSizes[i] >= min_width && processedSizes[i+1] >= min_height) {
            jpegSizes[jpegSizesCnt] = processedSizes[i];
            jpegSizes[jpegSizesCnt+1] = processedSizes[i+1];
            jpegSizesCnt += 2;
        }
    }
    return jpegSizesCnt;
}

/*===========================================================================
 * FUNCTION   : computeNoiseModelEntryS
 *
 * DESCRIPTION: function to map a given sensitivity to the S noise
 *              model parameters in the DNG noise model.
 *
 * PARAMETERS : sens : the sensor sensitivity
 *
 ** RETURN    : S (sensor amplification) noise
 *
 *==========================================================================*/
double QCamera3HardwareInterface::computeNoiseModelEntryS(int32_t sens) {
    double s = gCamCapability[mCameraId]->gradient_S * sens +
            gCamCapability[mCameraId]->offset_S;
    return ((s < 0.0) ? 0.0 : s);
}

/*===========================================================================
 * FUNCTION   : computeNoiseModelEntryO
 *
 * DESCRIPTION: function to map a given sensitivity to the O noise
 *              model parameters in the DNG noise model.
 *
 * PARAMETERS : sens : the sensor sensitivity
 *
 ** RETURN    : O (sensor readout) noise
 *
 *==========================================================================*/
double QCamera3HardwareInterface::computeNoiseModelEntryO(int32_t sens) {
    int32_t max_analog_sens = gCamCapability[mCameraId]->max_analog_sensitivity;
    double digital_gain = (1.0 * sens / max_analog_sens) < 1.0 ?
            1.0 : (1.0 * sens / max_analog_sens);
    double o = gCamCapability[mCameraId]->gradient_O * sens * sens +
            gCamCapability[mCameraId]->offset_O * digital_gain * digital_gain;
    return ((o < 0.0) ? 0.0 : o);
}

/*===========================================================================
 * FUNCTION   : getSensorSensitivity
 *
 * DESCRIPTION: convert iso_mode to an integer value
 *
 * PARAMETERS : iso_mode : the iso_mode supported by sensor
 *
 ** RETURN    : sensitivity supported by sensor
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::getSensorSensitivity(int32_t iso_mode)
{
    int32_t sensitivity;

    switch (iso_mode) {
    case CAM_ISO_MODE_100:
        sensitivity = 100;
        break;
    case CAM_ISO_MODE_200:
        sensitivity = 200;
        break;
    case CAM_ISO_MODE_400:
        sensitivity = 400;
        break;
    case CAM_ISO_MODE_800:
        sensitivity = 800;
        break;
    case CAM_ISO_MODE_1600:
        sensitivity = 1600;
        break;
    default:
        sensitivity = -1;
        break;
    }
    return sensitivity;
}

/*===========================================================================
 * FUNCTION   : getIsoMode
 *
 * DESCRIPTION: round down sensor sensitivity to an integer iso mode value
 *              i.e. 398 is converted to iso mode 3 (200), 802 is converted to iso mode 5 (800)
 *
 * PARAMETERS : sensitivity : the sensitivity supported by sensor
 *
 ** RETURN    : iso mode supported by sensor
 *
 *==========================================================================*/
uint8_t QCamera3HardwareInterface::getIsoMode(int32_t sensitivity)
{
    // error checking, make sure sensitivity in the range [100, 3200]
    if (sensitivity < 0) {
        LOGE("sensitivity < 0");
        return CAM_ISO_MODE_AUTO;
    }
    if (sensitivity < 100) {
        return CAM_ISO_MODE_AUTO;
    }
    if (sensitivity > 3200) {
        sensitivity = 3200;
    }

    // count the position of the highest set bit
    sensitivity /= 100;
    int32_t iso_mode = -1;
    while(sensitivity > 0) {
        iso_mode++;
        sensitivity >>= 1;
    }
    iso_mode += CAM_ISO_MODE_100;

    return iso_mode;
}

/*===========================================================================
 * FUNCTION   : getCamInfo
 *
 * DESCRIPTION: query camera capabilities
 *
 * PARAMETERS :
 *   @cameraId  : camera Id
 *   @info      : camera info struct to be filled in with camera capabilities
 *
 * RETURN     : int type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera3HardwareInterface::getCamInfo(uint32_t cameraId,
        struct camera_info *info)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_GET_CAM_INFO);
    int rc = 0;

    pthread_mutex_lock(&gCamLock);
    if (NULL == gCamCapability[cameraId]) {
        rc = initCapabilities(cameraId);
        if (rc < 0) {
            pthread_mutex_unlock(&gCamLock);
            return rc;
        }
        if (is_dual_camera_by_idx(cameraId)) {
            // Create and initialize FOV-control object
            QCameraFOVControl *pFovControl = QCameraFOVControl::create(
                    gCamCapability[cameraId]->main_cam_cap,
                    gCamCapability[cameraId]->aux_cam_cap, true);

            if (pFovControl) {
                *gCamCapability[cameraId] = pFovControl->consolidateCapabilities(
                        gCamCapability[cameraId]->main_cam_cap,
                        gCamCapability[cameraId]->aux_cam_cap);
                //delete fov control object
                delete pFovControl;
                pFovControl = NULL;
            } else {
                LOGE("FOV-control: Failed to create an object");
                return NO_MEMORY;
            }
        }
    }

    if (NULL == gStaticMetadata[cameraId]) {
        rc = initStaticMetadata(cameraId);
        if (rc < 0) {
            pthread_mutex_unlock(&gCamLock);
            return rc;
        }
    }

    switch(gCamCapability[cameraId]->position) {
    case CAM_POSITION_BACK:
    case CAM_POSITION_BACK_AUX:
        info->facing = CAMERA_FACING_BACK;
        break;

    case CAM_POSITION_FRONT:
    case CAM_POSITION_FRONT_AUX:
        info->facing = CAMERA_FACING_FRONT;
        break;

    default:
        LOGE("Unknown position type %d for camera id:%d",
                gCamCapability[cameraId]->position, cameraId);
        rc = -1;
        break;
    }


    info->orientation = (int)gCamCapability[cameraId]->sensor_mount_angle;
#ifdef USE_HAL_3_5
    info->device_version = CAMERA_DEVICE_API_VERSION_3_5;
#elif USE_HAL_3_3
    info->device_version = CAMERA_DEVICE_API_VERSION_3_4;
#else
    info->device_version = CAMERA_DEVICE_API_VERSION_3_3;
#endif
    info->static_camera_characteristics = gStaticMetadata[cameraId];

    //For now assume both cameras can operate independently.
    info->conflicting_devices = NULL;
    info->conflicting_devices_length = 0;

    //resource cost is 100 * MIN(1.0, m/M),
    //where m is throughput requirement with maximum stream configuration
    //and M is CPP maximum throughput.
    float max_fps = 0.0;
    for (uint32_t i = 0;
            i < gCamCapability[cameraId]->fps_ranges_tbl_cnt; i++) {
        if (max_fps < gCamCapability[cameraId]->fps_ranges_tbl[i].max_fps)
            max_fps = gCamCapability[cameraId]->fps_ranges_tbl[i].max_fps;
    }
    float ratio = 1.0 * MAX_PROCESSED_STREAMS *
            gCamCapability[cameraId]->active_array_size.width *
            gCamCapability[cameraId]->active_array_size.height * max_fps /
            gCamCapability[cameraId]->max_pixel_bandwidth;
    info->resource_cost = 100 * MIN(1.0, ratio);
    LOGI("camera %d resource cost is %d", cameraId,
            info->resource_cost);

    pthread_mutex_unlock(&gCamLock);
    return rc;
}

/*===========================================================================
 * FUNCTION   : translateCapabilityToMetadata
 *
 * DESCRIPTION: translate the capability into camera_metadata_t
 *
 * PARAMETERS : type of the request
 *
 *
 * RETURN     : success: camera_metadata_t*
 *              failure: NULL
 *
 *==========================================================================*/
camera_metadata_t* QCamera3HardwareInterface::translateCapabilityToMetadata(int type)
{
    if (mDefaultMetadata[type] != NULL) {
        return mDefaultMetadata[type];
    }
    //first time we are handling this request
    //fill up the metadata structure using the wrapper class
    CameraMetadata settings;
    //translate from cam_capability_t to camera_metadata_tag_t
    static const uint8_t requestType = ANDROID_REQUEST_TYPE_CAPTURE;
    settings.update(ANDROID_REQUEST_TYPE, &requestType, 1);
    int32_t defaultRequestID = 0;
    settings.update(ANDROID_REQUEST_ID, &defaultRequestID, 1);

    /* OIS disable */
    char ois_prop[PROPERTY_VALUE_MAX];
    memset(ois_prop, 0, sizeof(ois_prop));
    property_get("persist.vendor.camera.ois.disable", ois_prop, "0");
    uint8_t ois_disable = (uint8_t)atoi(ois_prop);

    /* Force video to use OIS */
    char videoOisProp[PROPERTY_VALUE_MAX];
    memset(videoOisProp, 0, sizeof(videoOisProp));
    property_get("persist.vendor.camera.ois.video", videoOisProp, "1");
    uint8_t forceVideoOis = (uint8_t)atoi(videoOisProp);
    uint8_t controlIntent = 0;
    uint8_t focusMode;
    uint8_t vsMode;
    uint8_t optStabMode;
    uint8_t cacMode;
    uint8_t edge_mode;
    uint8_t noise_red_mode;
    uint8_t tonemap_mode;
    uint8_t hotpixelMode = ANDROID_HOT_PIXEL_MODE_FAST;
    uint8_t shadingmode = ANDROID_SHADING_MODE_FAST;
    uint8_t shadingmap_mode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
    bool highQualityModeEntryAvailable = FALSE;
    bool fastModeEntryAvailable = FALSE;
    vsMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    switch (type) {
      case CAMERA3_TEMPLATE_PREVIEW:
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
        focusMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_ON;
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        edge_mode = ANDROID_EDGE_MODE_FAST;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_FAST;
        tonemap_mode = ANDROID_TONEMAP_MODE_FAST;
        break;
      case CAMERA3_TEMPLATE_STILL_CAPTURE:
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
        focusMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_ON;
        edge_mode = ANDROID_EDGE_MODE_HIGH_QUALITY;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY;
        tonemap_mode = ANDROID_TONEMAP_MODE_HIGH_QUALITY;
        hotpixelMode = ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY;
        shadingmode = ANDROID_SHADING_MODE_HIGH_QUALITY;
        if (CAM_SENSOR_RAW == gCamCapability[mCameraId]->sensor_type.sens_type) {
            shadingmap_mode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_ON;
        }
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF;
        // Order of priority for default CAC is HIGH Quality -> FAST -> OFF
        for (size_t i = 0; i < gCamCapability[mCameraId]->aberration_modes_count; i++) {
            if (gCamCapability[mCameraId]->aberration_modes[i] ==
                    CAM_COLOR_CORRECTION_ABERRATION_HIGH_QUALITY) {
                highQualityModeEntryAvailable = TRUE;
            } else if (gCamCapability[mCameraId]->aberration_modes[i] ==
                    CAM_COLOR_CORRECTION_ABERRATION_FAST) {
                fastModeEntryAvailable = TRUE;
            }
        }
        if (highQualityModeEntryAvailable) {
            cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY;
        } else if (fastModeEntryAvailable) {
            cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        }
        break;
      case CAMERA3_TEMPLATE_VIDEO_RECORD:
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
        focusMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        edge_mode = ANDROID_EDGE_MODE_FAST;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_FAST;
        tonemap_mode = ANDROID_TONEMAP_MODE_FAST;
        if (forceVideoOis)
            optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_ON;
        break;
      case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
        focusMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        edge_mode = ANDROID_EDGE_MODE_FAST;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_FAST;
        tonemap_mode = ANDROID_TONEMAP_MODE_FAST;
        if (forceVideoOis)
            optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_ON;
        break;
      case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
        focusMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_ON;
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        edge_mode = ANDROID_EDGE_MODE_ZERO_SHUTTER_LAG;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_ZERO_SHUTTER_LAG;
        tonemap_mode = ANDROID_TONEMAP_MODE_FAST;
        break;
      case CAMERA3_TEMPLATE_MANUAL:
        edge_mode = ANDROID_EDGE_MODE_FAST;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_FAST;
        tonemap_mode = ANDROID_TONEMAP_MODE_FAST;
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
        focusMode = ANDROID_CONTROL_AF_MODE_OFF;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        break;
      default:
        edge_mode = ANDROID_EDGE_MODE_FAST;
        noise_red_mode = ANDROID_NOISE_REDUCTION_MODE_FAST;
        tonemap_mode = ANDROID_TONEMAP_MODE_FAST;
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;
        focusMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        break;
    }
    // Set CAC to OFF if underlying device doesn't support
    if (gCamCapability[mCameraId]->aberration_modes_count == 0) {
        cacMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF;
    }
    settings.update(ANDROID_COLOR_CORRECTION_ABERRATION_MODE, &cacMode, 1);
    settings.update(ANDROID_CONTROL_CAPTURE_INTENT, &controlIntent, 1);
    settings.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &vsMode, 1);
    if (gCamCapability[mCameraId]->supported_focus_modes_cnt == 1) {
        focusMode = ANDROID_CONTROL_AF_MODE_OFF;
    }
    settings.update(ANDROID_CONTROL_AF_MODE, &focusMode, 1);

    if (gCamCapability[mCameraId]->optical_stab_modes_count == 1 &&
            gCamCapability[mCameraId]->optical_stab_modes[0] == CAM_OPT_STAB_ON)
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_ON;
    else if ((gCamCapability[mCameraId]->optical_stab_modes_count == 1 &&
            gCamCapability[mCameraId]->optical_stab_modes[0] == CAM_OPT_STAB_OFF)
            || ois_disable)
        optStabMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    settings.update(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &optStabMode, 1);

    settings.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
            &gCamCapability[mCameraId]->exposure_compensation_default, 1);

    static const uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
    settings.update(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);

    static const uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
    settings.update(ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);

    static const uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    settings.update(ANDROID_CONTROL_AWB_MODE, &awbMode, 1);

    static const uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    settings.update(ANDROID_CONTROL_MODE, &controlMode, 1);

    static const uint8_t effectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
    settings.update(ANDROID_CONTROL_EFFECT_MODE, &effectMode, 1);

    static const uint8_t sceneMode = ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY;
    settings.update(ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);

    static const uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    settings.update(ANDROID_CONTROL_AE_MODE, &aeMode, 1);

    /*flash*/
    static const uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
    settings.update(ANDROID_FLASH_MODE, &flashMode, 1);

    static const uint8_t flashFiringLevel = CAM_FLASH_FIRING_LEVEL_4;
    settings.update(ANDROID_FLASH_FIRING_POWER,
            &flashFiringLevel, 1);

    /* lens */
    float default_aperture = gCamCapability[mCameraId]->apertures[0];
    settings.update(ANDROID_LENS_APERTURE, &default_aperture, 1);

    if (gCamCapability[mCameraId]->filter_densities_count) {
        float default_filter_density = gCamCapability[mCameraId]->filter_densities[0];
        settings.update(ANDROID_LENS_FILTER_DENSITY, &default_filter_density,
                        gCamCapability[mCameraId]->filter_densities_count);
    }

    float default_focal_length = gCamCapability[mCameraId]->focal_length;
    settings.update(ANDROID_LENS_FOCAL_LENGTH, &default_focal_length, 1);

    static const uint8_t demosaicMode = ANDROID_DEMOSAIC_MODE_FAST;
    settings.update(ANDROID_DEMOSAIC_MODE, &demosaicMode, 1);

    settings.update(ANDROID_HOT_PIXEL_MODE, &hotpixelMode, 1);
    settings.update(ANDROID_SHADING_MODE, &shadingmode, 1);

    static const int32_t testpatternMode = ANDROID_SENSOR_TEST_PATTERN_MODE_OFF;
    settings.update(ANDROID_SENSOR_TEST_PATTERN_MODE, &testpatternMode, 1);

    /* face detection (default to OFF) */
    static const uint8_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    settings.update(ANDROID_STATISTICS_FACE_DETECT_MODE, &faceDetectMode, 1);

    static const uint8_t histogramMode = QCAMERA3_HISTOGRAM_MODE_OFF;
    settings.update(QCAMERA3_HISTOGRAM_MODE, &histogramMode, 1);

    static const uint8_t sharpnessMapMode = ANDROID_STATISTICS_SHARPNESS_MAP_MODE_OFF;
    settings.update(ANDROID_STATISTICS_SHARPNESS_MAP_MODE, &sharpnessMapMode, 1);

    static const uint8_t hotPixelMapMode = ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF;
    settings.update(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE, &hotPixelMapMode, 1);

    static const uint8_t lensShadingMode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
    settings.update(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &lensShadingMode, 1);

    static const uint8_t blackLevelLock = ANDROID_BLACK_LEVEL_LOCK_OFF;
    settings.update(ANDROID_BLACK_LEVEL_LOCK, &blackLevelLock, 1);

    /* Exposure time(Update the Min Exposure Time)*/
    int64_t default_exposure_time = gCamCapability[mCameraId]->exposure_time_range[0];
    settings.update(ANDROID_SENSOR_EXPOSURE_TIME, &default_exposure_time, 1);

    /* frame duration */
    static const int64_t default_frame_duration = NSEC_PER_33MSEC;
    settings.update(ANDROID_SENSOR_FRAME_DURATION, &default_frame_duration, 1);

    /* sensitivity */
    static const int32_t default_sensitivity = 100;
    settings.update(ANDROID_SENSOR_SENSITIVITY, &default_sensitivity, 1);
#ifndef USE_HAL_3_3
    static const int32_t default_isp_sensitivity =
            gCamCapability[mCameraId]->isp_sensitivity_range.min_sensitivity;
    settings.update(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST, &default_isp_sensitivity, 1);
#endif

    /*edge mode*/
    settings.update(ANDROID_EDGE_MODE, &edge_mode, 1);

    /*noise reduction mode*/
    settings.update(ANDROID_NOISE_REDUCTION_MODE, &noise_red_mode, 1);

    /*color correction mode*/
    static const uint8_t color_correct_mode = ANDROID_COLOR_CORRECTION_MODE_FAST;
    settings.update(ANDROID_COLOR_CORRECTION_MODE, &color_correct_mode, 1);

    /*transform matrix mode*/
    settings.update(ANDROID_TONEMAP_MODE, &tonemap_mode, 1);

    int32_t scaler_crop_region[4];
    scaler_crop_region[0] = 0;
    scaler_crop_region[1] = 0;
    scaler_crop_region[2] = gCamCapability[mCameraId]->active_array_size.width;
    scaler_crop_region[3] = gCamCapability[mCameraId]->active_array_size.height;
    settings.update(ANDROID_SCALER_CROP_REGION, scaler_crop_region, 4);

    static const uint8_t antibanding_mode = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    settings.update(ANDROID_CONTROL_AE_ANTIBANDING_MODE, &antibanding_mode, 1);

    /*focus distance*/
    float focus_distance = 0.0;
    settings.update(ANDROID_LENS_FOCUS_DISTANCE, &focus_distance, 1);

    /*target fps range: use maximum range for picture, and maximum fixed range for video*/
    /* Restrict template max_fps to 30 */
    float max_range = 0.0;
    float max_fixed_fps = 0.0;
    int32_t fps_range[2] = {0, 0};
    for (uint32_t i = 0; i < gCamCapability[mCameraId]->fps_ranges_tbl_cnt;
            i++) {
        if (gCamCapability[mCameraId]->fps_ranges_tbl[i].max_fps >
                TEMPLATE_MAX_PREVIEW_FPS) {
            continue;
        }
        float range = gCamCapability[mCameraId]->fps_ranges_tbl[i].max_fps -
            gCamCapability[mCameraId]->fps_ranges_tbl[i].min_fps;
        if (type == CAMERA3_TEMPLATE_PREVIEW ||
                type == CAMERA3_TEMPLATE_STILL_CAPTURE ||
                type == CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG) {
            if (range > max_range) {
                fps_range[0] =
                    (int32_t)gCamCapability[mCameraId]->fps_ranges_tbl[i].min_fps;
                fps_range[1] =
                    (int32_t)gCamCapability[mCameraId]->fps_ranges_tbl[i].max_fps;
                max_range = range;
            }
        } else {
            if (range < 0.01 && max_fixed_fps <
                    gCamCapability[mCameraId]->fps_ranges_tbl[i].max_fps) {
                fps_range[0] =
                    (int32_t)gCamCapability[mCameraId]->fps_ranges_tbl[i].min_fps;
                fps_range[1] =
                    (int32_t)gCamCapability[mCameraId]->fps_ranges_tbl[i].max_fps;
                max_fixed_fps = gCamCapability[mCameraId]->fps_ranges_tbl[i].max_fps;
            }
        }
    }
    settings.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fps_range, 2);

    /*precapture trigger*/
    uint8_t precapture_trigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    settings.update(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &precapture_trigger, 1);

    /*af trigger*/
    uint8_t af_trigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    settings.update(ANDROID_CONTROL_AF_TRIGGER, &af_trigger, 1);

    /* ae & af regions */
    int32_t active_region[] = {
            gCamCapability[mCameraId]->active_array_size.left,
            gCamCapability[mCameraId]->active_array_size.top,
            gCamCapability[mCameraId]->active_array_size.left +
                    gCamCapability[mCameraId]->active_array_size.width,
            gCamCapability[mCameraId]->active_array_size.top +
                    gCamCapability[mCameraId]->active_array_size.height,
            0};
    settings.update(ANDROID_CONTROL_AE_REGIONS, active_region,
            sizeof(active_region) / sizeof(active_region[0]));
    settings.update(ANDROID_CONTROL_AF_REGIONS, active_region,
            sizeof(active_region) / sizeof(active_region[0]));

    /* black level lock */
    uint8_t blacklevel_lock = ANDROID_BLACK_LEVEL_LOCK_OFF;
    settings.update(ANDROID_BLACK_LEVEL_LOCK, &blacklevel_lock, 1);

    settings.update(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &shadingmap_mode, 1);

    //special defaults for manual template
    if (type == CAMERA3_TEMPLATE_MANUAL) {
        static const uint8_t manualControlMode = ANDROID_CONTROL_MODE_OFF;
        settings.update(ANDROID_CONTROL_MODE, &manualControlMode, 1);

        static const uint8_t manualFocusMode = ANDROID_CONTROL_AF_MODE_OFF;
        settings.update(ANDROID_CONTROL_AF_MODE, &manualFocusMode, 1);

        static const uint8_t manualAeMode = ANDROID_CONTROL_AE_MODE_OFF;
        settings.update(ANDROID_CONTROL_AE_MODE, &manualAeMode, 1);

        static const uint8_t manualAwbMode = ANDROID_CONTROL_AWB_MODE_OFF;
        settings.update(ANDROID_CONTROL_AWB_MODE, &manualAwbMode, 1);

        static const uint8_t manualTonemapMode = ANDROID_TONEMAP_MODE_FAST;
        settings.update(ANDROID_TONEMAP_MODE, &manualTonemapMode, 1);

        static const uint8_t manualColorCorrectMode = ANDROID_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX;
        settings.update(ANDROID_COLOR_CORRECTION_MODE, &manualColorCorrectMode, 1);
    }


    /* TNR
     * We'll use this location to determine which modes TNR will be set.
     * We will enable TNR to be on if either of the Preview/Video stream requires TNR
     * This is not to be confused with linking on a per stream basis that decision
     * is still on per-session basis and will be handled as part of config stream
     */
    uint8_t tnr_enable = 0;

    if (m_bTnrPreview || m_bTnrVideo) {

        switch (type) {
            case CAMERA3_TEMPLATE_VIDEO_RECORD:
                    tnr_enable = 1;
                    break;

            default:
                    tnr_enable = 0;
                    break;
        }

        int32_t tnr_process_type = (int32_t)getTemporalDenoiseProcessPlate();
        settings.update(QCAMERA3_TEMPORAL_DENOISE_ENABLE, &tnr_enable, 1);
        settings.update(QCAMERA3_TEMPORAL_DENOISE_PROCESS_TYPE, &tnr_process_type, 1);

        LOGD("TNR:%d with process plate %d for template:%d",
                             tnr_enable, tnr_process_type, type);
    }

    //Update Link tags to default
    int32_t sync_type = CAM_TYPE_STANDALONE;
    settings.update(QCAMERA3_DUALCAM_LINK_ENABLE, &sync_type, 1);

    int32_t is_main = 0; //this doesn't matter as app should overwrite
    settings.update(QCAMERA3_DUALCAM_LINK_IS_MAIN, &is_main, 1);

    settings.update(QCAMERA3_DUALCAM_LINK_RELATED_CAMERA_ID, &is_main, 1);

    /* CDS default */
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.CDS", prop, "Off");
    cam_cds_mode_type_t cds_mode = CAM_CDS_MODE_AUTO;
    cds_mode = lookupProp(CDS_MAP, METADATA_MAP_SIZE(CDS_MAP), prop);
    if (CAM_CDS_MODE_MAX == cds_mode) {
        cds_mode = CAM_CDS_MODE_AUTO;
    }

    /* Disabling CDS in templates which have TNR enabled*/
    if (tnr_enable)
        cds_mode = CAM_CDS_MODE_OFF;

    int32_t mode = cds_mode;
    settings.update(QCAMERA3_CDS_MODE, &mode, 1);

    /* Manual Convergence AEC Speed is disabled by default*/
    float default_aec_speed = 0;
    settings.update(QCAMERA3_AEC_CONVERGENCE_SPEED, &default_aec_speed, 1);

    /* Manual Convergence AWB Speed is disabled by default*/
    float default_awb_speed = 0;
    settings.update(QCAMERA3_AWB_CONVERGENCE_SPEED, &default_awb_speed, 1);

    // Set instant AEC to normal convergence by default
    int32_t instant_aec_mode = (int32_t)QCAMERA3_INSTANT_AEC_NORMAL_CONVERGENCE;
    settings.update(QCAMERA3_INSTANT_AEC_MODE, &instant_aec_mode, 1);

    uint8_t enableZSL = 0;
    settings.update(ANDROID_CONTROL_ENABLE_ZSL, &enableZSL, 1);

    mDefaultMetadata[type] = settings.release();

    return mDefaultMetadata[type];
}

/*===========================================================================
 * FUNCTION   : setFrameParameters
 *
 * DESCRIPTION: set parameters per frame as requested in the metadata from
 *              framework
 *
 * PARAMETERS :
 *   @request   : request that needs to be serviced
 *   @streamsArray : Stream ID of all the requested streams
 *   @blob_request: Whether this request is a blob request or not
 *
 * RETURN     : success: NO_ERROR
 *              failure:
 *==========================================================================*/
int QCamera3HardwareInterface::setFrameParameters(
                    const camera_metadata_t *settings,
                    cam_stream_ID_t streamsArray,
                    int blob_request,
                    uint32_t snapshotStreamId,
                    metadata_buffer_t *mParameters,
                    const camera3_capture_request_t *request)
{
    /*translate from camera_metadata_t type to parm_type_t*/
    int rc = 0;
    int32_t hal_version = CAM_HAL_V3;
    uint32_t frame_number = request->frame_number;

    clear_metadata_buffer(mParameters);
    if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_HAL_VERSION, hal_version)) {
        LOGE("Failed to set hal version in the parameters");
        return BAD_VALUE;
    }

    /*we need to update the frame number in the parameters*/
    if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_FRAME_NUMBER,
            frame_number)) {
        LOGE("Failed to set the frame number in the parameters");
        return BAD_VALUE;
    }

    /* Update stream id of all the requested buffers */
    if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_STREAM_ID, streamsArray)) {
        LOGE("Failed to set stream type mask in the parameters");
        return BAD_VALUE;
    }

    if (mUpdateDebugLevel) {
        uint32_t dummyDebugLevel = 0;
        /* The value of dummyDebugLevel is irrelavent. On
         * CAM_INTF_PARM_UPDATE_DEBUG_LEVEL, read debug property */
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_UPDATE_DEBUG_LEVEL,
                dummyDebugLevel)) {
            LOGE("Failed to set UPDATE_DEBUG_LEVEL");
            return BAD_VALUE;
        }
        mUpdateDebugLevel = false;
    }

    if(settings != NULL){
        rc = translateToHalMetadata(settings, mParameters, snapshotStreamId, request);
        if (blob_request)
            memcpy(mPrevParameters, mParameters, sizeof(metadata_buffer_t));
    }

    if (isDualCamera() && !IS_PP_TYPE_NONE) {
        clear_metadata_buffer(mAuxParameters);
        rc = m_pFovControl->translateInputParams(mParameters, mAuxParameters);
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : setReprocParameters
 *
 * DESCRIPTION: Translate frameworks metadata to HAL metadata structure, and
 *              return it.
 *
 * PARAMETERS :
 *   @request   : request that needs to be serviced
 *
 * RETURN     : success: NO_ERROR
 *              failure:
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setReprocParameters(
        camera3_capture_request_t *request, metadata_buffer_t *reprocParam,
        uint32_t snapshotStreamId)
{
    /*translate from camera_metadata_t type to parm_type_t*/
    int rc = 0;

    if (NULL == request->settings){
        LOGE("Reprocess settings cannot be NULL");
        return BAD_VALUE;
    }

    if (NULL == reprocParam) {
        LOGE("Invalid reprocessing metadata buffer");
        return BAD_VALUE;
    }
    clear_metadata_buffer(reprocParam);

    /*we need to update the frame number in the parameters*/
    if (ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_FRAME_NUMBER,
            request->frame_number)) {
        LOGE("Failed to set the frame number in the parameters");
        return BAD_VALUE;
    }

    rc = translateToHalMetadata(request->settings, reprocParam, snapshotStreamId, request);
    if (rc < 0) {
        LOGE("Failed to translate reproc request");
        return rc;
    }

    CameraMetadata frame_settings;
    frame_settings = request->settings;
    if (frame_settings.exists(QCAMERA3_CROP_COUNT_REPROCESS) &&
            frame_settings.exists(QCAMERA3_CROP_REPROCESS)) {
        int32_t *crop_count =
                frame_settings.find(QCAMERA3_CROP_COUNT_REPROCESS).data.i32;
        int32_t *crop_data =
                frame_settings.find(QCAMERA3_CROP_REPROCESS).data.i32;
        int32_t *roi_map =
                frame_settings.find(QCAMERA3_CROP_ROI_MAP_REPROCESS).data.i32;
        if ((0 < *crop_count) && (*crop_count < MAX_NUM_STREAMS)) {
            cam_crop_data_t crop_meta;
            memset(&crop_meta, 0, sizeof(cam_crop_data_t));
            crop_meta.num_of_streams = 1;
            crop_meta.crop_info[0].crop.left   = crop_data[0];
            crop_meta.crop_info[0].crop.top    = crop_data[1];
            crop_meta.crop_info[0].crop.width  = crop_data[2];
            crop_meta.crop_info[0].crop.height = crop_data[3];

            crop_meta.crop_info[0].roi_map.left =
                    roi_map[0];
            crop_meta.crop_info[0].roi_map.top =
                    roi_map[1];
            crop_meta.crop_info[0].roi_map.width =
                    roi_map[2];
            crop_meta.crop_info[0].roi_map.height =
                    roi_map[3];

            if (ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_CROP_DATA, crop_meta)) {
                rc = BAD_VALUE;
            }
            LOGD("Found reprocess crop data for stream %p %dx%d, %dx%d",
                    request->input_buffer->stream,
                    crop_meta.crop_info[0].crop.left,
                    crop_meta.crop_info[0].crop.top,
                    crop_meta.crop_info[0].crop.width,
                    crop_meta.crop_info[0].crop.height);
            LOGD("Found reprocess roi map data for stream %p %dx%d, %dx%d",
                    request->input_buffer->stream,
                    crop_meta.crop_info[0].roi_map.left,
                    crop_meta.crop_info[0].roi_map.top,
                    crop_meta.crop_info[0].roi_map.width,
                    crop_meta.crop_info[0].roi_map.height);
            } else {
                LOGE("Invalid reprocess crop count %d!", *crop_count);
            }
    } else {
        LOGE("No crop data from matching output stream");
    }

    /* These settings are not needed for regular requests so handle them specially for
       reprocess requests; information needed for EXIF tags */
    if (frame_settings.exists(ANDROID_FLASH_MODE)) {
        int val = lookupHalName(FLASH_MODES_MAP, METADATA_MAP_SIZE(FLASH_MODES_MAP),
                    (int)frame_settings.find(ANDROID_FLASH_MODE).data.u8[0]);
        if (NAME_NOT_FOUND != val) {
            uint32_t flashMode = (uint32_t)val;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_FLASH_MODE, flashMode)) {
                rc = BAD_VALUE;
            }
        } else {
            LOGE("Could not map fwk flash mode %d to correct hal flash mode",
                    frame_settings.find(ANDROID_FLASH_MODE).data.u8[0]);
        }
    } else {
        LOGH("No flash mode in reprocess settings");
    }

    if (frame_settings.exists(ANDROID_FLASH_STATE)) {
        int32_t flashState = (int32_t)frame_settings.find(ANDROID_FLASH_STATE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_FLASH_STATE, flashState)) {
            rc = BAD_VALUE;
        }
    } else {
        LOGH("No flash state in reprocess settings");
    }

    if (frame_settings.exists(QCAMERA3_HAL_PRIVATEDATA_REPROCESS_FLAGS)) {
        uint8_t *reprocessFlags =
            frame_settings.find(QCAMERA3_HAL_PRIVATEDATA_REPROCESS_FLAGS).data.u8;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_REPROCESS_FLAGS,
                *reprocessFlags)) {
                rc = BAD_VALUE;
        }
    }

    // Add exif debug data to internal metadata
    if (frame_settings.exists(QCAMERA3_HAL_PRIVATEDATA_EXIF_DEBUG_DATA_BLOB)) {
        mm_jpeg_debug_exif_params_t *debug_params =
                (mm_jpeg_debug_exif_params_t *)frame_settings.find
                (QCAMERA3_HAL_PRIVATEDATA_EXIF_DEBUG_DATA_BLOB).data.u8;
        // AE
        if (debug_params->ae_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_AE,
                    debug_params->ae_debug_params);
        }
        // AWB
        if (debug_params->awb_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_AWB,
                debug_params->awb_debug_params);
        }
        // AF
       if (debug_params->af_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_AF,
                   debug_params->af_debug_params);
        }
        // ASD
        if (debug_params->asd_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_ASD,
                    debug_params->asd_debug_params);
        }
        // Stats
        if (debug_params->stats_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_STATS,
                    debug_params->stats_debug_params);
       }
        // BE Stats
        if (debug_params->bestats_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_BESTATS,
                    debug_params->bestats_debug_params);
        }
        // BHIST
        if (debug_params->bhist_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_BHIST,
                    debug_params->bhist_debug_params);
       }
        // 3A Tuning
        if (debug_params->q3a_tuning_debug_params_valid == TRUE) {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_EXIF_DEBUG_3A_TUNING,
                    debug_params->q3a_tuning_debug_params);
        }
    }

    // Add metadata which reprocess needs
    if (frame_settings.exists(QCAMERA3_HAL_PRIVATEDATA_REPROCESS_DATA_BLOB)) {
        cam_reprocess_info_t *repro_info =
                (cam_reprocess_info_t *)frame_settings.find
                (QCAMERA3_HAL_PRIVATEDATA_REPROCESS_DATA_BLOB).data.u8;
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_SNAP_CROP_INFO_SENSOR,
                repro_info->sensor_crop_info);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_SNAP_CROP_INFO_CAMIF,
                repro_info->camif_crop_info);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_SNAP_CROP_INFO_ISP,
                repro_info->isp_crop_info);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_SNAP_CROP_INFO_CPP,
                repro_info->cpp_crop_info);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_AF_FOCAL_LENGTH_RATIO,
                repro_info->af_focal_length_ratio);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_PARM_FLIP,
                repro_info->pipeline_flip);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_AF_ROI,
                repro_info->af_roi);
        ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_META_IMG_DYN_FEAT,
                repro_info->dyn_mask);
        /* If there is ANDROID_JPEG_ORIENTATION in frame setting,
           CAM_INTF_PARM_ROTATION metadata then has been added in
           translateToHalMetadata. HAL need to keep this new rotation
           metadata. Otherwise, the old rotation info saved in the vendor tag
           would be used */
        IF_META_AVAILABLE(cam_rotation_info_t, rotationInfo,
                CAM_INTF_PARM_ROTATION, reprocParam) {
            LOGD("CAM_INTF_PARM_ROTATION metadata is added in translateToHalMetadata");
        } else {
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_PARM_ROTATION,
                    repro_info->rotation_info);
        }
    }

    /* Add additional JPEG cropping information. App add QCAMERA3_JPEG_ENCODE_CROP_RECT
       to ask for cropping and use ROI for downscale/upscale during HW JPEG encoding.
       roi.width and roi.height would be the final JPEG size.
       For now, HAL only checks this for reprocess request */
    if (frame_settings.exists(QCAMERA3_JPEG_ENCODE_CROP_ENABLE) &&
            frame_settings.exists(QCAMERA3_JPEG_ENCODE_CROP_RECT)) {
        uint8_t *enable =
            frame_settings.find(QCAMERA3_JPEG_ENCODE_CROP_ENABLE).data.u8;
        if (*enable == TRUE) {
            int32_t *crop_data =
                    frame_settings.find(QCAMERA3_JPEG_ENCODE_CROP_RECT).data.i32;
            cam_stream_crop_info_t crop_meta;
            memset(&crop_meta, 0, sizeof(cam_stream_crop_info_t));
            crop_meta.stream_id = 0;
            crop_meta.crop.left   = crop_data[0];
            crop_meta.crop.top    = crop_data[1];
            crop_meta.crop.width  = crop_data[2];
            crop_meta.crop.height = crop_data[3];
            // The JPEG crop roi should match cpp output size
            IF_META_AVAILABLE(cam_stream_crop_info_t, cpp_crop,
                    CAM_INTF_META_SNAP_CROP_INFO_CPP, reprocParam) {
                crop_meta.roi_map.left = 0;
                crop_meta.roi_map.top = 0;
                crop_meta.roi_map.width = cpp_crop->crop.width;
                crop_meta.roi_map.height = cpp_crop->crop.height;
            }
            ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_PARM_JPEG_ENCODE_CROP,
                    crop_meta);
            LOGH("Add JPEG encode crop left %d, top %d, width %d, height %d, mCameraId %d",
                    crop_meta.crop.left, crop_meta.crop.top,
                    crop_meta.crop.width, crop_meta.crop.height, mCameraId);
            LOGH("Add JPEG encode crop ROI left %d, top %d, width %d, height %d, mCameraId %d",
                    crop_meta.roi_map.left, crop_meta.roi_map.top,
                    crop_meta.roi_map.width, crop_meta.roi_map.height, mCameraId);

            // Add JPEG scale information
            cam_dimension_t scale_dim;
            memset(&scale_dim, 0, sizeof(cam_dimension_t));
            if (frame_settings.exists(QCAMERA3_JPEG_ENCODE_CROP_ROI)) {
                int32_t *roi =
                    frame_settings.find(QCAMERA3_JPEG_ENCODE_CROP_ROI).data.i32;
                scale_dim.width = roi[2];
                scale_dim.height = roi[3];
                ADD_SET_PARAM_ENTRY_TO_BATCH(reprocParam, CAM_INTF_PARM_JPEG_SCALE_DIMENSION,
                    scale_dim);
                LOGH("Add JPEG encode scale width %d, height %d, mCameraId %d",
                    scale_dim.width, scale_dim.height, mCameraId);
            }
        }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : saveRequestSettings
 *
 * DESCRIPTION: Add any settings that might have changed to the request settings
 *              and save the settings to be applied on the frame
 *
 * PARAMETERS :
 *   @jpegMetadata : the extracted and/or modified jpeg metadata
 *   @request      : request with initial settings
 *
 * RETURN     :
 * camera_metadata_t* : pointer to the saved request settings
 *==========================================================================*/
camera_metadata_t* QCamera3HardwareInterface::saveRequestSettings(
        const CameraMetadata &jpegMetadata,
        camera3_capture_request_t *request)
{
    camera_metadata_t *resultMetadata;
    CameraMetadata camMetadata;
    camMetadata = request->settings;

    if (jpegMetadata.exists(ANDROID_JPEG_THUMBNAIL_SIZE)) {
        int32_t thumbnail_size[2];
        thumbnail_size[0] = jpegMetadata.find(ANDROID_JPEG_THUMBNAIL_SIZE).data.i32[0];
        thumbnail_size[1] = jpegMetadata.find(ANDROID_JPEG_THUMBNAIL_SIZE).data.i32[1];
        camMetadata.update(ANDROID_JPEG_THUMBNAIL_SIZE, thumbnail_size,
                jpegMetadata.find(ANDROID_JPEG_THUMBNAIL_SIZE).count);
    }

    if (request->input_buffer != NULL) {
        uint8_t reprocessFlags = 1;
        camMetadata.update(QCAMERA3_HAL_PRIVATEDATA_REPROCESS_FLAGS,
                (uint8_t*)&reprocessFlags,
                sizeof(reprocessFlags));
    }

    resultMetadata = camMetadata.release();
    return resultMetadata;
}

/*===========================================================================
 * FUNCTION   : setHalFpsRange
 *
 * DESCRIPTION: set FPS range parameter
 *
 *
 * PARAMETERS :
 *   @settings    : Metadata from framework
 *   @hal_metadata: Metadata buffer
 *
 *
 * RETURN     : success: NO_ERROR
 *              failure:
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setHalFpsRange(const CameraMetadata &settings,
        metadata_buffer_t *hal_metadata)
{
    int32_t rc = NO_ERROR;
    cam_fps_range_t fps_range;
    fps_range.min_fps = (float)
            settings.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE).data.i32[0];
    fps_range.max_fps = (float)
            settings.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE).data.i32[1];
    fps_range.video_min_fps = fps_range.min_fps;
    fps_range.video_max_fps = fps_range.max_fps;
    if (fps_range.min_fps == 0 && fps_range.max_fps == 0) {
         cam_dual_camera_perf_control_t perf_value[1];
         perf_value[0].perf_mode = CAM_PERF_SENSOR_SUSPEND;
         perf_value[0].enable = 1;
         perf_value[0].priority = 0;
         m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_LOW_POWER_MODE;
         memcpy(&m_pDualCamCmdPtr[0]->value, &perf_value[0],
                 sizeof(cam_dual_camera_perf_control_t));
         rc =  mCameraHandle->ops->set_dual_cam_cmd(mCameraHandle->camera_handle);
         if (rc != NO_ERROR) {
             LOGE("LPM not set");
             return rc;
         } else {
             m_bLPMEnabled = true;
         }
    } else if (m_bLPMEnabled && (fps_range.min_fps != 0 || fps_range.max_fps != 0)) {
         cam_dual_camera_perf_control_t perf_value[1];
         perf_value[0].perf_mode = CAM_PERF_NONE;
         perf_value[0].enable = 0;
         perf_value[0].priority = 0;
         m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_LOW_POWER_MODE;
         memcpy(&m_pDualCamCmdPtr[0]->value, &perf_value[0],
                 sizeof(cam_dual_camera_perf_control_t));
         rc =  mCameraHandle->ops->set_dual_cam_cmd(mCameraHandle->camera_handle);
         if (rc != NO_ERROR) {
             LOGE("LPM not reset");
             return rc;
         } else {
             m_bLPMEnabled = false;
         }
    }
    LOGD("aeTargetFpsRange fps: [%f %f]",
            fps_range.min_fps, fps_range.max_fps);
    /* In CONSTRAINED_HFR_MODE, sensor_fps is derived from aeTargetFpsRange as
     * follows:
     * ---------------------------------------------------------------|
     *      Video stream is absent in configure_streams               |
     *    (Camcorder preview before the first video record            |
     * ---------------------------------------------------------------|
     * vid_buf_requested | aeTgtFpsRng | snsrFpsMode | sensorFpsRange |
     *                   |             |             | vid_min/max_fps|
     * ---------------------------------------------------------------|
     *        NO         |  [ 30, 240] |     240     |  [240, 240]    |
     *                   |-------------|-------------|----------------|
     *                   |  [240, 240] |     240     |  [240, 240]    |
     * ---------------------------------------------------------------|
     *     Video stream is present in configure_streams               |
     * ---------------------------------------------------------------|
     * vid_buf_requested | aeTgtFpsRng | snsrFpsMode | sensorFpsRange |
     *                   |             |             | vid_min/max_fps|
     * ---------------------------------------------------------------|
     *        NO         |  [ 30, 240] |     240     |  [240, 240]    |
     * (camcorder prev   |-------------|-------------|----------------|
     *  after video rec  |  [240, 240] |     240     |  [240, 240]    |
     *  is stopped)      |             |             |                |
     * ---------------------------------------------------------------|
     *       YES         |  [ 30, 240] |     240     |  [240, 240]    |
     *                   |-------------|-------------|----------------|
     *                   |  [240, 240] |     240     |  [240, 240]    |
     * ---------------------------------------------------------------|
     * When Video stream is absent in configure_streams,
     * preview fps = sensor_fps / batchsize
     * Eg: for 240fps at batchSize 4, preview = 60fps
     *     for 120fps at batchSize 4, preview = 30fps
     *
     * When video stream is present in configure_streams, preview fps is as per
     * the ratio of preview buffers to video buffers requested in process
     * capture request
     */
    mBatchSize = 0;
    mHFRMode = CAM_HFR_MODE_OFF;
    if (CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE == mOpMode) {
        fps_range.min_fps = fps_range.video_max_fps;
        fps_range.video_min_fps = fps_range.video_max_fps;
        int val = lookupHalName(HFR_MODE_MAP, METADATA_MAP_SIZE(HFR_MODE_MAP),
                fps_range.max_fps);
        if (NAME_NOT_FOUND != val) {
            mHFRMode = (cam_hfr_mode_t)val;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_HFR, mHFRMode)) {
                return BAD_VALUE;
            }

            if (fps_range.max_fps >= MIN_FPS_FOR_BATCH_MODE) {
                /* If batchmode is currently in progress and the fps changes,
                 * set the flag to restart the sensor */
                if((mHFRVideoFps >= MIN_FPS_FOR_BATCH_MODE) &&
                        (mHFRVideoFps != fps_range.max_fps)) {
                    mNeedSensorRestart = true;
                }
                mHFRVideoFps = fps_range.max_fps;
                mBatchSize = mHFRVideoFps / PREVIEW_FPS_FOR_HFR;
                if (mBatchSize > MAX_HFR_BATCH_SIZE) {
                    mBatchSize = MAX_HFR_BATCH_SIZE;
                }
             }
            LOGD("hfrMode: %d batchSize: %d", mHFRMode, mBatchSize);

         }
    } else {
        /* HFR mode is session param in backend/ISP. This should be reset when
         * in non-HFR mode  */
        mHFRMode = CAM_HFR_MODE_OFF;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_HFR, mHFRMode)) {
            return BAD_VALUE;
        }
    }
    if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_FPS_RANGE, fps_range)) {
        return BAD_VALUE;
    }
    LOGD("fps: [%f %f] vid_fps: [%f %f]", fps_range.min_fps,
            fps_range.max_fps, fps_range.video_min_fps, fps_range.video_max_fps);
    return rc;
}

/*===========================================================================
 * FUNCTION   : translateToHalMetadata
 *
 * DESCRIPTION: read from the camera_metadata_t and change to parm_type_t
 *
 *
 * PARAMETERS :
 *   @request  : request sent from framework
 *
 *
 * RETURN     : success: NO_ERROR
 *              failure:
 *==========================================================================*/
int QCamera3HardwareInterface::translateToHalMetadata
                                  (const camera_metadata_t *settings,
                                   metadata_buffer_t *hal_metadata,
                                   uint32_t snapshotStreamId,
                                   const camera3_capture_request_t *request)
{
    int rc = 0;
    CameraMetadata frame_settings;
    frame_settings = settings;

    /* Do not change the order of the following list unless you know what you are
     * doing.
     * The order is laid out in such a way that parameters in the front of the table
     * may be used to override the parameters later in the table. Examples are:
     * 1. META_MODE should precede AEC/AWB/AF MODE
     * 2. AEC MODE should preced EXPOSURE_TIME/SENSITIVITY/FRAME_DURATION
     * 3. AWB_MODE should precede COLOR_CORRECTION_MODE
     * 4. Any mode should precede it's corresponding settings
     */
    if (frame_settings.exists(ANDROID_CONTROL_MODE)) {
        uint8_t metaMode = frame_settings.find(ANDROID_CONTROL_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_MODE, metaMode)) {
            rc = BAD_VALUE;
        }
        rc = extractSceneMode(frame_settings, metaMode, hal_metadata);
        if (rc != NO_ERROR) {
            LOGE("extractSceneMode failed");
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_MODE)) {
        uint8_t fwk_aeMode =
            frame_settings.find(ANDROID_CONTROL_AE_MODE).data.u8[0];
        LOGD("ANDROID_CONTROL_AE_MODE %d", fwk_aeMode);
        uint8_t aeMode;
        int32_t redeye;

        if (fwk_aeMode == ANDROID_CONTROL_AE_MODE_OFF ) {
            aeMode = CAM_AE_MODE_OFF;
        } else {
            aeMode = CAM_AE_MODE_ON;
        }
        if (fwk_aeMode == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE) {
            redeye = 1;
        } else {
            redeye = 0;
        }

        int val = lookupHalName(AE_FLASH_MODE_MAP, METADATA_MAP_SIZE(AE_FLASH_MODE_MAP),
                fwk_aeMode);
        if (NAME_NOT_FOUND != val) {
            int32_t flashMode = (int32_t)val;
            ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_LED_MODE, flashMode);
        }

        ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AEC_MODE, aeMode);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_REDEYE_REDUCTION, redeye)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AWB_MODE)) {
        uint8_t fwk_whiteLevel = frame_settings.find(ANDROID_CONTROL_AWB_MODE).data.u8[0];
        int val = lookupHalName(WHITE_BALANCE_MODES_MAP, METADATA_MAP_SIZE(WHITE_BALANCE_MODES_MAP),
                fwk_whiteLevel);
        if (NAME_NOT_FOUND != val) {
            uint8_t whiteLevel = (uint8_t)val;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_WHITE_BALANCE, whiteLevel)) {
                rc = BAD_VALUE;
            }
        }
    }

    if (frame_settings.exists(ANDROID_COLOR_CORRECTION_ABERRATION_MODE)) {
        uint8_t fwk_cacMode =
                frame_settings.find(
                        ANDROID_COLOR_CORRECTION_ABERRATION_MODE).data.u8[0];
        int val = lookupHalName(COLOR_ABERRATION_MAP, METADATA_MAP_SIZE(COLOR_ABERRATION_MAP),
                fwk_cacMode);
        if (NAME_NOT_FOUND != val) {
            cam_aberration_mode_t cacMode = (cam_aberration_mode_t) val;
            bool entryAvailable = FALSE;
            // Check whether Frameworks set CAC mode is supported in device or not
            for (size_t i = 0; i < gCamCapability[mCameraId]->aberration_modes_count; i++) {
                if (gCamCapability[mCameraId]->aberration_modes[i] == cacMode) {
                    entryAvailable = TRUE;
                    break;
                }
            }
            LOGD("FrameworksCacMode=%d entryAvailable=%d", cacMode, entryAvailable);
            // If entry not found then set the device supported mode instead of frameworks mode i.e,
            // Only HW ISP CAC + NO SW CAC : Advertise all 3 with High doing same as fast by ISP
            // NO HW ISP CAC + Only SW CAC : Advertise all 3 with Fast doing the same as OFF
            if (entryAvailable == FALSE) {
                if (gCamCapability[mCameraId]->aberration_modes_count == 0) {
                    cacMode = CAM_COLOR_CORRECTION_ABERRATION_OFF;
                } else {
                    if (cacMode == CAM_COLOR_CORRECTION_ABERRATION_HIGH_QUALITY) {
                        // High is not supported and so set the FAST as spec say's underlying
                        // device implementation can be the same for both modes.
                        cacMode = CAM_COLOR_CORRECTION_ABERRATION_FAST;
                    } else if (cacMode == CAM_COLOR_CORRECTION_ABERRATION_FAST) {
                        // Fast is not supported and so we cannot set HIGH or FAST but choose OFF
                        // in order to avoid the fps drop due to high quality
                        cacMode = CAM_COLOR_CORRECTION_ABERRATION_OFF;
                    } else {
                        cacMode = CAM_COLOR_CORRECTION_ABERRATION_OFF;
                    }
                }
            }
            LOGD("Final cacMode is %d", cacMode);
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_CAC, cacMode)) {
                rc = BAD_VALUE;
            }
        } else {
            LOGE("Invalid framework CAC mode: %d", fwk_cacMode);
        }
    }

    char af_value[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.af.infinity", af_value, "0");

    uint8_t fwk_focusMode = 0;
    if (atoi(af_value) == 0) {
        if (frame_settings.exists(ANDROID_CONTROL_AF_MODE)) {
            fwk_focusMode = frame_settings.find(ANDROID_CONTROL_AF_MODE).data.u8[0];
            int val = lookupHalName(FOCUS_MODES_MAP, METADATA_MAP_SIZE(FOCUS_MODES_MAP),
                    fwk_focusMode);
            if (NAME_NOT_FOUND != val) {
                uint8_t focusMode = (uint8_t)val;
                LOGD("set focus mode %d", focusMode);
                if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                         CAM_INTF_PARM_FOCUS_MODE, focusMode)) {
                    rc = BAD_VALUE;
                }
            }
        }
    } else {
        uint8_t focusMode = (uint8_t)CAM_FOCUS_MODE_INFINITY;
        LOGE("Focus forced to infinity %d", focusMode);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_FOCUS_MODE, focusMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_LENS_FOCUS_DISTANCE) &&
            fwk_focusMode == ANDROID_CONTROL_AF_MODE_OFF) {
        float focalDistance = frame_settings.find(ANDROID_LENS_FOCUS_DISTANCE).data.f[0];
        LOGD("ANDROID_LENS_FOCUS_DISTANCE %d", focalDistance);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_LENS_FOCUS_DISTANCE,
                focalDistance)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_ANTIBANDING_MODE)) {
        uint8_t fwk_antibandingMode =
                frame_settings.find(ANDROID_CONTROL_AE_ANTIBANDING_MODE).data.u8[0];
        int val = lookupHalName(ANTIBANDING_MODES_MAP,
                METADATA_MAP_SIZE(ANTIBANDING_MODES_MAP), fwk_antibandingMode);
        if (NAME_NOT_FOUND != val) {
            uint32_t hal_antibandingMode = (uint32_t)val;
            if (hal_antibandingMode == CAM_ANTIBANDING_MODE_AUTO) {
                char prop[PROPERTY_VALUE_MAX];
                memset(prop, 0, sizeof(prop));
                //4 : CAM_ANTIBANDING_MODE_AUTO_50HZ , 5 : CAM_ANTIBANDING_MODE_AUTO_60HZ
                property_get("persist.vendor.camera.set.afd", prop, "5");
                hal_antibandingMode = (uint32_t)atoi(prop);
            }
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_ANTIBANDING,
                    hal_antibandingMode)) {
                rc = BAD_VALUE;
            }
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION)) {
        int32_t expCompensation = frame_settings.find(
                ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION).data.i32[0];
        if (expCompensation < gCamCapability[mCameraId]->exposure_compensation_min)
            expCompensation = gCamCapability[mCameraId]->exposure_compensation_min;
        if (expCompensation > gCamCapability[mCameraId]->exposure_compensation_max)
            expCompensation = gCamCapability[mCameraId]->exposure_compensation_max;
        LOGD("Setting compensation:%d", expCompensation);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_EXPOSURE_COMPENSATION,
                expCompensation)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_LOCK)) {
        uint8_t aeLock = frame_settings.find(ANDROID_CONTROL_AE_LOCK).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_AEC_LOCK, aeLock)) {
            rc = BAD_VALUE;
        }
    }
    if (frame_settings.exists(ANDROID_CONTROL_AE_TARGET_FPS_RANGE)) {
        rc = setHalFpsRange(frame_settings, hal_metadata);
        if (rc != NO_ERROR) {
            LOGE("setHalFpsRange failed");
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AWB_LOCK)) {
        uint8_t awbLock = frame_settings.find(ANDROID_CONTROL_AWB_LOCK).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_AWB_LOCK, awbLock)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_EFFECT_MODE)) {
        uint8_t fwk_effectMode = frame_settings.find(ANDROID_CONTROL_EFFECT_MODE).data.u8[0];
        int val = lookupHalName(EFFECT_MODES_MAP, METADATA_MAP_SIZE(EFFECT_MODES_MAP),
                fwk_effectMode);
        if (NAME_NOT_FOUND != val) {
            uint8_t effectMode = (uint8_t)val;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_EFFECT, effectMode)) {
                rc = BAD_VALUE;
            }
        }
    }

    if (frame_settings.exists(ANDROID_COLOR_CORRECTION_MODE)) {
        uint8_t colorCorrectMode = frame_settings.find(ANDROID_COLOR_CORRECTION_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_COLOR_CORRECT_MODE,
                colorCorrectMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_COLOR_CORRECTION_GAINS)) {
        cam_color_correct_gains_t colorCorrectGains;
        for (size_t i = 0; i < CC_GAIN_MAX; i++) {
            colorCorrectGains.gains[i] =
                    frame_settings.find(ANDROID_COLOR_CORRECTION_GAINS).data.f[i];
        }
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_COLOR_CORRECT_GAINS,
                colorCorrectGains)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_COLOR_CORRECTION_TRANSFORM)) {
        cam_color_correct_matrix_t colorCorrectTransform;
        cam_rational_type_t transform_elem;
        size_t num = 0;
        for (size_t i = 0; i < CC_MATRIX_ROWS; i++) {
           for (size_t j = 0; j < CC_MATRIX_COLS; j++) {
              transform_elem.numerator =
                 frame_settings.find(ANDROID_COLOR_CORRECTION_TRANSFORM).data.r[num].numerator;
              transform_elem.denominator =
                 frame_settings.find(ANDROID_COLOR_CORRECTION_TRANSFORM).data.r[num].denominator;
              colorCorrectTransform.transform_matrix[i][j] = transform_elem;
              num++;
           }
        }
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_COLOR_CORRECT_TRANSFORM,
                colorCorrectTransform)) {
            rc = BAD_VALUE;
        }
    }

    cam_trigger_t aecTrigger;
    aecTrigger.trigger = CAM_AEC_TRIGGER_IDLE;
    aecTrigger.trigger_id = -1;
    if (frame_settings.exists(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER)&&
        frame_settings.exists(ANDROID_CONTROL_AE_PRECAPTURE_ID)) {
        aecTrigger.trigger =
            frame_settings.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER).data.u8[0];
        aecTrigger.trigger_id =
            frame_settings.find(ANDROID_CONTROL_AE_PRECAPTURE_ID).data.i32[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AEC_PRECAPTURE_TRIGGER,
                aecTrigger)) {
            rc = BAD_VALUE;
        }
        LOGD("precaptureTrigger: %d precaptureTriggerID: %d",
                aecTrigger.trigger, aecTrigger.trigger_id);
    }

    /*af_trigger must come with a trigger id*/
    if (frame_settings.exists(ANDROID_CONTROL_AF_TRIGGER) &&
        frame_settings.exists(ANDROID_CONTROL_AF_TRIGGER_ID)) {
        cam_trigger_t af_trigger;
        af_trigger.trigger =
            frame_settings.find(ANDROID_CONTROL_AF_TRIGGER).data.u8[0];
        af_trigger.trigger_id =
            frame_settings.find(ANDROID_CONTROL_AF_TRIGGER_ID).data.i32[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AF_TRIGGER, af_trigger)) {
            rc = BAD_VALUE;
        }
        LOGD("AfTrigger: %d AfTriggerID: %d",
                af_trigger.trigger, af_trigger.trigger_id);
    }

    if (frame_settings.exists(ANDROID_DEMOSAIC_MODE)) {
        int32_t demosaic = frame_settings.find(ANDROID_DEMOSAIC_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_DEMOSAIC, demosaic)) {
            rc = BAD_VALUE;
        }
    }
    if (frame_settings.exists(ANDROID_EDGE_MODE)) {
        cam_edge_application_t edge_application;
        edge_application.edge_mode = frame_settings.find(ANDROID_EDGE_MODE).data.u8[0];

        if (edge_application.edge_mode == CAM_EDGE_MODE_OFF) {
            edge_application.sharpness = 0;
        } else {
            edge_application.sharpness =
                    gCamCapability[mCameraId]->sharpness_ctrl.def_value; //default
            if (frame_settings.exists(QCAMERA3_SHARPNESS_STRENGTH)) {
                int32_t sharpness =
                        frame_settings.find(QCAMERA3_SHARPNESS_STRENGTH).data.i32[0];
                if (sharpness >= gCamCapability[mCameraId]->sharpness_ctrl.min_value &&
                    sharpness <= gCamCapability[mCameraId]->sharpness_ctrl.max_value) {
                    LOGD("Setting edge mode sharpness %d", sharpness);
                    edge_application.sharpness = sharpness;
                }
            }
        }
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_EDGE_MODE, edge_application)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_FLASH_MODE)) {
        int32_t respectFlashMode = 1;
        if (frame_settings.exists(ANDROID_CONTROL_AE_MODE)) {
            uint8_t fwk_aeMode =
                frame_settings.find(ANDROID_CONTROL_AE_MODE).data.u8[0];
            if (fwk_aeMode > ANDROID_CONTROL_AE_MODE_ON) {
                respectFlashMode = 0;
                LOGH("AE Mode controls flash, ignore android.flash.mode");
            }
        }
        if (respectFlashMode) {
            int val = lookupHalName(FLASH_MODES_MAP, METADATA_MAP_SIZE(FLASH_MODES_MAP),
                    (int)frame_settings.find(ANDROID_FLASH_MODE).data.u8[0]);
            LOGH("flash mode after mapping %d", val);
            // To check: CAM_INTF_META_FLASH_MODE usage
            if (NAME_NOT_FOUND != val) {
                uint8_t flashMode = (uint8_t)val;
                if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_LED_MODE, flashMode)) {
                    rc = BAD_VALUE;
                }
            }
        }
    }

    if (frame_settings.exists(ANDROID_FLASH_FIRING_POWER)) {
        uint8_t flashPower = frame_settings.find(ANDROID_FLASH_FIRING_POWER).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_FLASH_POWER, flashPower)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_FLASH_FIRING_TIME)) {
        int64_t flashFiringTime = frame_settings.find(ANDROID_FLASH_FIRING_TIME).data.i64[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_FLASH_FIRING_TIME,
                flashFiringTime)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_HOT_PIXEL_MODE)) {
        uint8_t hotPixelMode = frame_settings.find(ANDROID_HOT_PIXEL_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_HOTPIXEL_MODE,
                hotPixelMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_LENS_APERTURE)) {
        float lensAperture = frame_settings.find( ANDROID_LENS_APERTURE).data.f[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_LENS_APERTURE,
                lensAperture)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_LENS_FILTER_DENSITY)) {
        float filterDensity = frame_settings.find(ANDROID_LENS_FILTER_DENSITY).data.f[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_LENS_FILTERDENSITY,
                filterDensity)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_LENS_FOCAL_LENGTH)) {
        float focalLength = frame_settings.find(ANDROID_LENS_FOCAL_LENGTH).data.f[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_LENS_FOCAL_LENGTH,
                focalLength)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_LENS_OPTICAL_STABILIZATION_MODE)) {
        uint8_t optStabMode =
                frame_settings.find(ANDROID_LENS_OPTICAL_STABILIZATION_MODE).data.u8[0];
        cam_ois_mode_t oisMode = OIS_MODE_INACTIVE;
        if (optStabMode) {
            oisMode = OIS_MODE_ACTIVE;
        }
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_LENS_OPT_STAB_MODE,
                oisMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)) {
        uint8_t videoStabMode =
                frame_settings.find(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE).data.u8[0];
        LOGD("videoStabMode from APP = %d", videoStabMode);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_META_VIDEO_STAB_MODE,
                videoStabMode)) {
            rc = BAD_VALUE;
        }
    }


    if (frame_settings.exists(ANDROID_NOISE_REDUCTION_MODE)) {
        uint8_t noiseRedMode = frame_settings.find(ANDROID_NOISE_REDUCTION_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_NOISE_REDUCTION_MODE,
                noiseRedMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_REPROCESS_EFFECTIVE_EXPOSURE_FACTOR)) {
        float reprocessEffectiveExposureFactor =
            frame_settings.find(ANDROID_REPROCESS_EFFECTIVE_EXPOSURE_FACTOR).data.f[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_EFFECTIVE_EXPOSURE_FACTOR,
                reprocessEffectiveExposureFactor)) {
            rc = BAD_VALUE;
        }
    }

    cam_crop_region_t scalerCropRegion;
    bool scalerCropSet = false;
    if (frame_settings.exists(ANDROID_SCALER_CROP_REGION)) {
        scalerCropRegion.left = frame_settings.find(ANDROID_SCALER_CROP_REGION).data.i32[0];
        scalerCropRegion.top = frame_settings.find(ANDROID_SCALER_CROP_REGION).data.i32[1];
        scalerCropRegion.width = frame_settings.find(ANDROID_SCALER_CROP_REGION).data.i32[2];
        scalerCropRegion.height = frame_settings.find(ANDROID_SCALER_CROP_REGION).data.i32[3];

        if (mQuadraCfaStage == QCFA_RAW_OUTPUT || m_bQuadraCfaRequest) {
            LOGI("map coordinate to quadra cfa sensor output");
            int32_t sensorW = gCamCapability[mCameraId]->quadra_cfa_dim[0].width;
            int32_t sensorH = gCamCapability[mCameraId]->quadra_cfa_dim[0].height;
            int32_t activeArrayW = gCamCapability[mCameraId]->active_array_size.width;
            int32_t activeArrayH = gCamCapability[mCameraId]->active_array_size.height;

            scalerCropRegion.left   = scalerCropRegion.left   * sensorW / activeArrayW;
            scalerCropRegion.top    = scalerCropRegion.top    * sensorH / activeArrayH;
            scalerCropRegion.width  = scalerCropRegion.width  * sensorW / activeArrayW;
            scalerCropRegion.height = scalerCropRegion.height * sensorH / activeArrayH;
        } else {
            // Map coordinate system from active array to sensor output.
            mCropRegionMapper.toSensor(scalerCropRegion.left, scalerCropRegion.top,
                scalerCropRegion.width, scalerCropRegion.height);
        }

        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_SCALER_CROP_REGION,
                scalerCropRegion)) {
            rc = BAD_VALUE;
        }
        scalerCropSet = true;
    }

    if (frame_settings.exists(ANDROID_SENSOR_FRAME_DURATION)) {
        int64_t sensorFrameDuration =
                frame_settings.find(ANDROID_SENSOR_FRAME_DURATION).data.i64[0];
        int64_t minFrameDuration = getMinFrameDuration(request);
        sensorFrameDuration = MAX(sensorFrameDuration, minFrameDuration);
        if (sensorFrameDuration > gCamCapability[mCameraId]->max_frame_duration)
            sensorFrameDuration = gCamCapability[mCameraId]->max_frame_duration;
        LOGD("clamp sensorFrameDuration to %lld", sensorFrameDuration);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_SENSOR_FRAME_DURATION,
                sensorFrameDuration)) {
            rc = BAD_VALUE;
        }
    }

#ifndef USE_HAL_3_3
    if (frame_settings.exists(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST)) {
        int32_t ispSensitivity =
            frame_settings.find(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST).data.i32[0];
        if (ispSensitivity <
            gCamCapability[mCameraId]->isp_sensitivity_range.min_sensitivity) {
                ispSensitivity =
                    gCamCapability[mCameraId]->isp_sensitivity_range.min_sensitivity;
                LOGD("clamp ispSensitivity to %d", ispSensitivity);
        }
        if (ispSensitivity >
            gCamCapability[mCameraId]->isp_sensitivity_range.max_sensitivity) {
                ispSensitivity =
                    gCamCapability[mCameraId]->isp_sensitivity_range.max_sensitivity;
                LOGD("clamp ispSensitivity to %d", ispSensitivity);
        }
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_ISP_SENSITIVITY,
                ispSensitivity)) {
            rc = BAD_VALUE;
        }
    }
#endif

    if (frame_settings.exists(ANDROID_SHADING_MODE)) {
        uint8_t shadingMode = frame_settings.find(ANDROID_SHADING_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_SHADING_MODE, shadingMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_STATISTICS_FACE_DETECT_MODE)) {
        uint8_t fwk_facedetectMode =
                frame_settings.find(ANDROID_STATISTICS_FACE_DETECT_MODE).data.u8[0];

        int val = lookupHalName(FACEDETECT_MODES_MAP, METADATA_MAP_SIZE(FACEDETECT_MODES_MAP),
                fwk_facedetectMode);

        if (NAME_NOT_FOUND != val) {
            uint8_t facedetectMode = (uint8_t)val;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_STATS_FACEDETECT_MODE,
                    facedetectMode)) {
                rc = BAD_VALUE;
            }
#ifdef ENABLE_THROTTLE
            if (facedetectMode != mSettingInfo[0].fd) {
                mSettingInfo[0].fd = mSettingInfo[1].fd = facedetectMode;
                int perfLevel = predictFSM(FSM, &mStreamConfigInfo, &mSettingInfo[0], mSessionId);
                if (isDualCamera()) {
                    perfLevel = predictFSM(FSM, &mStreamConfigInfo, &mSettingInfo[1], CONFIG_INDEX_AUX);
                }
                m_thermalAdapter.SetPerfLevel(perfLevel);
            }
#endif
        }
    }

    if (frame_settings.exists(QCAMERA3_HISTOGRAM_MODE)) {
        uint8_t histogramMode =
                frame_settings.find(QCAMERA3_HISTOGRAM_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_STATS_HISTOGRAM_MODE,
                histogramMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_STATISTICS_SHARPNESS_MAP_MODE)) {
        uint8_t sharpnessMapMode =
                frame_settings.find(ANDROID_STATISTICS_SHARPNESS_MAP_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_STATS_SHARPNESS_MAP_MODE,
                sharpnessMapMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_TONEMAP_MODE)) {
        uint8_t tonemapMode =
                frame_settings.find(ANDROID_TONEMAP_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_TONEMAP_MODE, tonemapMode)) {
            rc = BAD_VALUE;
        }
    }
    /* Tonemap curve channels ch0 = G, ch 1 = B, ch 2 = R */
    /*All tonemap channels will have the same number of points*/
    if (frame_settings.exists(ANDROID_TONEMAP_CURVE_GREEN) &&
        frame_settings.exists(ANDROID_TONEMAP_CURVE_BLUE) &&
        frame_settings.exists(ANDROID_TONEMAP_CURVE_RED)) {
        cam_rgb_tonemap_curves tonemapCurves;
        tonemapCurves.tonemap_points_cnt = frame_settings.find(ANDROID_TONEMAP_CURVE_GREEN).count/2;
        if (tonemapCurves.tonemap_points_cnt > CAM_MAX_TONEMAP_CURVE_SIZE) {
            LOGE("Fatal: tonemap_points_cnt %d exceeds max value of %d",
                     tonemapCurves.tonemap_points_cnt,
                    CAM_MAX_TONEMAP_CURVE_SIZE);
            tonemapCurves.tonemap_points_cnt = CAM_MAX_TONEMAP_CURVE_SIZE;
        }

        /* ch0 = G*/
        size_t point = 0;
        cam_tonemap_curve_t tonemapCurveGreen;
        for (size_t i = 0; i < tonemapCurves.tonemap_points_cnt; i++) {
            for (size_t j = 0; j < 2; j++) {
               tonemapCurveGreen.tonemap_points[i][j] =
                  frame_settings.find(ANDROID_TONEMAP_CURVE_GREEN).data.f[point];
               point++;
            }
        }
        tonemapCurves.curves[0] = tonemapCurveGreen;

        /* ch 1 = B */
        point = 0;
        cam_tonemap_curve_t tonemapCurveBlue;
        for (size_t i = 0; i < tonemapCurves.tonemap_points_cnt; i++) {
            for (size_t j = 0; j < 2; j++) {
               tonemapCurveBlue.tonemap_points[i][j] =
                  frame_settings.find(ANDROID_TONEMAP_CURVE_BLUE).data.f[point];
               point++;
            }
        }
        tonemapCurves.curves[1] = tonemapCurveBlue;

        /* ch 2 = R */
        point = 0;
        cam_tonemap_curve_t tonemapCurveRed;
        for (size_t i = 0; i < tonemapCurves.tonemap_points_cnt; i++) {
            for (size_t j = 0; j < 2; j++) {
               tonemapCurveRed.tonemap_points[i][j] =
                  frame_settings.find(ANDROID_TONEMAP_CURVE_RED).data.f[point];
               point++;
            }
        }
        tonemapCurves.curves[2] = tonemapCurveRed;

        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_TONEMAP_CURVES,
                tonemapCurves)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_CAPTURE_INTENT)) {
        uint8_t captureIntent = frame_settings.find(ANDROID_CONTROL_CAPTURE_INTENT).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_CAPTURE_INTENT,
                captureIntent)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_BLACK_LEVEL_LOCK)) {
        uint8_t blackLevelLock = frame_settings.find(ANDROID_BLACK_LEVEL_LOCK).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_BLACK_LEVEL_LOCK,
                blackLevelLock)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE)) {
        uint8_t lensShadingMapMode =
                frame_settings.find(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_LENS_SHADING_MAP_MODE,
                lensShadingMapMode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_REGIONS)) {
        cam_area_t roi;
        bool reset = true;
        convertFromRegions(roi, settings, ANDROID_CONTROL_AE_REGIONS);

        // Map coordinate system from active array to sensor output.
        mCropRegionMapper.toSensor(roi.rect.left, roi.rect.top, roi.rect.width,
                roi.rect.height);

        if (scalerCropSet) {
            reset = resetIfNeededROI(&roi, &scalerCropRegion);
        }
        if (reset && ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AEC_ROI, roi)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AF_REGIONS)) {
        cam_area_t roi;
        bool reset = true;
        convertFromRegions(roi, settings, ANDROID_CONTROL_AF_REGIONS);

        // Map coordinate system from active array to sensor output.
        mCropRegionMapper.toSensor(roi.rect.left, roi.rect.top, roi.rect.width,
                roi.rect.height);

        if (scalerCropSet) {
            reset = resetIfNeededROI(&roi, &scalerCropRegion);
        }
        if (reset && ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AF_ROI, roi)) {
            rc = BAD_VALUE;
        }
    }

    // CDS for non-HFR non-video mode
    if ((mOpMode != CAMERA3_STREAM_CONFIGURATION_CONSTRAINED_HIGH_SPEED_MODE) &&
            !(m_bIsVideo) && frame_settings.exists(QCAMERA3_CDS_MODE)) {
        int32_t *fwk_cds = frame_settings.find(QCAMERA3_CDS_MODE).data.i32;
        if ((CAM_CDS_MODE_MAX <= *fwk_cds) || (0 > *fwk_cds)) {
            LOGE("Invalid CDS mode %d!", *fwk_cds);
        } else {
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                    CAM_INTF_PARM_CDS_MODE, *fwk_cds)) {
                rc = BAD_VALUE;
            }
        }
    }

    // Video HDR
    if (frame_settings.exists(QCAMERA3_VIDEO_HDR_MODE)) {
        cam_video_hdr_mode_t vhdr = (cam_video_hdr_mode_t)
                frame_settings.find(QCAMERA3_VIDEO_HDR_MODE).data.i32[0];
        int8_t curr_hdr_state = ((mCurrFeatureState & CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR) != 0);

        if(vhdr != curr_hdr_state)
           LOGH("PROFILE_SET_HDR_MODE %d" ,vhdr);

        rc = setVideoHdrMode(mParameters, vhdr);
        if (rc != NO_ERROR) {
            LOGE("setVideoHDR is failed");
        }
    }

    //IR
    if(frame_settings.exists(QCAMERA3_IR_MODE)) {
        cam_ir_mode_type_t fwk_ir = (cam_ir_mode_type_t)
                frame_settings.find(QCAMERA3_IR_MODE).data.i32[0];
        uint8_t curr_ir_state = ((mCurrFeatureState & CAM_QCOM_FEATURE_IR) != 0);
        uint8_t isIRon = 0;

        (fwk_ir >0) ? (isIRon = 1) : (isIRon = 0) ;
        if ((CAM_IR_MODE_MAX <= fwk_ir) || (0 > fwk_ir)) {
            LOGE("Invalid IR mode %d!", fwk_ir);
        } else {
            if(isIRon != curr_ir_state )
               LOGH("PROFILE_SET_IR_MODE %d" ,isIRon);

            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                    CAM_INTF_META_IR_MODE, fwk_ir)) {
                rc = BAD_VALUE;
            }
        }
    }

    //Binning Correction Mode
    if(frame_settings.exists(QCAMERA3_BINNING_CORRECTION_MODE)) {
        cam_binning_correction_mode_t fwk_binning_correction = (cam_binning_correction_mode_t)
                frame_settings.find(QCAMERA3_BINNING_CORRECTION_MODE).data.i32[0];
        if ((CAM_BINNING_CORRECTION_MODE_MAX <= fwk_binning_correction)
                || (0 > fwk_binning_correction)) {
            LOGE("Invalid binning correction mode %d!", fwk_binning_correction);
        } else {
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                    CAM_INTF_META_BINNING_CORRECTION_MODE, fwk_binning_correction)) {
                rc = BAD_VALUE;
            }
        }
    }

    if (frame_settings.exists(QCAMERA3_AEC_CONVERGENCE_SPEED)) {
        float aec_speed;
        aec_speed = frame_settings.find(QCAMERA3_AEC_CONVERGENCE_SPEED).data.f[0];
        LOGD("AEC Speed :%f", aec_speed);
        if ( aec_speed < 0 ) {
            LOGE("Invalid AEC mode %f!", aec_speed);
        } else {
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AEC_CONVERGENCE_SPEED,
                    aec_speed)) {
                rc = BAD_VALUE;
            }
        }
    }

    if (frame_settings.exists(QCAMERA3_AWB_CONVERGENCE_SPEED)) {
        float awb_speed;
        awb_speed = frame_settings.find(QCAMERA3_AWB_CONVERGENCE_SPEED).data.f[0];
        LOGD("AWB Speed :%f", awb_speed);
        if ( awb_speed < 0 ) {
            LOGE("Invalid AWB mode %f!", awb_speed);
        } else {
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_AWB_CONVERGENCE_SPEED,
                    awb_speed)) {
                rc = BAD_VALUE;
            }
        }
    }

    // TNR
    if (frame_settings.exists(QCAMERA3_TEMPORAL_DENOISE_ENABLE) &&
        frame_settings.exists(QCAMERA3_TEMPORAL_DENOISE_PROCESS_TYPE)) {
        uint8_t b_TnrRequested = 0;
        uint8_t curr_tnr_state = ((mCurrFeatureState & CAM_QTI_FEATURE_SW_TNR) != 0);
        cam_denoise_param_t tnr;
        tnr.denoise_enable = frame_settings.find(QCAMERA3_TEMPORAL_DENOISE_ENABLE).data.u8[0];
        tnr.process_plates =
            (cam_denoise_process_type_t)frame_settings.find(
            QCAMERA3_TEMPORAL_DENOISE_PROCESS_TYPE).data.i32[0];
        b_TnrRequested = tnr.denoise_enable;

        if(b_TnrRequested != curr_tnr_state)
           LOGH("PROFILE_SET_TNR_MODE %d" ,b_TnrRequested);

        if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_TEMPORAL_DENOISE, tnr)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(QCAMERA3_EXPOSURE_METER)) {
        int32_t* exposure_metering_mode =
                frame_settings.find(QCAMERA3_EXPOSURE_METER).data.i32;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_AEC_ALGO_TYPE,
                *exposure_metering_mode)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_SENSOR_TEST_PATTERN_MODE)) {
        int32_t fwk_testPatternMode =
                frame_settings.find(ANDROID_SENSOR_TEST_PATTERN_MODE).data.i32[0];
        int testPatternMode = lookupHalName(TEST_PATTERN_MAP,
                METADATA_MAP_SIZE(TEST_PATTERN_MAP), fwk_testPatternMode);

        if (NAME_NOT_FOUND != testPatternMode) {
            cam_test_pattern_data_t testPatternData;
            memset(&testPatternData, 0, sizeof(testPatternData));
            testPatternData.mode = (cam_test_pattern_mode_t)testPatternMode;
            if (testPatternMode == CAM_TEST_PATTERN_SOLID_COLOR &&
                    frame_settings.exists(ANDROID_SENSOR_TEST_PATTERN_DATA)) {
                int32_t *fwk_testPatternData =
                        frame_settings.find(ANDROID_SENSOR_TEST_PATTERN_DATA).data.i32;
                testPatternData.r = fwk_testPatternData[0];
                testPatternData.b = fwk_testPatternData[3];
                switch (gCamCapability[mCameraId]->color_arrangement) {
                    case CAM_FILTER_ARRANGEMENT_RGGB:
                    case CAM_FILTER_ARRANGEMENT_GRBG:
                        testPatternData.gr = fwk_testPatternData[1];
                        testPatternData.gb = fwk_testPatternData[2];
                        break;
                    case CAM_FILTER_ARRANGEMENT_GBRG:
                    case CAM_FILTER_ARRANGEMENT_BGGR:
                        testPatternData.gr = fwk_testPatternData[2];
                        testPatternData.gb = fwk_testPatternData[1];
                        break;
                    default:
                        LOGE("color arrangement %d is not supported",
                                gCamCapability[mCameraId]->color_arrangement);
                        break;
                }
            }
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_TEST_PATTERN_DATA,
                    testPatternData)) {
                rc = BAD_VALUE;
            }
        } else {
            LOGE("Invalid framework sensor test pattern mode %d",
                    fwk_testPatternMode);
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_GPS_COORDINATES)) {
        size_t count = 0;
        camera_metadata_entry_t gps_coords = frame_settings.find(ANDROID_JPEG_GPS_COORDINATES);
        ADD_SET_PARAM_ARRAY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_GPS_COORDINATES,
                gps_coords.data.d, gps_coords.count, count);
        if (gps_coords.count != count) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_GPS_PROCESSING_METHOD)) {
        char gps_methods[GPS_PROCESSING_METHOD_SIZE];
        size_t count = 0;
        const char *gps_methods_src = (const char *)
                frame_settings.find(ANDROID_JPEG_GPS_PROCESSING_METHOD).data.u8;
        memset(gps_methods, '\0', sizeof(gps_methods));
        strlcpy(gps_methods, gps_methods_src, sizeof(gps_methods));
        ADD_SET_PARAM_ARRAY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_GPS_PROC_METHODS,
                gps_methods, GPS_PROCESSING_METHOD_SIZE, count);
        if (GPS_PROCESSING_METHOD_SIZE != count) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_GPS_TIMESTAMP)) {
        int64_t gps_timestamp = frame_settings.find(ANDROID_JPEG_GPS_TIMESTAMP).data.i64[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_GPS_TIMESTAMP,
                gps_timestamp)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_ORIENTATION)) {
        int32_t orientation = frame_settings.find(ANDROID_JPEG_ORIENTATION).data.i32[0];
        cam_rotation_info_t rotation_info;
        if (orientation == 0) {
           rotation_info.rotation = ROTATE_0;
        } else if (orientation == 90) {
           rotation_info.rotation = ROTATE_90;
        } else if (orientation == 180) {
           rotation_info.rotation = ROTATE_180;
        } else if (orientation == 270) {
           rotation_info.rotation = ROTATE_270;
        }
        rotation_info.streamId = snapshotStreamId;
        ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_ORIENTATION, orientation);
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_ROTATION, rotation_info)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_QUALITY)) {
        uint32_t quality = (uint32_t) frame_settings.find(ANDROID_JPEG_QUALITY).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_QUALITY, quality)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_THUMBNAIL_QUALITY)) {
        uint32_t thumb_quality = (uint32_t)
                frame_settings.find(ANDROID_JPEG_THUMBNAIL_QUALITY).data.u8[0];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_THUMB_QUALITY,
                thumb_quality)) {
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(ANDROID_JPEG_THUMBNAIL_SIZE)) {
        cam_dimension_t dim;
        dim.width = frame_settings.find(ANDROID_JPEG_THUMBNAIL_SIZE).data.i32[0];
        dim.height = frame_settings.find(ANDROID_JPEG_THUMBNAIL_SIZE).data.i32[1];
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_JPEG_THUMB_SIZE, dim)) {
            rc = BAD_VALUE;
        }
    }

    // Internal metadata
    if (frame_settings.exists(QCAMERA3_PRIVATEDATA_REPROCESS)) {
        size_t count = 0;
        camera_metadata_entry_t privatedata = frame_settings.find(QCAMERA3_PRIVATEDATA_REPROCESS);
        ADD_SET_PARAM_ARRAY_TO_BATCH(hal_metadata, CAM_INTF_META_PRIVATE_DATA,
                privatedata.data.i32, privatedata.count, count);
        if (privatedata.count != count) {
            rc = BAD_VALUE;
        }
    }

    // ISO/Exposure Priority
    if (frame_settings.exists(QCAMERA3_USE_ISO_EXP_PRIORITY) &&
        frame_settings.exists(QCAMERA3_SELECT_PRIORITY)) {
        cam_priority_mode_t mode =
                (cam_priority_mode_t)frame_settings.find(QCAMERA3_SELECT_PRIORITY).data.i32[0];
        if((CAM_ISO_PRIORITY == mode) || (CAM_EXP_PRIORITY == mode)) {
            cam_intf_parm_manual_3a_t use_iso_exp_pty;
            use_iso_exp_pty.previewOnly = FALSE;
            uint64_t* ptr = (uint64_t*)frame_settings.find(QCAMERA3_USE_ISO_EXP_PRIORITY).data.i64;
            use_iso_exp_pty.value = *ptr;

            if(CAM_ISO_PRIORITY == mode) {
                if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_ISO,
                        use_iso_exp_pty)) {
                    rc = BAD_VALUE;
                }
            }
            else {
                if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_EXPOSURE_TIME,
                        use_iso_exp_pty)) {
                    rc = BAD_VALUE;
                }
            }

            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_ZSL_MODE, 1)) {
                    rc = BAD_VALUE;
            }
        }
    } else {
        if (frame_settings.exists(ANDROID_SENSOR_EXPOSURE_TIME)) {
            int64_t sensorExpTime =
                frame_settings.find(ANDROID_SENSOR_EXPOSURE_TIME).data.i64[0];
            LOGD("setting sensorExpTime %lld", sensorExpTime);
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_SENSOR_EXPOSURE_TIME,
                        sensorExpTime)) {
                rc = BAD_VALUE;
            }
        }
        if (frame_settings.exists(ANDROID_SENSOR_SENSITIVITY)) {
            int32_t sensorSensitivity = frame_settings.find(ANDROID_SENSOR_SENSITIVITY).data.i32[0];
            if (sensorSensitivity < gCamCapability[mCameraId]->sensitivity_range.min_sensitivity)
                sensorSensitivity = gCamCapability[mCameraId]->sensitivity_range.min_sensitivity;
            if (sensorSensitivity > gCamCapability[mCameraId]->sensitivity_range.max_sensitivity)
                sensorSensitivity = gCamCapability[mCameraId]->sensitivity_range.max_sensitivity;
            LOGD("clamp sensorSensitivity to %d", sensorSensitivity);
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_META_SENSOR_SENSITIVITY,
                        sensorSensitivity)) {
                rc = BAD_VALUE;
            }
        }

        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_ZSL_MODE, 0)) {
            rc = BAD_VALUE;
        }
    }

    // Saturation
    if (frame_settings.exists(QCAMERA3_USE_SATURATION)) {
        int32_t* use_saturation =
                frame_settings.find(QCAMERA3_USE_SATURATION).data.i32;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_SATURATION, *use_saturation)) {
            rc = BAD_VALUE;
        }
    }

    // EV step
    if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_EV_STEP,
            gCamCapability[mCameraId]->exp_compensation_step)) {
        rc = BAD_VALUE;
    }

    // CDS info
    if (frame_settings.exists(QCAMERA3_CDS_INFO)) {
        cam_cds_data_t *cdsData = (cam_cds_data_t *)
                frame_settings.find(QCAMERA3_CDS_INFO).data.u8;

        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                CAM_INTF_META_CDS_DATA, *cdsData)) {
            rc = BAD_VALUE;
        }
    }

    //bokeh
    if (frame_settings.exists(QCAMERA3_BOKEH_BLURLEVEL)) {
        cam_rtb_blur_info_t info;
        memset(&info, 0, sizeof(info));
        info.blur_level = frame_settings.find(QCAMERA3_BOKEH_BLURLEVEL).data.i32[0];
        LOGD("blur_level in vendor tag %d", info.blur_level);
        info.blur_min_value = MIN_BLUR;
        info.blur_max_value = MAX_BLUR;
        mBlurLevel = info.blur_level;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
                CAM_INTF_PARAM_BOKEH_BLUR_LEVEL, info)) {
            LOGE("Failed to update blur level");
            rc = BAD_VALUE;
        }
    }

    if (frame_settings.exists(QCAMERA3_MANUAL_WB_MODE)) {
        cam_manual_wb_mode_t mode =
                (cam_manual_wb_mode_t)frame_settings.find(QCAMERA3_MANUAL_WB_MODE).data.i32[0];
        if((CAM_MANUAL_WB_CCT == mode) || (CAM_MANUAL_WB_GAINS == mode)) {
            cam_manual_wb_parm_t manual_wb;
            memset(&manual_wb, 0, sizeof(manual_wb));
            if(CAM_MANUAL_WB_CCT == mode) {
                manual_wb.type = CAM_MANUAL_WB_MODE_CCT;
                manual_wb.cct = frame_settings.find(QCAMERA3_MANUAL_WB_CCT).data.i32[0];
            }
            else {
                manual_wb.type = CAM_MANUAL_WB_MODE_GAIN;
                manual_wb.gains.r_gain = frame_settings.find(QCAMERA3_MANUAL_WB_GAINS).data.f[0];
                manual_wb.gains.g_gain = frame_settings.find(QCAMERA3_MANUAL_WB_GAINS).data.f[1];
                manual_wb.gains.b_gain = frame_settings.find(QCAMERA3_MANUAL_WB_GAINS).data.f[2];
            }

            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_WB_MANUAL, manual_wb)) {
                return BAD_VALUE;
            }

            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata, CAM_INTF_PARM_ZSL_MODE, 1)) {
                    rc = BAD_VALUE;
            }
        }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : captureResultCb
 *
 * DESCRIPTION: Callback handler for all channels (streams, as well as metadata)
 *
 * PARAMETERS :
 *   @frame  : frame information from mm-camera-interface
 *   @buffer : actual gralloc buffer to be returned to frameworks. NULL if metadata.
 *   @userdata: userdata
 *
 * RETURN     : NONE
 *==========================================================================*/
void QCamera3HardwareInterface::captureResultCb(mm_camera_super_buf_t *metadata,
                camera3_stream_buffer_t *buffer,
                uint32_t frame_number, bool isInputBuffer, void *userdata)
{
    QCamera3HardwareInterface *hw = (QCamera3HardwareInterface *)userdata;
    if (hw == NULL) {
        LOGE("Invalid hw %p", hw);
        return;
    }

    hw->captureResultCb(metadata, buffer, frame_number, isInputBuffer);
    return;
}

void QCamera3HardwareInterface::internalMetaCb(mm_camera_super_buf_t *metadata,
                __unused camera3_stream_buffer_t *buffer,
                __unused uint32_t frame_number, __unused bool isInputBuffer, void *userdata)
{
    QCamera3HardwareInterface *hw = (QCamera3HardwareInterface *)userdata;
    if (hw == NULL) {
        LOGE("Invalid hw %p", hw);
        return;
    }

    hw->internalMetaCb(metadata);
    return;
}


/*===========================================================================
 * FUNCTION   : setBufferErrorStatus
 *
 * DESCRIPTION: Callback handler for channels to report any buffer errors
 *
 * PARAMETERS :
 *   @ch     : Channel on which buffer error is reported from
 *   @frame_number  : frame number on which buffer error is reported on
 *   @buffer_status : buffer error status
 *   @userdata: userdata
 *
 * RETURN     : NONE
 *==========================================================================*/
void QCamera3HardwareInterface::setBufferErrorStatus(QCamera3Channel* ch,
        uint32_t frame_number, camera3_buffer_status_t err, void *userdata)
{
    QCamera3HardwareInterface *hw = (QCamera3HardwareInterface *)userdata;
    if (hw == NULL) {
        LOGE("Invalid hw %p", hw);
        return;
    }

    hw->setBufferErrorStatus(ch, frame_number, err);
    return;
}

void QCamera3HardwareInterface::setBufferErrorStatus(QCamera3Channel* ch,
        uint32_t frameNumber, camera3_buffer_status_t err)
{
    pthread_mutex_lock(&mMutex);

    for (auto& req : mPendingBuffersMap.mPendingBuffersInRequest) {
        if (req.frame_number != frameNumber)
            continue;
        for (auto& k : req.mPendingBufferList) {
            if(k.stream->priv == ch) {
                LOGI("channel: %p, frame# %d, buf err: %d", ch, frameNumber, err);
                k.bufStatus = CAMERA3_BUFFER_STATUS_ERROR;
            }
        }
    }

    pthread_mutex_unlock(&mMutex);
    return;
}
/*===========================================================================
 * FUNCTION   : initialize
 *
 * DESCRIPTION: Pass framework callback pointers to HAL
 *
 * PARAMETERS :
 *
 *
 * RETURN     : Success : 0
 *              Failure: -ENODEV
 *==========================================================================*/

int QCamera3HardwareInterface::initialize(const struct camera3_device *device,
                                  const camera3_callback_ops_t *callback_ops)
{
    LOGD("E");
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(device->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return -ENODEV;
    }

    int rc = hw->initialize(callback_ops);
    LOGD("X");
    return rc;
}

/*===========================================================================
 * FUNCTION   : configure_streams
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *
 *
 * RETURN     : Success: 0
 *              Failure: -EINVAL (if stream configuration is invalid)
 *                       -ENODEV (fatal error)
 *==========================================================================*/

int QCamera3HardwareInterface::configure_streams(
        const struct camera3_device *device,
        camera3_stream_configuration_t *stream_list)
{
    LOGD("E");
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(device->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return -ENODEV;
    }
    int rc = hw->configureStreams(stream_list);
    LOGD("X");
    return rc;
}

/*===========================================================================
 * FUNCTION   : construct_default_request_settings
 *
 * DESCRIPTION: Configure a settings buffer to meet the required use case
 *
 * PARAMETERS :
 *
 *
 * RETURN     : Success: Return valid metadata
 *              Failure: Return NULL
 *==========================================================================*/
const camera_metadata_t* QCamera3HardwareInterface::
    construct_default_request_settings(const struct camera3_device *device,
                                        int type)
{

    LOGD("E");
    camera_metadata_t* fwk_metadata = NULL;
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(device->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return NULL;
    }

    fwk_metadata = hw->translateCapabilityToMetadata(type);

    LOGD("X");
    return fwk_metadata;
}

/*===========================================================================
 * FUNCTION   : process_capture_request
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *
 *
 * RETURN     :
 *==========================================================================*/
int QCamera3HardwareInterface::process_capture_request(
                    const struct camera3_device *device,
                    camera3_capture_request_t *request)
{
    LOGD("E");
    CAMSCOPE_UPDATE_FLAGS(CAMSCOPE_SECTION_HAL, kpi_camscope_flags);
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(device->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return -EINVAL;
    }

    int rc = hw->orchestrateRequest(request);
    LOGD("X");
    return rc;
}

/*===========================================================================
 * FUNCTION   : dump
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *
 *
 * RETURN     :
 *==========================================================================*/

void QCamera3HardwareInterface::dump(
                const struct camera3_device *device, int fd)
{
    /* Log level property is read when "adb shell dumpsys media.camera" is
       called so that the log level can be controlled without restarting
       the media server */
    getLogLevel();

    LOGD("E");
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(device->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return;
    }

    hw->dump(fd);
    LOGD("X");
    return;
}

/*===========================================================================
 * FUNCTION   : flush
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *
 *
 * RETURN     :
 *==========================================================================*/

int QCamera3HardwareInterface::flush(
                const struct camera3_device *device)
{
    int rc;
    LOGD("E");
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(device->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return -EINVAL;
    }

    pthread_mutex_lock(&hw->mMutex);
    // Validate current state
    switch (hw->mState) {
        case STARTED:
            /* valid state */
            break;

        case ERROR:
            pthread_mutex_unlock(&hw->mMutex);
            hw->handleCameraDeviceError();
            return -ENODEV;

        default:
            LOGI("Flush returned during state %d", hw->mState);
            pthread_mutex_unlock(&hw->mMutex);
            return 0;
    }
    pthread_mutex_unlock(&hw->mMutex);

    rc = hw->flush(true /* restart channels */ );
    LOGD("X");
    return rc;
}

/*===========================================================================
 * FUNCTION   : close_camera_device
 *
 * DESCRIPTION:
 *
 * PARAMETERS :
 *
 *
 * RETURN     :
 *==========================================================================*/
int QCamera3HardwareInterface::close_camera_device(struct hw_device_t* device)
{
    int ret = NO_ERROR;
    char prop[PROPERTY_VALUE_MAX];
    int enable_fdleak=0;
    int enable_memleak=0;
    QCamera3HardwareInterface *hw =
        reinterpret_cast<QCamera3HardwareInterface *>(
            reinterpret_cast<camera3_device_t *>(device)->priv);
    if (!hw) {
        LOGE("NULL camera device");
        return BAD_VALUE;
    }

    LOGI("[KPI Perf]: E camera id %d", hw->mCameraId);
    delete hw;
#ifdef FDLEAK_FLAG
    property_get("persist.vendor.camera.fdleak.enable", prop, "0");
    enable_fdleak = atoi(prop);
    if (enable_fdleak) {
       LOGI("fdleak tool dump list");
    hal_debug_dump_fdleak_trace();
    }
#endif
#ifdef MEMLEAK_FLAG
    property_get("persist.vendor.camera.memleak.enable", prop, "0");
    enable_memleak = atoi(prop);
    if (enable_memleak) {
       LOGI("memleak tool dump list");
    hal_debug_dump_memleak_trace();
    }
#endif
    LOGI("[KPI Perf]: X");
    CAMSCOPE_DESTROY(CAMSCOPE_SECTION_HAL);
    return ret;
}

/*===========================================================================
 * FUNCTION   : getWaveletDenoiseProcessPlate
 *
 * DESCRIPTION: query wavelet denoise process plate
 *
 * PARAMETERS : None
 *
 * RETURN     : WNR prcocess plate value
 *==========================================================================*/
cam_denoise_process_type_t QCamera3HardwareInterface::getWaveletDenoiseProcessPlate()
{
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.denoise.process.plates", prop, "0");
    int processPlate = atoi(prop);
    switch(processPlate) {
    case 0:
        return CAM_WAVELET_DENOISE_YCBCR_PLANE;
    case 1:
        return CAM_WAVELET_DENOISE_CBCR_ONLY;
    case 2:
        return CAM_WAVELET_DENOISE_STREAMLINE_YCBCR;
    case 3:
        return CAM_WAVELET_DENOISE_STREAMLINED_CBCR;
    default:
        return CAM_WAVELET_DENOISE_STREAMLINE_YCBCR;
    }
}


/*===========================================================================
 * FUNCTION   : getTemporalDenoiseProcessPlate
 *
 * DESCRIPTION: query temporal denoise process plate
 *
 * PARAMETERS : None
 *
 * RETURN     : TNR prcocess plate value
 *==========================================================================*/
cam_denoise_process_type_t QCamera3HardwareInterface::getTemporalDenoiseProcessPlate()
{
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.tnr.process.plates", prop, "0");
    int processPlate = atoi(prop);
    switch(processPlate) {
    case 0:
        return CAM_WAVELET_DENOISE_YCBCR_PLANE;
    case 1:
        return CAM_WAVELET_DENOISE_CBCR_ONLY;
    case 2:
        return CAM_WAVELET_DENOISE_STREAMLINE_YCBCR;
    case 3:
        return CAM_WAVELET_DENOISE_STREAMLINED_CBCR;
    default:
        return CAM_WAVELET_DENOISE_STREAMLINE_YCBCR;
    }
}


/*===========================================================================
 * FUNCTION   : extractSceneMode
 *
 * DESCRIPTION: Extract scene mode from frameworks set metadata
 *
 * PARAMETERS :
 *      @frame_settings: CameraMetadata reference
 *      @metaMode: ANDROID_CONTORL_MODE
 *      @hal_metadata: hal metadata structure
 *
 * RETURN     : None
 *==========================================================================*/
int32_t QCamera3HardwareInterface::extractSceneMode(
        const CameraMetadata &frame_settings, uint8_t metaMode,
        metadata_buffer_t *hal_metadata)
{
    int32_t rc = NO_ERROR;
    uint8_t sceneMode = CAM_SCENE_MODE_OFF;

    if (ANDROID_CONTROL_MODE_OFF_KEEP_STATE == metaMode) {
        LOGD("Ignoring control mode OFF_KEEP_STATE");
        return NO_ERROR;
    }

    if (metaMode == ANDROID_CONTROL_MODE_USE_SCENE_MODE) {
        camera_metadata_ro_entry entry =
                frame_settings.find(ANDROID_CONTROL_SCENE_MODE);
        if (0 == entry.count)
            return rc;

        mCurrentSceneMode = entry.data.u8[0];

        int val = lookupHalName(SCENE_MODES_MAP,
                sizeof(SCENE_MODES_MAP)/sizeof(SCENE_MODES_MAP[0]),
                mCurrentSceneMode);
        if (NAME_NOT_FOUND != val) {
            sceneMode = (uint8_t)val;
            LOGD("sceneMode: %d", sceneMode);
        }
    }

    if ((sceneMode == CAM_SCENE_MODE_HDR) || m_bSensorHDREnabled) {
        rc = setSensorHDR(hal_metadata, (sceneMode == CAM_SCENE_MODE_HDR));
    }

    if ((rc == NO_ERROR) && !m_bSensorHDREnabled) {
        uint8_t bestshot = sceneMode;
        if (sceneMode == CAM_SCENE_MODE_HDR) {
            cam_hdr_param_t hdr_params;
            hdr_params.hdr_enable = 1;
            hdr_params.hdr_mode = CAM_HDR_MODE_MULTIFRAME;
            hdr_params.hdr_need_1x = false;
            if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                    CAM_INTF_PARM_HAL_BRACKETING_HDR, hdr_params)) {
                rc = BAD_VALUE;
            }
            bestshot = 0;
        }

        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                CAM_INTF_PARM_BESTSHOT_MODE, bestshot)) {
            rc = BAD_VALUE;
        }
    }

    if (mForceHdrSnapshot) {
        cam_hdr_param_t hdr_params;
        hdr_params.hdr_enable = 1;
        hdr_params.hdr_mode = CAM_HDR_MODE_MULTIFRAME;
        hdr_params.hdr_need_1x = false;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                CAM_INTF_PARM_HAL_BRACKETING_HDR, hdr_params)) {
            rc = BAD_VALUE;
        }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : setVideoHdrMode
 *
 * DESCRIPTION: Set Video HDR mode from frameworks set metadata
 *
 * PARAMETERS :
 *      @hal_metadata: hal metadata structure
 *      @metaMode: QCAMERA3_VIDEO_HDR_MODE
 *
 * RETURN     : None
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setVideoHdrMode(
        metadata_buffer_t *hal_metadata, cam_video_hdr_mode_t vhdr)
{
    if ( (vhdr >= CAM_VIDEO_HDR_MODE_OFF) && (vhdr < CAM_VIDEO_HDR_MODE_MAX)) {
        return setSensorHDR(hal_metadata, (vhdr == CAM_VIDEO_HDR_MODE_ON), true);
    }

    LOGE("Invalid Video HDR mode %d!", vhdr);
    return BAD_VALUE;
}

/*===========================================================================
 * FUNCTION   : setSensorHDR
 *
 * DESCRIPTION: Enable/disable sensor HDR.
 *
 * PARAMETERS :
 *      @hal_metadata: hal metadata structure
 *      @enable: boolean whether to enable/disable sensor HDR
 *
 * RETURN     : None
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setSensorHDR(
        metadata_buffer_t *hal_metadata, bool enable, bool isVideoHdrEnable)
{
    int32_t rc = NO_ERROR;
    cam_sensor_hdr_type_t sensor_hdr = CAM_SENSOR_HDR_OFF;

    if (enable) {
        char sensor_hdr_prop[PROPERTY_VALUE_MAX];
        memset(sensor_hdr_prop, 0, sizeof(sensor_hdr_prop));
        #ifdef _LE_CAMERA_
        //Default to staggered HDR for IOT
        property_get("persist.vendor.camera.sensor.hdr", sensor_hdr_prop, "3");
        #else
        property_get("persist.vendor.camera.sensor.hdr", sensor_hdr_prop, "0");
        #endif
        sensor_hdr = (cam_sensor_hdr_type_t) atoi(sensor_hdr_prop);
    }

    bool isSupported = false;
    switch (sensor_hdr) {
        case CAM_SENSOR_HDR_IN_SENSOR:
            if (gCamCapability[mCameraId]->qcom_supported_feature_mask &
                    CAM_QCOM_FEATURE_SENSOR_HDR) {
                isSupported = true;
                LOGD("Setting HDR mode In Sensor");
            }
            break;
        case CAM_SENSOR_HDR_ZIGZAG:
            if (gCamCapability[mCameraId]->qcom_supported_feature_mask &
                    CAM_QCOM_FEATURE_ZIGZAG_HDR) {
                isSupported = true;
                LOGD("Setting HDR mode Zigzag");
            }
            break;
        case CAM_SENSOR_HDR_STAGGERED:
            if (gCamCapability[mCameraId]->qcom_supported_feature_mask &
                    CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR) {
                isSupported = true;
                LOGD("Setting HDR mode Staggered");
            }
            break;
        case CAM_SENSOR_HDR_OFF:
            isSupported = true;
            LOGD("Turning off sensor HDR");
            break;
        default:
            LOGE("HDR mode %d not supported", sensor_hdr);
            rc = BAD_VALUE;
            break;
    }

    if(isSupported && mShouldSetSensorHdr) {
        LOGD("send sensor HDR setting %d", sensor_hdr);
        mShouldSetSensorHdr = false;
        if (ADD_SET_PARAM_ENTRY_TO_BATCH(hal_metadata,
                CAM_INTF_PARM_SENSOR_HDR, sensor_hdr)) {
            rc = BAD_VALUE;
        } else {
            if(!isVideoHdrEnable)
                m_bSensorHDREnabled = (sensor_hdr != CAM_SENSOR_HDR_OFF);
        }
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : needRotationReprocess
 *
 * DESCRIPTION: if rotation needs to be done by reprocess in pp
 *
 * PARAMETERS : none
 *
 * RETURN     : true: needed
 *              false: no need
 *==========================================================================*/
bool QCamera3HardwareInterface::needRotationReprocess()
{
    if ((gCamCapability[mCameraId]->qcom_supported_feature_mask & CAM_QCOM_FEATURE_ROTATION) > 0) {
        // current rotation is not zero, and pp has the capability to process rotation
        LOGH("need do reprocess for rotation");
        return true;
    }

    return false;
}

/*===========================================================================
 * FUNCTION   : needReprocess
 *
 * DESCRIPTION: if reprocess in needed
 *
 * PARAMETERS : none
 *
 * RETURN     : true: needed
 *              false: no need
 *==========================================================================*/
bool QCamera3HardwareInterface::needReprocess(cam_feature_mask_t postprocess_mask)
{
    if (gCamCapability[mCameraId]->qcom_supported_feature_mask > 0) {
        // TODO: add for ZSL HDR later
        // pp module has min requirement for zsl reprocess, or WNR in ZSL mode
        if(postprocess_mask == CAM_QCOM_FEATURE_NONE){
            LOGH("need do reprocess for ZSL WNR or min PP reprocess");
            return true;
        } else {
            LOGH("already post processed frame");
            return false;
        }
    }
    return needRotationReprocess();
}

/*===========================================================================
 * FUNCTION   : needJpegExifRotation
 *
 * DESCRIPTION: if rotation from jpeg is needed
 *
 * PARAMETERS : none
 *
 * RETURN     : true: needed
 *              false: no need
 *==========================================================================*/
bool QCamera3HardwareInterface::needJpegExifRotation()
{
    /*If the pp does not have the ability to do rotation, enable jpeg rotation*/
    if (!(gCamCapability[mCameraId]->qcom_supported_feature_mask & CAM_QCOM_FEATURE_ROTATION)) {
       LOGD("Need use Jpeg EXIF Rotation");
       return true;
    }
    return false;
}

/*===========================================================================
 * FUNCTION   : useExifRotation
 *
 * DESCRIPTION: Check if jpeg exif rotation need to be used
 *
 * PARAMETERS : none
 *
 * RETURN     : true: if jpeg exif rotation need to be used
 *                    false: no need
 *==========================================================================*/
bool QCamera3HardwareInterface::useExifRotation() {
    char exifRotation[PROPERTY_VALUE_MAX];

    property_get("persist.vendor.camera.exif.rotation", exifRotation, "off");

    if (!strcmp(exifRotation, "on")) {
        return true;
    }

    property_get("persist.vendor.camera.lib2d.rotation", exifRotation, "off");
    if (!strcmp(exifRotation, "on")) {
        return false;
    }

    return true;
}

/*===========================================================================
 * FUNCTION   : addOfflineReprocChannel
 *
 * DESCRIPTION: add a reprocess channel that will do reprocess on frames
 *              coming from input channel
 *
 * PARAMETERS :
 *   @config  : reprocess configuration
 *   @inputChHandle : pointer to the input (source) channel
 *   @pp_channel_idx : current reproc chennale idx
 *
 *
 * RETURN     : Ptr to the newly created channel obj. NULL if failed.
 *==========================================================================*/
QCamera3ReprocessChannel *QCamera3HardwareInterface::addOfflineReprocChannel(
        const reprocess_config_t &config,
        QCamera3ProcessingChannel *inputChHandle,
        int8_t pp_channel_idx)
{
    int32_t rc = NO_ERROR;
    QCamera3ReprocessChannel *pChannel = NULL;

    LOGD("cur pp idx:%d, total pp cahnnel cnt:%d", pp_channel_idx, getReprocChannelCnt());
    uint32_t camera_handle = mCameraHandle->camera_handle;
    uint32_t channel_handle = mChannelHandle;

    if (isDualCamera()) {
        if (get_main_camera_handle(mChannelHandle) == inputChHandle->getMyHandle()) {
            camera_handle = get_main_camera_handle(mCameraHandle->camera_handle);
            channel_handle = get_main_camera_handle(mChannelHandle);
        } else {
            camera_handle = get_aux_camera_handle(mCameraHandle->camera_handle);
            channel_handle = get_aux_camera_handle(mChannelHandle);
        }
    }

    pChannel = new QCamera3ReprocessChannel(camera_handle,
            channel_handle, mCameraHandle->ops, captureResultCb, setBufferErrorStatus,
            config.padding, CAM_QCOM_FEATURE_NONE, this, inputChHandle);
    if (NULL == pChannel) {
        LOGE("no mem for reprocess channel");
        return NULL;
    }

    rc = pChannel->initialize(IS_TYPE_NONE);
    if (rc != NO_ERROR) {
        LOGE("init reprocess channel failed, ret = %d", rc);
        delete pChannel;
        return NULL;
    }

    if (pp_channel_idx >= 1) {
        pChannel->setReprocIndex(pp_channel_idx);
    }

    // pp feature config
    cam_pp_feature_config_t pp_config;
    memset(&pp_config, 0, sizeof(cam_pp_feature_config_t));

    pp_config.feature_mask |= CAM_QCOM_FEATURE_PP_SUPERSET_HAL3;
    if (gCamCapability[mCameraId]->qcom_supported_feature_mask
            & CAM_QCOM_FEATURE_DSDN) {
        //Use CPP CDS incase h/w supports it.
        pp_config.feature_mask &= ~CAM_QCOM_FEATURE_CDS;
        pp_config.feature_mask |= CAM_QCOM_FEATURE_DSDN;
    }
    if (!(gCamCapability[mCameraId]->qcom_supported_feature_mask & CAM_QCOM_FEATURE_ROTATION)) {
        pp_config.feature_mask &= ~CAM_QCOM_FEATURE_ROTATION;
    }

    if (config.hdr_param.hdr_enable) {
        pp_config.feature_mask |= CAM_QCOM_FEATURE_HDR;
        pp_config.hdr_param = config.hdr_param;
    }

    if (mForceHdrSnapshot) {
        pp_config.feature_mask |= CAM_QCOM_FEATURE_HDR;
        pp_config.hdr_param.hdr_enable = 1;
        pp_config.hdr_param.hdr_need_1x = 0;
        pp_config.hdr_param.hdr_mode = CAM_HDR_MODE_MULTIFRAME;
    }

    if (mbIsMultiFrameCapture) {
        char prop[PROPERTY_VALUE_MAX];
        property_get("persist.vendor.camera.multiframe.capture.precpp", prop, "1");
        bool bMultiFrameCapture_precpp = atoi(prop) ? TRUE : FALSE;
        if(bMultiFrameCapture_precpp) {
            pp_config.feature_mask |= CAM_QTI_FEATURE_MFPROC_PRECPP;
        } else {
            pp_config.feature_mask |= CAM_QTI_FEATURE_MFPROC_POSTCPP;
        }
        pp_config.burst_cnt = mMultiFrameCaptureCount;
    }

    if (m_bOfflineIsp) {
        LOGH("offline isp flag is set, add feature mask CAM_QCOM_FEATURE_RAW_PROCESSING");
        pp_config.feature_mask |= CAM_QCOM_FEATURE_RAW_PROCESSING;
    }

    if (m_bQuadraCfaRequest && getReprocChannelCnt() == 2 && pp_channel_idx == 0) {
        LOGD("reset feature mask to quadra cfa for remosaic reprocess");
        pp_config.feature_mask = CAM_QCOM_FEATURE_QUADRA_CFA;
    }

    LOGD("feature mask:%llx", pp_config.feature_mask);
    rc = pChannel->addReprocStreamsFromSource(pp_config,
            config,
            IS_TYPE_NONE,
            mMetadataChannel);

    if (rc != NO_ERROR) {
        delete pChannel;
        return NULL;
    }
    return pChannel;
}

/*===========================================================================
 * FUNCTION   : getMobicatMask
 *
 * DESCRIPTION: returns mobicat mask
 *
 * PARAMETERS : none
 *
 * RETURN     : mobicat mask
 *
 *==========================================================================*/
uint8_t QCamera3HardwareInterface::getMobicatMask()
{
    return m_MobicatMask;
}

/*===========================================================================
 * FUNCTION   : setMobicat
 *
 * DESCRIPTION: set Mobicat on/off.
 *
 * PARAMETERS :
 *   @params  : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setMobicat()
{
    char value [PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.mobicat", value, "0");
    int32_t ret = NO_ERROR;
    uint8_t enableMobi = (uint8_t)atoi(value);

    if (enableMobi) {
        tune_cmd_t tune_cmd;
        tune_cmd.type = SET_RELOAD_CHROMATIX;
        tune_cmd.module = MODULE_ALL;
        tune_cmd.value = TRUE;
        ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
                CAM_INTF_PARM_SET_VFE_COMMAND,
                tune_cmd);

        ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
                CAM_INTF_PARM_SET_PP_COMMAND,
                tune_cmd);
    }
    m_MobicatMask = enableMobi;

    return ret;
}

/*===========================================================================
* FUNCTION   : getLogLevel
*
* DESCRIPTION: Reads the log level property into a variable
*
* PARAMETERS :
*   None
*
* RETURN     :
*   None
*==========================================================================*/
void QCamera3HardwareInterface::getLogLevel()
{
    char prop[PROPERTY_VALUE_MAX];
    uint32_t globalLogLevel = 0;

    property_get("persist.vendor.camera.hal.debug", prop, "0");
    int val = atoi(prop);
    if (0 <= val) {
        gCamHal3LogLevel = (uint32_t)val;
    }

    property_get("persist.vendor.camera.kpi.debug", prop, "0");
    gKpiDebugLevel = atoi(prop);

    property_get("persist.vendor.camera.global.debug", prop, "0");
    val = atoi(prop);
    if (0 <= val) {
        globalLogLevel = (uint32_t)val;
    }

    /* Highest log level among hal.logs and global.logs is selected */
    if (gCamHal3LogLevel < globalLogLevel)
        gCamHal3LogLevel = globalLogLevel;

    return;
}

/*===========================================================================
 * FUNCTION   : validateStreamRotations
 *
 * DESCRIPTION: Check if the rotations requested are supported
 *
 * PARAMETERS :
 *   @stream_list : streams to be configured
 *
 * RETURN     : NO_ERROR on success
 *              -EINVAL on failure
 *
 *==========================================================================*/
int QCamera3HardwareInterface::validateStreamRotations(
        camera3_stream_configuration_t *streamList)
{
    int rc = NO_ERROR;

    /*
    * Loop through all streams requested in configuration
    * Check if unsupported rotations have been requested on any of them
    */
    for (size_t j = 0; j < streamList->num_streams; j++){
        camera3_stream_t *newStream = streamList->streams[j];

        bool isRotated = (newStream->rotation != CAMERA3_STREAM_ROTATION_0);
        bool isImplDef = (newStream->format ==
                HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);
        bool isZsl = (newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL &&
                isImplDef);

        if(newStream->rotation == -1) {
            LOGE("ERROR: Invalid stream rotation requested for stream"
                    "type %d and stream format: %d", newStream->stream_type,
                    newStream->format);
            rc = -EINVAL;
            break;
        }

        if (isRotated && (!isImplDef || isZsl)) {
            LOGE("Error: Unsupported rotation of %d requested for stream"
                    "type:%d and stream format:%d",
                    newStream->rotation, newStream->stream_type,
                    newStream->format);
            rc = -EINVAL;
            break;
        }
    }

    return rc;
}

/*===========================================================================
* FUNCTION   : getFlashInfo
*
* DESCRIPTION: Retrieve information about whether the device has a flash.
*
* PARAMETERS :
*   @cameraId  : Camera id to query
*   @hasFlash  : Boolean indicating whether there is a flash device
*                associated with given camera
*   @flashNode : If a flash device exists, this will be its device node.
*
* RETURN     :
*   None
*==========================================================================*/
void QCamera3HardwareInterface::getFlashInfo(const int cameraId,
        bool& hasFlash,
        char (&flashNode)[QCAMERA_MAX_FILEPATH_LENGTH])
{
    cam_capability_t* camCapability = gCamCapability[cameraId];
    if (NULL == camCapability) {
        hasFlash = false;
        flashNode[0] = '\0';
    } else {
        hasFlash = camCapability->flash_available;
        strlcpy(flashNode,
                (char*)camCapability->flash_dev_name,
                QCAMERA_MAX_FILEPATH_LENGTH);
    }
}

/*===========================================================================
* FUNCTION   : getEepromVersionInfo
*
* DESCRIPTION: Retrieve version info of the sensor EEPROM data
*
* PARAMETERS : None
*
* RETURN     : string describing EEPROM version
*              "\0" if no such info available
*==========================================================================*/
const char *QCamera3HardwareInterface::getEepromVersionInfo()
{
    return (const char *)&gCamCapability[mCameraId]->eeprom_version_info[0];
}

/*===========================================================================
* FUNCTION   : getLdafCalib
*
* DESCRIPTION: Retrieve Laser AF calibration data
*
* PARAMETERS : None
*
* RETURN     : Two uint32_t describing laser AF calibration data
*              NULL if none is available.
*==========================================================================*/
const uint32_t *QCamera3HardwareInterface::getLdafCalib()
{
    if (mLdafCalibExist) {
        return &mLdafCalib[0];
    } else {
        return NULL;
    }
}

/*===========================================================================
 * FUNCTION   : dynamicUpdateMetaStreamInfo
 *
 * DESCRIPTION: This function:
 *             (1) stops all the channels
 *             (2) returns error on pending requests and buffers
 *             (3) sends metastream_info in setparams
 *             (4) starts all channels
 *             This is useful when sensor has to be restarted to apply any
 *             settings such as frame rate from a different sensor mode
 *
 * PARAMETERS : None
 *
 * RETURN     : NO_ERROR on success
 *              Error codes on failure
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::dynamicUpdateMetaStreamInfo()
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_DYN_UPDATE_META_STRM_INFO);
    int rc = NO_ERROR;

    LOGD("E");

    rc = stopAllChannels();
    if (rc < 0) {
        LOGE("stopAllChannels failed");
        return rc;
    }

    rc = notifyErrorForPendingRequests();
    if (rc < 0) {
        LOGE("notifyErrorForPendingRequests failed");
        return rc;
    }

    uint32_t config_index = CONFIG_INDEX_MAIN;
    for (uint32_t i = 0; i < mStreamConfigInfo[config_index].num_streams; i++) {
        LOGI("STREAM INFO : type %d, wxh: %d x %d, pp_mask: 0x%x"
                "Format:%d",
                mStreamConfigInfo[config_index].type[i],
                mStreamConfigInfo[config_index].stream_sizes[i].width,
                mStreamConfigInfo[config_index].stream_sizes[i].height,
                mStreamConfigInfo[config_index].postprocess_mask[i],
                mStreamConfigInfo[config_index].format[i]);
    }

    /* Send meta stream info once again so that ISP can start */
    ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters,
            CAM_INTF_META_STREAM_INFO, mStreamConfigInfo[config_index]);
    rc = mCameraHandle->ops->set_parms(mCameraHandle->camera_handle,
            mParameters);
    if (rc < 0) {
        LOGE("set Metastreaminfo failed. Sensor mode does not change");
    }

    rc = startAllChannels();
    if (rc < 0) {
        LOGE("startAllChannels failed");
        return rc;
    }

    LOGD("X");
    return rc;
}

/*===========================================================================
 * FUNCTION   : stopAllChannels
 *
 * DESCRIPTION: This function stops (equivalent to stream-off) all channels
 *
 * PARAMETERS : None
 *
 * RETURN     : NO_ERROR on success
 *              Error codes on failure
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::stopAllChannels()
{
    int32_t rc = NO_ERROR;

    LOGD("Stopping all channels");

    if (mState == STARTED && mChannelHandle && isSecureMode()) {
        uint8_t close_hint = 1;
        LOGD("set_parms for close hint");
        clear_metadata_buffer(mParameters);
        ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_CLOSE_HINT,
            close_hint);
        rc = mCameraHandle->ops->set_parms(
            get_main_camera_handle(mCameraHandle->camera_handle), mParameters);
        if (rc < 0) {
            LOGE("set_parms failed for close hint");
        }
    }

    // Stop the Streams/Channels
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
        it != mStreamInfo.end(); it++) {
        QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
        if (channel) {
            channel->stop();
        }
        (*it)->status = INVALID;
    }

    if (mSupportChannel) {
        mSupportChannel->stop();
    }
    if (mAnalysisChannel) {
        mAnalysisChannel->stop();
    }
    if (mRawDumpChannel) {
        mRawDumpChannel->stop();
    }
    if (mMetadataChannel) {
        /* If content of mStreamInfo is not 0, there is metadata stream */
        mMetadataChannel->stop();
    }

    if (m_bLPMEnabled) {
         cam_dual_camera_perf_control_t perf_value[1];
         perf_value[0].perf_mode = CAM_PERF_NONE;
         perf_value[0].enable = 0;
         perf_value[0].priority = 0;
         m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_LOW_POWER_MODE;
         memcpy(&m_pDualCamCmdPtr[0]->value, &perf_value[0],
                 sizeof(cam_dual_camera_perf_control_t));
         rc =  mCameraHandle->ops->set_dual_cam_cmd(mCameraHandle->camera_handle);
         if (rc != NO_ERROR) {
             LOGE("LPM not reset, but still process to close");
         } else {
            m_bLPMEnabled = false;
         }
    }
    // unlink of dualcam
    if (mIsDeviceLinked) {
        cam_dual_camera_bundle_info_t *m_pRelCamSyncBuf =
                &(m_pDualCamCmdPtr[0]->bundle_info);
        m_pDualCamCmdPtr[0]->cmd_type = CAM_DUAL_CAMERA_BUNDLE_INFO;
        m_pRelCamSyncBuf->sync_control = CAM_SYNC_RELATED_SENSORS_OFF;
        m_pRelCamSyncBuf->sync_mechanism = CAM_SYNC_NO_SYNC;
        pthread_mutex_lock(&gCamLock);

        if (mIsMainCamera == 1) {
            m_pRelCamSyncBuf->mode = CAM_MODE_PRIMARY;
            m_pRelCamSyncBuf->type = CAM_TYPE_MAIN;
            m_pRelCamSyncBuf->sync_3a_config =
                        {CAM_3A_SYNC_FOLLOW, CAM_3A_SYNC_FOLLOW};
            // related session id should be session id of linked session
            m_pRelCamSyncBuf->related_sensor_session_id = sessionId[mLinkedCameraId];
        } else {
            m_pRelCamSyncBuf->mode = CAM_MODE_SECONDARY;
            m_pRelCamSyncBuf->type = CAM_TYPE_AUX;
            m_pRelCamSyncBuf->sync_3a_config =
                        {CAM_3A_SYNC_FOLLOW, CAM_3A_SYNC_FOLLOW};
            m_pRelCamSyncBuf->related_sensor_session_id = sessionId[mLinkedCameraId];
        }
        pthread_mutex_unlock(&gCamLock);

        rc = mCameraHandle->ops->set_dual_cam_cmd(
                mCameraHandle->camera_handle);
        if (rc < 0) {
            LOGE("Dualcam: Unlink failed, but still proceed to close");
        }
    }
    if (rc < 0) {
        LOGE("stopAllChannels failed");
        return rc;
    }

    if (mPictureChannel && m_bIsVideo && !m_bIs4KVideo) {
        mPictureChannel->stopChannel();
    }

    if (mChannelHandle) {
        mCameraHandle->ops->stop_channel(mCameraHandle->camera_handle,
                mChannelHandle);
    }

    m_bStopPicChannel = false;

    LOGD("All channels stopped");
    return rc;
}

/*===========================================================================
 * FUNCTION   : startAllChannels
 *
 * DESCRIPTION: This function starts (equivalent to stream-on) all channels
 *
 * PARAMETERS : None
 *
 * RETURN     : NO_ERROR on success
 *              Error codes on failure
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::startAllChannels()
{
    int32_t rc = NO_ERROR;

    LOGD("Start all channels ");
    // Start the Streams/Channels
    if (mMetadataChannel) {
        /* If content of mStreamInfo is not 0, there is metadata stream */
        rc = mMetadataChannel->start();
        if (rc < 0) {
            LOGE("META channel start failed");
            return rc;
        }
    }
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
        it != mStreamInfo.end(); it++) {
        QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
        if (channel) {
            rc = channel->start();
            if (rc < 0) {
                LOGE("channel start failed");
                return rc;
            }
        }
    }
    if (mAnalysisChannel) {
        mAnalysisChannel->start();
    }
    if (mSupportChannel) {
        rc = mSupportChannel->start();
        if (rc < 0) {
            LOGE("Support channel start failed");
            return rc;
        }
    }
    if (mRawDumpChannel) {
        rc = mRawDumpChannel->start();
        if (rc < 0) {
            LOGE("RAW dump channel start failed");
            return rc;
        }
    }

    if (mChannelHandle) {
        mCameraHandle->ops->start_channel(mCameraHandle->camera_handle,
                    mChannelHandle);
        if (rc < 0) {
            LOGE("start_channel failed");
            return rc;
        }
    }

    if (mZSLChannel != NULL) {
        mZSLChannel->startDeferredAllocation();
    }

    LOGD("All channels started");
    return rc;
}

/*===========================================================================
 * FUNCTION   : notifyErrorForPendingRequests
 *
 * DESCRIPTION: This function sends error for all the pending requests/buffers
 *
 * PARAMETERS : None
 *
 * RETURN     : Error codes
 *              NO_ERROR on success
 *
 *==========================================================================*/
int32_t QCamera3HardwareInterface::notifyErrorForPendingRequests()
{
    int32_t rc = NO_ERROR;
    unsigned int frameNum = 0;
    camera3_capture_result_t result;
    camera3_stream_buffer_t *pStream_Buf = NULL;

    memset(&result, 0, sizeof(camera3_capture_result_t));

    if (mPendingRequestsList.size() > 0) {
        pendingRequestIterator i = mPendingRequestsList.begin();
        frameNum = i->frame_number;
    } else {
        /* There might still be pending buffers even though there are
         no pending requests. Setting the frameNum to MAX so that
         all the buffers with smaller frame numbers are returned */
        frameNum = UINT_MAX;
    }

    LOGH("Oldest frame num on mPendingRequestsList = %u",
       frameNum);

    for (auto req = mPendingBuffersMap.mPendingBuffersInRequest.begin();
            req != mPendingBuffersMap.mPendingBuffersInRequest.end(); ) {

        if (req->frame_number < frameNum) {
            // Send Error notify to frameworks for each buffer for which
            // metadata buffer is already sent
            LOGH("Sending ERROR BUFFER for frame %d for %d buffer(s)",
                req->frame_number, req->mPendingBufferList.size());

            pStream_Buf = new camera3_stream_buffer_t[req->mPendingBufferList.size()];
            if (NULL == pStream_Buf) {
                LOGE("No memory for pending buffers array");
                return NO_MEMORY;
            }
            memset(pStream_Buf, 0,
                sizeof(camera3_stream_buffer_t)*req->mPendingBufferList.size());
            result.result = NULL;
            result.frame_number = req->frame_number;
            result.num_output_buffers = req->mPendingBufferList.size();
            result.output_buffers = pStream_Buf;

            size_t index = 0;
            for (auto info = req->mPendingBufferList.begin();
                info != req->mPendingBufferList.end(); ) {

                camera3_notify_msg_t notify_msg;
                memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
                notify_msg.type = CAMERA3_MSG_ERROR;
                notify_msg.message.error.error_code = CAMERA3_MSG_ERROR_BUFFER;
                notify_msg.message.error.error_stream = info->stream;
                notify_msg.message.error.frame_number = req->frame_number;
                pStream_Buf[index].acquire_fence = -1;
                pStream_Buf[index].release_fence = -1;
                pStream_Buf[index].buffer = info->buffer;
                pStream_Buf[index].status = CAMERA3_BUFFER_STATUS_ERROR;
                pStream_Buf[index].stream = info->stream;
                orchestrateNotify(&notify_msg);
                index++;
                // Remove buffer from list
                info = req->mPendingBufferList.erase(info);
            }

            // Remove this request from Map
            LOGD("Removing request %d. Remaining requests in mPendingBuffersMap: %d",
                req->frame_number, mPendingBuffersMap.mPendingBuffersInRequest.size());
            req = mPendingBuffersMap.mPendingBuffersInRequest.erase(req);

            orchestrateResult(&result);

            delete [] pStream_Buf;
        } else {

            // Go through the pending requests info and send error request to framework
            pendingRequestIterator i = mPendingRequestsList.begin(); //make sure i is at the beginning

            LOGH("Sending ERROR REQUEST for frame %d", req->frame_number);

            // Send error notify to frameworks
            camera3_notify_msg_t notify_msg;
            memset(&notify_msg, 0, sizeof(camera3_notify_msg_t));
            notify_msg.type = CAMERA3_MSG_ERROR;
            notify_msg.message.error.error_code = CAMERA3_MSG_ERROR_REQUEST;
            notify_msg.message.error.error_stream = NULL;
            notify_msg.message.error.frame_number = req->frame_number;
            orchestrateNotify(&notify_msg);

            pStream_Buf = new camera3_stream_buffer_t[req->mPendingBufferList.size()];
            if (NULL == pStream_Buf) {
                LOGE("No memory for pending buffers array");
                return NO_MEMORY;
            }
            memset(pStream_Buf, 0, sizeof(camera3_stream_buffer_t)*req->mPendingBufferList.size());

            result.result = NULL;
            result.frame_number = req->frame_number;
            result.input_buffer = i->input_buffer;
            result.num_output_buffers = req->mPendingBufferList.size();
            result.output_buffers = pStream_Buf;

            size_t index = 0;
            for (auto info = req->mPendingBufferList.begin();
                info != req->mPendingBufferList.end(); ) {
                pStream_Buf[index].acquire_fence = -1;
                pStream_Buf[index].release_fence = -1;
                pStream_Buf[index].buffer = info->buffer;
                pStream_Buf[index].status = CAMERA3_BUFFER_STATUS_ERROR;
                pStream_Buf[index].stream = info->stream;
                index++;
                // Remove buffer from list
                info = req->mPendingBufferList.erase(info);
            }

            // Remove this request from Map
            LOGD("Removing request %d. Remaining requests in mPendingBuffersMap: %d",
                req->frame_number, mPendingBuffersMap.mPendingBuffersInRequest.size());
            req = mPendingBuffersMap.mPendingBuffersInRequest.erase(req);

            orchestrateResult(&result);
            delete [] pStream_Buf;
            i = erasePendingRequest(i);
        }
    }

    /* Reset pending frame Drop list and requests list */
    mPendingFrameDropList.clear();

    for (auto &req : mPendingBuffersMap.mPendingBuffersInRequest) {
        req.mPendingBufferList.clear();
    }
    mPendingBuffersMap.mPendingBuffersInRequest.clear();
    mPendingReprocessResultList.clear();
    LOGH("Cleared all the pending buffers ");

    return rc;
}

bool QCamera3HardwareInterface::isOnEncoder(
        const cam_dimension_t max_viewfinder_size,
        uint32_t width, uint32_t height)
{
    return ((width > (uint32_t)max_viewfinder_size.width) ||
            (height > (uint32_t)max_viewfinder_size.height) ||
            (width > (uint32_t)VIDEO_4K_WIDTH) ||
            (height > (uint32_t)VIDEO_4K_HEIGHT));
}

/*===========================================================================
 * FUNCTION   : setBundleInfo
 *
 * DESCRIPTION: Set bundle info for all streams that are bundle.
 *
 * PARAMETERS : None
 *
 * RETURN     : NO_ERROR on success
 *              Error codes on failure
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setBundleInfo()
{
    int32_t rc = NO_ERROR;

    if (mChannelHandle) {
        cam_bundle_config_t bundleInfo;
        memset(&bundleInfo, 0, sizeof(bundleInfo));
        rc = mCameraHandle->ops->get_bundle_info(
                get_main_camera_handle(mCameraHandle->camera_handle),
                get_main_camera_handle(mChannelHandle), &bundleInfo);
        if (rc != NO_ERROR) {
            LOGE("get_bundle_info failed");
            return rc;
        }
        if (mAnalysisChannel && !mCommon.skipAnalysisBundling()) {
            mAnalysisChannel->setBundleInfo(bundleInfo);
        }
        if (mSupportChannel) {
            mSupportChannel->setBundleInfo(bundleInfo);
        }
        for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
                it != mStreamInfo.end(); it++) {
            QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
            channel->setBundleInfo(bundleInfo);
        }
        if (mRawDumpChannel) {
            mRawDumpChannel->setBundleInfo(bundleInfo);
        }

        /* temporally use mStreams[0] to check if bundling qcfa channel or not.
         * as we call channel::destroy() but don't delete the channel obj after get qcfa raw */
        if (mQCFACaptureChannel && mQCFACaptureChannel->getStreamByIndex(0) != NULL) {
            mQCFACaptureChannel->setBundleInfo(bundleInfo);
        }


        if(isDualCamera()) {
            setAuxBundleInfo();
        }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : setAuxBundleInfo
 *
 * DESCRIPTION: Set bundle info for all streams that are bundled in Aux session
 *
 * PARAMETERS : None
 *
 * RETURN     : NO_ERROR on success
 *              Error codes on failure
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setAuxBundleInfo()
{
    int32_t rc = NO_ERROR;

    if (mChannelHandle) {
        cam_bundle_config_t bundleInfo;
        memset(&bundleInfo, 0, sizeof(bundleInfo));
        rc = mCameraHandle->ops->get_bundle_info(
                get_aux_camera_handle(mCameraHandle->camera_handle),
                get_aux_camera_handle(mChannelHandle), &bundleInfo);
        if (rc != NO_ERROR) {
            LOGE("get_bundle_info failed");
            return rc;
        }
        if(!IS_PP_TYPE_NONE)
        {
            if (mAnalysisChannel && !mCommon.skipAnalysisBundling()) {
                mAnalysisChannel->setBundleInfo(bundleInfo, CAM_TYPE_AUX);
            }
            if (mSupportChannel) {
                mSupportChannel->setBundleInfo(bundleInfo, CAM_TYPE_AUX);
            }
            for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
                    it != mStreamInfo.end(); it++) {
                QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
                if(IS_VALID_PTR(mZSLChannel) && (mZSLChannel->getAuxHandle() != channel))
                channel->setBundleInfo(bundleInfo, CAM_TYPE_AUX);
            }
            if (mRawDumpChannel) {
                mRawDumpChannel->setBundleInfo(bundleInfo, CAM_TYPE_AUX);
            }
        }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : setInstantAEC
 *
 * DESCRIPTION: Set Instant AEC related params.
 *
 * PARAMETERS :
 *      @meta: CameraMetadata reference
 *
 * RETURN     : NO_ERROR on success
 *              Error codes on failure
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setInstantAEC(const CameraMetadata &meta)
{
    int32_t rc = NO_ERROR;
    uint8_t val = 0;
    char prop[PROPERTY_VALUE_MAX];

    // First try to configure instant AEC from framework metadata
    if (meta.exists(QCAMERA3_INSTANT_AEC_MODE)) {
        val = (uint8_t)meta.find(QCAMERA3_INSTANT_AEC_MODE).data.i32[0];
    }

    // If framework did not set this value, try to read from set prop.
    if (val == 0) {
        memset(prop, 0, sizeof(prop));
        property_get("persist.vendor.camera.instant.aec", prop, "0");
        val = (uint8_t)atoi(prop);
    }

    if ((val >= (uint8_t)CAM_AEC_NORMAL_CONVERGENCE) &&
           ( val < (uint8_t)CAM_AEC_CONVERGENCE_MAX)) {
        ADD_SET_PARAM_ENTRY_TO_BATCH(mParameters, CAM_INTF_PARM_INSTANT_AEC, val);
        mInstantAEC = val;
        mInstantAECSettledFrameNumber = 0;
        mInstantAecFrameIdxCount = 0;
        LOGH("instantAEC value set %d",val);
        if (mInstantAEC) {
            memset(prop, 0, sizeof(prop));
            property_get("persist.vendor.camera.ae.instant.bound", prop, "10");
            int32_t aec_frame_skip_cnt = atoi(prop);
            if (aec_frame_skip_cnt >= 0) {
                mAecSkipDisplayFrameBound = (uint8_t)aec_frame_skip_cnt;
            } else {
                LOGE("Invalid prop for aec frame bound %d", aec_frame_skip_cnt);
                rc = BAD_VALUE;
            }
        }
    } else {
        LOGE("Bad instant aec value set %d", val);
        rc = BAD_VALUE;
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : get_num_overall_buffers
 *
 * DESCRIPTION: Estimate number of pending buffers across all requests.
 *
 * PARAMETERS : None
 *
 * RETURN     : Number of overall pending buffers
 *
 *==========================================================================*/
uint32_t PendingBuffersMap::get_num_overall_buffers()
{
    uint32_t sum_buffers = 0;
    for (auto &req : mPendingBuffersInRequest) {
        sum_buffers += req.mPendingBufferList.size();
    }
    return sum_buffers;
}

/*===========================================================================
 * FUNCTION   : removeBuf
 *
 * DESCRIPTION: Remove a matching buffer from tracker.
 *
 * PARAMETERS : @buffer: image buffer for the callback
 *
 * RETURN     : None
 *
 *==========================================================================*/
void PendingBuffersMap::removeBuf(buffer_handle_t *buffer)
{
    bool buffer_found = false;
    for (auto req = mPendingBuffersInRequest.begin();
            req != mPendingBuffersInRequest.end(); req++) {
        for (auto k = req->mPendingBufferList.begin();
                k != req->mPendingBufferList.end(); k++ ) {
            if (k->buffer == buffer) {
                LOGD("Frame %d: Found Frame buffer %p, take it out from mPendingBufferList",
                        req->frame_number, buffer);
                k = req->mPendingBufferList.erase(k);
                if (req->mPendingBufferList.empty()) {
                    // Remove this request from Map
                    req = mPendingBuffersInRequest.erase(req);
                }
                buffer_found = true;
                break;
            }
        }
        if (buffer_found) {
            break;
        }
    }
    LOGD("mPendingBuffersMap.num_overall_buffers = %d",
            get_num_overall_buffers());
}

/*===========================================================================
 * FUNCTION   : getBufErrStatus
 *
 * DESCRIPTION: get buffer error status
 *
 * PARAMETERS : @buffer: buffer handle
 *
 * RETURN     : Error status
 *
 *==========================================================================*/
int32_t PendingBuffersMap::getBufErrStatus(buffer_handle_t *buffer)
{
    for (auto& req : mPendingBuffersInRequest) {
        for (auto& k : req.mPendingBufferList) {
            if (k.buffer == buffer) {
                if (k.bufStatus & CAMERA3_BUFFER_STATUS_ERROR) {
                    LOGH("CAMERA3_BUFFER_STATUS_ERROR, buffer=%p", buffer);
                }
                return k.bufStatus;
            }
        }
    }
    return CAMERA3_BUFFER_STATUS_OK;
}

/*===========================================================================
 * FUNCTION   : setPAAFSupport
 *
 * DESCRIPTION: Set the preview-assisted auto focus support bit in
 *              feature mask according to stream type and filter
 *              arrangement
 *
 * PARAMETERS : @feature_mask: current feature mask, which may be modified
 *              @stream_type: stream type
 *              @filter_arrangement: filter arrangement
 *
 * RETURN     : None
 *==========================================================================*/
void QCamera3HardwareInterface::setPAAFSupport(
        cam_feature_mask_t& feature_mask,
        cam_stream_type_t stream_type,
        cam_color_filter_arrangement_t filter_arrangement)
{
    LOGD("feature_mask=0x%llx; stream_type=%d, filter_arrangement=%d",
            feature_mask, stream_type, filter_arrangement);

    switch (filter_arrangement) {
    case CAM_FILTER_ARRANGEMENT_RGGB:
    case CAM_FILTER_ARRANGEMENT_GRBG:
    case CAM_FILTER_ARRANGEMENT_GBRG:
    case CAM_FILTER_ARRANGEMENT_BGGR:
        if (stream_type == CAM_STREAM_TYPE_PREVIEW) {
            if (!(feature_mask & CAM_QTI_FEATURE_PPEISCORE))
                feature_mask |= CAM_QCOM_FEATURE_PAAF;
        }
        else if (stream_type == CAM_STREAM_TYPE_VIDEO) {
            if (!(feature_mask & CAM_QTI_FEATURE_PPEISCORE) &&
                !(feature_mask & CAM_QTI_FEATURE_VENDOR_EIS))
                feature_mask |= CAM_QCOM_FEATURE_PAAF;
        }
        break;
    case CAM_FILTER_ARRANGEMENT_Y:
        if (stream_type == CAM_STREAM_TYPE_ANALYSIS) {
            feature_mask |= CAM_QCOM_FEATURE_PAAF;
        }
        break;
    default:
        break;
    }
}


void QCamera3HardwareInterface::setDCFeature(
        cam_feature_mask_t& feature_mask,
        cam_stream_type_t stream_type)
{
    if(isDualCamera()) {
        char prop[PROPERTY_VALUE_MAX];
        bool satEnabledFlag = FALSE;
        bool sacEnabledFlag = FALSE;
        bool rtbdmEnabledFlag = FALSE;
        bool rtbEnabledFlag = FALSE;
        memset(prop, 0, sizeof(prop));
        property_get("persist.vendor.camera.sat.enable", prop, "0");
        satEnabledFlag = atoi(prop);

        if (satEnabledFlag &&
                (getHalPPType() != CAM_HAL_PP_TYPE_BOKEH) &&
                (getHalPPType() != CAM_HAL_PP_TYPE_CLEARSIGHT)) {
            LOGH("SAT flag enabled");
            if ((stream_type == CAM_STREAM_TYPE_VIDEO) ||
                (stream_type == CAM_STREAM_TYPE_PREVIEW)) {
                feature_mask |= CAM_QTI_FEATURE_SAT;
                LOGH("SAT feature mask set");
            }
        }

        memset(prop, 0, sizeof(prop));
        property_get("persist.vendor.camera.sac.enable", prop, "0");
        sacEnabledFlag = atoi(prop);

        if (sacEnabledFlag  &&
                (getHalPPType() != CAM_HAL_PP_TYPE_BOKEH) &&
                (getHalPPType() != CAM_HAL_PP_TYPE_CLEARSIGHT)) {
            LOGH("SAC flag enabled");
            if ((stream_type == CAM_STREAM_TYPE_ANALYSIS) ||
                (stream_type == CAM_STREAM_TYPE_VIDEO) ||
                (stream_type == CAM_STREAM_TYPE_PREVIEW)) {
                feature_mask |= CAM_QTI_FEATURE_SAC;
                LOGH("SAC feature mask set");
            }
        }

        memset(prop, 0, sizeof(prop));
        property_get("persist.vendor.camera.rtbdm.enable", prop, "0");
        rtbdmEnabledFlag = atoi(prop);

        if (rtbdmEnabledFlag  &&
                (getHalPPType() == CAM_HAL_PP_TYPE_BOKEH)) {
            LOGH("RTBDM flag enabled");
            if (stream_type == CAM_STREAM_TYPE_ANALYSIS) {
                feature_mask |= CAM_QTI_FEATURE_RTBDM;
                LOGH("RTBDM feature mask set");
            }
        }

        memset(prop, 0, sizeof(prop));
        property_get("persist.vendor.camera.rtb.enable", prop, "0");
        rtbEnabledFlag = atoi(prop);

        if (rtbEnabledFlag ||
                (getHalPPType() == CAM_HAL_PP_TYPE_BOKEH)) {
            LOGH("RTB flag enabled");
            if (stream_type == CAM_STREAM_TYPE_PREVIEW) {
                feature_mask |= CAM_QTI_FEATURE_RTB;
                LOGH("RTB feature mask set");
            }
        }
    }
}

/*===========================================================================
* FUNCTION   : getSensorMountAngle
*
* DESCRIPTION: Retrieve sensor mount angle
*
* PARAMETERS : None
*
* RETURN     : sensor mount angle in uint32_t
*==========================================================================*/
uint32_t QCamera3HardwareInterface::getSensorMountAngle()
{
    return gCamCapability[mCameraId]->sensor_mount_angle;
}

/*===========================================================================
* FUNCTION   : getRelatedCalibrationData
*
* DESCRIPTION: Retrieve related system calibration data
*
* PARAMETERS : None
*
* RETURN     : Pointer of related system calibration data
*==========================================================================*/
const cam_related_system_calibration_data_t *QCamera3HardwareInterface::getRelatedCalibrationData()
{
    return (const cam_related_system_calibration_data_t *)
            &(gCamCapability[mCameraId]->related_cam_calibration);
}

/*===========================================================================
 * FUNCTION   : bundleRelatedCameras
 *
 * DESCRIPTION: send trigger for bundling related camera sessions in the server
 *
 * PARAMETERS :
 *   @sync_enable :indicates whether syncing is On or Off
 *   @sessionid   :session id for other camera session
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *NOTE: This bundle info needs to called only once per session.
 * Should be called after open and before start stream.
 * Application can trigger this function to enable module SYNC in dual camera case
 *==========================================================================*/
int32_t QCamera3HardwareInterface::bundleRelatedCameras(bool enable_sync)
{
    if (!isDualCamera()) {
        return NO_ERROR;
    }

    int32_t rc = NO_ERROR;
    cam_3a_sync_mode_t sync_3a_mode = CAM_3A_SYNC_FOLLOW;
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));

    cam_sync_related_sensors_control_t syncControl = CAM_SYNC_RELATED_SENSORS_ON;
    property_get("persist.vendor.camera.stats.test.2outs", prop, "0");
    sync_3a_mode = (atoi(prop) > 0) ? CAM_3A_SYNC_ALGO_CTRL : sync_3a_mode;
    cam_3a_sync_mode_t sync_stats_common, af_sync;
    sync_stats_common = af_sync = sync_3a_mode;

    //Set AF sync mode to none, if either of the sensors doesn't support auto focus.
    if (!mCommon.isAutoFocusSupported(CAM_TYPE_MAIN) || !mCommon.isAutoFocusSupported(CAM_TYPE_AUX))
        af_sync = CAM_3A_SYNC_NONE;

    cam_3a_sync_config_t sync_config_3a = {sync_stats_common, af_sync};

    //Trigger dual camera Link command before Meta info
    cam_dual_camera_bundle_info_t bundle_info[MM_CAMERA_MAX_CAM_CNT];
    uint8_t num_cam = 0;
    uint32_t sessionID = 0;
    // Update syncControl based on DUALCAM_SYNC_MECHANISM setting
    if (enable_sync) {
        syncControl = (DUALCAM_SYNC_MECHANISM == CAM_SYNC_NO_SYNC) ?
                CAM_SYNC_RELATED_SENSORS_OFF : CAM_SYNC_RELATED_SENSORS_ON;
    } else {
        syncControl = CAM_SYNC_RELATED_SENSORS_OFF;
    }

    bundle_info[num_cam].sync_control = syncControl;
    bundle_info[num_cam].type = CAM_TYPE_MAIN;
    bundle_info[num_cam].mode = CAM_MODE_PRIMARY;
    if (isBayerMono())
        bundle_info[num_cam].cam_role = CAM_ROLE_BAYER;
    else
        bundle_info[num_cam].cam_role = CAM_ROLE_WIDE;
    bundle_info[num_cam].sync_3a_config = sync_config_3a;
    mCameraHandle->ops->get_session_id(
            get_aux_camera_handle(mCameraHandle->camera_handle), &sessionID);
    bundle_info[num_cam].related_sensor_session_id = sessionID;
    bundle_info[num_cam].perf_mode = getLowPowerMode(CAM_TYPE_MAIN);
    bundle_info[num_cam].sync_mechanism = DUALCAM_SYNC_MECHANISM;
    bundle_info[num_cam].hal_lpm_control = true;
    num_cam++;

    bundle_info[num_cam].sync_control = syncControl;
    bundle_info[num_cam].type = CAM_TYPE_AUX;
    bundle_info[num_cam].mode = CAM_MODE_SECONDARY;
    if (isBayerMono())
        bundle_info[num_cam].cam_role = CAM_ROLE_MONO;
    else
        bundle_info[num_cam].cam_role = CAM_ROLE_TELE;
    bundle_info[num_cam].sync_3a_config = sync_config_3a;
    mCameraHandle->ops->get_session_id(
            get_main_camera_handle(mCameraHandle->camera_handle), &sessionID);
    bundle_info[num_cam].related_sensor_session_id = sessionID;
    bundle_info[num_cam].perf_mode = getLowPowerMode(CAM_TYPE_AUX);
    bundle_info[num_cam].sync_mechanism = DUALCAM_SYNC_MECHANISM;
    bundle_info[num_cam].hal_lpm_control = true;
    num_cam++;

    rc = sendDualCamCmd(CAM_DUAL_CAMERA_BUNDLE_INFO,
            num_cam, &bundle_info[0]);
    return rc;
}


/*===========================================================================
 * FUNCTION   : getLowPowerMode
 *
 * DESCRIPTION: Get Low Power Mode for the given camera
 *
 * PARAMETERS :
 * @cam       : Camera type for which Low Power Mode is queried
 *
 * RETURN     : Low Power Mode with type cam_dual_camera_perf_mode_t
 *==========================================================================*/
cam_dual_camera_perf_mode_t QCamera3HardwareInterface::getLowPowerMode(cam_sync_type_t cam)
{
    char prop[PROPERTY_VALUE_MAX];
    int32_t lpm = 0;
    int32_t lpmConfig = 0;

    // LPM is disabled for Bokeh mode as both sensors have to be running all the time
    if ((getHalPPType() == CAM_HAL_PP_TYPE_BOKEH) ||  IS_YUV_ZSL) {
        LOGD("LPM disabled in bokeh mode:  %s camera", cam == CAM_TYPE_MAIN ? "main" : "aux");
        return CAM_PERF_NONE;
    }

    if (cam == CAM_TYPE_MAIN) {
        property_get("persist.vendor.dualcam.lpm.main", prop, "0");
        lpm = atoi(prop);
        lpmConfig = DUALCAM_LPM_MAIN;
    } else if (cam == CAM_TYPE_AUX) {
        property_get("persist.vendor.dualcam.lpm.aux", prop, "0");
        lpm = atoi(prop);
        lpmConfig = DUALCAM_LPM_AUX;
    } else {
        LOGE("Invalid camera type queried for LPM");
        return CAM_PERF_NONE;
    }

    // If setprop doesn't set low power mode read the mode from config file QCameraDualCamSettings.h
    if (lpm == 0) {
        lpm = lpmConfig;
    }
    LOGD("LPM for %s camera: %d", cam == CAM_TYPE_MAIN ? "main" : "aux", lpm);
    return (cam_dual_camera_perf_mode_t)lpm;
}

bool QCamera3HardwareInterface::isPPMaskSetForScaling(cam_feature_mask_t pp_mask)
{
    if(pp_mask & CAM_QCOM_FEATURE_PP_SUPERSET_HAL3)
    {
        return true;
    }

    return false;
}


/*===========================================================================
 * FUNCTION   : sendDualCamCmd
 *
 * DESCRIPTION: send dual camera related commands
 *
 * PARAMETERS :
 *   @sync_enable        :indicates whether syncing is On or Off
 *   @sessionid  :session id for other camera session
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *NOTE: This bundle info needs to called only once per session.
 * Should be called after open and before start stream.
 *==========================================================================*/
int32_t QCamera3HardwareInterface::sendDualCamCmd(cam_dual_camera_cmd_type type,
        uint8_t num_cam, void *cmd_value)
{
    int32_t rc = NO_ERROR;
    if (NULL == mCameraHandle) {
        LOGE("Ops not initialized");
        return NO_INIT;
    }

    if (cmd_value == NULL || num_cam > MM_CAMERA_MAX_CAM_CNT
            || m_pDualCamCmdPtr[0] == NULL) {
        LOGE("Invalid argument = %d, %p", num_cam, cmd_value);
        return BAD_VALUE;
    }

    for (int i = 0; i < MM_CAMERA_MAX_CAM_CNT; i++) {
        memset(m_pDualCamCmdPtr[i], 0,
                sizeof(cam_dual_camera_cmd_info_t));
    }

    switch(type) {
        case CAM_DUAL_CAMERA_BUNDLE_INFO: {
            for (int i = 0; i < num_cam; i++) {
                cam_dual_camera_bundle_info_t *info =
                        (cam_dual_camera_bundle_info_t *)cmd_value;
                m_pDualCamCmdPtr[i]->cmd_type = type;
                memcpy(&m_pDualCamCmdPtr[i]->bundle_info,
                        &info[i],
                        sizeof(cam_dual_camera_bundle_info_t));

                LOGH("SYNC CMD %d: cmd %d mode %d type %d sync-control %d "
                        "sync-mechanism %d session - %d", i,
                        m_pDualCamCmdPtr[i]->cmd_type,
                        m_pDualCamCmdPtr[i]->bundle_info.mode,
                        m_pDualCamCmdPtr[i]->bundle_info.type,
                        m_pDualCamCmdPtr[i]->bundle_info.sync_control,
                        m_pDualCamCmdPtr[i]->bundle_info.sync_mechanism,
                        m_pDualCamCmdPtr[i]->bundle_info.related_sensor_session_id);
            }
        }
        break;

        case CAM_DUAL_CAMERA_LOW_POWER_MODE: {
            for (int i = 0; i < num_cam; i++) {
                cam_dual_camera_perf_control_t *info =
                        (cam_dual_camera_perf_control_t *)cmd_value;
                m_pDualCamCmdPtr[i]->cmd_type = type;
                memcpy(&m_pDualCamCmdPtr[i]->value,
                        &info[i],
                        sizeof(cam_dual_camera_perf_control_t));
                LOGH("LPM CMD %d: cmd %d LPM Enable - %d mode = %d", i,
                        m_pDualCamCmdPtr[i]->cmd_type,
                        m_pDualCamCmdPtr[i]->value.enable,
                        m_pDualCamCmdPtr[i]->value.perf_mode);
            }
        }
        break;

        case CAM_DUAL_CAMERA_MASTER_INFO: {
            for (int i = 0; i < num_cam; i++) {
                cam_dual_camera_master_info_t *info =
                        (cam_dual_camera_master_info_t *)cmd_value;
                m_pDualCamCmdPtr[i]->cmd_type = type;
                memcpy(&m_pDualCamCmdPtr[i]->mode,
                        &info[i],
                        sizeof(cam_dual_camera_master_info_t));
                LOGH("MASTER INFO CMD %d: cmd %d value %d", i,
                        m_pDualCamCmdPtr[i]->cmd_type,
                        m_pDualCamCmdPtr[i]->mode);
            }
        }
        break;

        case CAM_DUAL_CAMERA_DEFER_INFO: {
            cam_dual_camera_defer_cmd_t *info =
                    (cam_dual_camera_defer_cmd_t *)cmd_value;
            for (int i = 0; i < num_cam; i++) {
                m_pDualCamCmdPtr[i]->cmd_type = type;
                memcpy(&m_pDualCamCmdPtr[i]->defer_cmd,
                        &info[i],
                        sizeof(cam_dual_camera_master_info_t));
                LOGH("DEFER INFO CMD %d: cmd %d value %d", i,
                        m_pDualCamCmdPtr[i]->cmd_type,
                        m_pDualCamCmdPtr[i]->defer_cmd);
            }
        }
        break;

        case CAM_DUAL_CAMERA_FALLBACK_INFO: {
            for (int i = 0; i < num_cam; i++) {
                cam_dual_camera_fallback_info_t *info =
                        (cam_dual_camera_fallback_info_t *)cmd_value;
                m_pDualCamCmdPtr[i]->cmd_type = type;
                memcpy(&m_pDualCamCmdPtr[i]->fallback,
                        &info[i],
                        sizeof(cam_dual_camera_fallback_info_t));
                LOGH("FALLBACK INFO CMD %d: cmd %d value %d", i,
                        m_pDualCamCmdPtr[i]->cmd_type,
                        m_pDualCamCmdPtr[i]->fallback);
            }
        }
        break;

        default :
        break;
    }

    rc = mCameraHandle->ops->set_dual_cam_cmd(mCameraHandle->camera_handle);
    return rc;
}

/*===========================================================================
 * FUNCTION   : configureHalPostProcess
 *
 * DESCRIPTION: config hal postproc (HALPP) for current snapshot.
 *
 * PARAMETERS : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::configureHalPostProcess(bool bIsInput)
{
    LOGD("E");
    int32_t rc = NO_ERROR;

    /* check if halpp is needed in dual camera mode */
    if (isDualCamera() && mBundledSnapshot && !bIsInput &&
              ((getHalPPType() != CAM_HAL_PP_TYPE_NONE)
              && (getHalPPType() != CAM_HAL_PP_TYPE_SAT)) && !m_bIsVideo) {
        LOGH("Use HALPP for dual camera bundle snapshot.");
        m_bNeedHalPP = TRUE;
        // In Bokhe mode if RTB is status not success, halpp should be false
        if((getHalPPType() == CAM_HAL_PP_TYPE_BOKEH) &&
                 (mRTBStatus != CAM_RTB_MSG_DEPTH_EFFECT_SUCCESS))
        {
            m_bNeedHalPP = FALSE;
        }
    }else {
        m_bNeedHalPP = FALSE;
    }

    return rc;
    LOGD("X");
}

/*===========================================================================
 * FUNCTION   : getCamHalCapabilities
 *
 * DESCRIPTION: get the HAL capabilities structure
 *
 * PARAMETERS :
 *   @cameraId  : camera Id
 *
 * RETURN     : capability structure of respective camera
 *
 *==========================================================================*/
cam_capability_t* QCamera3HardwareInterface::getCamHalCapabilities()
{
    return gCamCapability[mCameraId];
}

/*===========================================================================
 * FUNCTION   : switchMaster
 *
 * DESCRIPTION: switch master camera in all the channels
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3HardwareInterface::switchMaster(uint32_t masterCam)
{
    LOGD("E");
    if (!isDualCamera()) {
        return;
    }
    for (List<stream_info_t *>::iterator it = mStreamInfo.begin();
        it != mStreamInfo.end(); it++) {
        QCamera3Channel *channel = (QCamera3Channel *)(*it)->stream->priv;
        if (channel != NULL && (channel != channel->getAuxHandle())) {
            channel->switchMaster(masterCam);
        }
    }
    LOGD("X");
    return;
}

/*===========================================================================
 * FUNCTION   : setDCMasterInfo
 *
 * DESCRIPTION: Trigger event to inform about camera role switch
 *
 * PARAMETERS :
 *         @camMaster : Master camera
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setDCMasterInfo(uint32_t camMaster)
{
    int32_t rc = NO_ERROR;
    cam_dual_camera_master_info_t camState[MM_CAMERA_MAX_CAM_CNT];
    uint8_t num_cam = 0;

    if (camMaster == MM_CAMERA_TYPE_MAIN) {
        camState[0].mode = CAM_MODE_PRIMARY;
        camState[1].mode = CAM_MODE_SECONDARY;
    } else if (camMaster == MM_CAMERA_TYPE_AUX) {
        camState[0].mode = CAM_MODE_SECONDARY;
        camState[1].mode = CAM_MODE_PRIMARY;
    } else {
        LOGW("Invalid master camera info");
        return rc;
    }

    LOGH("Switching master to %s", (camMaster == MM_CAMERA_TYPE_MAIN) ?
            "CAM_TYPE_MAIN" : "CAM_TYPE_AUX");

    num_cam = MM_CAMERA_MAX_CAM_CNT;
    rc = sendDualCamCmd(CAM_DUAL_CAMERA_MASTER_INFO,
              num_cam, &camState[0]);
    return rc;
}

/*===========================================================================
 * FUNCTION   : setCameraControls
 *
 * DESCRIPTION: activate or deactive camera's
 *
 * PARAMETERS :
 *         @state          : Flag with camera bit field set in case of dual camera
 *         @bundleSnapshot : Flag to update bundle snapshot info
 *         @fallback : Fallback mode for master in case of low light / macro scene
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setDCControls(uint32_t camMaster, uint32_t state,
        bool bundleSnap, cam_fallback_mode_t fallback)
{
    int32_t rc = NO_ERROR;

    if (camMaster != mMasterCamera) {
        mMasterCamera = camMaster;
        rc = setDCMasterInfo(camMaster);
        switchMaster(camMaster);
    }
    if (state != mActiveCameras) {
        mActiveCameras = state;
        rc = setDCLowPowerMode(state);
    }
    if (fallback != mFallbackMode) {
        mFallbackMode = fallback;
        rc = setDCFallbackMode(fallback);
    }
    mBundledSnapshot = bundleSnap;
    return rc;
}

/*===========================================================================
 * FUNCTION   : setDCLowPowerMode
 *
 * DESCRIPTION: trigger low power mode in dual camera.
 *
 * PARAMETERS :
 *    @state : Flag with camera bit field set in case of dual camera
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setDCLowPowerMode(uint32_t state)
{
    int32_t rc = NO_ERROR;

    if (mLPMEnable) {
        LOGH("Setting lpm state to %d", state);
        int32_t cameraControl[MM_CAMERA_MAX_CAM_CNT] = {0};
        cam_dual_camera_perf_mode_t lpmMain = CAM_PERF_NONE;
        cam_dual_camera_perf_mode_t lpmAux  = CAM_PERF_NONE;

        cam_dual_camera_perf_control_t perf_value[MM_CAMERA_MAX_CAM_CNT];
        uint8_t num_cam = 0;

        lpmMain = getLowPowerMode(CAM_TYPE_MAIN);
        lpmAux  = getLowPowerMode(CAM_TYPE_AUX);

        // Keep the camera active if indicated by the active state or if LPM is NONE
        if ((state & MM_CAMERA_TYPE_MAIN) ||
                (lpmMain == CAM_PERF_NONE)) {
            cameraControl[0] = 1;
        } else {
            cameraControl[0] = 0;
        }

        // Keep the camera active if indicated by the active state or if LPM is NONE
        if ((state & MM_CAMERA_TYPE_AUX)  ||
                 (lpmAux == CAM_PERF_NONE)) {
             cameraControl[1] = 1;
        } else {
             cameraControl[1] = 0;
        }

        perf_value[num_cam].perf_mode = lpmMain;
        perf_value[num_cam].enable = cameraControl[0] ? 0 : 1;
        perf_value[num_cam].priority = 0;
        num_cam++;
        perf_value[num_cam].perf_mode = lpmAux;
        perf_value[num_cam].enable = cameraControl[1] ? 0 : 1;
        perf_value[num_cam].priority = 0;
        num_cam++;

        rc = sendDualCamCmd(CAM_DUAL_CAMERA_LOW_POWER_MODE,
                 num_cam, &perf_value[0]);
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : setDCFallbackMode
 *
 * DESCRIPTION: Trigger fallback mode in dual camera.
 *
 * PARAMETERS :
 *         @fallback : Fallback mode for master in case of low light / macro scene
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setDCFallbackMode(cam_fallback_mode_t fallback)
{
    int32_t rc = NO_ERROR;

    cam_dual_camera_fallback_info_t fallbackMode[MM_CAMERA_MAX_CAM_CNT];
    uint8_t num_cam = 0;
    LOGH("Setting fallback mode to %d", fallback);

    fallbackMode[num_cam].fallback = fallback;
    num_cam++;
    fallbackMode[num_cam].fallback = fallback;
    num_cam++;

    rc = sendDualCamCmd(CAM_DUAL_CAMERA_FALLBACK_INFO,
            num_cam, &fallbackMode[0]);

    return rc;
}

/*===========================================================================
 * FUNCTION   : setDeferCamera
 *
 * DESCRIPTION: configure camera in background for KPI in dual camera
 *
 * PARAMETERS :
 *         @type : Type of defer command
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3HardwareInterface::setDCDeferCamera(cam_dual_camera_defer_cmd_t type)
{
    int32_t rc = NO_ERROR;
    char prop[PROPERTY_VALUE_MAX];
    bool deferEnable = TRUE;
    if(IS_PP_TYPE_NONE)
    {
        return rc;
    }
    property_get("persist.vendor.dualcam.defer.enable", prop, "1");
    deferEnable = atoi(prop) ? TRUE : FALSE;

    if (deferEnable) {
        cam_dual_camera_defer_cmd_t defer_val[MM_CAMERA_MAX_CAM_CNT];
        memset(&defer_val[0], 0, sizeof(defer_val));

        if (mMasterCamera == MM_CAMERA_TYPE_MAIN) {
            defer_val[1] = type;
        } else if (mMasterCamera == MM_CAMERA_TYPE_AUX) {
            defer_val[0] = type;
        } else {
            LOGW("Invalid master camera info");
            return rc;
        }
        LOGH("Deferring %s camera", (mMasterCamera == MM_CAMERA_TYPE_MAIN)?"Aux":"Main");
        sendDualCamCmd(CAM_DUAL_CAMERA_DEFER_INFO, MM_CAMERA_MAX_CAM_CNT,
                &defer_val[0]);
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : initDCSettings
 *
 * DESCRIPTION: initialize dual camera settings
 *
 * PARAMETERS : None
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3HardwareInterface::initDCSettings()
{
    char prop[PROPERTY_VALUE_MAX];

    if (!isDualCamera() || !m_pFovControl || IS_PP_TYPE_NONE) {
        return;
    }

    // LPM is enabled by default.
    // It can disabled at the compile time using DUALCAM_LPM_ENABLE from QCameraDualCamSettings.h
    // It can be disabled dynamically using the setprop persist.dualcam.lpm.enable.
    property_get("persist.vendor.dualcam.lpm.enable", prop, "1");
    mLPMEnable = atoi(prop) ? TRUE : FALSE;

    if (DUALCAM_LPM_ENABLE == 0) {
        mLPMEnable = 0;
    }

    fov_control_result_t fovControlResult = m_pFovControl->getFovControlResult();
    if (fovControlResult.isValid) {
        mActiveCameras = fovControlResult.activeCameras;
        mMasterCamera = fovControlResult.camMasterPreview;
        mBundledSnapshot = fovControlResult.snapshotPostProcess;
        mFallbackMode = fovControlResult.fallback;
    }

    // Send dual cam cmd for master camera info
    setDCMasterInfo(mMasterCamera);
    //set LPM mode
    setDCLowPowerMode(mActiveCameras);
}

void QCamera3HardwareInterface::fillUBWCStats(camera3_stream_buffer_t *buffer)
{
    UBWCStats stats[2];
    memset(&stats, 0, sizeof(stats));
    stats[0].version = UBWC_1_0;
    stats[0].bDataValid = 1;
    stats[0].ubwc_stats.nCRStatsTile32 = UBWC_COMP_RATIO * (1<<16);

    struct private_handle_t *priv_handle =
                        (struct private_handle_t *) (*(buffer->buffer));
    setMetaData(priv_handle, SET_UBWC_CR_STATS_INFO, &stats);
}

bool QCamera3HardwareInterface::needZSLCapture(const camera3_capture_request_t *request)
{
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.forcezsl", prop, "0");

    bool needZSL = false;
    bool bZSLSetting = false;
    uint8_t captureIntent;
    CameraMetadata frame_settings;

    if (!request->settings) {
        return false;
    }

    frame_settings = request->settings;
    if (frame_settings.exists(ANDROID_CONTROL_ENABLE_ZSL)) {
        bZSLSetting = frame_settings.find(ANDROID_CONTROL_ENABLE_ZSL).data.u8[0];
        LOGD("Control ZSL in request settings is %d", bZSLSetting);
    }
    if (frame_settings.exists(ANDROID_CONTROL_CAPTURE_INTENT)) {
        captureIntent = frame_settings.find(ANDROID_CONTROL_CAPTURE_INTENT).data.u8[0];
    }
    if (bZSLSetting && (captureIntent == ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE)) {
        needZSL = true;
    } else {
        //No need to check other params
        return false;
    }

    if (mFlashNeeded) {
        needZSL = false;
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_MODE)) {
        uint8_t fwk_aeMode =
            frame_settings.find(ANDROID_CONTROL_AE_MODE).data.u8[0];
        if (fwk_aeMode == ANDROID_CONTROL_AE_MODE_OFF ) {
            needZSL = false;
        }
        if (fwk_aeMode == ANDROID_CONTROL_AE_MODE_ON) {
            if (frame_settings.exists(ANDROID_FLASH_MODE)) {
                uint8_t flashMode = frame_settings.find(ANDROID_FLASH_MODE).data.u8[0];
                if (ANDROID_FLASH_MODE_OFF != flashMode) {
                    needZSL = false;
                }
            }
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AWB_MODE)) {
        uint8_t fwk_awbMode =
            frame_settings.find(ANDROID_CONTROL_AWB_MODE).data.u8[0];
        if (fwk_awbMode == ANDROID_CONTROL_AWB_MODE_OFF ) {
            needZSL = false;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AF_MODE)) {
        uint8_t fwk_afMode =
            frame_settings.find(ANDROID_CONTROL_AF_MODE).data.u8[0];
        if (fwk_afMode == ANDROID_CONTROL_AF_MODE_OFF ) {
            needZSL = false;
        }
    }

    if (frame_settings.exists(ANDROID_CONTROL_AE_LOCK)) {
        uint8_t aeLock = frame_settings.find(ANDROID_CONTROL_AE_LOCK).data.u8[0];
        if ((aeLock == ANDROID_CONTROL_AE_LOCK_ON) &&
            frame_settings.exists(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION)) {
            needZSL = false;
        }
    }

    if (frame_settings.exists(QCAMERA3_USE_ISO_EXP_PRIORITY) &&
        frame_settings.exists(QCAMERA3_SELECT_PRIORITY)) {
        cam_priority_mode_t mode =
                (cam_priority_mode_t)frame_settings.find(QCAMERA3_SELECT_PRIORITY).data.i32[0];
        if((CAM_ISO_PRIORITY == mode)) {
            needZSL = false;
        }
    }

    if (atoi(prop) == 1) {
        needZSL = true;
    }
    LOGH("needZSL = %d for frame_number %d", needZSL, request->frame_number);
    return needZSL;
}

int32_t QCamera3HardwareInterface::addZSLChannel()
{
    int32_t rc = NO_ERROR;
    mm_camera_channel_attr_t attr;

    memset(&attr, 0, sizeof(mm_camera_channel_attr_t));
    attr.notify_mode = MM_CAMERA_SUPER_BUF_NOTIFY_BURST;
    attr.max_unmatched_frames = ZSL_UNMATCH_COUNT;
    attr.look_back = ZSL_LOOK_BACK;
    attr.post_frame_skip = ZSL_POST_FRAME_SKIP;
    attr.water_mark = ZSL_WATER_MARK;
    attr.priority = MM_CAMERA_SUPER_BUF_PRIORITY_MATCH_META;

    mChannelHandle = mCameraHandle->ops->add_channel(mCameraHandle->camera_handle,
                                      &attr,
                                      zsl_channel_cb,
                                      this);

    if (mChannelHandle == 0) {
        LOGE("Add channel failed");
        rc = UNKNOWN_ERROR;
    }
    return rc;
}

void QCamera3HardwareInterface::zsl_channel_cb(mm_camera_super_buf_t *recvd_frame,
                                               void *userdata)
{
    QCamera3HardwareInterface* halObj = (QCamera3HardwareInterface*)userdata;
    if (halObj && halObj->mZSLChannel) {
        halObj->mZSLChannel->ZSLChannelCb(recvd_frame);
    }
}

}; //end namespace qcamera
