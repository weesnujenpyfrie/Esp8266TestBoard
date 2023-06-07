//======================================================================
//! @file   setup.c
//! @brief  システム設定
//======================================================================
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi.h"
#include "driver/i2c.h"
#include "esp8266/spi_struct.h"

#include "global.h"
#include "setup.h"
#include "lcd.h"
#include "sd.h"
#include "wifi.h"

static const gpio_config_t pinInitialSettings[] =			// pin初期設定
{
	// pin mask,   mode,              pull-up,             pull-down,             interrupt type
	{GPIO_Pin_0,   GPIO_MODE_INPUT,   GPIO_PULLUP_ENABLE,  GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// boot mode 1 / Switch
	//{GPIO_Pin_1,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// TXD
	{GPIO_Pin_2,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// boot mode 0 / -
	//{GPIO_Pin_3,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// RXD
	{GPIO_Pin_4,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// SD CS / LCD RST
	{GPIO_Pin_5,   GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// LCD CS / LCD RST
	//{GPIO_Pin_6,   GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// internal flash CLK
	//{GPIO_Pin_7,   GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// internal flash MISO
	//{GPIO_Pin_8,   GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// internal flash MOSI
	//{GPIO_Pin_9,   GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// internal flash SPIHD
	//{GPIO_Pin_10,  GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// internal flash SPIWP
	//{GPIO_Pin_11,  GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// internal flash CS0
	{GPIO_Pin_12,  GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// MISO / LCD RS
	{GPIO_Pin_13,  GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// MOSI
	{GPIO_Pin_14,  GPIO_MODE_OUTPUT,  GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// CLK
	{GPIO_Pin_15,  GPIO_MODE_OUTPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// boot mode 2 / -
	{GPIO_Pin_16,  GPIO_MODE_OUTPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE},	// LED

	{GPIO_Pin_All, GPIO_MODE_INPUT,   GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE,  GPIO_INTR_DISABLE}	// End of data
};

typedef struct
{
	uint32_t pinName;
	uint32_t function;
} function_config_t;

static const function_config_t functionInitialSettings[] =	// pin機能初期設定
{
	{PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0},		// boot mode 1 / Switch
	{PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2},		// boot mode 0 / -
	{PERIPHS_IO_MUX_GPIO4_U, FUNC_GPIO4},		// SD CS / LCD RST
	{PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5},		// LCD CS / LCD RST
	{PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12},		// MISO / LCD RS
	{PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13},		// MOSI
	{PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14},		// CLK
	{PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15},		// boot mode 2 / -

	{PAD_XPD_DCDC_CONF, 0}						// End of data
};

static volatile int s_spiTransDone;					// SPI 転送完了フラグ
static enum PinSetting s_pinStatus;					// 競合ピン設定
static xSemaphoreHandle s_communicationPinMutex;	// 通信ピンmutex

static void SetAllGpio(void);
static void SetSpi(int mosiEnable, int misoEnable, spi_clk_div_t div, int prescale);
static void SetI2c(enum PinSetting setting);
static void IRAM_ATTR SpiEventCallback(int event, void *arg);

//----------------------------------------------------------------------
//! @brief  システムの初期化
//! @note	システム全体を初期状態にする.最初に実行すること
//----------------------------------------------------------------------
int set_Initialize(void)
{
	esp_err_t ret;

	//----- pinの初期化 -----
	// 一旦全てGPIOにする
	for(int i = 0; functionInitialSettings[i].pinName != PAD_XPD_DCDC_CONF; i++)
	{
		PIN_FUNC_SELECT(functionInitialSettings[i].pinName, functionInitialSettings[i].function);
	}
	for(int i = 0; pinInitialSettings[i].pin_bit_mask != GPIO_Pin_All; i++)
	{
		gpio_config(&pinInitialSettings[i]);
	}

	// 初期出力状態
	gpio_set_level(GPIO_SWITCH_NUM, 0);		// ずっと入力ピンなのでどうでもよい
	gpio_set_level(GPIO_RES0_NUM, 0);
	gpio_set_level(GPIO_SDCS_NUM, 1);
	gpio_set_level(GPIO_LCDCS_NUM, 1);
	gpio_set_level(GPIO_MISO_LCDRS_NUM, 0);
	gpio_set_level(GPIO_MOSI_NUM, 0);
	gpio_set_level(GPIO_SCLK_NUM, 0);
	gpio_set_level(GPIO_RES1_NUM, 0);
	gpio_set_level(GPIO_LED_NUM, 0);

	//----- 内部コンポーネントの初期化 -----
	// NVS (Non-Volatile Storage) : Wi-FiモジュールがNVSの値を参照するため
	ret = nvs_flash_init();
	if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		nvs_flash_erase();
		ret = nvs_flash_init();
	}

	// I2C (将来的に使用する可能性があるため初期化しておく)
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER);

	// SPI
	spi_config_t spiConfig;											// 設定用の入れ物
	spiConfig.interface.cpol = SPI_CPOL_LOW;						// アイドル時clk=L
	spiConfig.interface.cpha = SPI_CPHA_LOW;						// clk=!POL時サンプリング
	spiConfig.interface.bit_tx_order = SPI_BIT_ORDER_LSB_FIRST;		// [SDK誤記] この設定でMSB Firstになる
	spiConfig.interface.bit_rx_order = SPI_BIT_ORDER_LSB_FIRST;		// [SDK誤記] この設定でMSB Firstになる
	spiConfig.interface.byte_tx_order = SPI_BYTE_ORDER_LSB_FIRST;	// little endian (cmd=little固定, address=big固定)
	spiConfig.interface.byte_rx_order = SPI_BYTE_ORDER_LSB_FIRST;	// little endian
	spiConfig.interface.mosi_en = 1;
	spiConfig.interface.miso_en = 0;
	spiConfig.interface.cs_en = 0;
	spiConfig.intr_enable.val = SPI_MASTER_DEFAULT_INTR_ENABLE;		// TRANS_DONE:true, WRITE_STATUS:false, READ_STATUS:false, WRITE_BUFFER:false, READ_BUFFER:false
	spiConfig.mode = SPI_MASTER_MODE;								// マスターモード
	spiConfig.clk_div = SPI_2MHz_DIV;								// デフォルト速度2MHz
	spiConfig.event_cb = SpiEventCallback;							// SPIイベントコールバック関数登録
	spi_init(HSPI_HOST, &spiConfig);

	//----- 設定値の初期化 -----
	s_communicationPinMutex = xSemaphoreCreateMutex();
	s_pinStatus = PinSetting_Inititialized;
	s_spiTransDone = 1;

	//----- モジュールの初期化 -----
	sd_Initialize();
	sd_Mount();
	lcd_Initialize();
	wifi_Initialize();

	return 1;
}

//----------------------------------------------------------------------
//! @brief	競合ピン(gpio12,13,14)の設定
//! @param	setting		[I]設定するピン設定
//! @param	param		[I]パラメータ
//----------------------------------------------------------------------
void set_SetPin(enum PinSetting setting, void *param)
{
	const int mosiEnable = 1, mosiDisable = 0;
	const int misoEnable = 1, misoDisable = 0;
	const uint32_t sdMountSpiClockDivider = 10;

	if(setting != s_pinStatus)
	{
		s_pinStatus = setting;
		switch(setting)
		{
		case PinSetting_LcdMain:
			SetSpi(mosiEnable, misoDisable, SPI_20MHz_DIV, 1);
			break;
		case PinSetting_SdMount:
			SetSpi(mosiEnable, misoEnable, SPI_4MHz_DIV, sdMountSpiClockDivider);
			break;
		case PinSetting_SdMain:
			SetSpi(mosiEnable, misoEnable, SPI_20MHz_DIV, 1);
			break;
		case PinSetting_SdRead:
			SetSpi(mosiDisable, misoEnable, SPI_2MHz_DIV, 1);
			break;
		case PinSetting_I2c:
			SetI2c(PinSetting_I2c);
			break;
		case PinSetting_Inititialized:
			SetAllGpio();
			break;
		default:
			break;
		}
	}
}

//----------------------------------------------------------------------
//! @brief	GPIOピン設定
//----------------------------------------------------------------------
void SetAllGpio(void)
{
	for(int i = 0; functionInitialSettings[i].pinName != PAD_XPD_DCDC_CONF; i++)
	{
		if(functionInitialSettings[i].pinName == PERIPHS_IO_MUX_MTDI_U
		|| functionInitialSettings[i].pinName == PERIPHS_IO_MUX_MTCK_U
		|| functionInitialSettings[i].pinName == PERIPHS_IO_MUX_MTMS_U)
		{
			PIN_FUNC_SELECT(functionInitialSettings[i].pinName, functionInitialSettings[i].function);
		}
	}
	for(int i = 0; pinInitialSettings[i].pin_bit_mask != GPIO_Pin_All; i++)
	{
		if(pinInitialSettings[i].pin_bit_mask == GPIO_Pin_12
		|| pinInitialSettings[i].pin_bit_mask == GPIO_Pin_13
		|| pinInitialSettings[i].pin_bit_mask == GPIO_Pin_14)
		{
			gpio_config(&pinInitialSettings[i]);
		}
	}
}

//----------------------------------------------------------------------
//! @brief	SPIピン設定
//----------------------------------------------------------------------
void SetSpi(int mosiEnable, int misoEnable, spi_clk_div_t div, int prescale)
{
	vPortEnterCritical();

	//----- インターフェイス設定 -----
	spi_interface_t interface;								// 設定用の入れ物
	interface.cpol = SPI_CPOL_LOW;							// アイドル時clk=L
	interface.cpha = SPI_CPHA_LOW;							// clk=!POL時サンプリング
	interface.bit_tx_order = SPI_BIT_ORDER_LSB_FIRST;		// [SDK誤記] この設定でMSB Firstになる
	interface.bit_rx_order = SPI_BIT_ORDER_LSB_FIRST;		// [SDK誤記] この設定でMSB Firstになる
	interface.byte_tx_order = SPI_BYTE_ORDER_LSB_FIRST;		// mosi=little endian(cmd=little固定, address=big固定)
	interface.byte_rx_order = SPI_BYTE_ORDER_LSB_FIRST;		// miso=little endian
	interface.mosi_en = (mosiEnable == 0) ? 0 : 1;
	interface.miso_en = (misoEnable == 0) ? 0 : 1;
	interface.cs_en = 0;									// CSは手動でH/Lするので使用しない
    spi_set_interface(HSPI_HOST, &interface);

	if(mosiEnable == 0)
	{
		PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(GPIO_MOSI_NUM), FUNC_GPIO13);
		gpio_set_direction(GPIO_MOSI_NUM, GPIO_MODE_INPUT);
	}
	if(misoEnable == 0)
	{
		PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(GPIO_MISO_LCDRS_NUM), FUNC_GPIO12);
		gpio_set_direction(GPIO_MISO_LCDRS_NUM, GPIO_MODE_OUTPUT);
	}

	//----- クロック設定 -----
    spi_set_clk_div(HSPI_HOST, &div);
	(&SPI1)->clock.clkdiv_pre = prescale - 1;

	vPortExitCritical();
}

//----------------------------------------------------------------------
//! @brief	I2Cピン設定
//----------------------------------------------------------------------
void SetI2c(enum PinSetting setting)
{
	i2c_config_t i2cConfig;					// 設定用の入れ物
	i2cConfig.mode = I2C_MODE_MASTER;
	i2cConfig.scl_io_num = GPIO_SCLK_NUM;
	i2cConfig.scl_pullup_en = GPIO_PULLUP_ENABLE;
	i2cConfig.sda_io_num = GPIO_MOSI_NUM;
	i2cConfig.sda_pullup_en = GPIO_PULLUP_ENABLE;
	i2cConfig.clk_stretch_tick = 1;
	i2c_param_config(I2C_NUM_0, &i2cConfig);
}

//----------------------------------------------------------------------
//! @brief	タスクの初期化
//! @note	排他処理していないのでシングルタスクで呼ぶこと
//----------------------------------------------------------------------
void set_Task(void)
{
	//const uint16_t taskStackSize = 512;
	//const UBaseType_t taskPriority = 1;

	// タスク生成
	//xTaskCreate(test_CallBack, "test", taskStackSize, NULL, taskPriority, NULL);
	//xTaskCreate(test_CallBack2, "test2", taskStackSize, NULL, taskPriority, NULL);
}

//----------------------------------------------------------------------
//! @brief	SPI通信イベント処理
//! @param	event	[I]イベントID
//! @param	arg		[I]パラメータ
//----------------------------------------------------------------------
void IRAM_ATTR SpiEventCallback(int event, void *arg)
{
	switch(event)
	{
	case SPI_TRANS_DONE_EVENT:
		s_spiTransDone = 1;
		break;
	default:
		break;
	}
}

//----------------------------------------------------------------------
//! @brief	SPI送信待ち
//----------------------------------------------------------------------
void set_WaitSpiTrans(void)
{
	const TickType_t minimumWait = 1;
	while(s_spiTransDone == 0)
	{
		vTaskDelay(minimumWait);
	}
}

//----------------------------------------------------------------------
//! @brief	SPI送信フラグセット
//----------------------------------------------------------------------
void set_SetSpiTransFlag(int value)
{
	s_spiTransDone = value;
}

//----------------------------------------------------------------------
//! @brief  通信ピンのミューテックス取得
//----------------------------------------------------------------------
void set_TakeCommunicationMutex(void)
{
	xSemaphoreTake(s_communicationPinMutex, portMAX_DELAY);
}

//----------------------------------------------------------------------
//! @brief  通信ピンのミューテックス返却
//----------------------------------------------------------------------
void set_GiveCommunicationMutex(void)
{
	xSemaphoreGive(s_communicationPinMutex);
}

