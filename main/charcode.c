//======================================================================
//! @file	charcode.c
//! @brief	文字コード変換
//======================================================================
#include <stdint.h>
#include <stdio.h>
#include "charcode.h"
#include "ff.h"

#define JIS_KU_COUNT  94
#define JIS_TEN_COUNT 94

//----------------------------------------------------------------------
//! @brief	UTF-8の文字列から1文字分を区点連番コードとして抽出
//! @param	text	[I]UTF-8文字列
//! @param	width	[O]抽出した1文字の文字幅
//! @param	bytes	[O]抽出した1文字のバイト数
//! @return	抽出した1文字の区点連番コード
//----------------------------------------------------------------------
int char_TransUtf8ToSerial(const char *text, int *width, int *bytes)
{
	int code = 0;
	int count = 0;

	// UTF-8 1文字をUnicodeとして抽出 & UTF-8バイト数カウント
	if(*text == '\0')
	{
		// 文字列の終端
	}
	else if(*text < 0x80)
	{
		count = 1;
		code = *text;
	}
	else if(*text < 0xc0)
	{
		// 文字コードの途中
	}
	else if(*text < 0xe0)
	{
		count = 2;
		code = *text & 0x1f;
	}
	else if(*text < 0xf0)
	{
		count = 3;
		code = *text & 0x0f;
	}
	else if(*text < 0xf8)
	{
		count = 4;
		code = *text & 0x07;
	}
	else
	{
		// ありえないコード
	}

	for(int i = 1; i < count; i++)
	{
		text++;
		code <<= 6;
		code |= *text & 0x3f;
	}

	if(bytes != NULL)
	{
		*bytes = count;
	}

	// Unicode -> 区点連番 or 8bitコード変換
	uint16_t sjis = (uint16_t)ff_uni2oem((DWORD)code, FF_CODE_PAGE);
	char c[2] = {0, 0};
	if(sjis <= 0xff)
	{
		c[0] = sjis;
	}
	else
	{
		c[0] = (sjis >> 8) & 0xff;
		c[1] = sjis & 0xff;
	}

	return char_TransSjisToSerial(c, width, NULL);
}

//----------------------------------------------------------------------
//! @brief	Shift-JIS → 区点連番コード変換
//! @param	text	[I]Shift-JIS文字列
//! @param	width	[O]抽出した1文字の文字幅
//! @param	bytes	[O]抽出した1文字のバイト数
//! @return	抽出した1文字の区点連番コード
//----------------------------------------------------------------------
int char_TransSjisToSerial(const char *text, int *width, int *bytes)
{
	uint8_t code[2] = {0, 0};
	int ret;

	code[0] = (uint8_t)text[0];
	if((code[0] >= 0x81 && code[0] <= 0x9f)	|| (code[0] >= 0xe0 && code[0] <= 0xef))
	{
		code[1] = (uint8_t)text[1];
		code[0] -= (code[0] >= 0xe0) ? (0xe0 - 0x1f) : 0x81;
		code[1] -= (code[1] >= 0x80) ? (0x80 - 0x3f) : 0x40;
		if(bytes != NULL)
		{
			*bytes = 2;
		}
		if(width != NULL)
		{
			*width = 2;
		}
		ret = (code[0] << 1) * JIS_TEN_COUNT + code[1];
	}
	else
	{
		if(bytes != NULL)
		{
			*bytes = 1;
		}
		if(width != NULL)
		{
			*width = 1;
		}
		ret = code[0];
	}

	return ret;
}

//----------------------------------------------------------------------
//! @brief	UTF-8 → UTF-16変換
//! @param	utf8 [I]UTF-8コード
//! @return	対応するUTF-16コード(Unicode)
//----------------------------------------------------------------------
int char_TransUtf8ToUtf16(uint32_t utf8)
{
	int ret = 0;

	#define SHIFTAND(value, bytes, mask) (((value) >> (bytes * 2)) & ((mask) << (bytes * 6)))
	if(utf8 < 0x80)
	{
		ret = utf8;
	}
	else if(utf8 < 0xe000)
	{
		ret = SHIFTAND(utf8, 1, 0x1f)
			| SHIFTAND(utf8, 0, 0x3f);
	}
	else if(utf8 < 0xf00000)
	{
		ret = SHIFTAND(utf8, 2, 0x0f)
			| SHIFTAND(utf8, 1, 0x3f)
			| SHIFTAND(utf8, 0, 0x3f);
	}
	else
	{
		ret = SHIFTAND(utf8, 3, 0x07)
			| SHIFTAND(utf8, 2, 0x3f)
			| SHIFTAND(utf8, 1, 0x3f)
			| SHIFTAND(utf8, 0, 0x3f);
	}
	#undef SHIFTAND

	return ret;
}

// 		Unicode   文字種類
// 		-------------------------
// 		0000-007f ラテン(ASCII)
// 		0370-03ff ギリシア
// 		0400-04ff キリル
// 		2460-24ff 囲み数字
// 		2500-2570 罫線
// 		3040-309f ひらがな
// 		30a0-30ff カタカナ
// 		3300-33ff CJK互換文字(単位)
// 		4e00-9fff 漢字
// 		ff60-ff9f 半角カタカナ code - 0xff60 + 0xa0
