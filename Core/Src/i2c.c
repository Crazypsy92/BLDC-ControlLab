/**
 * @file    i2c.c
 * @brief   I2C1 初始化：PB6/PB7 (AF4)，快速模式 400 kHz
 * @details 用于 AS5600 磁编码器等传感器。PCLK1 = 42 MHz，
 *          LL_I2C_Init() 内部自动获取 PCLK1 频率计算 CCR/TRISE。
 */
#include "i2c.h"
#include "main.h"

void MX_I2C1_Init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    LL_I2C_InitTypeDef  I2C_InitStruct = {0};

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOB);

    /* PB6 = SCL, PB7 = SDA，开漏 + 上拉，AF4 */
    GPIO_InitStruct.Pin        = LL_GPIO_PIN_6 | LL_GPIO_PIN_7;
    GPIO_InitStruct.Mode       = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_OPENDRAIN;
    GPIO_InitStruct.Pull       = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    LL_GPIO_SetAFPin_0_7(GPIOB, LL_GPIO_PIN_6, LL_GPIO_AF_4);
    LL_GPIO_SetAFPin_0_7(GPIOB, LL_GPIO_PIN_7, LL_GPIO_AF_4);

    I2C_InitStruct.PeripheralMode  = LL_I2C_MODE_I2C;
    I2C_InitStruct.ClockSpeed      = 400000U;
    I2C_InitStruct.DutyCycle       = LL_I2C_DUTYCYCLE_2;
    I2C_InitStruct.AnalogFilter    = LL_I2C_ANALOGFILTER_ENABLE;
    I2C_InitStruct.DigitalFilter   = 0U;
    I2C_InitStruct.OwnAddress1     = 0U;
    I2C_InitStruct.TypeAcknowledge = LL_I2C_ACK;
    I2C_InitStruct.OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT;

    (void)LL_I2C_Init(I2C1, &I2C_InitStruct);
}
