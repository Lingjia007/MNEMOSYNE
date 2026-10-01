/**
 ******************************************************************************
 * @file    audio_recorder.h
 * @brief   按键录音模块：I2S2(INMP441) DMA 采集 -> 环形缓冲 -> SD 卡 WAV 文件
 *          16kHz / 单声道 / 16bit PCM，文件保存在 SD 卡 AUDIO_REC 目录下
 *          按键一次开始录音（LED 亮），再按一次停止（LED 灭）
 ******************************************************************************
 */
#ifndef __AUDIO_RECORDER_H
#define __AUDIO_RECORDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "ff.h"

typedef enum
{
    AUDIO_REC_IDLE = 0,   /* 空闲 */
    AUDIO_REC_RECORDING,  /* 录音中 */
} AudioRecState;

/* 初始化内部状态（在 SD 卡挂载成功后调用） */
void AudioRec_Init(void);

/* 开始录音：创建目录/查找空闲文件号/写 WAV 头/启动 I2S DMA
 * 返回 FR_OK 表示成功（主循环据此点亮 LED） */
FRESULT AudioRec_Start(void);

/* 停止录音：停 DMA/把环形缓冲剩余数据写完/回填 WAV 头长度/关文件 */
FRESULT AudioRec_Stop(void);

/* 主循环调用：把环形缓冲中的数据搬写到 SD 卡 */
void AudioRec_Process(void);

AudioRecState AudioRec_GetState(void);
const char   *AudioRec_GetFilePath(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_RECORDER_H */
