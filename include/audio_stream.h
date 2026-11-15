/*
 * audio_stream.h
 * Module doc luong HTTP Radio + phan tach ICY Metadata (Core 0, Priority 6)
 * va Giai ma MP3/AAC + xuat I2S DMA ra MAX98357A (Core 1, Priority 16).
 */
#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Khoi tao GPIO 33 (SD_MODE), kenh I2S Standard DMA va khoi chay 2 task Audio/HTTP */
esp_err_t audio_stream_init(void);

/* Dieu khien truc tiep chan Mute phan cung MAX98357A (GPIO 33: false=0 Mute, true=1 Unmute) */
void audio_hw_set_unmute(bool unmute);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_STREAM_H */
