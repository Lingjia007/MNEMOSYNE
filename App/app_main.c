/**
 ******************************************************************************
 * @file    app_main.c
 * @brief   应用层主流程（从 main.c USER CODE 块迁入，逻辑未变）
 *
 * 职责：SD 卡挂载序列、USB MSC 启停决策、按键事件 -> 录音启停、LED 状态管理。
 * 硬件细节封装在 BSP 层（key/led/audio_recorder），本文件只做流程编排。
 ******************************************************************************
 */
#include "app_main.h"
#include "main.h"
#include <stdio.h>
#include "fatfs.h"       /* SDFatFS */
#include "sdio.h"        /* hsd, MX_SDIO_SD_Init_Fix() */
#include "usb_device.h"  /* MX_USB_DEVICE_Init() */
#include "key.h"
#include "led.h"
#include "audio_recorder.h"

void App_Init(void)
{
    printf("\r\n=== STM32F407 MNEMOSYNE ===\r\n");

    MX_SDIO_SD_Init_Fix();

    printf("SD detect pin: %s\r\n",
           (HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) == GPIO_PIN_RESET)
               ? "LOW (inserted)"
               : "HIGH (not inserted)");

    FRESULT fres = f_mount(&SDFatFS, "0:", 1);
    if (fres != FR_OK)
    {
        printf("f_mount failed: %d\r\n", (int)fres);
    }

    /* 等待 SD 卡进入 TRANSFER 就绪状态（100ms 超时） */
    HAL_SD_CardStateTypeDef sd_state;
    uint32_t start_tick = HAL_GetTick();
    do
    {
        sd_state = HAL_SD_GetCardState(&hsd);
        if (HAL_GetTick() - start_tick > 100)
        {
            sd_state = HAL_SD_CARD_ERROR;
            printf("SD card state timeout\r\n");
            break;
        }
        HAL_Delay(1);
    } while (sd_state == HAL_SD_CARD_PROGRAMMING || sd_state == HAL_SD_CARD_RECEIVING);

    /* SD 卡就绪后再启动 USB MSC，把 SD 卡挂载为 U 盘；失败则跳过并继续运行 */
    if (sd_state == HAL_SD_CARD_TRANSFER)
    {
        printf("SD card ready, starting USB MSC...\r\n");
        MX_USB_DEVICE_Init();
        printf("USB MSC started.\r\n");
    }
    else
    {
        printf("SD card not ready (%d), USB skipped\r\n", (int)sd_state);
    }
}

/* ---- TOGGLE 触发模式：按一下开始，再按一下结束（REC_TRIGGER_MODE = REC_TRIGGER_TOGGLE） ---- */
static void App_RecToggle(void)
{
    static bool s_pending = false;      /* 延时启动等待中 */
    static uint32_t s_pending_tick = 0; /* 按键事件确认时刻 */

    if (Key_PopEvent())
    {
        if (AudioRec_GetState() == AUDIO_REC_IDLE)
        {
            if (s_pending)
            {
                s_pending = false; /* 延时等待期间再按一次 = 取消本次启动 */
            }
            else if (REC_START_DELAY_MS == 0U)
            {
                if (AudioRec_Start() == FR_OK)
                {
                    LED_ON();
                }
            }
            else
            {
                s_pending = true; /* 进入延时等待 */
                s_pending_tick = HAL_GetTick();
            }
        }
        else
        {
            s_pending = false;
            (void)AudioRec_Stop(); /* 录音中按键 = 立即停止（不受延时影响） */
            LED_OFF();
        }
    }

    /* 延时到达：正式启动录音 */
    if (s_pending && (HAL_GetTick() - s_pending_tick >= REC_START_DELAY_MS))
    {
        s_pending = false;
        if ((AudioRec_GetState() == AUDIO_REC_IDLE) && (AudioRec_Start() == FR_OK))
        {
            LED_ON();
        }
    }
}

/* ---- HOLD 触发模式：按住期间录音，松开后停止（REC_TRIGGER_MODE = REC_TRIGGER_HOLD） ---- */
static void App_RecHold(void)
{
    static bool s_pending = false;
    static uint32_t s_pending_tick = 0;
    static uint32_t s_release_tick = 0; /* 0 = 按住中；非 0 = 首次读到松开的时刻 */

    /* 按下沿事件：启动录音（或进入延时等待） */
    if (Key_PopEvent() && (AudioRec_GetState() == AUDIO_REC_IDLE))
    {
        if (REC_START_DELAY_MS == 0U)
        {
            if (AudioRec_Start() == FR_OK)
            {
                LED_ON();
            }
        }
        else
        {
            s_pending = true;
            s_pending_tick = HAL_GetTick();
        }
    }

    /* 延时等待：期间松开 = 取消；到时且仍按住 = 启动 */
    if (s_pending)
    {
        if (!Key_IsPressed())
        {
            s_pending = false;
        }
        else if (HAL_GetTick() - s_pending_tick >= REC_START_DELAY_MS)
        {
            s_pending = false;
            if (AudioRec_Start() == FR_OK)
            {
                LED_ON();
            }
        }
    }

    /* 松开检测：连续 KEY_DEBOUNCE_MS 高电平才确认释放（滤释放抖动） */
    if (AudioRec_GetState() == AUDIO_REC_RECORDING)
    {
        if (!Key_IsPressed())
        {
            if (s_release_tick == 0U)
            {
                s_release_tick = HAL_GetTick();
            }
            else if (HAL_GetTick() - s_release_tick >= KEY_DEBOUNCE_MS)
            {
                s_release_tick = 0U;
                (void)AudioRec_Stop();
                LED_OFF();
            }
        }
        else
        {
            s_release_tick = 0U;
        }
    }
}

void App_Loop(void)
{
#if REC_TRIGGER_MODE == REC_TRIGGER_HOLD
    App_RecHold();
#else
    App_RecToggle();
#endif

    /* 把采集环形缓冲中的数据搬到 SD 卡 */
    AudioRec_Process();

    /* 写卡失败等异常导致录音中途结束：同步熄灭 LED */
    if ((AudioRec_GetState() == AUDIO_REC_IDLE) && LED_IS_ON())
    {
        LED_OFF();
    }
}
