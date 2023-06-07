//======================================================================
//! @file   setup.h
//! @brief  システム初期化.
//======================================================================
#ifndef _SETUP_H_
#define _SETUP_H_

// ピンの設定
enum PinSetting
{
	PinSetting_Inititialized = 0,	// 初期設定時
	PinSetting_SdMount,				// SDカードマウント時(SPI 400kHz)
	PinSetting_SdMain,				// SDカードアクセス時(SPI 任意周波数)
	PinSetting_SdRead,				// SDカード読み込み時(SPI MOSI=H固定)
	PinSetting_LcdMain,				// LCD通信(SPI 20MHz)
	PinSetting_I2c					// I2C
};

int set_Initialize(void);
void set_SetPin(enum PinSetting setting, void *param);
void set_Task(void);
void set_WaitSpiTrans(void);
void set_SetSpiTransFlag(int value);
void set_TakeCommunicationMutex(void);
void set_GiveCommunicationMutex(void);

#endif
