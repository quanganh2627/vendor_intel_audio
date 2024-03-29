/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <linux/ioctl.h>
#include <sys/time.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <sound/asound.h>
#include <tinyalsa/asoundlib.h>

#include <audio_utils/resampler.h>

#include "audio_route.h"

#define CARD_CTRL_PATH "/dev/snd/controlC%u"
#define MAX_CARDS 4
#define MAX_INTERNAL_CARDS 2

#define PCH_ID_STR "PCH"
#define PCH_DEVICE 0
#define PCH_DEVICE_SCO 2

#define HDMI_ID_STR "MID"
#define HDMI_DEVICE 3

#define INTERNAL_DRIVER_STR "HDA-Intel"

#define USB_DRIVER_STR "USB-Audio"
#define USB_DEVICE 0
#define USB_MIC_CAPTURE_SWITCH_STR "Mic Capture Switch"
#define USB_MIC_CAPTURE_SWITCH_OFF "0"
#define USB_MIC_CAPTURE_SWITCH_ON "1"
#define USB_MIC_CAPTURE_VOLUME_STR "Mic Capture Volume"
#define USB_MIC_CAPTURE_VOLUME_DEFAULT "10"

#define CODEC_CHIP_NAME_PATH "/sys/class/sound/hwC%uD0/chip_name"

#define OTHER_DEVICE 0

#define MAX_RETRIES 100

#define CARD_SLOT_NOT_FOUND -1

#define OUT_PERIOD_SIZE 512
#define OUT_SHORT_PERIOD_COUNT 2
#define OUT_LONG_PERIOD_COUNT 8
#define OUT_SAMPLING_RATE 44100

#define IN_PERIOD_SIZE 1024
#define IN_PERIOD_COUNT 4
#define IN_SAMPLING_RATE 44100

#define SCO_PERIOD_SIZE 256
#define SCO_PERIOD_COUNT 4
#define SCO_SAMPLING_RATE 8000

/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 2000
#define MAX_WRITE_SLEEP_US ((OUT_PERIOD_SIZE * OUT_SHORT_PERIOD_COUNT * 1000000) \
                                / OUT_SAMPLING_RATE)

enum {
    OUT_BUFFER_TYPE_UNKNOWN,
    OUT_BUFFER_TYPE_SHORT,
    OUT_BUFFER_TYPE_LONG,
};

/* Enumeration of all MAX_CARDS possible cards */
enum {
    AUDIO_CARD_PCH = 0,
    AUDIO_CARD_HDMI = 1,
    AUDIO_CARD_USB = 2,
    AUDIO_CARD_OTHER = 3,
};

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_LONG_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = OUT_PERIOD_SIZE * OUT_SHORT_PERIOD_COUNT,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
};

struct pcm_config pcm_config_usb_in = {
    .channels = 1,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
};

struct pcm_config pcm_config_sco = {
    .channels = 1,
    .rate = SCO_SAMPLING_RATE,
    .period_size = SCO_PERIOD_SIZE,
    .period_count = SCO_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct audio_card {
    int card_slot;
    int device;
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    unsigned int out_device;
    unsigned int in_device;
    bool standby;
    bool mic_mute;
    struct audio_route *ar;
    int orientation;
    bool screen_off;

    struct audio_card card[MAX_CARDS];
    int card_out_index;
    int card_in_index;

    struct stream_out *active_out;
    struct stream_in *active_in;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config *pcm_config;
    bool standby;

    struct resampler_itfe *resampler;
    int16_t *buffer;
    size_t buffer_frames;

    int write_threshold;
    int cur_write_threshold;
    int buffer_type;

    struct audio_device *dev;
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config *pcm_config;
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t buffer_size;
    size_t frames_in;
    int read_status;

    struct audio_device *dev;
};

enum {
    ORIENTATION_LANDSCAPE,
    ORIENTATION_PORTRAIT,
    ORIENTATION_SQUARE,
    ORIENTATION_UNDEFINED,
};

static int init_cards_and_route(struct audio_device *adev, bool report_card_errors);
static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static size_t out_get_buffer_size(const struct audio_stream *stream);
static audio_format_t out_get_format(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static size_t in_get_buffer_size(const struct audio_stream *stream);
static audio_format_t in_get_format(const struct audio_stream *stream);
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer);
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer);

/*
 * NOTE: when multiple mutexes have to be acquired, always take the
 * audio_device mutex first, followed by the stream_in and/or
 * stream_out mutexes.
 */

/* Helper functions */

static void retrieve_codec_name(char *codec_name_path, char *codec_name, int size)
{
    int fd, cnt;

    fd = open(codec_name_path, O_RDONLY);
    if (fd == -1) {
        ALOGE("Failed to open %s", codec_name_path);
        /* If no codec name file, then use unknown. */
        return;
    } else {
        cnt = read(fd, codec_name, size);
        if (cnt <= 0) {
            ALOGE("Failed to read vendor name");
        } else {
           codec_name[cnt-1] = '\0';
        }
        close(fd);
    }
}

static void find_card_slot(struct audio_device *adev)
{
    int slot_num;
    int internal_cards_found;
    int retval, fd;
    char control_path[PATH_MAX];
    char codec_name_path[PATH_MAX];
    char codec_name[PATH_MAX];
    char error_str[255];
    struct snd_ctl_card_info card_info;

    adev->card[AUDIO_CARD_HDMI].card_slot = CARD_SLOT_NOT_FOUND;
    adev->card[AUDIO_CARD_PCH].card_slot = CARD_SLOT_NOT_FOUND;
    adev->card[AUDIO_CARD_USB].card_slot = CARD_SLOT_NOT_FOUND;
    adev->card[AUDIO_CARD_OTHER].card_slot = CARD_SLOT_NOT_FOUND;

    internal_cards_found = 0;

        for (slot_num = 0; slot_num < MAX_CARDS; slot_num++) {
            if (internal_cards_found == MAX_INTERNAL_CARDS)
                return;

            snprintf(control_path, sizeof(control_path), CARD_CTRL_PATH,
                     slot_num);

            fd = open(control_path, O_RDWR);
            ALOGV("[%d][%d]find_card_slot: open %s fd=%d",
                  slot_num, internal_cards_found, control_path, fd);
            if (fd == -1) {
                strerror_r(errno, error_str, sizeof(error_str));
                ALOGE("%s", error_str);
            }
            else {
                if ((retval = ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO,
                                    &card_info)) < 0) {
                    ALOGE("find_card_slot: ioctl() failed. retval=%d", retval);
                    close(fd);
                    continue;
                }
                close(fd);
                if (strncmp(INTERNAL_DRIVER_STR, (char *) &card_info.driver[0],
                            strlen(INTERNAL_DRIVER_STR)) == 0) {
                    snprintf(codec_name_path, sizeof(codec_name_path), CODEC_CHIP_NAME_PATH, slot_num);
                    retrieve_codec_name((char*) &codec_name_path, (char*) &codec_name, sizeof(codec_name));
                    if ((strncmp(codec_name, "ALC262", strlen(codec_name)) == 0) ||
                        (strncmp(codec_name, "ALC283", strlen(codec_name)) == 0)) {
                        if (strncmp(HDMI_ID_STR, (char *) card_info.id, strlen(HDMI_ID_STR)) == 0) {
                            if (adev->card[AUDIO_CARD_PCH].card_slot == CARD_SLOT_NOT_FOUND) {
                                adev->card[AUDIO_CARD_PCH].card_slot = slot_num;
                                adev->card[AUDIO_CARD_PCH].device = PCH_DEVICE;
                                adev->card[AUDIO_CARD_HDMI].card_slot = slot_num;
                                adev->card[AUDIO_CARD_HDMI].device = HDMI_DEVICE;
                                ALOGV("Set PCH slot %d", slot_num);
                            }
                            return;
                        }
                    } else {
                        if ((strncmp(HDMI_ID_STR, (char *) card_info.id,
                                strlen(HDMI_ID_STR)) == 0)) {
                            if (adev->card[AUDIO_CARD_HDMI].card_slot == CARD_SLOT_NOT_FOUND) {
                                adev->card[AUDIO_CARD_HDMI].card_slot = slot_num;
                                adev->card[AUDIO_CARD_HDMI].device = HDMI_DEVICE;
                                internal_cards_found++;
                                ALOGV("Set HDMI slot %d", slot_num);
                            }
                        } else if (strncmp(PCH_ID_STR, (char *) card_info.id, strlen(PCH_ID_STR)) == 0) {
                            if (adev->card[AUDIO_CARD_PCH].card_slot ==
                                CARD_SLOT_NOT_FOUND) {
                                adev->card[AUDIO_CARD_PCH].card_slot = slot_num;
                                adev->card[AUDIO_CARD_PCH].device = PCH_DEVICE;
                                internal_cards_found++;
                                ALOGV("Set PCH slot %d", slot_num);
                            }
                        }
                    }
                }
            }
        }
}

static bool find_usb_card_slot(struct audio_device *adev)
{
    int slot_num;
    int retval, fd;
    char control_path[PATH_MAX];
    char error_str[255];
    struct snd_ctl_card_info card_info;

    adev->card[AUDIO_CARD_USB].card_slot = CARD_SLOT_NOT_FOUND;

    for (slot_num = 0; (slot_num < MAX_CARDS); slot_num++) {
        if (adev->card[AUDIO_CARD_PCH].card_slot == slot_num)
            continue;
        if (adev->card[AUDIO_CARD_HDMI].card_slot == slot_num)
            continue;

        snprintf(control_path, sizeof(control_path), CARD_CTRL_PATH,
                 slot_num);
        fd = open(control_path, O_RDWR);
        ALOGV("find_usb_card_slot: open %s fd=%d", control_path, fd);
        if (fd == -1) {
            strerror_r(errno, error_str, sizeof(error_str));
            ALOGE("%s", error_str);
        }
        else {
            if ((retval = ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO,
                                &card_info)) < 0) {
                ALOGE("find_usb_card_slot: ioctl() failed. retval=%d",
                      retval);
                close(fd);
                continue;
            }
            close(fd);
            if (strncmp(USB_DRIVER_STR, (char *) card_info.driver,
                        strlen(USB_DRIVER_STR)) == 0) {
                adev->card[AUDIO_CARD_USB].card_slot = slot_num;
                adev->card[AUDIO_CARD_USB].device = USB_DEVICE;
                return(true);
            }
        }
    }

    return(false);
}

static void select_devices(struct audio_device *adev) {
    int headphone_on;
    int headset_on;
    int speaker_on;
    int docked;
    int main_mic_on;
    int hdmi_on;
    int usb_out_on;
    int usb_in_on;
    int ret;
    ret =  init_cards_and_route(adev, true);
    if (ret < 0){
        return;
    }

    headphone_on = adev->out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    headset_on = ((adev->out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) |
                 (adev->in_device & AUDIO_DEVICE_IN_WIRED_HEADSET));
    speaker_on = adev->out_device & AUDIO_DEVICE_OUT_SPEAKER;
    docked = adev->out_device & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
    hdmi_on = adev->out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL;
    usb_out_on = adev->out_device & AUDIO_DEVICE_OUT_USB_DEVICE;
    usb_in_on = adev->in_device & AUDIO_DEVICE_IN_USB_DEVICE;
    main_mic_on = adev->in_device & AUDIO_DEVICE_IN_BUILTIN_MIC;

    reset_mixer_state(adev->ar);

    if (headphone_on || headset_on) {
        audio_route_apply_path(adev->ar, "headphone");
        adev->card_out_index = AUDIO_CARD_PCH;
    }
    if (speaker_on) {
        audio_route_apply_path(adev->ar, "speaker");
        adev->card_out_index = AUDIO_CARD_PCH;
    }
    if (docked) {
        audio_route_apply_path(adev->ar, "dock");
        adev->card_out_index = AUDIO_CARD_PCH;
    }
    if (hdmi_on) {
        audio_route_apply_path(adev->ar, "hdmi");
        adev->card_out_index = AUDIO_CARD_HDMI;
    }
    if (usb_out_on || usb_in_on) {
        find_usb_card_slot(adev);
        audio_route_apply_path(adev->ar, "usb");
        if (usb_out_on) {
            adev->card_out_index = AUDIO_CARD_USB;
        }
        if (usb_in_on) {
            adev->card_in_index = AUDIO_CARD_USB;
            audio_route_control_set_number((unsigned int)
                  adev->card[adev->card_in_index].card_slot,
                  USB_MIC_CAPTURE_SWITCH_STR, USB_MIC_CAPTURE_SWITCH_ON);
            audio_route_control_set_number((unsigned int)
                  adev->card[adev->card_in_index].card_slot,
                  USB_MIC_CAPTURE_VOLUME_STR, USB_MIC_CAPTURE_VOLUME_DEFAULT);
        }
    }
    if (main_mic_on || headset_on) {
        if (adev->orientation == ORIENTATION_LANDSCAPE)
            audio_route_apply_path(adev->ar, "main-mic-left");
        else
            audio_route_apply_path(adev->ar, "main-mic-top");
        adev->card_in_index = AUDIO_CARD_PCH;
    }

    update_mixer_state(adev->ar);

    ALOGV("hp=%c speaker=%c dock=%c hdmi=%c",
      headphone_on ? 'y' : 'n',
      speaker_on ? 'y' : 'n',
      docked ? 'y' : 'n', hdmi_on ? 'y' : 'n');
    ALOGV("  usb-out=%c usb-in=%c main-mic=%c",
      usb_out_on ? 'y' : 'n', usb_in_on ? 'y' : 'n',
      main_mic_on ? 'y' : 'n');
}

/* must be called with hw device and output stream mutexes locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_out = NULL;
        if (out->resampler) {
            release_resampler(out->resampler);
            out->resampler = NULL;
        }
        if (out->buffer) {
            free(out->buffer);
            out->buffer = NULL;
        }
        out->standby = true;
    }
}

static int select_card(int card, unsigned int device, int d)
{
    int i;
    char dname[32];

    if (card == CARD_SLOT_NOT_FOUND) {
        ALOGE("no pcm card found!");
        return(-1);
    }

    snprintf(dname, sizeof(dname), "/dev/snd/pcmC%dD%d%c", card, device,
      (d == PCM_IN) ? 'c' : 'p');
    if (!access(dname, R_OK | W_OK)) {
        ALOGD("found %s %s", (d == PCM_IN) ? "in" : "out", dname);
        return(card);
    }
    ALOGE("no pcm card found!");
    return(-1);
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_in = NULL;
        if (in->resampler) {
            release_resampler(in->resampler);
            in->resampler = NULL;
        }
        if (in->buffer) {
            free(in->buffer);
            in->buffer = NULL;
        }
        in->standby = true;
    }
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int card;
    unsigned int device;
    int ret;
    ret =  init_cards_and_route(adev, true);
    if (ret < 0)
        return ret;

    /*
     * Due to the lack of sample rate converters in the SoC,
     * it greatly simplifies things to have only the main
     * (speaker/headphone) PCM or the BC SCO PCM open at
     * the same time.
     */
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        return -1;
    } else {
        card = adev->card[adev->card_out_index].card_slot;
        device = adev->card[adev->card_out_index].device;
        out->pcm_config = &pcm_config_out;
        out->buffer_type = OUT_BUFFER_TYPE_UNKNOWN;
    }

    /*
     * All open PCMs can only use a single group of rates at once:
     * Group 1: 11.025, 22.05, 44.1
     * Group 2: 8, 16, 32, 48
     * Group 1 is used for digital audio playback since 44.1 is
     * the most common rate, but group 2 is required for SCO.
     */
    if (adev->active_in) {
        struct stream_in *in = adev->active_in;
        pthread_mutex_lock(&in->lock);
        if (((out->pcm_config->rate % 8000 == 0) &&
                 (in->pcm_config->rate % 8000) != 0) ||
                 ((out->pcm_config->rate % 11025 == 0) &&
                 (in->pcm_config->rate % 11025) != 0))
            do_in_standby(in);
        pthread_mutex_unlock(&in->lock);
    }

    ret = select_card(card, device, PCM_OUT);
    if (ret < 0) {
        return -ENODEV;
    }
    out->pcm = pcm_open(card, device, PCM_OUT | PCM_NORESTART, out->pcm_config);

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (out_get_sample_rate(&out->stream.common) != out->pcm_config->rate) {
        ret = create_resampler(out_get_sample_rate(&out->stream.common),
                               out->pcm_config->rate,
                               out->pcm_config->channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               NULL,
                               &out->resampler);
        out->buffer_frames = (pcm_config_out.period_size * out->pcm_config->rate) /
                out_get_sample_rate(&out->stream.common) + 1;

        out->buffer = malloc(pcm_frames_to_bytes(out->pcm, out->buffer_frames));
    }

    adev->active_out = out;

    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    int card;
    unsigned int device;
    int ret;

    ret =  init_cards_and_route(adev, true);
    if (ret < 0)
        return ret;

    /*
     * Due to the lack of sample rate converters in the SoC,
     * it greatly simplifies things to have only the main
     * mic PCM or the BC SCO PCM open at the same time.
     */
    if (adev->in_device & AUDIO_DEVICE_IN_ALL_SCO) {
        return -1;
    } else {
        card = adev->card[adev->card_in_index].card_slot;
        device = adev->card[adev->card_in_index].device;
        if (adev->in_device & AUDIO_DEVICE_IN_USB_DEVICE) {
            in->pcm_config = &pcm_config_usb_in;
        } else {
            in->pcm_config = &pcm_config_in;
        }
    }

    /*
     * All open PCMs can only use a single group of rates at once:
     * Group 1: 11.025, 22.05, 44.1
     * Group 2: 8, 16, 32, 48
     * Group 1 is used for digital audio playback since 44.1 is
     * the most common rate, but group 2 is required for SCO.
     */
    if (adev->active_out) {
        struct stream_out *out = adev->active_out;
        pthread_mutex_lock(&out->lock);
        if (((in->pcm_config->rate % 8000 == 0) &&
                 (out->pcm_config->rate % 8000) != 0) ||
                 ((in->pcm_config->rate % 11025 == 0) &&
                 (out->pcm_config->rate % 11025) != 0))
            do_out_standby(out);
        pthread_mutex_unlock(&out->lock);
    }

    ret = select_card(card, device, PCM_IN);
    if (ret < 0) {
        return -ENODEV;
    }
    in->pcm = pcm_open(card, device, PCM_IN, in->pcm_config);

    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open(in) failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (in_get_sample_rate(&in->stream.common) != in->pcm_config->rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(in->pcm_config->rate,
                               in_get_sample_rate(&in->stream.common),
                               1,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
    }
    in->buffer_size = pcm_frames_to_bytes(in->pcm,
                                          in->pcm_config->period_size);
    in->buffer = malloc(in->buffer_size);
    in->frames_in = 0;

    adev->active_in = in;

    return 0;
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   in->buffer_size);
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->frames_in = in->pcm_config->period_size;
        if (in->pcm_config->channels == 2) {
            unsigned int i;

            /* Discard right channel */
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
        }
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->pcm_config->period_size - in->frames_in);

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                            frames_wr * audio_stream_frame_size(&in->stream.common)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                           frames_wr * audio_stream_frame_size(&in->stream.common),
                        buf.raw,
                        buf.frame_count * audio_stream_frame_size(&in->stream.common));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return pcm_config_out.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return pcm_config_out.period_size *
               audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value);
        if (((adev->out_device != val) && (val != 0))
            || (val & AUDIO_DEVICE_OUT_USB_DEVICE)) {
            /*
             * Always go into standby when a device changes, or if the
             *   device is USB, which may have been removed, inserted,
             *   or re-inserted.
             */
            pthread_mutex_lock(&out->lock);
            do_out_standby(out);
            pthread_mutex_unlock(&out->lock);

            adev->out_device = val;
            select_devices(adev);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t period_count;

    pthread_mutex_lock(&adev->lock);

    if (adev->screen_off && !adev->active_in && !(adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO))
        period_count = OUT_LONG_PERIOD_COUNT;
    else
        period_count = OUT_SHORT_PERIOD_COUNT;

    pthread_mutex_unlock(&adev->lock);

    return (pcm_config_out.period_size * period_count * 1000) / pcm_config_out.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t frame_size = audio_stream_frame_size(&out->stream.common);
    int16_t *in_buffer = (int16_t *)buffer;
    size_t in_frames = bytes / frame_size;
    size_t out_frames;
    int buffer_type;
    int kernel_frames;
    bool sco_on;

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = false;
    }
    buffer_type = (adev->screen_off && !adev->active_in) ?
            OUT_BUFFER_TYPE_LONG : OUT_BUFFER_TYPE_SHORT;
    sco_on = (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO);
    pthread_mutex_unlock(&adev->lock);

    /* detect changes in screen ON/OFF state and adapt buffer size
     * if needed. Do not change buffer size when routed to SCO device. */
    if (!sco_on && (buffer_type != out->buffer_type)) {
        size_t period_count;

        if (buffer_type == OUT_BUFFER_TYPE_LONG)
            period_count = OUT_LONG_PERIOD_COUNT;
        else
            period_count = OUT_SHORT_PERIOD_COUNT;

        out->write_threshold = out->pcm_config->period_size * period_count;
        /* reset current threshold if exiting standby */
        if (out->buffer_type == OUT_BUFFER_TYPE_UNKNOWN)
            out->cur_write_threshold = out->write_threshold;
        out->buffer_type = buffer_type;
    }

    /* Reduce number of channels, if necessary */
    if (popcount(out_get_channels(&stream->common)) >
                 (int)out->pcm_config->channels) {
        unsigned int i;

        /* Discard right channel */
        for (i = 1; i < in_frames; i++)
            in_buffer[i] = in_buffer[i * 2];

        /* The frame size is now half */
        frame_size /= 2;
    }

    /* Change sample rate, if necessary */
    if (out_get_sample_rate(&stream->common) != out->pcm_config->rate) {
        out_frames = out->buffer_frames;
        out->resampler->resample_from_input(out->resampler,
                                            in_buffer, &in_frames,
                                            out->buffer, &out_frames);
        in_buffer = out->buffer;
    } else {
        out_frames = in_frames;
    }

    if (!sco_on) {
        int total_sleep_time_us = 0;
        size_t period_size = out->pcm_config->period_size;

        /* do not allow more than out->cur_write_threshold frames in kernel
         * pcm driver buffer */
        do {
            struct timespec time_stamp;
            if (pcm_get_htimestamp(out->pcm,
                                   (unsigned int *)&kernel_frames,
                                   &time_stamp) < 0)
                break;
            kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

            if (kernel_frames > out->cur_write_threshold) {
                int sleep_time_us =
                    (int)(((int64_t)(kernel_frames - out->cur_write_threshold)
                                    * 1000000) / out->pcm_config->rate);
                if (sleep_time_us < MIN_WRITE_SLEEP_US)
                    break;
                total_sleep_time_us += sleep_time_us;
                if (total_sleep_time_us > MAX_WRITE_SLEEP_US) {
                    ALOGW("out_write() limiting sleep time %d to %d",
                          total_sleep_time_us, MAX_WRITE_SLEEP_US);
                    sleep_time_us = MAX_WRITE_SLEEP_US -
                                        (total_sleep_time_us - sleep_time_us);
                }
                usleep(sleep_time_us);
            }

        } while ((kernel_frames > out->cur_write_threshold) &&
                (total_sleep_time_us <= MAX_WRITE_SLEEP_US));

        /* do not allow abrupt changes on buffer size. Increasing/decreasing
         * the threshold by steps of 1/4th of the buffer size keeps the write
         * time within a reasonable range during transitions.
         * Also reset current threshold just above current filling status when
         * kernel buffer is really depleted to allow for smooth catching up with
         * target threshold.
         */
        if (out->cur_write_threshold > out->write_threshold) {
            out->cur_write_threshold -= period_size / 4;
            if (out->cur_write_threshold < out->write_threshold) {
                out->cur_write_threshold = out->write_threshold;
            }
        } else if (out->cur_write_threshold < out->write_threshold) {
            out->cur_write_threshold += period_size / 4;
            if (out->cur_write_threshold > out->write_threshold) {
                out->cur_write_threshold = out->write_threshold;
            }
        } else if ((kernel_frames < out->write_threshold) &&
            ((out->write_threshold - kernel_frames) >
                (int)(period_size * OUT_SHORT_PERIOD_COUNT))) {
            out->cur_write_threshold = (kernel_frames / period_size + 1) * period_size;
            out->cur_write_threshold += period_size / 4;
        }
    }

    ret = pcm_write(out->pcm, in_buffer, out_frames * frame_size);
    if (ret == -EPIPE) {
        /* In case of underrun, don't sleep since we want to catch up asap */
        pthread_mutex_unlock(&out->lock);
        return ret;
    }

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (in->pcm_config->period_size * in_get_sample_rate(stream)) /
            in->pcm_config->rate;
    size = ((size + 15) / 16) * 16;

    return size * audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        if ((adev->in_device != val) && (val != 0)) {
            /*
             * If SCO is turned on/off, we need to put audio into standby
             * because SCO uses a different PCM.
             */
            if ((val & AUDIO_DEVICE_IN_ALL_SCO) ^
                    (adev->in_device & AUDIO_DEVICE_IN_ALL_SCO)) {
                pthread_mutex_lock(&in->lock);
                do_in_standby(in);
                pthread_mutex_unlock(&in->lock);
            }

            adev->in_device = val;
            select_devices(adev);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_frame_size(&stream->common);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret == 0)
            in->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

    /*if (in->num_preprocessors != 0) {
        ret = process_frames(in, buffer, frames_rq);
    } else */if (in->resampler != NULL) {
        ret = read_frames(in, buffer, frames_rq);
    } else if (in->pcm_config->channels == 2) {
        /*
         * If the PCM is stereo, capture twice as many frames and
         * discard the right channel.
         */
        unsigned int i;
        int16_t *in_buffer = (int16_t *)buffer;

        ret = pcm_read(in->pcm, in->buffer, bytes * 2);

        /* Discard right channel */
        for (i = 0; i < frames_rq; i++)
            in_buffer[i] = in->buffer[i * 2];
    } else {
        ret = pcm_read(in->pcm, buffer, bytes);
    }

    if (ret > 0)
        ret = 0;

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    return 0;
}


static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    out_standby(&stream->common);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret = -1;

    parms = str_parms_create_str(kvpairs);
    if (!parms)
        return ret;
    ret = str_parms_get_str(parms, "orientation", value, sizeof(value));
    if (ret >= 0) {
        int orientation;

        if (strcmp(value, "landscape") == 0)
            orientation = ORIENTATION_LANDSCAPE;
        else if (strcmp(value, "portrait") == 0)
            orientation = ORIENTATION_PORTRAIT;
        else if (strcmp(value, "square") == 0)
            orientation = ORIENTATION_SQUARE;
        else
            orientation = ORIENTATION_UNDEFINED;

        pthread_mutex_lock(&adev->lock);
        if (orientation != adev->orientation) {
            adev->orientation = orientation;
            /*
             * Orientation changes can occur with the input device
             * closed so we must call select_devices() here to set
             * up the mixer. This is because select_devices() will
             * not be called when the input device is opened if no
             * other input parameter is changed.
             */
            select_devices(adev);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->screen_off = false;
        else
            adev->screen_off = true;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (pcm_config_in.period_size * config->sample_rate) / pcm_config_in.rate;
    size = ((size + 15) / 16) * 16;

    return (size * popcount(config->channel_mask) *
                audio_bytes_per_sample(config->format));
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;

    *stream_in = NULL;

    /* Respond with a request for mono if a different format is given. */
    if (config->channel_mask != AUDIO_CHANNEL_IN_MONO) {
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        return -EINVAL;
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    in->pcm_config = &pcm_config_in; /* default PCM config */

    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    in_standby(&stream->common);
    free(stream);
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    pthread_mutex_lock(&adev->lock);
    audio_route_free(adev->ar);
    pthread_mutex_unlock(&adev->lock);

    free(device);
    return 0;
}

static int init_cards_and_route(struct audio_device *adev, bool report_card_errors)
{
    if (adev->card[adev->card_out_index].card_slot == CARD_SLOT_NOT_FOUND) {
        find_card_slot(adev);
        if (adev->card[adev->card_out_index].card_slot != CARD_SLOT_NOT_FOUND) {
            adev->ar = audio_route_init((unsigned int)
                adev->card[adev->card_out_index].card_slot);
            if (adev->ar == NULL) {
                return -EINVAL;
            }
            /*
                select_devices will call init_cards_and_route, but there is no
                deep recursion happening. select_devices will be called only if
                card is found. when select_devices calls init_cards_and_route
                the method will never pass initial if check (the card was found
                so it is not CARD_SLOT_NOT_FOUND).
            */
            select_devices(adev);
        }
        else if (report_card_errors){
            return -EINVAL;
        }
    }
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int index, ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;
    /*
    * Hard-coded to the internal codec device for now, an xml file
    *   is needed to continue.
    */
    adev->card_out_index = AUDIO_CARD_PCH;
    adev->card[adev->card_out_index].card_slot = CARD_SLOT_NOT_FOUND;

    pthread_mutex_lock(&adev->lock);
    ret =  init_cards_and_route(adev, false);
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0){
        free(adev);
        return ret;
    }
    switch (adev->card_out_index) {
        case AUDIO_CARD_HDMI:
            adev->out_device = AUDIO_DEVICE_OUT_AUX_DIGITAL;
            break;
        case AUDIO_CARD_USB:
            adev->out_device = AUDIO_DEVICE_OUT_USB_DEVICE;
            break;
        case AUDIO_CARD_PCH:
        default:
            adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
            break;
    }

    adev->card_in_index = AUDIO_CARD_PCH;
    adev->orientation = ORIENTATION_UNDEFINED;
    adev->in_device = AUDIO_DEVICE_IN_BUILTIN_MIC & ~AUDIO_DEVICE_BIT_IN;

    *device = &adev->hw_device.common;

    select_devices(adev);
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Intel HD-Audio HAL",
        .author = "The Android Open Source Project & Intel Corporation.",
        .methods = &hal_module_methods,
    },
};
