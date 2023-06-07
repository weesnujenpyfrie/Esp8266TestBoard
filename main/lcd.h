//======================================================================
//! @file   lcd.h
//! @brief  LCD表示関連
//======================================================================
#ifndef _LCD_H_
#define _LCD_H_

#include <stdint.h>

typedef enum {Code_Utf8, Code_Sjis} CharCode;		// キャラクタコード
typedef struct										// 四角形
{
	int16_t x, y;		// 座標(x,y)
	uint16_t w, h;		// サイズ w x h
} Rect;

void lcd_Initialize(void);
void lcd_Cls(void);
void lcd_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void lcd_Puts(Rect area, const char *text, CharCode charCode);
void lcd_PutImage(Rect imageRect, const uint8_t *image, const uint8_t *mask);
void lcd_Update(void);
void lcd_BeginDrawing(void);
void lcd_EndDrawing(void);

#endif
