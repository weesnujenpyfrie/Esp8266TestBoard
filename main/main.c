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

#define MUTEX_EN 0

void ExecTaskToUpdateLcd(void *arg)
{
	while(1)
	{
		lcd_Update();
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

void ExecTaskToDrawLcd(void *arg)
{
	while(1)
	{
#if MUTEX_EN
		lcd_BeginDrawing();
#endif
		Rect area = {0, 0, 16, 8};
		lcd_Cls();
		for(int y = 8; y < 64; y += 8)
		{
			area.y = y;
			for(int x = 0; x < 128; x += 16)
			{
				area.x = x;
				lcd_Puts(area, "末吉", Code_Utf8);
			}
		}
		lcd_Cls();
		for(int y = 8; y < 64; y += 8)
		{
			area.y = y;
			for(int x = 0; x < 128; x += 16)
			{
				area.x = x;
				lcd_Puts(area, "小吉", Code_Utf8);
			}
		}
		lcd_Cls();
		for(int y = 8; y < 64; y += 8)
		{
			area.y = y;
			for(int x = 0; x < 128; x += 16)
			{
				area.x = x;
				lcd_Puts(area, "中吉", Code_Utf8);
			}
		}
		lcd_Cls();
		for(int y = 8; y < 64; y += 8)
		{
			area.y = y;
			for(int x = 0; x < 128; x += 16)
			{
				area.x = x;
				lcd_Puts(area, "大吉", Code_Utf8);
			}
		}
#if MUTEX_EN
		lcd_EndDrawing();
#endif
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

//----------------------------------------------------------------------
//! @brief  エントリポイント
//----------------------------------------------------------------------
void app_main()
{
	//----- 初期化 -----
	set_Initialize();

	//----- テスト -----
	const uint16_t taskStackSize = 768;
	const UBaseType_t taskPriority = 1;

	// タスク生成
	xTaskCreate(ExecTaskToUpdateLcd, "update", taskStackSize, NULL, taskPriority, NULL);
	xTaskCreate(ExecTaskToDrawLcd, "draw", taskStackSize, NULL, taskPriority, NULL);

	//----- ループ(Lチカ) -----
	int led = 0;
	while(1)
	{
		led ^= 1;
		gpio_set_level(GPIO_LED_NUM, led);
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}
