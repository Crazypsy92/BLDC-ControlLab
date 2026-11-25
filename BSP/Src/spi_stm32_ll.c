/**
 * @file    spi_stm32_ll.c
 * @brief   SPI 总线的 STM32F4 LL 实现（由 STM32F103 版移植）
 * @details 负责 GPIO 复用、SPI 外设时钟/参数配置与阻塞式全双工传输。
 *          与 F1 版的差异：
 *          - GPIO 时钟在 AHB1，且必须设置 Alternate Function（AF5）
 *          - DMA 由「Channel」改为「Stream + ChannelSelection」
 *          - DMA 标志寄存器分为 LISR/HISR 与 LIFCR/HIFCR
 *          上层只看到 spi_bus.h，换 MCU 只需替换本文件。
 */
#include "spi_stm32_ll.h"
#include <string.h>

/* 超时预算（单位：CPU 周期，168 MHz） */
#define SPI_BYTE_TIMEOUT_CYCLES   1000000U
#define SPI_BSY_TIMEOUT_CYCLES    1000000U
#define SPI_RECOVER_BSY_CYCLES    2000000U
#define SPI_DMA_MARGIN_CYCLES     2000000U

#define SPI_DMA_MIN_LEN           16U      /* 小于该长度走轮询 */
#define SPI_DMA_MAX_CHUNK         65535U   /* NDTR 为 16 位 */

/* 调试计数（HIL 验收用） */
volatile uint32_t g_ll_recover_count = 0U;
volatile int32_t  g_ll_last_error    = 0;

/* ================================================================================== */
/* 时钟使能（F4：GPIO/DMA 在 AHB1，SPI1 在 APB2，SPI2/3 在 APB1）                      */
/* ================================================================================== */

static void gpio_clock_enable(GPIO_TypeDef *port)
{
    if (port == GPIOA)      { LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA); }
    else if (port == GPIOB) { LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOB); }
    else if (port == GPIOC) { LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOC); }
    else if (port == GPIOE) { LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOE); }
}

static void spi_clock_enable(SPI_TypeDef *periph)
{
    if (periph == SPI1)      { LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1); }
    else if (periph == SPI2) { LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2); }
    else if (periph == SPI3) { LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI3); }
}

static void dma_clock_enable(DMA_TypeDef *dma)
{
    if (dma == DMA1)      { LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1); }
    else if (dma == DMA2) { LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2); }
}

/* ================================================================================== */
/* DWT 计时辅助（基于实际时间，处理 32 位回绕）                                        */
/* ================================================================================== */

static void dwt_enable(void)
{
    if ((DWT->CTRL & 1U) == 0U) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL  |= 1U;
    }
}

static inline uint32_t ll_dwt_cycles(void)
{
    return DWT->CYCCNT;
}

static inline int ll_timeout_expired(uint32_t start, uint32_t timeout)
{
    return ((ll_dwt_cycles() - start) > timeout) ? 1 : 0;
}

/* ================================================================================== */
/* 参数换算                                                                           */
/* ================================================================================== */

/** @brief 按目标频率选择不大于目标值的最近分频（取整到 2 的幂） */
static uint32_t spi_select_prescaler(uint32_t pclk_hz, uint32_t target_hz)
{
    uint32_t div;

    if (target_hz == 0U || pclk_hz == 0U) {
        return LL_SPI_BAUDRATEPRESCALER_DIV8;
    }
    for (div = 2U; div < 256U; div <<= 1) {
        if ((pclk_hz / div) <= target_hz) {
            break;
        }
    }
    if (div > 256U) {
        div = 256U;
    }

    switch (div) {
        case 2U:   return LL_SPI_BAUDRATEPRESCALER_DIV2;
        case 4U:   return LL_SPI_BAUDRATEPRESCALER_DIV4;
        case 8U:   return LL_SPI_BAUDRATEPRESCALER_DIV8;
        case 16U:  return LL_SPI_BAUDRATEPRESCALER_DIV16;
        case 32U:  return LL_SPI_BAUDRATEPRESCALER_DIV32;
        case 64U:  return LL_SPI_BAUDRATEPRESCALER_DIV64;
        case 128U: return LL_SPI_BAUDRATEPRESCALER_DIV128;
        default:   return LL_SPI_BAUDRATEPRESCALER_DIV256;
    }
}

/* ================================================================================== */
/* 抽象总线操作实现                                                                   */
/* ================================================================================== */

static inline void spi_drain_rx_and_clear_ovr(SPI_TypeDef *spi)
{
    while (LL_SPI_IsActiveFlag_RXNE(spi)) {
        (void)LL_SPI_ReceiveData8(spi);
    }
    (void)LL_SPI_ReceiveData8(spi);
    LL_SPI_ClearFlag_OVR(spi);
}

static int ll_configure(spi_bus_t *bus, const spi_bus_config_t *cfg)
{
    spi_stm32_ll_t *ll = (spi_stm32_ll_t *)bus->ctx;
    SPI_TypeDef *spi = ll->hw->periph;
    uint8_t need_spi_init;

    if (cfg == NULL || ll->hw == NULL) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    if (cfg->clock_hz == 0U || cfg->clock_hz > ll->hw->pclk_hz) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    if (cfg->mode > SPI_BUS_MODE_3) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    if (LL_SPI_IsActiveFlag_BSY(spi)) {
        return (int)SPI_LL_ERROR_BUSY;
    }
    if (ll->hw->dma != NULL) {
        if (LL_DMA_IsEnabledStream(ll->hw->dma, ll->hw->dma_tx_stream) ||
            LL_DMA_IsEnabledStream(ll->hw->dma, ll->hw->dma_rx_stream)) {
            return (int)SPI_LL_ERROR_BUSY;
        }
    }

    need_spi_init = (uint8_t)(!ll->configured ||
                              ll->current.clock_hz != cfg->clock_hz ||
                              ll->current.mode     != cfg->mode);

    if (need_spi_init) {
        LL_SPI_Disable(spi);
        LL_SPI_SetBaudRatePrescaler(spi, spi_select_prescaler(ll->hw->pclk_hz, cfg->clock_hz));

        switch (cfg->mode) {
            case SPI_BUS_MODE_1:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_LOW);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_2EDGE);
                break;
            case SPI_BUS_MODE_2:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_HIGH);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_1EDGE);
                break;
            case SPI_BUS_MODE_3:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_HIGH);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_2EDGE);
                break;
            case SPI_BUS_MODE_0:
            default:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_LOW);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_1EDGE);
                break;
        }
        LL_SPI_Enable(spi);
        spi_drain_rx_and_clear_ovr(spi);
    }

    ll->current = *cfg;
    ll->configured = 1U;
    return (int)SPI_LL_OK;
}

/* ================================================================================== */
/* DMA 辅助（F4：LISR/LIFCR 管 Stream0~3，HISR/HIFCR 管 Stream4~7）                   */
/* 每个 Stream 占 6 bit：FEIF(bit0) DMEIF(bit2) TEIF(bit3) HTIF(bit4) TCIF(bit5)      */
/* ================================================================================== */

static uint32_t dma_flag_shift(uint32_t stream)
{
    static const uint8_t shift[8] = {0U, 6U, 16U, 22U, 0U, 6U, 16U, 22U};
    return (uint32_t)shift[stream & 7U];
}

static uint32_t dma_read_flags(DMA_TypeDef *dma, uint32_t stream)
{
    return (stream < LL_DMA_STREAM_4) ? dma->LISR : dma->HISR;
}

/** @brief 清指定 Stream 的全部标志（FEIF/DMEIF/TEIF/HTIF/TCIF = 0x3D） */
static void dma_clear_flags(DMA_TypeDef *dma, uint32_t stream)
{
    uint32_t mask = 0x3DU << dma_flag_shift(stream);
    if (stream < LL_DMA_STREAM_4) {
        dma->LIFCR = mask;
    } else {
        dma->HIFCR = mask;
    }
}

/** @brief 等待指定 Stream 完成：检查 TCIF 与错误标志 */
static spi_ll_status_t dma_wait(DMA_TypeDef *dma, uint32_t stream, uint32_t timeout_cycles)
{
    uint32_t shift   = dma_flag_shift(stream);
    uint32_t tc_mask = 1U << (shift + 5U);
    uint32_t er_mask = (1U << shift) | (1U << (shift + 2U)) | (1U << (shift + 3U));
    uint32_t start = ll_dwt_cycles();

    while (1) {
        uint32_t isr = dma_read_flags(dma, stream);
        if ((isr & er_mask) != 0U) {
            return SPI_LL_ERROR_DMA;
        }
        if ((isr & tc_mask) != 0U) {
            return SPI_LL_OK;
        }
        if (ll_timeout_expired(start, timeout_cycles)) {
            return SPI_LL_ERROR_TIMEOUT;
        }
    }
}

/** @brief 配置一个 DMA Stream（字节宽度、单次模式、内存自增） */
static void dma_setup_stream(DMA_TypeDef *dma, uint32_t stream, uint32_t channel,
                             const uint8_t *mem, uint32_t periph_addr,
                             uint32_t direction, uint32_t len)
{
    LL_DMA_DisableStream(dma, stream);
    while (LL_DMA_IsEnabledStream(dma, stream) != 0U) { }
    dma_clear_flags(dma, stream);

    LL_DMA_SetChannelSelection(dma, stream, channel);
    LL_DMA_SetDataTransferDirection(dma, stream, direction);
    LL_DMA_SetMode(dma, stream, LL_DMA_MODE_NORMAL);
    LL_DMA_SetPeriphIncMode(dma, stream, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(dma, stream, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(dma, stream, LL_DMA_PDATAALIGN_BYTE);
    LL_DMA_SetMemorySize(dma, stream, LL_DMA_MDATAALIGN_BYTE);
    LL_DMA_SetStreamPriorityLevel(dma, stream, LL_DMA_PRIORITY_HIGH);
    LL_DMA_SetPeriphAddress(dma, stream, periph_addr);
    LL_DMA_SetMemoryAddress(dma, stream, (uint32_t)(uintptr_t)mem);
    LL_DMA_SetDataLength(dma, stream, len);
}

/* ================================================================================== */
/* 统一恢复                                                                           */
/* ================================================================================== */

static spi_ll_status_t spi_recover(spi_stm32_ll_t *ll)
{
    SPI_TypeDef *spi = ll->hw->periph;
    DMA_TypeDef *dma = ll->hw->dma;
    uint32_t start;

    ll->recover_count++;
    g_ll_recover_count++;

    LL_SPI_DisableDMAReq_TX(spi);
    LL_SPI_DisableDMAReq_RX(spi);

    if (dma != NULL) {
        LL_DMA_DisableStream(dma, ll->hw->dma_tx_stream);
        LL_DMA_DisableStream(dma, ll->hw->dma_rx_stream);
        dma_clear_flags(dma, ll->hw->dma_tx_stream);
        dma_clear_flags(dma, ll->hw->dma_rx_stream);
    }

    spi_drain_rx_and_clear_ovr(spi);

    start = ll_dwt_cycles();
    while (LL_SPI_IsActiveFlag_BSY(spi)) {
        if (ll_timeout_expired(start, SPI_RECOVER_BSY_CYCLES)) {
            LL_SPI_Disable(spi);
            LL_SPI_Enable(spi);
            spi_drain_rx_and_clear_ovr(spi);
            break;
        }
    }

    if (ll->current.clock_hz > 0U) {
        spi_bus_config_t saved = ll->current;
        ll->configured = 0U;
        if (ll_configure(&ll->bus, &saved) != (int)SPI_LL_OK) {
            return SPI_LL_ERROR_RECOVERY;
        }
    }
    return SPI_LL_OK;
}

/* ================================================================================== */
/* 传输实现                                                                           */
/* ================================================================================== */

static spi_ll_status_t ll_transfer_dma(spi_stm32_ll_t *ll,
                                       const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI_TypeDef *spi = ll->hw->periph;
    DMA_TypeDef *dma = ll->hw->dma;
    uint32_t txs = ll->hw->dma_tx_stream;
    uint32_t rxs = ll->hw->dma_rx_stream;
    uint32_t ch  = ll->hw->dma_channel;
    uint32_t offset = 0U;
    spi_ll_status_t status = SPI_LL_OK;
    uint32_t periph_addr = LL_SPI_DMA_GetRegAddr(spi);

    while (offset < len) {
        uint32_t chunk = len - offset;
        uint32_t timeout;
        if (chunk > SPI_DMA_MAX_CHUNK) {
            chunk = SPI_DMA_MAX_CHUNK;
        }

        spi_drain_rx_and_clear_ovr(spi);

        /* 先 RX 后 TX，保证时钟到来前接收端就绪 */
        if (rx != NULL) {
            dma_setup_stream(dma, rxs, ch, rx + offset, periph_addr,
                             LL_DMA_DIRECTION_PERIPH_TO_MEMORY, chunk);
        }
        dma_setup_stream(dma, txs, ch, tx + offset, periph_addr,
                         LL_DMA_DIRECTION_MEMORY_TO_PERIPH, chunk);

        if (rx != NULL) {
            LL_SPI_EnableDMAReq_RX(spi);
            LL_DMA_EnableStream(dma, rxs);
        }
        LL_SPI_EnableDMAReq_TX(spi);
        LL_DMA_EnableStream(dma, txs);

        timeout = (chunk * 8U * (ll->hw->pclk_hz / ll->current.clock_hz)) + SPI_DMA_MARGIN_CYCLES;

        status = dma_wait(dma, txs, timeout);
        if (status == SPI_LL_OK && rx != NULL) {
            status = dma_wait(dma, rxs, timeout);
        }
        if (status != SPI_LL_OK) {
            goto cleanup;
        }

        /* TCIF 只代表 DMA 搬完，仍要等 SPI 最后一位移出 */
        {
            uint32_t start = ll_dwt_cycles();
            while (!LL_SPI_IsActiveFlag_TXE(spi)) {
                if (ll_timeout_expired(start, SPI_BYTE_TIMEOUT_CYCLES)) {
                    status = SPI_LL_ERROR_TIMEOUT;
                    goto cleanup;
                }
            }
            start = ll_dwt_cycles();
            while (LL_SPI_IsActiveFlag_BSY(spi)) {
                if (ll_timeout_expired(start, SPI_BSY_TIMEOUT_CYCLES)) {
                    status = SPI_LL_ERROR_TIMEOUT;
                    goto cleanup;
                }
            }
        }

        LL_SPI_DisableDMAReq_TX(spi);
        LL_SPI_DisableDMAReq_RX(spi);
        LL_DMA_DisableStream(dma, txs);
        LL_DMA_DisableStream(dma, rxs);
        dma_clear_flags(dma, txs);
        dma_clear_flags(dma, rxs);
        if (rx == NULL) {
            spi_drain_rx_and_clear_ovr(spi);
        }
        offset += chunk;
    }

cleanup:
    LL_SPI_DisableDMAReq_TX(spi);
    LL_SPI_DisableDMAReq_RX(spi);
    if (dma != NULL) {
        LL_DMA_DisableStream(dma, txs);
        LL_DMA_DisableStream(dma, rxs);
        dma_clear_flags(dma, txs);
        dma_clear_flags(dma, rxs);
    }
    if (status != SPI_LL_OK) {
        (void)spi_recover(ll);
    }
    return status;
}

static spi_ll_status_t ll_transfer_poll(spi_stm32_ll_t *ll,
                                        const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI_TypeDef *spi = ll->hw->periph;
    uint32_t i;

    spi_drain_rx_and_clear_ovr(spi);

    for (i = 0U; i < len; i++) {
        uint32_t start = ll_dwt_cycles();
        while (!LL_SPI_IsActiveFlag_TXE(spi)) {
            if (ll_timeout_expired(start, SPI_BYTE_TIMEOUT_CYCLES)) {
                (void)spi_recover(ll);
                return SPI_LL_ERROR_TIMEOUT;
            }
        }
        LL_SPI_TransmitData8(spi, tx[i]);

        start = ll_dwt_cycles();
        while (!LL_SPI_IsActiveFlag_RXNE(spi)) {
            if (ll_timeout_expired(start, SPI_BYTE_TIMEOUT_CYCLES)) {
                (void)spi_recover(ll);
                return SPI_LL_ERROR_TIMEOUT;
            }
        }
        uint8_t b = LL_SPI_ReceiveData8(spi);
        if (rx != NULL) {
            rx[i] = b;
        }
    }

    {
        uint32_t start = ll_dwt_cycles();
        while (LL_SPI_IsActiveFlag_BSY(spi)) {
            if (ll_timeout_expired(start, SPI_BSY_TIMEOUT_CYCLES)) {
                (void)spi_recover(ll);
                return SPI_LL_ERROR_TIMEOUT;
            }
        }
    }
    if (rx == NULL) {
        spi_drain_rx_and_clear_ovr(spi);
    }
    return SPI_LL_OK;
}

static int ll_transfer(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    spi_stm32_ll_t *ll = (spi_stm32_ll_t *)bus->ctx;
    int ret;

    if (tx == NULL || len == 0U) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    if (ll->current.use_dma && ll->hw->dma != NULL && len >= SPI_DMA_MIN_LEN) {
        ret = (int)ll_transfer_dma(ll, tx, rx, len);
    } else {
        ret = (int)ll_transfer_poll(ll, tx, rx, len);
    }
    ll->last_error = ret;
    if (ret != (int)SPI_LL_OK) {
        g_ll_last_error = ret;
    }
    return ret;
}

static int ll_in_isr(spi_bus_t *bus)
{
    (void)bus;
    return (__get_IPSR() != 0U) ? 1 : 0;
}

static const spi_bus_ops_t s_stm32_ll_ops = {
    .configure = ll_configure,
    .transfer  = ll_transfer,
    .in_isr    = ll_in_isr,
};

/* ================================================================================== */
/* 对外接口                                                                           */
/* ================================================================================== */

static void gpio_af_init(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    LL_GPIO_InitTypeDef gpio = {0};

    gpio.Pin        = pin;
    gpio.Mode       = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull       = LL_GPIO_PULL_UP;
    LL_GPIO_Init(port, &gpio);

    /* F4 必须显式写 AFRL/AFRH；F1 无此步骤 */
    if ((pin & 0x00FFU) != 0U) {
        LL_GPIO_SetAFPin_0_7(port, pin, af);
    } else {
        LL_GPIO_SetAFPin_8_15(port, pin, af);
    }
}

void spi_stm32_ll_init(spi_stm32_ll_t *ll, const spi_stm32_ll_hw_t *hw)
{
    SPI_TypeDef *spi;

    if (ll == NULL || hw == NULL) {
        return;
    }

    ll->hw = hw;
    ll->bus.ops = &s_stm32_ll_ops;
    ll->bus.ctx = ll;
    ll->bus.owner = NULL;
    ll->bus.depth = 0U;
    ll->bus.rejects = 0U;
    ll->configured = 0U;
    ll->recover_count = 0U;
    ll->last_error = 0;
    memset(&ll->current, 0, sizeof(ll->current));

    dwt_enable();

    spi = hw->periph;
    gpio_clock_enable(hw->sck_port);
    gpio_clock_enable(hw->miso_port);
    gpio_clock_enable(hw->mosi_port);
    spi_clock_enable(spi);
    if (hw->dma != NULL) {
        dma_clock_enable(hw->dma);
    }

    /* SCK / MOSI / MISO 均为复用功能（AF5），MISO 由外设控制方向 */
    gpio_af_init(hw->sck_port,  hw->sck_pin,  hw->gpio_af);
    gpio_af_init(hw->mosi_port, hw->mosi_pin, hw->gpio_af);
    gpio_af_init(hw->miso_port, hw->miso_pin, hw->gpio_af);

    /* SPI 主机、全双工、8 位、软件 NSS（片选由各设备自己管） */
    LL_SPI_Disable(spi);
    LL_SPI_SetMode(spi, LL_SPI_MODE_MASTER);
    LL_SPI_SetTransferDirection(spi, LL_SPI_FULL_DUPLEX);
    LL_SPI_SetDataWidth(spi, LL_SPI_DATAWIDTH_8BIT);
    LL_SPI_SetNSSMode(spi, LL_SPI_NSS_SOFT);
    LL_SPI_SetBaudRatePrescaler(spi, LL_SPI_BAUDRATEPRESCALER_DIV8);
    LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_LOW);
    LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_1EDGE);
    LL_SPI_Enable(spi);

    (void)LL_SPI_ReceiveData8(spi);
}

spi_bus_t *spi_stm32_ll_bus(spi_stm32_ll_t *ll)
{
    return (ll == NULL) ? NULL : &ll->bus;
}

int spi_stm32_ll_recover(spi_stm32_ll_t *ll)
{
    if (ll == NULL || ll->hw == NULL) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    return (int)spi_recover(ll);
}

uint32_t spi_stm32_ll_get_recover_count(const spi_stm32_ll_t *ll)
{
    return (ll == NULL) ? 0U : ll->recover_count;
}

int32_t spi_stm32_ll_get_last_error(const spi_stm32_ll_t *ll)
{
    return (ll == NULL) ? 0 : ll->last_error;
}

void spi_stm32_ll_clear_error(spi_stm32_ll_t *ll)
{
    if (ll != NULL) {
        ll->last_error = 0;
    }
}
