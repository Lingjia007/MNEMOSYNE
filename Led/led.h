/**
  ******************************************************************************
  * @file    led.h
  * @brief   LED 最快控制宏（寄存器直接访问，绕过 HAL 函数调用开销）
  *          引脚由 CubeMX 配置（main.h 中的 LED_Pin / LED_GPIO_Port = PB2）
  ******************************************************************************
  */
#ifndef __LED_H
#define __LED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/*
 * 速度说明（Cortex-M4 @168MHz，APB2 桥无等待）：
 *  - LED_ON/OFF  : 写 BSRR，编译为 1 条 STR，原子操作，不被中断撕裂
 *  - LED_TOGGLE  : ODR 读-改-写（LDR+EOR+STR，约 3~4 周期），
 *                  已是最快写法；HAL_GPIO_TogglePin 还要多付函数调用 + 参数检查
 *  - 若在中断与主循环同时翻转 LED，请改用临界区保护 ODR 读-改-写
 */

/* 点亮/熄灭：BSRR 低 16 位置 1（置位），高 16 位（复位） */
#define LED_ON()       (LED_GPIO_Port->BSRR = (uint32_t)LED_Pin)
#define LED_OFF()      (LED_GPIO_Port->BSRR = (uint32_t)LED_Pin << 16U)

/* 翻转：直接 ODR 异或 */
#define LED_TOGGLE()   (LED_GPIO_Port->ODR ^= LED_Pin)

/* 读当前输出状态：1=高电平 */
#define LED_IS_ON()    (((LED_GPIO_Port->ODR) & LED_Pin) != 0U)

#ifdef __cplusplus
}
#endif

#endif /* __LED_H */
