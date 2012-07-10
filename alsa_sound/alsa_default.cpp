/* alsa_default.cpp
 **
 ** Copyright 2009 Wind River Systems
 ** Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#define LOG_TAG "alsa.msm8960"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <cutils/properties.h>
#include <linux/ioctl.h>
#include "AudioHardwareALSA.h"
#include <media/AudioRecord.h>
#if 0
extern "C" {
#include "csd_client.h"
}
#endif

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define BTSCO_RATE_16KHZ 16000
#define USECASE_TYPE_RX 1
#define USECASE_TYPE_TX 2

namespace android_audio_legacy
{

static int      s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int      s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *);
static status_t s_close(alsa_handle_t *);
static status_t s_standby(alsa_handle_t *);
static status_t s_route(alsa_handle_t *, uint32_t, int);
static status_t s_start_voice_call(alsa_handle_t *);
static status_t s_start_voip_call(alsa_handle_t *);
static status_t s_start_fm(alsa_handle_t *);
static void     s_set_voice_volume(int);
static void     s_set_voip_volume(int);
static void     s_set_mic_mute(int);
static void     s_set_voip_mic_mute(int);
static void     s_set_voip_config(int, int);
static status_t s_set_fm_vol(int);
static void     s_set_btsco_rate(int);
static status_t s_set_lpa_vol(int);
static void     s_enable_wide_voice(bool flag);
static void     s_enable_fens(bool flag);
static void     s_set_flags(uint32_t flags);
static status_t s_set_compressed_vol(int);
static void     s_enable_slow_talk(bool flag);
static void     s_set_voc_rec_mode(uint8_t mode);
static void     s_set_volte_mic_mute(int state);
static void     s_set_volte_volume(int vol);

static char mic_type[25];
static char curRxUCMDevice[50];
static char curTxUCMDevice[50];
static int fluence_mode;
static int fmVolume;
static uint32_t mDevSettingsFlag = TTY_OFF;
static int btsco_samplerate = 8000;
static bool pflag = false;
static ALSAUseCaseList mUseCaseList;

static hw_module_methods_t s_module_methods = {
    open            : s_device_open
};

extern "C" hw_module_t HAL_MODULE_INFO_SYM = {
    tag             : HARDWARE_MODULE_TAG,
    version_major   : 1,
    version_minor   : 0,
    id              : ALSA_HARDWARE_MODULE_ID,
    name            : "QCOM ALSA module",
    author          : "QuIC Inc",
    methods         : &s_module_methods,
    dso             : 0,
    reserved        : {0,},
};

static int s_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    char value[128];
    alsa_device_t *dev;
    dev = (alsa_device_t *) malloc(sizeof(*dev));
    if (!dev) return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    /* initialize the procs */
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *) module;
    dev->common.close = s_device_close;
    dev->init = s_init;
    dev->open = s_open;
    dev->close = s_close;
    dev->route = s_route;
    dev->standby = s_standby;
    dev->startVoiceCall = s_start_voice_call;
    dev->startVoipCall = s_start_voip_call;
    dev->startFm = s_start_fm;
    dev->setVoiceVolume = s_set_voice_volume;
    dev->setVoipVolume = s_set_voip_volume;
    dev->setMicMute = s_set_mic_mute;
    dev->setVoipMicMute = s_set_voip_mic_mute;
    dev->setVoipConfig = s_set_voip_config;
    dev->setFmVolume = s_set_fm_vol;
    dev->setBtscoRate = s_set_btsco_rate;
    dev->setLpaVolume = s_set_lpa_vol;
    dev->enableWideVoice = s_enable_wide_voice;
    dev->enableFENS = s_enable_fens;
    dev->setFlags = s_set_flags;
    dev->setCompressedVolume = s_set_compressed_vol;
    dev->enableSlowTalk = s_enable_slow_talk;
    dev->setVocRecMode = s_set_voc_rec_mode;
    dev->setVoLTEMicMute = s_set_volte_mic_mute;
    dev->setVoLTEVolume = s_set_volte_volume;

    *device = &dev->common;

    property_get("persist.audio.handset.mic",value,"0");
    strlcpy(mic_type, value, sizeof(mic_type));
    property_get("persist.audio.fluence.mode",value,"0");
    if (!strcmp("broadside", value)) {
        fluence_mode = FLUENCE_MODE_BROADSIDE;
    } else {
        fluence_mode = FLUENCE_MODE_ENDFIRE;
    }
    strlcpy(curRxUCMDevice, "None", sizeof(curRxUCMDevice));
    strlcpy(curTxUCMDevice, "None", sizeof(curTxUCMDevice));
    ALOGD("ALSA module opened");

    return 0;
}

static int s_device_close(hw_device_t* device)
{
    free(device);
    device = NULL;
    return 0;
}

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

static void switchDevice(alsa_handle_t *handle, uint32_t devices, uint32_t mode);
static char *getUCMDevice(uint32_t devices, int input, char *rxDevice);
static void disableDevice(alsa_handle_t *handle);
int getUseCaseType(const char *useCase);

static int callMode = AudioSystem::MODE_NORMAL;
// ----------------------------------------------------------------------------

bool platform_is_Fusion3()
{
    char platform[128], baseband[128];
    property_get("ro.board.platform", platform, "");
    property_get("ro.baseband", baseband, "");
    if (!strcmp("msm8960", platform) && !strcmp("mdm", baseband))
        return true;
    else
        return false;
}

int deviceName(alsa_handle_t *handle, unsigned flags, char **value)
{
    int ret = 0;
    char ident[70];

    if (flags & PCM_IN) {
        strlcpy(ident, "CapturePCM/", sizeof(ident));
    } else {
        strlcpy(ident, "PlaybackPCM/", sizeof(ident));
    }
    strlcat(ident, handle->useCase, sizeof(ident));
    ret = snd_use_case_get(handle->ucMgr, ident, (const char **)value);
    ALOGD("Device value returned is %s", (*value));
    return ret;
}

status_t setHardwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_hw_params *params;
    unsigned long bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;
    unsigned int requestedRate = handle->sampleRate;
    int status = 0;
    int channels = handle->channels;
    snd_pcm_format_t format = SNDRV_PCM_FORMAT_S16_LE;

    params = (snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
        ALOGE("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    reqBuffSize = handle->bufferSize;
    ALOGD("setHardwareParams: reqBuffSize %d channels %d sampleRate %d",
         (int) reqBuffSize, handle->channels, handle->sampleRate);

#ifdef SSR_ENABLED
    if (channels == 6) {
        if (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
            || !strncmp(handle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
            ALOGV("HWParams: Use 4 channels in kernel for 5.1(%s) recording ", handle->useCase);
            channels = 4;
            reqBuffSize = DEFAULT_IN_BUFFER_SIZE;
        }
    }
#endif

    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
                   SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    if (handle->format != SNDRV_PCM_FORMAT_S16_LE) {
        if (handle->format == AudioSystem::AMR_NB ||
            handle->format == AudioSystem::AMR_WB ||
            handle->format == AudioSystem::EVRC ||
            handle->format == AudioSystem::EVRCB ||
            handle->format == AudioSystem::EVRCWB)
              format = SNDRV_PCM_FORMAT_SPECIAL;
    }
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
                   format);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                   SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, reqBuffSize);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                   channels * 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                  channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, handle->sampleRate);
    param_set_hw_refine(handle->handle, params);

    if (param_set_hw_params(handle->handle, params)) {
        ALOGE("cannot set hw params");
        return NO_INIT;
    }
    param_dump(params);

    handle->handle->buffer_size = pcm_buffer_size(params);
    handle->handle->period_size = pcm_period_size(params);
    handle->handle->period_cnt = handle->handle->buffer_size/handle->handle->period_size;
    ALOGD("setHardwareParams: buffer_size %d, period_size %d, period_cnt %d",
        handle->handle->buffer_size, handle->handle->period_size,
        handle->handle->period_cnt);
    handle->handle->rate = handle->sampleRate;
    handle->handle->channels = handle->channels;
    handle->periodSize = handle->handle->period_size;
    if (strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_REC) &&
        strcmp(handle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC) &&
        (6 != handle->channels)) {
        //Do not update buffersize for 5.1 recording
        handle->bufferSize = handle->handle->period_size;
    }

    return NO_ERROR;
}

status_t setSoftwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;

    unsigned long periodSize = pcm->period_size;
    int channels = handle->channels;

    params = (snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

#ifdef SSR_ENABLED
    if (channels == 6) {
        if (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
            || !strncmp(handle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
            ALOGV("SWParams: Use 4 channels in kernel for 5.1(%s) recording ", handle->useCase);
            channels = 4;
        }
    }
#endif

    // Get the current software parameters
    params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    if(((!strcmp(handle->useCase,SND_USE_CASE_MOD_PLAY_VOIP)) ||
        (!strcmp(handle->useCase,SND_USE_CASE_VERB_IP_VOICECALL)))){
          ALOGV("setparam:  start & stop threshold for Voip ");
          params->avail_min = handle->channels - 1 ? periodSize/4 : periodSize/2;
          params->start_threshold = periodSize/2;
          params->stop_threshold = INT_MAX;
     } else {
         params->avail_min = periodSize/2;
         params->start_threshold = channels * (periodSize/4);
         params->stop_threshold = INT_MAX;
     }
    params->silence_threshold = 0;
    params->silence_size = 0;

    if (param_set_sw_params(handle->handle, params)) {
        ALOGE("cannot set sw params");
        return NO_INIT;
    }
    return NO_ERROR;
}

void switchDevice(alsa_handle_t *handle, uint32_t devices, uint32_t mode)
{
    const char **mods_list;
    use_case_t useCaseNode;
    unsigned usecase_type = 0;
    bool inCallDevSwitch = false;
    char *rxDevice, *txDevice, ident[70], *use_case = NULL;
    int err = 0, index, mods_size;
    int rx_dev_id, tx_dev_id;
    ALOGV("%s: device %d", __FUNCTION__, devices);

    if ((mode == AudioSystem::MODE_IN_CALL)  || (mode == AudioSystem::MODE_IN_COMMUNICATION)) {
        if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADSET |
                      AudioSystem::DEVICE_IN_WIRED_HEADSET);
        } else if (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        } else if ((devices & AudioSystem::DEVICE_OUT_EARPIECE) ||
                  (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC)) {
            devices = devices | (AudioSystem::DEVICE_IN_BUILTIN_MIC |
                      AudioSystem::DEVICE_OUT_EARPIECE);
        } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            devices = devices | (AudioSystem::DEVICE_IN_BUILTIN_MIC |
                       AudioSystem::DEVICE_OUT_SPEAKER);
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET |
                      AudioSystem::DEVICE_OUT_BLUETOOTH_SCO);
#if 0
        } else if ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_OUT_ANC_HEADSET |
                      AudioSystem::DEVICE_IN_ANC_HEADSET);
        } else if (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE) {
            devices = devices | (AudioSystem::DEVICE_OUT_ANC_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
#endif
        } else if (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
            devices = devices | (AudioSystem::DEVICE_OUT_AUX_DIGITAL |
                      AudioSystem::DEVICE_IN_AUX_DIGITAL);
#if 0
        } else if ((devices & AudioSystem::DEVICE_OUT_PROXY) ||
                  (devices & AudioSystem::DEVICE_IN_PROXY)) {
            devices = devices | (AudioSystem::DEVICE_OUT_PROXY |
                      AudioSystem::DEVICE_IN_PROXY);
#endif
        }
    }
#ifdef SSR_ENABLED
    if ((devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) && ( 6 == handle->channels)) {
        if (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
            || !strncmp(handle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
            ALOGV(" switchDevice , use ssr devices for channels:%d usecase:%s",handle->channels,handle->useCase);
            s_set_flags(SSRQMIC_FLAG);
        }
    }
#endif

    rxDevice = getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL, 0, NULL);
    txDevice = getUCMDevice(devices & AudioSystem::DEVICE_IN_ALL, 1, rxDevice);

    if (rxDevice != NULL) {
        if ((handle->handle) && (((!strncmp(rxDevice, DEVICE_SPEAKER_HEADSET, strlen(DEVICE_SPEAKER_HEADSET))) &&
            ((!strncmp(curRxUCMDevice, DEVICE_HEADPHONES, strlen(DEVICE_HEADPHONES))) ||
            (!strncmp(curRxUCMDevice, DEVICE_HEADSET, strlen(DEVICE_HEADSET))))) ||
            (((!strncmp(curRxUCMDevice, DEVICE_SPEAKER_HEADSET, strlen(DEVICE_SPEAKER_HEADSET))) &&
            ((!strncmp(rxDevice, DEVICE_HEADPHONES, strlen(DEVICE_HEADPHONES))) ||
            (!strncmp(rxDevice, DEVICE_HEADSET, strlen(DEVICE_HEADSET))))))) &&
            ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI, strlen(SND_USE_CASE_VERB_HIFI))) ||
            (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC, strlen(SND_USE_CASE_MOD_PLAY_MUSIC))))) {
            pcm_close(handle->handle);
            handle->handle=NULL;
            handle->rxHandle=NULL;
            pflag = true;
        }
    }

    if ((rxDevice != NULL) && (txDevice != NULL)) {
        if (((strncmp(rxDevice, curRxUCMDevice, MAX_STR_LEN)) ||
             (strncmp(txDevice, curTxUCMDevice, MAX_STR_LEN))) && (mode == AudioSystem::MODE_IN_CALL))
            inCallDevSwitch = true;
    }
    snd_use_case_get(handle->ucMgr, "_verb", (const char **)&use_case);
    mods_size = snd_use_case_get_list(handle->ucMgr, "_enamods", &mods_list);
    if (rxDevice != NULL) {
        if ((strncmp(curRxUCMDevice, "None", 4)) &&
            ((strncmp(rxDevice, curRxUCMDevice, MAX_STR_LEN)) || (inCallDevSwitch == true))) {
            if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                strlen(SND_USE_CASE_VERB_INACTIVE)))) {
                usecase_type = getUseCaseType(use_case);
                if (usecase_type & USECASE_TYPE_RX) {
                    ALOGD("Deroute use case %s type is %d\n", use_case, usecase_type);
                    strlcpy(useCaseNode.useCase, use_case, MAX_STR_LEN);
                    snd_use_case_set(handle->ucMgr, "_verb", SND_USE_CASE_VERB_INACTIVE);
                    mUseCaseList.push_front(useCaseNode);
                }
            }
            if (mods_size) {
                for(index = 0; index < mods_size; index++) {
                    usecase_type = getUseCaseType(mods_list[index]);
                    if (usecase_type & USECASE_TYPE_RX) {
                        ALOGD("Deroute use case %s type is %d\n", mods_list[index], usecase_type);
                        strlcpy(useCaseNode.useCase, mods_list[index], MAX_STR_LEN);
                        snd_use_case_set(handle->ucMgr, "_dismod", mods_list[index]);
                        mUseCaseList.push_back(useCaseNode);
                    }
                }
            }
            snd_use_case_set(handle->ucMgr, "_disdev", curRxUCMDevice);
        }
    }
    if (txDevice != NULL) {
        if ((strncmp(curTxUCMDevice, "None", 4)) &&
            ((strncmp(txDevice, curTxUCMDevice, MAX_STR_LEN)) || (inCallDevSwitch == true))) {
            if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                strlen(SND_USE_CASE_VERB_INACTIVE)))) {
                usecase_type = getUseCaseType(use_case);
                if ((usecase_type & USECASE_TYPE_TX) && (!(usecase_type & USECASE_TYPE_RX))) {
                    ALOGD("Deroute use case %s type is %d\n", use_case, usecase_type);
                    strlcpy(useCaseNode.useCase, use_case, MAX_STR_LEN);
                    snd_use_case_set(handle->ucMgr, "_verb", SND_USE_CASE_VERB_INACTIVE);
                    mUseCaseList.push_front(useCaseNode);
                }
            }
            if (mods_size) {
                for(index = 0; index < mods_size; index++) {
                    usecase_type = getUseCaseType(mods_list[index]);
                    if ((usecase_type & USECASE_TYPE_TX) && (!(usecase_type & USECASE_TYPE_RX))) {
                        ALOGD("Deroute use case %s type is %d\n", mods_list[index], usecase_type);
                        strlcpy(useCaseNode.useCase, mods_list[index], MAX_STR_LEN);
                        snd_use_case_set(handle->ucMgr, "_dismod", mods_list[index]);
                        mUseCaseList.push_back(useCaseNode);
                    }
                }
            }
            snd_use_case_set(handle->ucMgr, "_disdev", curTxUCMDevice);
       }
    }
    ALOGV("%s,rxDev:%s, txDev:%s, curRxDev:%s, curTxDev:%s\n", __FUNCTION__, rxDevice, txDevice, curRxUCMDevice, curTxUCMDevice);
#if 0
    if (mode == AudioSystem::MODE_IN_CALL && platform_is_Fusion3() && (inCallDevSwitch == true)) {
        err = csd_client_disable_device();
        if (err < 0)
        {
            ALOGE("csd_client_disable_device, failed, error %d", err);
        }
    }
#endif

    if (rxDevice != NULL) {
        snd_use_case_set(handle->ucMgr, "_enadev", rxDevice);
        strlcpy(curRxUCMDevice, rxDevice, sizeof(curRxUCMDevice));
#if 0
        if (devices & AudioSystem::DEVICE_OUT_FM)
            s_set_fm_vol(fmVolume);
#endif
    }
    if (txDevice != NULL) {
       snd_use_case_set(handle->ucMgr, "_enadev", txDevice);
       strlcpy(curTxUCMDevice, txDevice, sizeof(curTxUCMDevice));
    }
    for(ALSAUseCaseList::iterator it = mUseCaseList.begin(); it != mUseCaseList.end(); ++it) {
        ALOGD("Route use case %s\n", it->useCase);
        if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
            strlen(SND_USE_CASE_VERB_INACTIVE))) && (!strncmp(use_case, it->useCase, MAX_UC_LEN))) {
            snd_use_case_set(handle->ucMgr, "_verb", it->useCase);
        } else {
            snd_use_case_set(handle->ucMgr, "_enamod", it->useCase);
        }
    }
    if (!mUseCaseList.empty())
        mUseCaseList.clear();
    if (use_case != NULL) {
        free(use_case);
        use_case = NULL;
    }
    ALOGD("switchDevice: curTxUCMDevivce %s curRxDevDevice %s", curTxUCMDevice, curRxUCMDevice);

    if (mode == AudioSystem::MODE_IN_CALL && platform_is_Fusion3() && (inCallDevSwitch == true)) {
        /* get tx acdb id */
        memset(&ident,0,sizeof(ident));
        strlcpy(ident, "ACDBID/", sizeof(ident));
        strlcat(ident, curTxUCMDevice, sizeof(ident));
        tx_dev_id = snd_use_case_get(handle->ucMgr, ident, NULL);

       /* get rx acdb id */
        memset(&ident,0,sizeof(ident));
        strlcpy(ident, "ACDBID/", sizeof(ident));
        strlcat(ident, curRxUCMDevice, sizeof(ident));
        rx_dev_id = snd_use_case_get(handle->ucMgr, ident, NULL);

        if (rx_dev_id == DEVICE_SPEAKER_RX_ACDB_ID && tx_dev_id == DEVICE_HANDSET_TX_ACDB_ID) {
            tx_dev_id = DEVICE_SPEAKER_TX_ACDB_ID;
        }

#if 0
        ALOGV("rx_dev_id=%d, tx_dev_id=%d\n", rx_dev_id, tx_dev_id);
        err = csd_client_enable_device(rx_dev_id, tx_dev_id, mDevSettingsFlag);
        if (err < 0)
        {
            ALOGE("csd_client_disable_device failed, error %d", err);
        }
#endif
    }

    if (rxDevice != NULL) {
        if (pflag && (((!strncmp(rxDevice, DEVICE_SPEAKER_HEADSET, strlen(DEVICE_SPEAKER_HEADSET))) &&
            ((!strncmp(curRxUCMDevice, DEVICE_HEADPHONES, strlen(DEVICE_HEADPHONES))) ||
            (!strncmp(curRxUCMDevice, DEVICE_HEADSET, strlen(DEVICE_HEADSET))))) ||
            (((!strncmp(curRxUCMDevice, DEVICE_SPEAKER_HEADSET, strlen(DEVICE_SPEAKER_HEADSET))) &&
            ((!strncmp(rxDevice, DEVICE_HEADPHONES, strlen(DEVICE_HEADPHONES))) ||
            (!strncmp(rxDevice, DEVICE_HEADSET, strlen(DEVICE_HEADSET))))))) &&
            ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI, strlen(SND_USE_CASE_VERB_HIFI))) ||
            (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC, strlen(SND_USE_CASE_MOD_PLAY_MUSIC))))) {
            s_open(handle);
            pflag = false;
        }
    }

    if (rxDevice != NULL) {
        free(rxDevice);
        rxDevice = NULL;
    }
    if (txDevice != NULL) {
        free(txDevice);
        txDevice = NULL;
    }
}

// ----------------------------------------------------------------------------

static status_t s_init(alsa_device_t *module, ALSAHandleList &list)
{
    ALOGD("s_init: Initializing devices for ALSA module");

    list.clear();

    return NO_ERROR;
}

static status_t s_open(alsa_handle_t *handle)
{
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    /* No need to call s_close for LPA as pcm device open and close is handled by LPAPlayer in stagefright */
    if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) || (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA))
    ||(!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) || (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) {
        ALOGD("s_open: Opening LPA /Tunnel playback");
        return NO_ERROR;
    }

    s_close(handle);

    ALOGD("s_open: handle %p", handle);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    if ((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI)) ||
        (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC))) {
        flags = PCM_OUT;
    } else {
        flags = PCM_IN;
    }
    if (handle->channels == 1) {
        flags |= PCM_MONO;
    } 
#ifdef SSR_ENABLED
    else if (handle->channels == 4 ) {
        flags |= PCM_QUAD;
    } else if (handle->channels == 6 ) {
        if (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
            || !strncmp(handle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
            flags |= PCM_QUAD;
        } else {
            flags |= PCM_5POINT1;
        }
    } 
#endif
    else {
        flags |= PCM_STEREO;
    }
    if (deviceName(handle, flags, &devName) < 0) {
        ALOGE("Failed to get pcm device node: %s", devName);
        return NO_INIT;
    }
    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName);
    } else {
        ALOGE("Failed to get pcm device node");
        return NO_INIT;
    }

    if (!handle->handle) {
        ALOGE("s_open: Failed to initialize ALSA device '%s'", devName);
        free(devName);
        return NO_INIT;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);

    if (err == NO_ERROR) {
        err = setSoftwareParams(handle);
    }

    if(err != NO_ERROR) {
        ALOGE("Set HW/SW params failed: Closing the pcm stream");
        s_standby(handle);
    }

    free(devName);
    return NO_ERROR;
}

static status_t s_start_voip_call(alsa_handle_t *handle)
{

    char* devName;
    char* devName1;
    unsigned flags = 0;
    int err = NO_ERROR;
    uint8_t voc_pkt[VOIP_BUFFER_MAX_SIZE];

    s_close(handle);
    flags = PCM_OUT;
    flags |= PCM_MONO;
    ALOGV("s_open:s_start_voip_call  handle %p", handle);

    if (deviceName(handle, flags, &devName) < 0) {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }

    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName);
    } else {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }

     if (!handle->handle) {
          free(devName);
          ALOGE("s_open: Failed to initialize ALSA device '%s'", devName);
          return NO_INIT;
     }

     if (!pcm_ready(handle->handle)) {
         ALOGE(" pcm ready failed");
     }

     handle->handle->flags = flags;
     err = setHardwareParams(handle);

     if (err == NO_ERROR) {
         err = setSoftwareParams(handle);
     }

     err = pcm_prepare(handle->handle);
     if(err != NO_ERROR) {
         ALOGE("DEVICE_OUT_DIRECTOUTPUT: pcm_prepare failed");
     }

     /* first write required start dsp */
     memset(&voc_pkt,0,sizeof(voc_pkt));
     pcm_write(handle->handle,&voc_pkt,handle->handle->period_size);
     handle->rxHandle = handle->handle;
     free(devName);
     ALOGV("s_open: DEVICE_IN_COMMUNICATION ");
     flags = PCM_IN;
     flags |= PCM_MONO;
     handle->handle = 0;

     if (deviceName(handle, flags, &devName1) < 0) {
        ALOGE("Failed to get pcm device node");
        return NO_INIT;
     }
    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName1);
    } else {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }

     if (!handle->handle) {
         free(devName);
         ALOGE("s_open: Failed to initialize ALSA device '%s'", devName);
         return NO_INIT;
     }

     if (!pcm_ready(handle->handle)) {
        ALOGE(" pcm ready in failed");
     }

     handle->handle->flags = flags;

     err = setHardwareParams(handle);

     if (err == NO_ERROR) {
         err = setSoftwareParams(handle);
     }


     err = pcm_prepare(handle->handle);
     if(err != NO_ERROR) {
         ALOGE("DEVICE_IN_COMMUNICATION: pcm_prepare failed");
     }

     /* first read required start dsp */
     memset(&voc_pkt,0,sizeof(voc_pkt));
     pcm_read(handle->handle,&voc_pkt,handle->handle->period_size);
     return NO_ERROR;
}

static status_t s_start_voice_call(alsa_handle_t *handle)
{
    char* devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    ALOGD("s_start_voice_call: handle %p", handle);
    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_MONO;
    if (deviceName(handle, flags, &devName) < 0) {
        ALOGE("Failed to get pcm device node");
        return NO_INIT;
    }
    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName);
    } else {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }
    if (!handle->handle) {
        ALOGE("s_start_voicecall: could not open PCM device");
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_voice_call: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_voice_call: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_voice_call: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        ALOGE("s_start_voice_call:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    // Store the PCM playback device pointer in rxHandle
    handle->rxHandle = handle->handle;
    free(devName);

    // Open PCM capture device
    flags = PCM_IN | PCM_MONO;
    if (deviceName(handle, flags, &devName) < 0) {
        ALOGE("Failed to get pcm device node");
        goto Error;
    }
    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName);
    } else {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }
    if (!handle->handle) {
        free(devName);
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_voice_call: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_voice_call: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_voice_call: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        ALOGE("s_start_voice_call:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    if (platform_is_Fusion3()) {
#if 0
        err = csd_client_start_voice();
        if (err < 0) {
            ALOGE("s_start_voice_call: csd_client error %d\n", err);
            goto Error;
        }
#endif
    }

    free(devName);
    return NO_ERROR;

Error:
    ALOGE("s_start_voice_call: Failed to initialize ALSA device '%s'", devName);
    free(devName);
    s_close(handle);
    return NO_INIT;
}

static status_t s_start_fm(alsa_handle_t *handle)
{
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    ALOGE("s_start_fm: handle %p", handle);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_STEREO;
    if (deviceName(handle, flags, &devName) < 0) {
        ALOGE("Failed to get pcm device node");
        goto Error;
    }
    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName);
    } else {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }
    if (!handle->handle) {
        ALOGE("s_start_fm: could not open PCM device");
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        ALOGE("s_start_fm: SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    // Store the PCM playback device pointer in rxHandle
    handle->rxHandle = handle->handle;
    free(devName);

    // Open PCM capture device
    flags = PCM_IN | PCM_STEREO;
    if (deviceName(handle, flags, &devName) < 0) {
        ALOGE("Failed to get pcm device node");
        goto Error;
    }
    if (devName != NULL) {
        handle->handle = pcm_open(flags, (char*)devName);
    } else {
         ALOGE("Failed to get pcm device node");
         return NO_INIT;
    }
    if (!handle->handle) {
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        ALOGE("s_start_fm: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        ALOGE("s_start_fm: SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    s_set_fm_vol(fmVolume);
    free(devName);
    return NO_ERROR;

Error:
    free(devName);
    s_close(handle);
    return NO_INIT;
}

static status_t s_set_fm_vol(int value)
{
    status_t err = NO_ERROR;

    ALSAControl control("/dev/snd/controlC0");
    control.set("Internal FM RX Volume",value,0);
    fmVolume = value;

    return err;
}

static status_t s_set_lpa_vol(int value)
{
    status_t err = NO_ERROR;

    ALSAControl control("/dev/snd/controlC0");
    control.set("LPA RX Volume",value,0);

    return err;
}

static status_t s_start(alsa_handle_t *handle)
{
    status_t err = NO_ERROR;

    if(!handle->handle) {
        ALOGE("No active PCM driver to start");
        return err;
    }

    err = pcm_prepare(handle->handle);

    return err;
}

static status_t s_close(alsa_handle_t *handle)
{
    int ret;
    status_t err = NO_ERROR;
     struct pcm *h = handle->rxHandle;

    handle->rxHandle = 0;
    ALOGD("s_close: handle %p h %p", handle, h);
    if (h) {
        ALOGV("s_close rxHandle\n");
        err = pcm_close(h);
        if(err != NO_ERROR) {
            ALOGE("s_close: pcm_close failed for rxHandle with err %d", err);
        }
    }

    h = handle->handle;
    handle->handle = 0;

    if (h) {
          ALOGV("s_close handle h %p\n", h);
        err = pcm_close(h);
        if(err != NO_ERROR) {
            ALOGE("s_close: pcm_close failed for handle with err %d", err);
        }

        if ((!strcmp(handle->useCase, SND_USE_CASE_VERB_VOICECALL) ||
             !strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_VOICE)) &&
            platform_is_Fusion3()) {
#if 0
            err = csd_client_stop_voice();
            if (err < 0) {
                ALOGE("s_close: csd_client error %d\n", err);
            }
#endif
        }

        disableDevice(handle);
    } else if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))){
        disableDevice(handle);
    }

    return err;
}

/*
    this is same as s_close, but don't discard
    the device/mode info. This way we can still
    close the device, hit idle and power-save, reopen the pcm
    for the same device/mode after resuming
*/
static status_t s_standby(alsa_handle_t *handle)
{
    int ret;
    status_t err = NO_ERROR;  
    struct pcm *h = handle->rxHandle;
    handle->rxHandle = 0;
    ALOGD("s_standby: handle %p h %p", handle, h);
    if (h) {
        ALOGE("s_standby  rxHandle\n");
        err = pcm_close(h);
        if(err != NO_ERROR) {
            ALOGE("s_standby: pcm_close failed for rxHandle with err %d", err);
        }
    }

    h = handle->handle;
    handle->handle = 0;

    if (h) {
          ALOGE("s_standby handle h %p\n", h);
        err = pcm_close(h);
        if(err != NO_ERROR) {
            ALOGE("s_standby: pcm_close failed for handle with err %d", err);
        }
        disableDevice(handle);
    } else if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) {
        disableDevice(handle);
    }

    return err;
}

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode)
{
    status_t status = NO_ERROR;

    ALOGD("s_route: devices 0x%x in mode %d", devices, mode);
    callMode = mode;
    switchDevice(handle, devices, mode);
    return status;
}

int getUseCaseType(const char *useCase)
{
    ALOGE("use case is %s\n", useCase);
    if (!strncmp(useCase, SND_USE_CASE_VERB_HIFI,
           strlen(SND_USE_CASE_VERB_HIFI)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
            strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
            strlen(SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_DIGITAL_RADIO,
            strlen(SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_MUSIC,
            strlen(SND_USE_CASE_MOD_PLAY_MUSIC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_LPA,
            strlen(SND_USE_CASE_MOD_PLAY_LPA)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
            strlen(SND_USE_CASE_MOD_PLAY_TUNNEL)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_FM,
            strlen(SND_USE_CASE_MOD_PLAY_FM))) {
        return USECASE_TYPE_RX;
    } else if (!strncmp(useCase, SND_USE_CASE_VERB_HIFI_REC,
            strlen(SND_USE_CASE_VERB_HIFI_REC)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_FM_REC,
            strlen(SND_USE_CASE_VERB_FM_REC)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_FM_A2DP_REC,
            strlen(SND_USE_CASE_VERB_FM_A2DP_REC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC,
            strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_FM,
            strlen(SND_USE_CASE_MOD_CAPTURE_FM)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM,
            strlen(SND_USE_CASE_MOD_CAPTURE_A2DP_FM))) {
        return USECASE_TYPE_TX;
    } else if (!strncmp(useCase, SND_USE_CASE_VERB_VOICECALL,
            strlen(SND_USE_CASE_VERB_VOICECALL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_IP_VOICECALL,
            strlen(SND_USE_CASE_VERB_IP_VOICECALL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_DL_REC,
            strlen(SND_USE_CASE_VERB_DL_REC)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_UL_DL_REC,
            strlen(SND_USE_CASE_VERB_UL_DL_REC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_VOICE,
            strlen(SND_USE_CASE_MOD_PLAY_VOICE)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_VOIP,
            strlen(SND_USE_CASE_MOD_PLAY_VOIP)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
            strlen(SND_USE_CASE_MOD_CAPTURE_VOICE_DL)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
            strlen(SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
            strlen(SND_USE_CASE_MOD_CAPTURE_VOICE)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_VOLTE,
            strlen(SND_USE_CASE_VERB_VOLTE)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_VOLTE,
            strlen(SND_USE_CASE_MOD_PLAY_VOLTE))) {
        return (USECASE_TYPE_RX | USECASE_TYPE_TX);
    } else {
        ALOGE("unknown use case %s\n", useCase);
        return 0;
    }
}

static void disableDevice(alsa_handle_t *handle)
{
    unsigned usecase_type = 0;
    int i, mods_size;
    char *useCase;
    const char **mods_list;

    snd_use_case_get(handle->ucMgr, "_verb", (const char **)&useCase);
    if (useCase != NULL) {
        if (!strncmp(useCase, handle->useCase, MAX_UC_LEN)) {
            snd_use_case_set(handle->ucMgr, "_verb", SND_USE_CASE_VERB_INACTIVE);
        } else {
            snd_use_case_set(handle->ucMgr, "_dismod", handle->useCase);
        }
        free(useCase);
        snd_use_case_get(handle->ucMgr, "_verb", (const char **)&useCase);
        if (strncmp(useCase, SND_USE_CASE_VERB_INACTIVE,
               strlen(SND_USE_CASE_VERB_INACTIVE)))
            usecase_type |= getUseCaseType(useCase);
        mods_size = snd_use_case_get_list(handle->ucMgr, "_enamods", &mods_list);
        ALOGE("Number of modifiers %d\n", mods_size);
        if (mods_size) {
            for(i = 0; i < mods_size; i++) {
                ALOGE("index %d modifier %s\n", i, mods_list[i]);
                usecase_type |= getUseCaseType(mods_list[i]);
            }
        }
        ALOGE("usecase_type is %d\n", usecase_type);
        if (!(usecase_type & USECASE_TYPE_TX) && (strncmp(curTxUCMDevice, "None", 4)))
            snd_use_case_set(handle->ucMgr, "_disdev", curTxUCMDevice);
        if (!(usecase_type & USECASE_TYPE_RX) && (strncmp(curRxUCMDevice, "None", 4)))
            snd_use_case_set(handle->ucMgr, "_disdev", curRxUCMDevice);
    } else {
        ALOGE("Invalid state, no valid use case found to disable");
    }
    free(useCase);
}

char *getUCMDevice(uint32_t devices, int input, char *rxDevice)
{
    if (!input) {
        if (!(mDevSettingsFlag & TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) { 
#if 0 
             ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
#endif
             if (mDevSettingsFlag & TTY_VCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_RX);
             } else if (mDevSettingsFlag & TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_RX);
             } else if (mDevSettingsFlag & TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HANDSET_RX); /* HANDSET RX */
             }
        }else if ((devices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET) ||
                  (devices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)) {
             return strdup(SND_USE_CASE_DEV_PROXY_RX); /* PROXY RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_SPEAKER_ANC_HEADSET); /* COMBO SPEAKER+ANC HEADSET RX */
            } else {
                return strdup(SND_USE_CASE_DEV_SPEAKER_HEADSET); /* COMBO SPEAKER+HEADSET RX */
            }
#if 0
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_ANC_HEADSET); /* COMBO SPEAKER+ANC HEADSET RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                 (devices & AudioSystem::DEVICE_OUT_FM_TX)) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_FM_TX); /* COMBO SPEAKER+FM_TX RX */
#endif
        } else if (devices & AudioSystem::DEVICE_OUT_EARPIECE) {
            return strdup(SND_USE_CASE_DEV_EARPIECE); /* HANDSET RX */
        } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                   (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_ANC_HEADSET); /* ANC HEADSET RX */
            } else {
                return strdup(SND_USE_CASE_DEV_HEADPHONES); /* HEADSET RX */
            }
#if 0
        } else if ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE)) {
            return strdup(SND_USE_CASE_DEV_ANC_HEADSET); /* ANC HEADSET RX */
#endif
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                  (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                  (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT)) {
            if (btsco_samplerate == BTSCO_RATE_16KHZ)
                return strdup(SND_USE_CASE_DEV_BTSCO_WB_RX); /* BTSCO RX*/
            else
                return strdup(SND_USE_CASE_DEV_BTSCO_NB_RX); /* BTSCO RX*/
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES) ||
//commented JB
/*                   (devices & AudioSystem::DEVICE_OUT_DIRECTOUTPUT) ||*/
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER)) {
            /* Nothing to be done, use current active device */
            if (strncmp(curRxUCMDevice, "None", 4)) {
                return strdup(curRxUCMDevice);
            }
        } else if (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
            return strdup(SND_USE_CASE_DEV_HDMI); /* HDMI RX */
#if 0
        } else if (devices & AudioSystem::DEVICE_OUT_PROXY) {
            return strdup(SND_USE_CASE_DEV_PROXY_RX); /* PROXY RX */
        } else if (devices & AudioSystem::DEVICE_OUT_FM_TX) {
            return strdup(SND_USE_CASE_DEV_FM_TX); /* FM Tx */
#endif
        } else if (devices & AudioSystem::DEVICE_OUT_DEFAULT) {
            return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
        } else {
            ALOGD("No valid output device: %u", devices);
        }
    } else {
        if (!(mDevSettingsFlag & TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET))) { 
#if 0
            ||(devices & AudioSystem::DEVICE_IN_ANC_HEADSET))) {
#endif
             if (mDevSettingsFlag & TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_TX);
             } else if (mDevSettingsFlag & TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_TX);
             } else if (mDevSettingsFlag & TTY_VCO) {
#if 0
                 if (!strncmp(mic_type, "analog", 6)) {
                     return strdup(SND_USE_CASE_DEV_TTY_HANDSET_ANAALOG_TX);
                 } else {
                     return strdup(SND_USE_CASE_DEV_TTY_HANDSET_TX);
                 }
#else
                 return strdup(SND_USE_CASE_DEV_TTY_HANDSET_TX);
#endif
             }
        } else if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                if (mDevSettingsFlag & DMIC_FLAG) {
                    if (((rxDevice != NULL) &&
                        !strncmp(rxDevice, SND_USE_CASE_DEV_SPEAKER,
                        (strlen(SND_USE_CASE_DEV_SPEAKER)+1))) ||
                        !strncmp(curRxUCMDevice, SND_USE_CASE_DEV_SPEAKER,
                        (strlen(SND_USE_CASE_DEV_SPEAKER)+1))) {
                        if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                            return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                        } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                            return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                        }
                    } else {
                        if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                            return strdup(SND_USE_CASE_DEV_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                        } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                            return strdup(SND_USE_CASE_DEV_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                        }
                    }
                } else if (mDevSettingsFlag & QMIC_FLAG){
                    return strdup(SND_USE_CASE_DEV_QUAD_MIC);
                } 
#ifdef SSR_ENABLED
                else if (mDevSettingsFlag & SSRQMIC_FLAG){
                    ALOGV("return SSRQMIC_FLAG: 0x%x devices:0x%x",mDevSettingsFlag,devices);
                    // Mapping for quad mic input device.
                    return strdup(SND_USE_CASE_DEV_SSR_QUAD_MIC); /* SSR Quad MIC */
                } 
#endif
                else {
                    return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                }
            }
        } else if (devices & AudioSystem::DEVICE_IN_AUX_DIGITAL) {
            return strdup(SND_USE_CASE_DEV_HDMI_TX); /* HDMI TX */
#if 0
        } else if ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            return strdup(SND_USE_CASE_DEV_HEADSET); /* HEADSET TX */
#endif
        } else if (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
             if (btsco_samplerate == BTSCO_RATE_16KHZ)
                 return strdup(SND_USE_CASE_DEV_BTSCO_WB_TX); /* BTSCO TX*/
             else
                 return strdup(SND_USE_CASE_DEV_BTSCO_NB_TX); /* BTSCO TX*/
#if 0
        } else if ((devices & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_PROXY)) {
            return strdup(SND_USE_CASE_DEV_PROXY_TX); /* PROXY TX */
#endif
        } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
                   (devices & AudioSystem::DEVICE_IN_VOICE_CALL)) {
            /* Nothing to be done, use current active device */
            if (strncmp(curTxUCMDevice, "None", 4)) {
                return strdup(curTxUCMDevice);
            }
#if 0
        } else if ((devices & AudioSystem::DEVICE_IN_FM_RX) ||
                   (devices & AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
            /* Nothing to be done, use current tx device or set dummy device */
            if (strncmp(curTxUCMDevice, "None", 4)) {
                return strdup(curTxUCMDevice);
            } else {
                return strdup(SND_USE_CASE_DEV_DUMMY_TX);
            }
#endif
        } else if ((devices & AudioSystem::DEVICE_IN_AMBIENT) ||
                   (devices & AudioSystem::DEVICE_IN_BACK_MIC)) {
            ALOGI("No proper mapping found with UCM device list, setting default");
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
            }
        } else {
            ALOGD("No valid input device: %u", devices);
        }
    }
    return NULL;
}

void s_set_voice_volume(int vol)
{
    int err = 0;
    ALOGD("s_set_voice_volume: volume %d", vol);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voice Rx Volume", vol, 0);

    if (platform_is_Fusion3()) {
#if 0
        err = csd_client_volume(vol);
        if (err < 0) {
            ALOGE("s_set_voice_volume: csd_client error %d", err);
        } 
#endif
    }
}

void s_set_volte_volume(int vol)
{
    ALOGD("s_set_volte_volume: volume %d", vol);
    ALSAControl control("/dev/snd/controlC0");
    control.set("VoLTE Rx Volume", vol, 0);
}


void s_set_voip_volume(int vol)
{
    ALOGD("s_set_voip_volume: volume %d", vol);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voip Rx Volume", vol, 0);
}
void s_set_mic_mute(int state)
{
    int err = 0;
    ALOGD("s_set_mic_mute: state %d", state);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voice Tx Mute", state, 0);

    if (platform_is_Fusion3()) {
#if 0
        err = csd_client_mic_mute(state);
        if (err < 0) {
            ALOGE("s_set_mic_mute: csd_client error %d", err);
        }
#endif
    }
}
void s_set_volte_mic_mute(int state)
{
    ALOGD("s_set_volte_mic_mute: state %d", state);
    ALSAControl control("/dev/snd/controlC0");
    control.set("VoLTE Tx Mute", state, 0);
}

void s_set_voip_mic_mute(int state)
{
    ALOGD("s_set_voip_mic_mute: state %d", state);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voip Tx Mute", state, 0);
}

void s_set_voip_config(int mode, int rate)
{
    ALOGD("s_set_voip_config: mode %d,rate %d", mode, rate);
    ALSAControl control("/dev/snd/controlC0");
    char** setValues;
    setValues = (char**)malloc(2*sizeof(char*));
    if (setValues == NULL) {
          return;
    }
    setValues[0] = (char*)malloc(4*sizeof(char));
    if (setValues[0] == NULL) {
          free(setValues);
          return;
    }

    setValues[1] = (char*)malloc(8*sizeof(char));
    if (setValues[1] == NULL) {
          free(setValues);
          free(setValues[0]);
          return;
    }

    sprintf(setValues[0], "%d",mode);
    sprintf(setValues[1], "%d",rate);

    control.setext("Voip Mode Rate Config", 2, setValues);
    free(setValues[1]);
    free(setValues[0]);
    free(setValues);
    return;
}

void s_set_btsco_rate(int rate)
{
    btsco_samplerate = rate;
}

void s_enable_wide_voice(bool flag)
{
    int err = 0;

    ALOGD("s_enable_wide_voice: flag %d", flag);
    ALSAControl control("/dev/snd/controlC0");
    if(flag == true) {
        control.set("Widevoice Enable", 1, 0);
    } else {
        control.set("Widevoice Enable", 0, 0);
    }

    if (platform_is_Fusion3()) {
#if 0
        err == csd_client_wide_voice(flag);
        if (err < 0) {
            ALOGE("s_enable_wide_voice: csd_client error %d", err);
        }
#endif
    }
}

void s_set_voc_rec_mode(uint8_t mode)
{
    ALOGD("s_set_voc_rec_mode: mode %d", mode);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Incall Rec Mode", mode, 0);
}

void s_enable_fens(bool flag)
{
    int err = 0;

    ALOGD("s_enable_fens: flag %d", flag);
    ALSAControl control("/dev/snd/controlC0");
    if(flag == true) {
        control.set("FENS Enable", 1, 0);
    } else {
        control.set("FENS Enable", 0, 0);
    }

    if (platform_is_Fusion3()) {
#if 0
        err = csd_client_fens(flag);
        if (err < 0) {
            ALOGE("s_enable_fens: csd_client error %d", err);
        }
#endif
    }
}

void s_enable_slow_talk(bool flag)
{
    int err = 0;

    ALOGD("s_enable_slow_talk: flag %d", flag);
    ALSAControl control("/dev/snd/controlC0");
    if(flag == true) {
        control.set("Slowtalk Enable", 1, 0);
    } else {
        control.set("Slowtalk Enable", 0, 0);
    }

    if (platform_is_Fusion3()) {
#if 0
        err = csd_client_slow_talk(flag);
        if (err < 0) {
            ALOGE("s_enable_slow_talk: csd_client error %d", err);
        }
#endif
    }
}

void s_set_flags(uint32_t flags)
{
    ALOGV("s_set_flags: flags %d", flags);
    mDevSettingsFlag = flags;
}

static status_t s_set_compressed_vol(int value)
{
    status_t err = NO_ERROR;

    ALSAControl control("/dev/snd/controlC0");
    control.set("COMPRESSED RX Volume",value,0);

    return err;
}

}
