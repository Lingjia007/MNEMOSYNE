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

void App_Loop(void)
{
    /* 按键事件（EXTI 中断置标志，见 BSP/key.c）：
     * 一次按键 = 开始录音（LED 亮）；再次按键 = 停止录音（LED 灭） */
    if (Key_PopEvent())
    {
        if (AudioRec_GetState() == AUDIO_REC_IDLE)
        {
            if (AudioRec_Start() == FR_OK)
            {
                LED_ON();
            }
        }
        else
        {
            (void)AudioRec_Stop();
            LED_OFF();
        }
    }

    /* 把采集环形缓冲中的数据搬到 SD 卡 */
    AudioRec_Process();

    /* 写卡失败等异常导致录音中途结束：同步熄灭 LED */
    if ((AudioRec_GetState() == AUDIO_REC_IDLE) && LED_IS_ON())
    {
        LED_OFF();
    }
}
