/**
 * @file bsp_includes.h
 * @brief BSP 统一入口（裸机，仅保留驱动层，不含 Demo / 按键 / RTOS）
 */
#ifndef __BSP_INCLUDES_H__
#define __BSP_INCLUDES_H__

#include "main.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "delay.h"          /* DWT/SysTick 时间基准 */
#include "spi_bus.h"        /* SPI 总线抽象接口（MCU 无关） */
#include "bsp_spi.h"        /* 板级 SPI1 / SPI2 实例 */
#include "spi_stm32_ll.h"   /* STM32F4 LL 硬件适配层 */
#include "gc9a01.h"         /* GC9A01 TFT 协议驱动 */
#include "gc9a01_port.h"    /* GC9A01 板级移植层 */
#include "w25qxx.h"         /* W25QXX Flash 协议驱动 */
#include "w25qxx_port.h"    /* W25QXX 板级移植层 */

#endif /* __BSP_INCLUDES_H__ */
