//======================================================================
//! @file	charcode.h
//! @brief	文字コード変換
//======================================================================
#ifndef _CHARCODE_H_
#define _CHARCODE_H_

#include <stdint.h>

int char_TransUtf8ToSerial(const char *text, int *width, int *bytes);
int char_TransSjisToSerial(const char *text, int *width, int *bytes);
int char_TransUtf8ToUtf16(uint32_t utf8);

#endif	//_CHARCODE_H_
