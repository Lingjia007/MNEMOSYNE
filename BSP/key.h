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
#include <stdbool.h>

/* 消抖时间 ms：小于该间隔的连续边沿视为抖动丢弃 */
#define KEY_DEBOUNCE_MS 20U

/* 中断里置事件标志，主循环轮询取走（录音启停涉及文件操作，须在主循环做） */
bool Key_PopEvent(void);

/* 按键当前是否按住（低电平 = 按下）：供 HOLD 触发模式的按住/松开检测使用，
 * 在主循环轮询调用，返回值配合时间去抖 */
bool Key_IsPressed(void);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H */
