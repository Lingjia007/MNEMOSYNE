/**
 ******************************************************************************
 * @file    audio_recorder.c
 * @brief   按键录音实现（采集部分参考 STM32WB55XX-HERA 的 Audio/App 实现）
 *
 * 数据流（16kHz 单声道 int16）：
 *   I2S2 DMA 循环模式（半/全传输中断，5ms 半块）
 *     -> 中断里把左槽样本转成 int16 推入环形缓冲
 *     -> 主循环 AudioRec_Process() 搬写到 SD 卡 WAV 文件
 *
 * I2S2 硬件配置（CubeMX 生成，见 Core/Src/i2s.c）：
 *   主接收 / Philips 标准 / 32bit 数据格式 / 16kHz / DMA1_Stream3 循环字传输。
 *   32bit 槽需两次 16 位 DR 读出；DMA 按 32 位字读 DR 为零扩展（高半字恒 0），
 *   故每槽占 2 词、每帧占 4 词：[左高16][左低16][右高16][右低16]。
 *   INMP441 L/R=GND 输出在左槽，int16 样本 = 每帧首词的低半字。
 ******************************************************************************
 */
#include "audio_recorder.h"
#include "i2s.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "usb_device.h"
#include "usbd_core.h"

/* usb_device.h 未导出句柄，这里自行声明（定义在 usb_device.c） */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* ---- 可调参数 ------------------------------------------------------------ */
#define REC_SAMPLE_RATE 16000U /* 采样率 Hz（与 I2S2 配置一致） */
#define REC_DMA_BUF_WORDS 640U /* DMA 双缓冲总字数（32bit）：160 帧，半缓冲 80 帧 = 5ms */
#define REC_DMA_HALF_WORDS (REC_DMA_BUF_WORDS / 2U)
#define REC_DMA_HALF_FRAMES (REC_DMA_HALF_WORDS / 4U) /* 每帧 4 词（见 Rec_DmaHalfDone），半缓冲 80 帧 */
#define REC_RING_SAMPLES 16384U                       /* 环形缓冲样本数：1.024s @16kHz，吸收 SD 写卡顿 */
#define REC_CHUNK 512U                                /* 单次 f_write 最大样本数（1KB） */
#define REC_DIR "0:/AUDIO_REC"                        /* SD 卡上的录音目录 */
#define REC_MAX_FILES 9999U                           /* REC0001.WAV ~ REC9999.WAV */
#define REC_PREALLOC_BYTES (32U * 1024U * 1024U)      /* 录音开始时预分配的文件空间：约 17.5min @32KB/s */
#define REC_GAIN_SHIFT 1U                             /* 软件增益（左移位数）：0=原声, 1=2倍, 2=4倍；放大后饱和截断防溢出 */
#define REC_DC_SHIFT 8U /* 直流跟踪 EMA 移位：时间常数 2^8=256 样本≈16ms@16kHz */
#define REC_FADE_SAMPLES ((REC_FADE_MS * REC_SAMPLE_RATE) / 1000U) /* 首尾淡入淡出样本数（分块处理，不受 REC_CHUNK 限制） */

/* ---- 模块状态 ------------------------------------------------------------ */
static uint32_t s_dma_buf[REC_DMA_BUF_WORDS]; /* DMA 循环缓冲（DMA/CPU 共享） */
static int16_t s_mono[REC_DMA_HALF_FRAMES];   /* 半缓冲转换结果（ISR 访问） */
static int16_t s_file_buf[REC_CHUNK];         /* 写文件线性缓冲（主循环访问） */

/* 环形缓冲：生产者 = I2S DMA 中断，消费者 = 主循环
 * count 为 32 位对齐变量，Cortex-M4 上读写原子 */
static int16_t s_ring[REC_RING_SAMPLES];
static volatile uint32_t s_ring_wpos;  /* ISR 写位置 */
static volatile uint32_t s_ring_count; /* 可读样本数 */
static uint32_t s_ring_rpos;           /* 主循环读位置（仅主循环访问） */

static FIL s_rec_file;           /* 录音文件对象 */
static char s_path[48];          /* 当前文件路径 */
static uint32_t s_file_bytes;    /* 已写入的 PCM 数据字节数（不含 44B 头） */
static uint32_t s_drop_count;    /* 环满丢弃的样本数 */
static uint32_t s_i2s_err_count; /* I2S 硬件错误（溢出等）次数 */
static uint32_t s_peak;          /* 录音期间样本峰值 |sample|（诊断用） */
static int32_t s_dc_q;           /* 直流分量 EMA 估计（Q8 定点，≈256 倍真实直流） */
static uint32_t s_rec_start_tick; /* 录音启动时刻：开头 REC_TRIM_HEAD_MS 内的样本据此丢弃 */
static bool s_usb_paused = false; /* 录音期间由本模块暂停了 USB MSC（准确追踪暂停状态，
                                   * hUsbDeviceFS.pData 在 DeInit 后仍非 NULL，不能作判据） */
static AudioRecState s_state = AUDIO_REC_IDLE;

/* ---- 内部工具 ------------------------------------------------------------ */
static void Rec_Put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8U);
}

static void Rec_Put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8U) & 0xFFU);
    p[2] = (uint8_t)((v >> 16U) & 0xFFU);
    p[3] = (uint8_t)(v >> 24U);
}

/* 构造 44 字节标准 PCM WAV 头 */
static void Rec_BuildWavHeader(uint8_t hdr[44], uint32_t data_size)
{
    memcpy(&hdr[0], "RIFF", 4);
    Rec_Put32(&hdr[4], 36U + data_size); /* 文件总长 - 8 */
    memcpy(&hdr[8], "WAVE", 4);
    memcpy(&hdr[12], "fmt ", 4);
    Rec_Put32(&hdr[16], 16U); /* fmt 块长度 */
    Rec_Put16(&hdr[20], 1U);  /* PCM */
    Rec_Put16(&hdr[22], 1U);  /* 单声道 */
    Rec_Put32(&hdr[24], REC_SAMPLE_RATE);
    Rec_Put32(&hdr[28], REC_SAMPLE_RATE * 2U); /* 字节率 = 采样率 * 声道数 * 2 */
    Rec_Put16(&hdr[32], 2U);                   /* 块对齐 */
    Rec_Put16(&hdr[34], 16U);                  /* 位深 */
    memcpy(&hdr[36], "data", 4);
    Rec_Put32(&hdr[40], data_size);
}

/* ---- 生产者：DMA 半/全传输中断 ------------------------------------------- */
/* half: 0 = 前半缓冲，1 = 后半缓冲 */
static void Rec_DmaHalfDone(uint8_t half)
{
    const uint32_t *src = &s_dma_buf[(uint32_t)half * REC_DMA_HALF_WORDS];

    for (uint32_t i = 0U; i < REC_DMA_HALF_FRAMES; i++)
    {
        /* 每帧 4 词：[左槽高16][左槽低16][右槽高16][右槽低16]；
         * DMA 字读 16 位 DR 为零扩展，样本 = 每帧首词的低半字 */
        int32_t raw = (int16_t)(src[4U * i] & 0xFFFFU);

        /* 直流去除：INMP441 输出带恒定直流偏置，不去除的话 WAV 首样本从 0
         * 跳到偏置值、末样本从偏置跳回 0——播放时首尾各一声低频"噗"，
         * 与 trim 时长无关（加长裁剪无效、首次录音也有的原因）。
         * EMA 跟踪直流并减去；裁剪窗口内样本虽被丢弃但仍参与跟踪，
         * 窗口结束时估计已收敛，进文件的首样本≈0，首尾阶跃消失 */
        s_dc_q += raw - (s_dc_q >> REC_DC_SHIFT);
        int16_t sample = (int16_t)(raw - (s_dc_q >> REC_DC_SHIFT));

        /* 开头裁剪（REC_TRIM_HEAD_MS）：启动后该时段内的样本直接丢弃，
         * 不进环形缓冲（不写入文件、不计峰值）——按下瞬间的按键机械噪声
         * 集中在这一段。HAL_GetTick() 在 ISR 中调用安全（仅读 tick 计数） */
        if ((HAL_GetTick() - s_rec_start_tick) < REC_TRIM_HEAD_MS)
        {
            continue;
        }

        /* 峰值统计：判断音量偏低的真实幅度（INMP441 灵敏度 -26dBFS@94dB SPL），
         * 统计的是去直流后、增益前的原始幅度 */
        uint32_t mag = (sample < 0) ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
        if (mag > s_peak)
        {
            s_peak = mag;
        }

#if REC_GAIN_SHIFT > 0
        /* 软件增益：int32 乘 2^n 后 __SSAT 饱和回 16bit，防溢出翻转（削顶不失真） */
        sample = (int16_t)__SSAT((int32_t)sample * (1 << REC_GAIN_SHIFT), 16);
#endif

        if (s_ring_count < REC_RING_SAMPLES)
        {
            s_ring[s_ring_wpos] = sample;
            s_ring_wpos = (s_ring_wpos + 1U) % REC_RING_SAMPLES;
            s_ring_count++;
        }
        else
        {
            s_drop_count++; /* 环满：SD 卡写入太慢，丢弃本样本 */
        }
    }
}

void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2)
    {
        Rec_DmaHalfDone(0U);
    }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2)
    {
        Rec_DmaHalfDone(1U);
    }
}

void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
    /* 接收溢出（OVR）等：DMA 仍在循环运行，仅丢失个别样本，记录计数即可 */
    if (hi2s->Instance == SPI2)
    {
        s_i2s_err_count++;
    }
}

/* ---- 消费者：主循环把环形缓冲写入文件 ------------------------------------- */
/* 把环形缓冲中当前所有样本写入文件（处理回绕），失败返回错误码 */
static FRESULT Rec_WriteRingToFile(void)
{
    FRESULT res = FR_OK;

    while (s_ring_count > 0U)
    {
        uint32_t n = (s_ring_count > REC_CHUNK) ? REC_CHUNK : s_ring_count;

        /* 环形缓冲可能回绕，分两段拷贝到线性缓冲 */
        uint32_t first = REC_RING_SAMPLES - s_ring_rpos;
        if (first > n)
        {
            first = n;
        }
        memcpy(s_file_buf, &s_ring[s_ring_rpos], first * sizeof(int16_t));
        if (first < n)
        {
            memcpy(&s_file_buf[first], &s_ring[0], (n - first) * sizeof(int16_t));
        }

        UINT bw = 0U;
        res = f_write(&s_rec_file, s_file_buf, (UINT)(n * sizeof(int16_t)), &bw);
        s_file_bytes += bw;
        if ((res != FR_OK) || (bw != (UINT)(n * sizeof(int16_t))))
        {
            return (res != FR_OK) ? res : FR_DISK_ERR;
        }

        s_ring_rpos = (s_ring_rpos + n) % REC_RING_SAMPLES;
        s_ring_count -= n;
    }

    return res;
}

/* ---- 首尾淡入淡出（主循环 Stop 流程内调用，复用 s_file_buf，分块处理） ----- */
/* 对文件中 [file_off, file_off + total 样本) 区间做余弦坡幅度过渡：
 * fade_in=true  系数 0→1（首个样本 = 0，末个 ≈ 原幅）
 * fade_in=false 系数 1→0（首个样本 ≈ 原幅，末个 = 0）
 * 读-乘-写回文件；失败仅打印不中止（非致命，只影响首尾听感） */
#if REC_FADE_MS > 0U
static void Rec_FadeRegion(uint32_t file_off, uint32_t total, bool fade_in)
{
    for (uint32_t done = 0U; done < total; )
    {
        uint32_t chunk = total - done;
        if (chunk > REC_CHUNK)
        {
            chunk = REC_CHUNK;
        }

        UINT br = 0U;
        if ((f_lseek(&s_rec_file, file_off) != FR_OK) ||
            (f_read(&s_rec_file, s_file_buf, chunk * 2U, &br) != FR_OK) || (br != chunk * 2U))
        {
            printf("REC fade read fail\r\n");
            return;
        }

        for (uint32_t i = 0U; i < chunk; i++)
        {
            /* 余弦坡系数：起止斜率为 0，无线性坡的斜率突变，听感更平滑。
             * 主循环一次性处理，M4F 硬件浮点直接算。fade_in: 0→~1，fade_out: ~1→0 */
            float c = 0.5f * (1.0f - cosf((float)3.14159265f * (float)(done + i) / (float)total));
            if (!fade_in)
            {
                c = 1.0f - c;
            }
            s_file_buf[i] = (int16_t)((float)s_file_buf[i] * c);
        }

        UINT bw = 0U;
        if ((f_lseek(&s_rec_file, file_off) != FR_OK) ||
            (f_write(&s_rec_file, s_file_buf, chunk * 2U, &bw) != FR_OK) || (bw != chunk * 2U))
        {
            printf("REC fade write fail\r\n");
            return;
        }

        file_off += chunk * 2U;
        done += chunk;
    }
}
#endif

/* ---- 对外接口 ------------------------------------------------------------ */
void AudioRec_Init(void)
{
    s_state = AUDIO_REC_IDLE;
    s_ring_wpos = 0U;
    s_ring_rpos = 0U;
    s_ring_count = 0U;
    s_file_bytes = 0U;
    s_drop_count = 0U;
    s_i2s_err_count = 0U;
    s_peak = 0U;
    s_dc_q = 0;
}

FRESULT AudioRec_Start(void)
{
    if (s_state == AUDIO_REC_RECORDING)
    {
        return FR_OK; /* 已在录音，幂等返回 */
    }

    /* 0. 录音前先断开 USB MSC：主机（Windows）会后台读写 U 盘，与 FatFS 在
     *    SDIO 上并发冲突——卡忙时 disk_status 报 STA_NOINIT，FatFS 卷被作废，
     *    f_write 全部返回 FR_INVALID_OBJECT(9)。断开后主机侧表现为 U 盘拔出。
     *    s_usb_paused 追踪暂停状态：若已处于暂停（如连续录多段未恢复）则跳过。 */
    if (!s_usb_paused && (hUsbDeviceFS.pData != NULL))
    {
        (void)USBD_Stop(&hUsbDeviceFS);
        (void)USBD_DeInit(&hUsbDeviceFS);
        s_usb_paused = true;
        printf("USB MSC paused for recording\r\n");
    }

    /* 1. 创建录音目录（已存在则忽略） */
    FRESULT res = f_mkdir(REC_DIR);
    if ((res != FR_OK) && (res != FR_EXIST))
    {
        printf("REC mkdir fail: %d\r\n", (int)res);
        return res;
    }

    /* 2. 顺序查找空闲文件号 REC0001.WAV ... */
    FILINFO fno;
    UINT idx = 1U;
    do
    {
        snprintf(s_path, sizeof(s_path), REC_DIR "/REC%04u.WAV", (unsigned int)idx);
        res = f_stat(s_path, &fno);
        idx++;
    } while ((res == FR_OK) && (idx <= REC_MAX_FILES));

    if (res != FR_NO_FILE)
    {
        printf("REC no free file name (%d)\r\n", (int)res);
        return res;
    }

    /* 3. 打开文件并写 WAV 头（data 长度先置 0，停止时回填） */
    res = f_open(&s_rec_file, s_path, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
    if (res != FR_OK)
    {
        printf("REC open fail: %d\r\n", (int)res);
        return res;
    }

    uint8_t hdr[44];
    Rec_BuildWavHeader(hdr, 0U);

    UINT bw = 0U;
    res = f_write(&s_rec_file, hdr, sizeof(hdr), &bw);
    if ((res != FR_OK) || (bw != sizeof(hdr)))
    {
        printf("REC header write fail: %d\r\n", (int)res);
        f_close(&s_rec_file);
        return (res != FR_OK) ? res : FR_DISK_ERR;
    }

    /* 3.5 预分配文件空间（参考 STM32F407_Bootloader 的 OTA 写法）：
     * f_lseek 越过 EOF 会一次性把所需簇全部分配好并写入 FAT 表，此后录音中的
     * f_write 只写数据区、不再触碰 FAT——运行时簇分配要连续读写 FAT 扇区，
     * 叠加 SD 卡内部 GC 卡顿可长达数百 ms，环形缓冲被填满后 ISR 只能丢样本，
     * 听感就是"局部加速"。预分配把这一开销挪到录音开始前完成。
     * 停止时再 f_truncate 到实际长度，释放未用的预分配簇。 */
    res = f_lseek(&s_rec_file, REC_PREALLOC_BYTES);
    if (res == FR_OK)
    {
        res = f_truncate(&s_rec_file);
    }
    if (res == FR_OK)
    {
        res = f_lseek(&s_rec_file, (FSIZE_t)sizeof(hdr));
    }
    if (res != FR_OK)
    {
        /* 预分配失败（如卡剩余空间不足）不致命：回到数据起点，退化为运行时分配 */
        printf("REC prealloc fail: %d (continue without)\r\n", (int)res);
        f_lseek(&s_rec_file, (FSIZE_t)sizeof(hdr));
    }

    s_file_bytes = 0U;
    s_drop_count = 0U;
    s_i2s_err_count = 0U;
    s_peak = 0U;
    s_dc_q = 0; /* 直流估计清零，400ms 裁剪窗口内重新收敛（16ms 时间常数绰绰有余） */
    s_ring_wpos = 0U;
    s_ring_rpos = 0U;
    s_ring_count = 0U;

    /* 4. 启动 I2S DMA 循环采集前，先清零 DMA 缓冲区：
     * s_dma_buf 是静态数组，残留着上一次录音的数据（或上电随机值）。
     * DMA 一启动就会立刻把这些旧样本搬进文件，产生短促的"咔/噗"声，
     * 且落在 trim 窗口内无法被裁剪掉。清零后启动瞬态只表现为静音或底噪。 */
    memset(s_dma_buf, 0, sizeof(s_dma_buf));

    /* 注意：HAL_I2S_Receive_DMA 对 24B/32B 格式会把 Size 左移一位再写入 DMA
     * NDTR（HAL 假定 DMA 为半字传输，见 stm32f4xx_hal_i2s.c）。本工程 I2S2
     * DMA 配置为 32 位字对齐，NDTR 单位是字——若 Size 按字数传入，实际搬运
     * 量翻倍，越界 2560B 踩坏紧随其后的 s_file_buf / s_rec_file(FIL) / s_ring，
     * 导致 f_write 全报 FR_INVALID_OBJECT(9)。故 Size 必须传字数的一半。 */
    if (HAL_I2S_Receive_DMA(&hi2s2, (uint16_t *)s_dma_buf, REC_DMA_BUF_WORDS / 2U) != HAL_OK)
    {
        printf("REC I2S DMA start fail\r\n");
        f_close(&s_rec_file);
        return FR_DISK_ERR;
    }

    /* 裁剪窗口起点必须在 DMA 启动成功后立刻记录：
     * 此前 s_rec_start_tick 放在 DMA 启动前赋值，但 f_open/prealloc 等文件
     * 操作耗时几十~几百 ms，导致 DMA 启动后第一个中断到来时 tick 差值已
     * 超过 trim 阈值（100ms），开头数据未被丢弃，爆破音进文件。
     * 现在放到 DMA 启动后：第一个半传输中断在 5ms 后到来，差值一定 < trim。 */
    s_rec_start_tick = HAL_GetTick();

    s_state = AUDIO_REC_RECORDING;
    printf("REC start: %s (gain x%lu, trim %lums)\r\n", s_path,
           (unsigned long)(1UL << REC_GAIN_SHIFT),
           (unsigned long)REC_TRIM_HEAD_MS);
    return FR_OK;
}

FRESULT AudioRec_Stop(void)
{
    if (s_state != AUDIO_REC_RECORDING)
    {
        return FR_OK; /* 未在录音，幂等返回 */
    }

    /* 先切状态，避免中断/Process 继续往文件写 */
    s_state = AUDIO_REC_IDLE;

    HAL_I2S_DMAStop(&hi2s2);

    /* 把环形缓冲中剩余数据写完（可能带最后一次写错误） */
    FRESULT res = Rec_WriteRingToFile();
    if (res != FR_OK)
    {
        printf("REC final write fail: %d\r\n", (int)res);
    }

    /* 截断到实际长度：f_truncate 以当前 fptr 为新文件大小并释放其后预分配的
     * 簇；WAV 头中的 data 长度字段本就以 s_file_bytes 为准，二者一致 */
    if (res == FR_OK)
    {
#if REC_TRIM_TAIL_MS > 0U
        /* 末尾裁剪（REC_TRIM_TAIL_MS）：停止时刻的最后 N ms（停止按键的机械
         * 声/DMA 停止瞬态）与开头裁剪同理从文件尾截掉——数据是顺序写入的，
         * 直接回退文件长度即可，后续 f_truncate 一并完成截断与簇释放 */
        uint32_t tail_bytes = (uint32_t)REC_TRIM_TAIL_MS * 32U; /* 16kHz/16bit 单声道 = 32B/ms */
        s_file_bytes = (s_file_bytes > tail_bytes) ? (s_file_bytes - tail_bytes) : 0U;
#endif
        f_lseek(&s_rec_file, 44U + s_file_bytes);
        f_truncate(&s_rec_file);
    }

    /* 5. 首尾线性淡入淡出（REC_FADE_MS）：消除裁剪/截断点的硬截止咔哒声。
     *    在截尾后进行，淡出作用于最终保留数据的结尾；淡入作用于文件头。 */
#if REC_FADE_MS > 0U
    if (res == FR_OK)
    {
        uint32_t fade_n = REC_FADE_SAMPLES;
        if (fade_n * 2U > s_file_bytes)
        {
            fade_n = s_file_bytes / 2U; /* 数据比淡入淡出区还短：按实际长度 */
        }
        if (fade_n > 0U)
        {
            Rec_FadeRegion(44U, fade_n, true);                                /* 淡入：data 区起点 */
            Rec_FadeRegion(44U + s_file_bytes - fade_n * 2U, fade_n, false);   /* 淡出：最终结尾 */
        }
    }
#endif

    /* 回填 WAV 头中的长度字段（offset 4: RIFF 大小；offset 40: data 大小） */
    if (res == FR_OK)
    {
        uint8_t sz[4];
        UINT bw = 0U;

        Rec_Put32(sz, 36U + s_file_bytes);
        f_lseek(&s_rec_file, 4U);
        f_write(&s_rec_file, sz, 4U, &bw);

        Rec_Put32(sz, s_file_bytes);
        f_lseek(&s_rec_file, 40U);
        f_write(&s_rec_file, sz, 4U, &bw);
    }

    FRESULT cres = f_close(&s_rec_file);

    printf("REC stop: %s, %lu bytes, %lu.%lu s",
           s_path, (unsigned long)s_file_bytes,
           (unsigned long)(s_file_bytes / 32000U),
           (unsigned long)((s_file_bytes % 32000U) * 10U / 32000U));
    if (s_drop_count > 0U)
    {
        printf(", drop %lu", (unsigned long)s_drop_count);
    }
    if (s_i2s_err_count > 0U)
    {
        printf(", i2s err %lu", (unsigned long)s_i2s_err_count);
    }
    printf(", peak %lu/32767", (unsigned long)s_peak);
    printf(", dc %ld", (long)(s_dc_q >> REC_DC_SHIFT)); /* 直流估计：旧固件此值≈4700，去除后应≈0 */
    printf("\r\n");

    /* 4. 录音结束，按配置决定是否恢复 USB MSC（重新枚举，U 盘重新出现在主机上）；
     *    REC_USB_RESUME_ON_STOP=0 时保持断开，由外部调用 AudioRec_UsbResume() 恢复 */
#if REC_USB_RESUME_ON_STOP
    AudioRec_UsbResume();
#endif

    return (res != FR_OK) ? res : cres;
}

void AudioRec_UsbResume(void)
{
    /* 仅当本模块暂停过 USB 才恢复（s_usb_paused 追踪，幂等安全） */
    if (s_usb_paused)
    {
        MX_USB_DEVICE_Init();
        s_usb_paused = false;
        printf("USB MSC resumed\r\n");
    }
}

void AudioRec_Process(void)
{
    if (s_state != AUDIO_REC_RECORDING)
    {
        return;
    }

    if (s_ring_count == 0U)
    {
        return;
    }

    FRESULT res = Rec_WriteRingToFile();
    if (res != FR_OK)
    {
        /* 写卡失败：中止本次录音（Stop 内部会回收并尽量保存已录数据） */
        printf("REC write error: %d, abort\r\n", (int)res);
        (void)AudioRec_Stop();
    }
}

AudioRecState AudioRec_GetState(void)
{
    return s_state;
}

const char *AudioRec_GetFilePath(void)
{
    return s_path;
}
