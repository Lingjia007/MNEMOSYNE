/**
  ******************************************************************************
  * @file    key.h
  * @brief   按键驱动头文件（EXTI 中断方式）
  *          引脚由 CubeMX 配置（main.h 中的 Key_Pin / Key_GPIO_Port）
  ******************************************************************************
  */
#ifndef __KEY_H
#define __KEY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 消抖时间 ms：小于该间隔的连续边沿视为抖动丢弃 */
#define KEY_DEBOUNCE_MS 20U

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H */
