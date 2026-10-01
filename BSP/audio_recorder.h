/**
 ******************************************************************************
 * @file    audio_recorder.h
 * @brief   按键录音模块：I2S2(INMP441) DMA 采集 -> 环形缓冲 -> SD 卡 WAV 文件
 *          16kHz / 单声道 / 16bit PCM，文件保存在 SD 卡 AUDIO_REC 目录下
 ******************************************************************************
 */
#ifndef __AUDIO_RECORDER_H
#define __AUDIO_RECORDER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "main.h"
#include "ff.h"

/* ========================== 用户配置区（按需修改后重新编译） ==========================
 * 录音行为的三项配置集中在本区域；修改后重新编译烧录即可生效。
 * 采集类参数（采样率/缓冲大小/预分配空间/软件增益）见 audio_recorder.c 顶部"可调参数"区。
 * ==================================================================================== */

/* ---- 1. 录音触发模式 --------------------------------------------------------------- */
#define REC_TRIGGER_TOGGLE 0 /* 按一下开始录音，再按一下结束 */
#define REC_TRIGGER_HOLD 1   /* 按住期间持续录音，松开后停止 */

/* 触发模式选择：改成 REC_TRIGGER_TOGGLE 或 REC_TRIGGER_HOLD */
#define REC_TRIGGER_MODE REC_TRIGGER_TOGGLE

/* ---- 2. 按键确认后的录音启动延时（毫秒） -------------------------------------------- */
#define REC_START_DELAY_MS 600U
/* 0    = 按键事件确认后立即启动录音（默认，与原行为一致）
 * 300  = 推荐 200~500：按下瞬间的机械噪声/爆破音很响（peak 常触顶 32767），
 *        延时启动可把这段噪声跳过，且给声源一个准备时间
 * TOGGLE 模式：延时等待期间再按一次按键 = 取消本次启动
 * HOLD   模式：延时等待期间松开按键 = 取消本次启动（按住满延时才开始录） */

/* ---- 3. 录音结束后是否自动恢复 USB MSC（U 盘重新枚举） ------------------------------ */
#define REC_USB_RESUME_ON_STOP 1
    /* 1 = 停止录音后立即恢复 USB MSC，U 盘重新出现在电脑上（默认，与原行为一致）
     * 0 = 录音结束后保持 U 盘断开：适合连续录多段（避免每段都经历一次拔插枚举），
     *     需要拷文件时调用 AudioRec_UsbResume() 手动恢复（例如接到某个按键组合/命令时） */

    /* ========================== 配置区结束 =============================================== */

    typedef enum
    {
        AUDIO_REC_IDLE = 0,  /* 空闲 */
        AUDIO_REC_RECORDING, /* 录音中 */
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

    /* 手动恢复 USB MSC（REC_USB_RESUME_ON_STOP=0 时用于在需要拷文件时重新枚举 U 盘；
     * 若 USB 本就未被暂停则不做任何事） */
    void AudioRec_UsbResume(void);

    AudioRecState AudioRec_GetState(void);
    const char *AudioRec_GetFilePath(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_RECORDER_H */
