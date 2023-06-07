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

	//----- テスト -----
	char str[32 + 1];
	size_t strLength;
	FILE *fp = fopen("/sd/config.txt","rt");
	if(fp != NULL)
	{
		strLength = fread(str, sizeof(char), 32, fp);
		str[strLength] = '\0';
		fclose(fp);
		Rect area = {0, 8, 128, 8};
		lcd_BeginDrawing();
		lcd_Puts(area, str, Code_Utf8);
		lcd_EndDrawing();
		lcd_Update();
	}

	//----- ループ(Lチカ) -----
	int led = 0;
	while(1)
	{
		led ^= 1;
		gpio_set_level(GPIO_LED_NUM, led);
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}
