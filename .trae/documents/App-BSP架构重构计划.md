# App/BSP 两层架构重构计划

## Context

当前工程的自建模块（Audio/Key/Led）散落在项目根目录，全部业务逻辑（SD 挂载、USB MSC 启停、按键→录音流程、LED 管理）塞在 [main.c](file:///d:/edgedownload/STM32WB55XX-HERA/STM32F407XX-MNEMOSYNE/Core/Src/main.c) 的 USER CODE 块里。移植到新工程时需要从 main.c 里逐段抠代码，且目录无分层语义。

目标：重构为 **App/BSP 两层**，业务与硬件分离，移植新工程 = 拷贝 App/ + BSP/ 目录 + main.c 里接 2 个函数 + 改引脚配置。CubeMX 管辖目录（Core/Drivers/Middlewares/FATFS/USB_DEVICE）完全不动，保持再生成能力。

用户已确认的决策：
- 分层深度：App/BSP 两层（BSP 内部直接用 HAL，不做纯接口抽象层）
- key→LED 解耦：现状已满足（key.c 已是纯事件上报，LED 控制已在主循环），保持
- led.h 依赖 main.h 引脚宏、printf 重定向留在 usart.c——均保持现状不动

## 目标结构

```
STM32F407XX-MNEMOSYNE/
├── App/
│   ├── app_main.h        新增：App_Init() / App_Loop() 接口
│   └── app_main.c        新增：main.c USER CODE 2/3 的全部业务逻辑
├── BSP/
│   ├── led.h             ← 从 Led/ 移入（纯头文件，寄存器宏）
│   ├── key.c / key.h     ← 从 Key/ 移入（EXTI 按键事件）
│   └── audio_recorder.c / .h ← 从 Audio/ 移入（I2S 采集→环形缓冲→WAV）
├── Core/ Drivers/ Middlewares/ FATFS/ USB_DEVICE/   CubeMX 管辖，不动
└── MDK-ARM/               uvprojx 更新分组与 include 路径
```

分层职责：
- **App**（app_main.c）：业务流程——SD 卡挂载序列、USB MSC 启停决策、按键事件→录音启停、LED 状态管理。不直接碰任何 HAL 句柄以外的硬件细节（引脚宏经由 BSP 头）。
- **BSP**：硬件服务——LED 寄存器宏、按键 EXTI 事件、录音采集与文件写入。内部直接用 HAL/CubeMX 句柄（hi2s2、hsd、hUsbDeviceFS、SDFatFS）。

## 实施步骤

### 1. 移动文件（保留原内容不变）
```
Move-Item Led/led.h                → BSP/led.h
Move-Item Key/key.c, Key/key.h     → BSP/
Move-Item Audio/audio_recorder.c/.h → BSP/
删除空目录 Led/ Key/ Audio/
```

### 2. 新增 App/app_main.h
```c
#ifndef __APP_MAIN_H
#define __APP_MAIN_H
void App_Init(void);   /* 外设就绪后的应用初始化（SD 挂载、USB MSC 启动） */
void App_Loop(void);   /* 主循环业务（按键→录音→LED），由 main while(1) 调用 */
#endif
```

### 3. 新增 App/app_main.c
内容 = 现 [main.c L104-146](file:///d:/edgedownload/STM32WB55XX-HERA/STM32F407XX-MNEMOSYNE/Core/Src/main.c#L104-L146)（USER CODE 2 初始化序列）+ [L154-182](file:///d:/edgedownload/STM32WB55XX-HERA/STM32F407XX-MNEMOSYNE/Core/Src/main.c#L154-L182)（主循环业务），逐行搬移不改逻辑：
- 头文件：`main.h`、`stdio.h`、`fatfs.h`（SDFatFS）、`sdio.h`（hsd/MX_SDIO_SD_Init_Fix）、`usb_device.h`、`key.h`、`led.h`、`audio_recorder.h`
- `App_Init()`：printf banner → `MX_SDIO_SD_Init_Fix()` → SD 检测引脚打印（PD3）→ `f_mount` → 等 TRANSFER（100ms 超时）→ 就绪则 `MX_USB_DEVICE_Init()`
- `App_Loop()`：`Key_PopEvent()` → 录音启停 + `LED_ON()/LED_OFF()`；`AudioRec_Process()`；录音异常结束同步熄 LED

### 4. main.c USER CODE 块瘦身（CubeMX 再生成安全）
- Includes 块：6 个 include → 仅 `#include "app_main.h"`
- BEGIN 2 块：全部业务 → `App_Init();`
- BEGIN 3 块（while 内）：全部业务 → `App_Loop();`

### 5. MDK-ARM/STM32F407XX-MNEMOSYNE.uvprojx
- [L344](file:///d:/edgedownload/STM32WB55XX-HERA/STM32F407XX-MNEMOSYNE/MDK-ARM/STM32F407XX-MNEMOSYNE.uvprojx#L344) IncludePath：`../Audio;../Key;../Led` → `../App;../BSP`
- [L548-563](file:///d:/edgedownload/STM32WB55XX-HERA/STM32F407XX-MNEMOSYNE/MDK-ARM/STM32F407XX-MNEMOSYNE.uvprojx#L548-L563) 分组：`Key` 组 + `Audio` 组 → `App` 组（../App/app_main.c）+ `BSP` 组（../BSP/key.c、../BSP/audio_recorder.c）

### 6. .clangd（L11-13）
`-I.../Audio`、`-I.../Key`、`-I.../Led` 三行 → `-I.../App`、`-I.../BSP` 两行（绝对路径形式）

### 7. .vscode/c_cpp_properties.json（L19-21）
`"Audio","Key","Led"` → `"App","BSP"`

### 8. compile_commands.json（追加式生成文件，PowerShell 脚本批量处理）
- 全文替换 `Key/` → `BSP/`（覆盖 file、-c、-o 三处路径）及 `"directory": "Key"` → `"BSP"`（54 条）
- 同理 `Audio/` → `BSP/`（44 条）
- 克隆 main.c 条目（55 条）为 app_main.c 条目（directory=`App`、file=`App/app_main.c`、-o=`build/.../App/app_main.o`）

## 验证

1. UV4 命令行构建：ExitCode=0、0 Error 0 Warning（沿用 Start-Process -Wait -PassThru 方式，日志绝对路径）
2. 烧录后功能回归（用户执行）：串口 banner 与 SD 挂载日志 → 按键录音启停 + LED 指示 → 停止后 USB MSC 重新枚举可拷出 WAV
3. IDE 检查 clangd/IntelliSense 对 App/BSP 路径无报错

## 已知约束（不属本次改动范围）

- CubeMX 再生成会重写 uvprojx 的 IncludePath，`../App;../BSP` 需重加（.mxproject 不含自建路径，regen 后核对）
- sd_diskio.c 的 SD_status 超时修复在固定段，regen 会回退（既有已知问题，维持现状）
