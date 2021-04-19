# Delete existing hals
rm -rf hardware/qcom-caf/msm8998/display
rm -rf hardware/qcom-caf/msm8998/audio
rm -rf hardware/qcom-caf/msm8998/media
rm -rf vendor/codeaurora/telephony

# Clone and use hals from AOSP-11
git clone https://github.com/TechWiz007/hardware_qcom-caf_display_msm8998.git hardware/qcom-caf/msm8998/display
git clone https://github.com/TechWiz007/hardware_qcom-caf_media_msm8998.git hardware/qcom-caf/msm8998/media
git clone https://github.com/TechWiz007/hardware_qcom-caf_audio_msm8998.git hardware/qcom-caf/msm8998/audio
git clone https://github.com/TechWiz007/android_vendor_codeaurora_telephony.git vendor/codeaurora/telephony