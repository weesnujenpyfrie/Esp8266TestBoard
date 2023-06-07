//======================================================================
//! @file   main.c
//! @brief  エントリポイント
//======================================================================
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "global.h"
#include "lcd.h"

//----------------------------------------------------------------------
//! @brief  システムの初期化
//----------------------------------------------------------------------
void set_Initialize(void)
{
	const gpio_config_t pinInitialSettings[] =			// pin設定
	{
		// pin mask,   mode,              pull-up,             pull-down,             interrupt type
		{GPIO_Pin_0,   GPIO_MODE_INPUT,   GPIO_PULLUP_ENABLE,  GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// boot mode 1 / Switch
		{GPIO_Pin_2,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// boot mode 0 / -
		{GPIO_Pin_4,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// SD CS / LCD RST
		{GPIO_Pin_5,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// LCD CS / LCD RST
		{GPIO_Pin_12,  GPIO_MODE_OUTPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// MISO / LCD RS
		{GPIO_Pin_13,  GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// MOSI
		{GPIO_Pin_14,  GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// CLK
		{GPIO_Pin_15,  GPIO_MODE_OUTPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// boot mode 2 / -
		{GPIO_Pin_16,  GPIO_MODE_OUTPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// (wake) / LED

		{GPIO_Pin_All, GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE,  GPIO_INTR_DISABLE}	// End of data
	};

	//----- pin機能の初期化 -----
	for(int i = 0; pinInitialSettings[i].pin_bit_mask != GPIO_Pin_All; i++)
	{
		gpio_config(&pinInitialSettings[i]);
	}
	gpio_set_level(GPIO_SDCS_NUM, 1);	// SD cs
	gpio_set_level(GPIO_LCDCS_NUM, 1);	// LCD cs
	gpio_set_level(GPIO_LED_NUM, 0);	// LED
}

//----------------------------------------------------------------------
//! @brief  星表示
//----------------------------------------------------------------------
void DrawStar(int cx, int cy)
{
	const float longLength = 12.0f;
	const float shortLength = longLength / 2.0f;
	const float pi = 3.141592854f;

	for(float theta = 0.0f; theta < 359.0f; theta += 72.0f)
	{
		lcd_DrawLine(
			cx + (int)(longLength * cosf((theta + 90.0f) * pi / 180.0f)),
			cy - (int)(longLength * sinf((theta + 90.0f) * pi / 180.0f)),
			cx + (int)(shortLength * cosf((theta + 90.0f + 72.0f / 2.0f) * pi / 180.0f)),
			cy - (int)(shortLength * sinf((theta + 90.0f + 72.0f / 2.0f) * pi / 180.0f))
		);
		lcd_DrawLine(
			cx + (int)(longLength * cosf((theta + 90.0f) * pi / 180.0f)),
			cy - (int)(longLength * sinf((theta + 90.0f) * pi / 180.0f)),
			cx + (int)(shortLength * cosf((theta + 90.0f - 72.0f / 2.0f) * pi / 180.0f)),
			cy - (int)(shortLength * sinf((theta + 90.0f - 72.0f / 2.0f) * pi / 180.0f))
		);
	}
}

//----------------------------------------------------------------------
//! @brief  エントリポイント
//----------------------------------------------------------------------
void app_main()
{
	//----- 初期化 -----
	set_Initialize();
	lcd_Initialize();

	//----- テスト表示 -----
	// 星表示
	int x, y;
	for(int i = 0; i < 8; i++)
	{
		x = rand() % 128;
		y = rand() % 64;
		DrawStar(x, y);
	}

	// 文字列表示
	const char *testStr = "LCDテスト\nこの文字列はクリッピングされます";
	Rect area = { 12, 12, 128, 13 };
	lcd_DrawLine(area.x - 2, area.y - 2, area.x + area.w + 1, area.y - 2);
	lcd_DrawLine(area.x - 2, area.y - 2, area.x - 2, area.y + area.h + 1);
	lcd_DrawLine(area.x + area.w + 1, area.y - 2, area.x + area.w + 1, area.y + area.h + 1);
	lcd_DrawLine(area.x - 2, area.y + area.h + 1, area.x + area.w + 1, area.y + area.h + 1);
	lcd_Puts(area, testStr, Code_Utf8);
	lcd_Update();

	//----- ループ(Lチカ) -----
	int led = 0;
	while(1)
	{
		led ^= 1;
		gpio_set_level(GPIO_LED_NUM, led);
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}
