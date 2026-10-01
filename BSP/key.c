/**
 ******************************************************************************
 * @file    key.c
 * @brief   按键驱动（EXTI 中断方式）
 *          Key（PA0）配置为下降沿中断（GPIO_MODE_IT_FALLING + 上拉），
 *          按下瞬间进入 HAL_GPIO_EXTI_Callback，软件消抖后置事件标志，
 *          主循环通过 Key_PopEvent() 取走事件（启停录音等文件操作在主循环做）
 ******************************************************************************
 */
#include "key.h"

static volatile bool s_press_event = false;

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
        s_press_event = true;
    }
}

/**
 * @brief  主循环调用：取走一次按键事件，有事件返回 true（只取一次）
 */
bool Key_PopEvent(void)
{
    bool event = s_press_event;
    s_press_event = false;
    return event;
}
