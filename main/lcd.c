//======================================================================
//! @file	lcd.c
//! @brief	LCD表示関連
//======================================================================
#include <stdint.h>
#include <stdio.h>
#include <memory.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi.h"
#include "driver/gpio.h"

#include "global.h"
#include "lcd.h"
#include "font.h"
#include "charcode.h"
#include "setup.h"

// 定義
#define LCD_W 128									// LCD 横サイズ
#define LCD_H 64									// LCD 縦サイズ
#define LCD_LINES (LCD_H / 8)						// LCD 行数
#define VRAM_SIZE (LCD_W * LCD_LINES)				// LCD VRAM

// 定数
static const int bitPerByte = 8;					// 1byteあたりのbit数
static const uint8_t noUpdate = 0xff;				// 更新箇所のない行

// 静的変数
static uint8_t s_vram[VRAM_SIZE];					// 1画面分のデータ.まずはこのデータを書き換えて、後でまとめてLCDに転送する.
static uint8_t s_update[LCD_LINES][2];				// vram更新範囲 [行][0]開始位置、[行][1]終了位置
static xSemaphoreHandle s_lcdDataMutex;				// s_vram, s_updateに対するミューテックス

// プロトタイプ宣言
static const uint8_t *GetFont(const char *text, int *width, int *count, CharCode charCode);
static void SendData(const uint8_t *data, int size);
static inline void WaitMs(uint32_t timeMs);

//----------------------------------------------------------------------
//! @brief  LCD初期設定
//! @note   起動時に1回だけ呼ぶ.
//----------------------------------------------------------------------
void lcd_Initialize(void)
{
	const uint8_t lcdInitialCommands[] =		// 初期化コマンド
	{
		// リセット
		0xe2,				//15.soft reset

		// パネル設定
		0xa2 | ((0) << 0),	//17.LCD bias = 0:1/9
		0x20 | ((4) << 0),	// 8.display contrast (R ratio) = 4
		0x81,				// 9.display contrast (electronic volume) ...continue
		0x00 | ((24) << 0),	//   contrast value = 24
		0x28 | ((7) << 0),	// 5.power control = all power circuits on

		// データ設定
		0x40 | ((0) << 0),	// 6.display start line = 0
		0xa0 | ((0) << 0),	//13.data order (X) = 0:not reverse
		0xc0 | ((1) << 3),	//14.data order (Y) = 1:reverse 今回LCDを逆さで使用するので.
		0xe0,				//18.cursor update mode on
		//0xee,				//19.cursor update mode off
		0x00 | ((0) << 0),	// 4.column address(LSB) = 0
		0x10 | ((0) << 0),	// 4.column address(MSB) = 0
		0xb0 | ((0) << 0),	// 7.page address = 0

		// 表示設定
		0xa6 | ((0) << 0),	//11.inverse display = 0:not inverse
		0xa4 | ((0) << 0),	//10.all pixel on = 0:not on
		0xae | ((1) << 0),	//12.display = on

		//0xe3,				//16.no operation (NOP)
	};
	const int lcdInitialCommandSize = sizeof(lcdInitialCommands) / sizeof(uint8_t);
	const uint32_t resetTimeMs = 3;				// リセットかける時間 1ms以上
	const uint32_t commandDelayTimeMs = 8;		// リセット後コマンド受け付けるまで 6ms以上
	const uint32_t activeDelayTimeMs = 100;		// 初期化後データを受け付けるまで 120ms以上

	//----- 変数初期化 -----
	s_lcdDataMutex = xSemaphoreCreateMutex();

	//----- リセット -----
	gpio_set_level(GPIO_LCDCS_NUM, 0);	// LCD RST = L
	gpio_set_level(GPIO_SDCS_NUM, 0);
	WaitMs(resetTimeMs);
	gpio_set_level(GPIO_LCDCS_NUM, 1);	// LCD RST = H
	gpio_set_level(GPIO_SDCS_NUM, 1);

	//----- 初期化コマンド送信 -----
	set_TakeCommunicationMutex();
	set_SetPin(PinSetting_LcdMain, NULL);
	WaitMs(commandDelayTimeMs);
	gpio_set_level(GPIO_LCDCS_NUM, 0);	// CS=L
	gpio_set_level(GPIO_MISO_LCDRS_NUM, 0);	// CD=L command
	SendData(lcdInitialCommands, lcdInitialCommandSize);
	gpio_set_level(GPIO_LCDCS_NUM, 1);	// CS=H
	set_GiveCommunicationMutex();

	WaitMs(activeDelayTimeMs);
	lcd_BeginDrawing();
	lcd_Cls();
	lcd_EndDrawing();
	lcd_Update();
}

//----------------------------------------------------------------------
//! @brief  画面消去
//----------------------------------------------------------------------
void lcd_Cls(void)
{
	memset(s_vram, 0, VRAM_SIZE);
	for(int y = 0; y < LCD_LINES; y++)
	{
		s_update[y][0] = 0;
		s_update[y][1] = LCD_W - 1;
	}
}

//----------------------------------------------------------------------
//! @brief  直線描画
//! @param	x0		[I]始点x
//! @param	y0		[I]始点y
//! @param	x1		[I]終点x
//! @param	y1		[I]終点y
//----------------------------------------------------------------------
void lcd_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
	int dx = (x0 < x1) ? x1 - x0 : x0 - x1;
	int dy = (y0 < y1) ? y1 - y0 : y0 - y1;
	int sx = (x0 < x1) ? 1 : -1;
	int sy = (y0 < y1) ? 1 : -1;
	int error = dx - dy;
	int error2;
	int index;

	for(;;)
	{
		// 点描画
		if(y0 >= 0 && y0 < LCD_H && x0 >= 0 && x0 < LCD_W)
		{
			index = y0 / 8;
			s_vram[index * LCD_W + x0] |= 1 << (y0 % 8);

			// 更新位置
			if(s_update[index][0] == noUpdate || x0 < s_update[index][0])
			{
				s_update[index][0] = x0;
			}
			if(s_update[index][1] == noUpdate || x0 > s_update[index][1])
			{
				s_update[index][1] = x0;
			}
		}

		// 終了条件
		if(x0 == x1 && y0 == y1)
		{
			break;
		}

		// 誤差計算
		error2 = 2 * error;
		if(error2 > -dy)
		{
			error -= dy;
			x0 += sx;
		}
		if(error2 < dx)
		{
			error += dx;
			y0 += sy;
		}
	}
}

//----------------------------------------------------------------------
//! @brief  文字列描画
//! @param	area		[I]文字列表示エリア
//! @param	text		[I]文字列(Shift-JIS)
//! @param	charCode	[I]キャラクタコード
//----------------------------------------------------------------------
void lcd_Puts(Rect area, const char *text, CharCode charCode)
{
	const int fontHeight = 8;
	const uint8_t *font;
	int width, count;
	Rect letter;

	letter.x = area.x;
	letter.y = area.y;
	while(*text != '\0' && letter.y < LCD_H && letter.y < area.y + area.h)
	{
		if(*text == '\n')
		{
			text += 1;
			if(letter.x != area.x)
			{
				letter.x = area.x;
				letter.y += fontHeight;
			}
		}
		else if(*text == '\r')
		{
			text += 1;
			letter.x = area.x;
		}
		else
		{
			font = GetFont(text, &width, &count, charCode);
			text += count;
			width *= 4;
			if(letter.x + width > area.x + area.w)
			{
				letter.x = area.x;
				letter.y += fontHeight;
			}
			letter.w = (uint16_t)width;
			letter.h = (letter.y + fontHeight <= area.y + area.h) ? fontHeight : area.y + area.h - letter.y;
			lcd_PutImage(letter, font, NULL);
			letter.x += width;
		}
	}
}

//----------------------------------------------------------------------
//! @brief  フォントデータ位置取得
//! @param	text		[I]文字列(Shift-JIS)
//! @param	width		[O]1文字の文字幅
//! @param	count		[O]1文字のbyte数
//! @param	charCode	[I]キャラクタコード
//! @return	フォントデータ位置アドレス
//----------------------------------------------------------------------
const uint8_t *GetFont(const char *text, int *width, int *count, CharCode charCode)
{
	int code;

	code = (charCode == Code_Sjis)
		? char_TransSjisToSerial(text, width, count)
		: char_TransUtf8ToSerial(text, width, count);

	return (*width == 2) ? &jisFont[code * 8] : &asciiFont[code * 4];
}

//----------------------------------------------------------------------
//! @brief  画像描画
//! @param	imageRect	[I]表示位置、画像サイズ[dot]
//! @param	image		[I]画像データ
//! @param	mask		[I]マスクデータ 表示部=1, NULL指定でマスクなし
//----------------------------------------------------------------------
void lcd_PutImage(Rect imageRect, const uint8_t *image, const uint8_t *mask)
{
	int dx, dy;							// LCD上の位置
	uint8_t dmask, dimage;				// LCD上のマスク、絵
	int dindex;							// vram位置
	int dalign = (imageRect.y >= 0) ? imageRect.y % 8 : 8 - (-imageRect.y % 8);		// LCD-画像間の8dot境界とのずれ
	int dextra;							// 画像の端数
	int plines = (imageRect.h + 7) / 8;	// 画像上の行数

	//----- 描画 -----
	for(int x = 0; x < imageRect.w; x++)
	{
		dx = imageRect.x + x;
		if(dx < 0)		{ continue; }
		if(dx >= LCD_W)	{ break; }

		for(int y = 0; y < plines; y++)
		{
			dy = (imageRect.y >= 0) ? imageRect.y / 8 + y : (imageRect.y - 7) / 8 + y;
			if(dy >= LCD_LINES)	{ break; }

			dindex = dy * LCD_W + dx;
			dimage = image[y * imageRect.w + x];
			dextra = imageRect.h - y * 8;
			dmask = (mask != NULL) ? mask[y * imageRect.w + x] : 0xff;
			dmask &= (1 << ((dextra < 8) ? dextra : 8)) - 1;				// クリッピング

			// 8dot境界までを書き換え
			if(dy >= 0)
			{
				s_vram[dindex] = (s_vram[dindex] & ~(dmask << dalign)) | ((dimage << dalign) & (dmask << dalign));
			}
			// 8dot境界から残り部分までを書き換え
			dy++;
			if(dalign != 0 && (dextra + dalign) > 8 && dy >= 0 && dy < LCD_LINES)
			{
				dindex += LCD_W;
				s_vram[dindex] = (s_vram[dindex] & ~(dmask >> (8 - dalign))) | ((dimage >> (8 - dalign)) & (dmask >> (8 - dalign)));
			}
		}
	}

	//----- 更新箇所更新 -----
	if(imageRect.x < LCD_W && imageRect.y < LCD_H)
	{
		int x;
		for(dy = imageRect.y / 8; dy < (imageRect.y + imageRect.h + 7) / 8; dy++)
		{
			if(dy < 0)			{ continue; }
			if(dy >= LCD_LINES)	{ break; }
			dx = (imageRect.x >= 0) ? imageRect.x : 0;
			x = s_update[dy][0];
			s_update[dy][0] = (x == noUpdate) || (x > dx) ? dx : x;
			dx = (imageRect.x + imageRect.w < LCD_W) ? imageRect.x + imageRect.w - 1 : LCD_W - 1;
			x = s_update[dy][1];
			s_update[dy][1] = (x == noUpdate) || (x < dx) ? dx : x;
		}
	}
}

//----------------------------------------------------------------------
//! @brief  画面更新
//----------------------------------------------------------------------
void lcd_Update(void)
{
	uint8_t x, w, y;
	uint8_t cmd[3];

	set_TakeCommunicationMutex();
	set_SetPin(PinSetting_LcdMain, NULL);
	gpio_set_level(GPIO_LCDCS_NUM, 0);	// CS=L

	xSemaphoreTake(s_lcdDataMutex, portMAX_DELAY);
	for(y = 0; y < LCD_LINES; y++)
	{
		x = s_update[y][0];
		if(x != noUpdate)
		{
			gpio_set_level(GPIO_MISO_LCDRS_NUM, 0);		// CD=L command
			cmd[0] = 0xb0 | y;					// page
			cmd[1] = 0x00 | (x & 0x0f);			// column(LSB)
			cmd[2] = 0x10 | ((x >> 4) & 0x0f);	// column(MSB)
			SendData(cmd, 3);

			gpio_set_level(GPIO_MISO_LCDRS_NUM, 1);		// CD=H data
			w = s_update[y][1] - x + 1;
			SendData(&s_vram[y * LCD_W + x], w);

			s_update[y][0] = s_update[y][1] = noUpdate;
		}
	}
	xSemaphoreGive(s_lcdDataMutex);

	gpio_set_level(GPIO_LCDCS_NUM, 1);	// CS=H
	set_GiveCommunicationMutex();
}

//----------------------------------------------------------------------
//! @brief  描画開始
//----------------------------------------------------------------------
void lcd_BeginDrawing(void)
{
	xSemaphoreTake(s_lcdDataMutex, portMAX_DELAY);
}

//----------------------------------------------------------------------
//! @brief  描画終了
//----------------------------------------------------------------------
void lcd_EndDrawing(void)
{
	xSemaphoreGive(s_lcdDataMutex);
}

//----------------------------------------------------------------------
//! @brief  データ送信
//! @param	data	[I]送信データ
//! @param	size	[I]送信データ数[byte]
//----------------------------------------------------------------------
void SendData(const uint8_t *data, int size)
{
	const int maxTransferbytes = 64;
	const int alignmentSize = 4;

	int extraSize = alignmentSize - ((int)(data) & (alignmentSize - 1));	// 4byte境界になるようなbyte数
	if(size < extraSize)
	{
		extraSize = size;
	}

	uint16_t cmd;
	uint32_t addr;
	spi_trans_t trans;
	trans.cmd = &cmd;
	trans.addr = &addr;
	trans.bits.miso = 0;

	set_SetSpiTransFlag(1);

	for(int pos = 0; size > 0; )
	{
		//-- 4バイト境界までのデータセット
		if(extraSize == 1)
		{
			cmd = data[pos + 0];
			trans.bits.cmd = 1 * bitPerByte;
		}
		else // extraSize == 2,3,4
		{
			cmd = (data[pos + 1] << 8) | data[pos + 0];
			trans.bits.cmd = 2 * bitPerByte;
		}
		if(extraSize == 3)
		{
			addr = (data[pos + 2] << 24);							// addrはbig endian
			trans.bits.addr = 1 * bitPerByte;
		}
		else if(extraSize == 4)
		{
			addr = (data[pos + 2] << 24) | (data[pos + 3] << 16);	// addrはbig endian
			trans.bits.addr = 2 * bitPerByte;
		}
		else // extraSize == 1,2
		{
			trans.bits.addr = 0;
		}
		pos += extraSize;
		size -= extraSize;

		//-- 残りの転送データセット
		trans.mosi = (size == 0) ? NULL : (uint32_t *)&data[pos];
		trans.bits.mosi = ((size >= maxTransferbytes) ? maxTransferbytes : size) * bitPerByte;

		set_WaitSpiTrans();
		set_SetSpiTransFlag(0);
		spi_trans(HSPI_HOST, &trans);

		pos += (size >= maxTransferbytes) ? maxTransferbytes : size;
		size -= (size >= maxTransferbytes) ? maxTransferbytes : size;
		extraSize = (size < alignmentSize) ? size : alignmentSize;
	}
	set_WaitSpiTrans();
}

//----------------------------------------------------------------------
//! @brief  おおよその時間待つ
//! @param	timeMs	[I]時間[ms]
//----------------------------------------------------------------------
inline void WaitMs(uint32_t timeMs)
{
	vTaskDelay(timeMs / portTICK_PERIOD_MS);
}
