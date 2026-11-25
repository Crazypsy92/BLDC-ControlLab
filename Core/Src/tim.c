/**
 * @file    tim.c
 * @brief   TIM1 高级定时器初始化：三相中心对齐 PWM（3PWM / FOC）
 * @details 引脚 PE9 / PE11 / PE13 = TIM1_CH1 / CH2 / CH3，AF1。
 *          这三个脚避开 BSP 占用的 PA 口（SPI1 + TFT 控制线）与 PB13~15（SPI2）。
 *          20 kHz 中心对齐，PWM1 模式，三路同步，适合 DRV8313 等 3PWM 驱动。
 */
#include "tim.h"
#include "main.h"

void MX_TIM1_Init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOE);

    /* PE9 / PE11 / PE13 -> TIM1_CH1 / CH2 / CH3 (AF1) */
    GPIO_InitStruct.Pin        = LL_GPIO_PIN_9 | LL_GPIO_PIN_11 | LL_GPIO_PIN_13;
    GPIO_InitStruct.Mode       = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull       = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOE, &GPIO_InitStruct);
    LL_GPIO_SetAFPin_8_15(GPIOE, LL_GPIO_PIN_9,  LL_GPIO_AF_1);
    LL_GPIO_SetAFPin_8_15(GPIOE, LL_GPIO_PIN_11, LL_GPIO_AF_1);
    LL_GPIO_SetAFPin_8_15(GPIOE, LL_GPIO_PIN_13, LL_GPIO_AF_1);

    /* 时基：中心对齐，f_pwm = 168 MHz / (2 * (ARR+1)) = 20 kHz */
    LL_TIM_SetPrescaler(TIM1, 0U);
    LL_TIM_SetCounterMode(TIM1, LL_TIM_COUNTERMODE_CENTER_UP_DOWN);
    LL_TIM_SetAutoReload(TIM1, TIM1_PWM_ARR);
    LL_TIM_EnableARRPreload(TIM1);

    /* 三路 PWM1，高有效，占空比初值 0（输出关闭，安全上电） */
    LL_TIM_OC_SetMode(TIM1,     LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1);
    LL_TIM_OC_SetMode(TIM1,     LL_TIM_CHANNEL_CH2, LL_TIM_OCMODE_PWM1);
    LL_TIM_OC_SetMode(TIM1,     LL_TIM_CHANNEL_CH3, LL_TIM_OCMODE_PWM1);
    LL_TIM_OC_SetPolarity(TIM1, LL_TIM_CHANNEL_CH1, LL_TIM_OCPOLARITY_HIGH);
    LL_TIM_OC_SetPolarity(TIM1, LL_TIM_CHANNEL_CH2, LL_TIM_OCPOLARITY_HIGH);
    LL_TIM_OC_SetPolarity(TIM1, LL_TIM_CHANNEL_CH3, LL_TIM_OCPOLARITY_HIGH);
    LL_TIM_OC_EnablePreload(TIM1, LL_TIM_CHANNEL_CH1);
    LL_TIM_OC_EnablePreload(TIM1, LL_TIM_CHANNEL_CH2);
    LL_TIM_OC_EnablePreload(TIM1, LL_TIM_CHANNEL_CH3);
    LL_TIM_OC_SetCompareCH1(TIM1, 0U);
    LL_TIM_OC_SetCompareCH2(TIM1, 0U);
    LL_TIM_OC_SetCompareCH3(TIM1, 0U);

    /* 高级定时器必须置位 MOE 才会有输出 */
    LL_TIM_EnableAllOutputs(TIM1);
    LL_TIM_GenerateEvent_UPDATE(TIM1);
    LL_TIM_EnableCounter(TIM1);
}

void TIM1_SetDuty3(uint16_t ch1, uint16_t ch2, uint16_t ch3)
{
    if (ch1 > TIM1_PWM_ARR) { ch1 = TIM1_PWM_ARR; }
    if (ch2 > TIM1_PWM_ARR) { ch2 = TIM1_PWM_ARR; }
    if (ch3 > TIM1_PWM_ARR) { ch3 = TIM1_PWM_ARR; }
    LL_TIM_OC_SetCompareCH1(TIM1, ch1);
    LL_TIM_OC_SetCompareCH2(TIM1, ch2);
    LL_TIM_OC_SetCompareCH3(TIM1, ch3);
}
