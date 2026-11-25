/**
 * @file    tim.h
 * @brief   TIM1 高级定时器：三相 PWM（FOC / 3PWM 驱动器）
 */
#ifndef __TIM_H__
#define __TIM_H__

#include "main.h"

/* 中心对齐 20 kHz：f_pwm = 168 MHz / (2 * ARR) */
#define TIM1_PWM_ARR        4199U
#define TIM1_PWM_FREQ_HZ    20000U

void MX_TIM1_Init(void);

/** @brief 设置三相占空比（0 ~ TIM1_PWM_ARR），供 FOC / SVPWM 调用 */
void TIM1_SetDuty3(uint16_t ch1, uint16_t ch2, uint16_t ch3);

#endif /* __TIM_H__ */
