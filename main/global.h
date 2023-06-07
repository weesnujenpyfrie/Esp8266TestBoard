//======================================================================
//! @file   global.h
//! @brief  グローバルな定義
//======================================================================
#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include "driver/gpio.h"

// ポート
#define GPIO_SWITCH_NUM		GPIO_NUM_0		// switch
#define GPIO_RES0_NUM		GPIO_NUM_2		// -
#define GPIO_SDCS_NUM		GPIO_NUM_4		// SD cs
#define GPIO_LCDCS_NUM		GPIO_NUM_5		// LCD cs
#define GPIO_MISO_LCDRS_NUM	GPIO_NUM_12		// SD sout / LCD rs
#define GPIO_MOSI_NUM		GPIO_NUM_13		// SD sin  / LCD sin
#define GPIO_SCLK_NUM		GPIO_NUM_14		// SD sclk / LCD sclk
#define GPIO_RES1_NUM		GPIO_NUM_15		// -
#define GPIO_LED_NUM		GPIO_NUM_16		// LED

#endif
