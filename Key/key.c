/**
 ******************************************************************************
 * @file    key.c
 * @brief   按键驱动（EXTI 中断方式）
 *          Key（PA0）配置为下降沿中断（GPIO_MODE_IT_FALLING + 上拉），
 *          按下瞬间进入 HAL_GPIO_EXTI_Callback，软件消抖后翻转 LED
 ******************************************************************************
 */
#include "key.h"
#include "led.h"

/**
 * @brief  EXTI 回调：由 HAL_GPIO_EXTI_IRQHandler 调用，运行在中断上下文
 *         消抖策略：两次有效边沿的间隔小于 KEY_DEBOUNCE_MS 视为抖动
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    static uint32_t s_last_tick = 0;

    if (GPIO_Pin != Key_Pin)
    {
        return;
    }

    uint32_t now = HAL_GetTick();
    if ((now - s_last_tick) >= KEY_DEBOUNCE_MS)
    {
        s_last_tick = now;
        LED_TOGGLE();
    }
}
