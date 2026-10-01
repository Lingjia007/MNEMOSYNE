/**
 ******************************************************************************
 * @file    app_main.h
 * @brief   应用层入口（业务流程层，不含硬件细节）
 *          main.c 的 USER CODE 块只调用这两个函数，
 *          移植时拷贝 App/ + BSP/ 目录，在 main.c 里重新接线即可
 ******************************************************************************
 */
#ifndef __APP_MAIN_H
#define __APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* 外设初始化完成后的应用初始化：SD 卡挂载、USB MSC 启动 */
void App_Init(void);

/* 主循环业务：按键事件 -> 录音启停 + LED 指示，由 main 的 while(1) 调用 */
void App_Loop(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MAIN_H */
