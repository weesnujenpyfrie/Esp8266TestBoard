//======================================================================
//! @file   main.c
//! @brief  エントリポイント
//======================================================================
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "global.h"
#include "setup.h"
#include "lcd.h"

//----------------------------------------------------------------------
//! @brief  エントリポイント
//----------------------------------------------------------------------
void app_main()
{
	//----- 初期化 -----
	set_Initialize();

	//----- ループ(Lチカ) -----
	int led = 0;
	while(1)
	{
		led ^= 1;
		gpio_set_level(GPIO_LED_NUM, led);
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}
