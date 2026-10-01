/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file   fatfs.c
 * @brief  Code for fatfs applications
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
#include "fatfs.h"

uint8_t retSD;    /* Return value for SD */
char SDPath[4];   /* SD logical drive path */
FATFS SDFatFS;    /* File system object for SD logical drive */
FIL SDFile;       /* File object for SD */

/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

void MX_FATFS_Init(void)
{
  /*## FatFS: Link the SD driver ###########################*/
  retSD = FATFS_LinkDriver(&SD_Driver, SDPath);

  /* USER CODE BEGIN Init */
  /* additional user code for init */
  /* USER CODE END Init */
}

/**
  * @brief  Gets Time from RTC
  * @param  None
  * @retval Time in DWORD
  */
DWORD get_fattime(void)
{
  /* USER CODE BEGIN get_fattime */
  return fat_GetFatTimeFromRTC();
  /* USER CODE END get_fattime */
}

/* USER CODE BEGIN Application */
DWORD fat_GetFatTimeFromRTC(void)
{
  RTC_TimeTypeDef sTime;
  RTC_DateTypeDef sDate;
  if (HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN) == HAL_OK)
  {
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
    WORD date = ((2000 + sDate.Year - 1980) << 9) | (sDate.Month << 5) | sDate.Date;
    WORD time = (sTime.Hours << 11) | (sTime.Minutes << 5) | (sTime.Seconds >> 1);

    DWORD fatTime = ((DWORD)date << 16) | time;
    return fatTime;
  }
  else
  {
    return 0;
  }
}
/* USER CODE END Application */
