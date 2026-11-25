/**
 * @file    bsp_spi.c
 * @brief   板级 SPI 总线实现（STM32F407ZGT6 + LL）
 * @details 全工程唯一的 SPI 引脚/外设配置点。
 *          SPI1 -> GC9A01 TFT，SPI2 -> W25QXX Flash，各自独立 DMA。
 *          由 STM32F103 版移植：PCLK、AF、DMA Stream/Channel 均按 F407 重配。
 */
#include "bsp_spi.h"
#include "spi_stm32_ll.h"
#include "gc9a01_port.h"
#include "main.h"
#include <string.h>

/* ================================================================================== */
/* SPI1 板级配置：PA5/PA6/PA7 (AF5)，DMA2 Stream3(TX)/Stream0(RX)，Channel3          */
/* PCLK2 = HCLK/2 = 84 MHz                                                           */
/* ================================================================================== */

#define BSP_SPI1_PERIPH          SPI1
#define BSP_SPI1_PCLK_HZ         84000000U
#define BSP_SPI1_AF              LL_GPIO_AF_5

#define BSP_SPI1_SCK_PORT        GPIOA
#define BSP_SPI1_SCK_PIN         LL_GPIO_PIN_5
#define BSP_SPI1_MISO_PORT       GPIOA
#define BSP_SPI1_MISO_PIN        LL_GPIO_PIN_6
#define BSP_SPI1_MOSI_PORT       GPIOA
#define BSP_SPI1_MOSI_PIN        LL_GPIO_PIN_7

#define BSP_SPI1_DMA             DMA2
#define BSP_SPI1_DMA_TX_STREAM   LL_DMA_STREAM_3
#define BSP_SPI1_DMA_RX_STREAM   LL_DMA_STREAM_0
#define BSP_SPI1_DMA_CHANNEL     LL_DMA_CHANNEL_3

/* ================================================================================== */
/* SPI2 板级配置：PB13/PB14/PB15 (AF5)，DMA1 Stream4(TX)/Stream3(RX)，Channel0       */
/* PCLK1 = HCLK/4 = 42 MHz                                                           */
/* ================================================================================== */

#define BSP_SPI2_PERIPH          SPI2
#define BSP_SPI2_PCLK_HZ         42000000U
#define BSP_SPI2_AF              LL_GPIO_AF_5

#define BSP_SPI2_SCK_PORT        GPIOB
#define BSP_SPI2_SCK_PIN         LL_GPIO_PIN_13
#define BSP_SPI2_MISO_PORT       GPIOB
#define BSP_SPI2_MISO_PIN        LL_GPIO_PIN_14
#define BSP_SPI2_MOSI_PORT       GPIOB
#define BSP_SPI2_MOSI_PIN        LL_GPIO_PIN_15

#define BSP_SPI2_DMA             DMA1
#define BSP_SPI2_DMA_TX_STREAM   LL_DMA_STREAM_4
#define BSP_SPI2_DMA_RX_STREAM   LL_DMA_STREAM_3
#define BSP_SPI2_DMA_CHANNEL     LL_DMA_CHANNEL_0

/* ================================================================================== */
/* 内部变量                                                                           */
/* ================================================================================== */

static const spi_stm32_ll_hw_t s_spi1_hw = {
    .periph         = BSP_SPI1_PERIPH,
    .pclk_hz        = BSP_SPI1_PCLK_HZ,
    .sck_port       = BSP_SPI1_SCK_PORT,
    .sck_pin        = BSP_SPI1_SCK_PIN,
    .miso_port      = BSP_SPI1_MISO_PORT,
    .miso_pin       = BSP_SPI1_MISO_PIN,
    .mosi_port      = BSP_SPI1_MOSI_PORT,
    .mosi_pin       = BSP_SPI1_MOSI_PIN,
    .gpio_af        = BSP_SPI1_AF,
    .dma            = BSP_SPI1_DMA,
    .dma_tx_stream  = BSP_SPI1_DMA_TX_STREAM,
    .dma_rx_stream  = BSP_SPI1_DMA_RX_STREAM,
    .dma_channel    = BSP_SPI1_DMA_CHANNEL,
};

static const spi_stm32_ll_hw_t s_spi2_hw = {
    .periph         = BSP_SPI2_PERIPH,
    .pclk_hz        = BSP_SPI2_PCLK_HZ,
    .sck_port       = BSP_SPI2_SCK_PORT,
    .sck_pin        = BSP_SPI2_SCK_PIN,
    .miso_port      = BSP_SPI2_MISO_PORT,
    .miso_pin       = BSP_SPI2_MISO_PIN,
    .mosi_port      = BSP_SPI2_MOSI_PORT,
    .mosi_pin       = BSP_SPI2_MOSI_PIN,
    .gpio_af        = BSP_SPI2_AF,
    .dma            = BSP_SPI2_DMA,
    .dma_tx_stream  = BSP_SPI2_DMA_TX_STREAM,
    .dma_rx_stream  = BSP_SPI2_DMA_RX_STREAM,
    .dma_channel    = BSP_SPI2_DMA_CHANNEL,
};

static spi_stm32_ll_t s_spi_instances[BSP_SPI_BUS_MAX];
static uint8_t s_spi_inited[BSP_SPI_BUS_MAX] = {0U, 0U};

/* ================================================================================== */
/* 对外接口                                                                           */
/* ================================================================================== */

uint8_t BSP_SPI_IsBusActive(bsp_spi_id_t id)
{
    if ((uint32_t)id >= (uint32_t)BSP_SPI_BUS_MAX) {
        return 0U;
    }
    return s_spi_inited[id];
}

spi_bus_t *BSP_SPI_GetBus(bsp_spi_id_t id)
{
    if ((uint32_t)id >= (uint32_t)BSP_SPI_BUS_MAX) {
        return NULL;
    }
    if (!s_spi_inited[id]) {
        if (id == BSP_SPI_BUS_2) {
            /* 引脚冲突检查：若 TFT DC 正占用 PB13，拒绝启用 SPI2 */
            if (GC9A01_Port_IsPinUsed(GPIOB, LL_GPIO_PIN_13)) {
                return NULL;
            }
        }
        const spi_stm32_ll_hw_t *hw = (id == BSP_SPI_BUS_1) ? &s_spi1_hw : &s_spi2_hw;
        spi_stm32_ll_init(&s_spi_instances[id], hw);
        s_spi_inited[id] = 1U;
    }
    return spi_stm32_ll_bus(&s_spi_instances[id]);
}

spi_bus_t *BSP_SPI1_GetBus(void)
{
    return BSP_SPI_GetBus(BSP_SPI_BUS_1);
}

spi_bus_t *BSP_SPI2_GetBus(void)
{
    return BSP_SPI_GetBus(BSP_SPI_BUS_2);
}

int BSP_SPI_RecoverBus(spi_bus_t *bus)
{
    if (bus == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < BSP_SPI_BUS_MAX; i++) {
        if (s_spi_inited[i] && bus == spi_stm32_ll_bus(&s_spi_instances[i])) {
            return spi_stm32_ll_recover(&s_spi_instances[i]);
        }
    }
    return -1;
}

void BSP_SPI_ResetBuses(void)
{
    for (uint32_t i = 0; i < BSP_SPI_BUS_MAX; i++) {
        s_spi_inited[i] = 0U;
        memset(&s_spi_instances[i], 0, sizeof(s_spi_instances[i]));
    }
}

int BSP_SPI1_Recover(void)
{
    return BSP_SPI_RecoverBus(BSP_SPI1_GetBus());
}
