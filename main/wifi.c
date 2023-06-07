//======================================================================
//! @file   wifi.c
//! @brief  Wi-Fi接続
//======================================================================
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/apps/sntp.h"

#include "global.h"
#include "wifi.h"
#include "lcd.h"

//----- 定義 -----
#define WIFI_SSID		"ssid-hogehoge"
#define WIFI_PASS		"pass-hogehoge"
#define MAXIMUM_RETRY	5
#define SNTP_SERVER		"pool.ntp.org"

static const uint8_t statusIcon[] =
{
	0x70, 0x0c, 0x66, 0x12, 0x09, 0x65, 0x65, 0x00,		// 接続済
	0x01, 0x03, 0x7f, 0x03, 0x51, 0x20, 0x50, 0x00,		// 接続失敗
	0x08, 0x22, 0x00, 0x41, 0x00, 0x22, 0x08, 0x00,		// 接続中
};

//----- 変数 -----
static int s_retry_num = 0;
static int s_isWifiInitialized = 0;

static void PerformSntp(void *arg);
static void HandleWifiEvent(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData);

//----------------------------------------------------------------------
//! @brief	SNTPタスク
//! @param	arg			[I]パラメータ
//----------------------------------------------------------------------
void PerformSntp(void *arg)
{
	// タイムゾーンを日本標準時間に設定
	setenv("TZ", "JST-9", 1);
	tzset();

	// SNTP設定
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, SNTP_SERVER);
	sntp_init();

	time_t now;
	struct tm timeinfo;
	Rect timeArea = {108, 0, 20, 8};
	char timeStr[5+1];
	while(1)
	{
		time(&now);
		localtime_r(&now, &timeinfo);

		if(timeinfo.tm_year < (2016 - 1900))
		{
			// 時刻未設定時
			strcpy(timeStr, "--:--");
		}
		else
		{
			snprintf(timeStr, 5+1, "%2d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
		}
		lcd_Puts(timeArea, timeStr, Code_Sjis);
		lcd_Update();

		vTaskDelay(1000 / portTICK_RATE_MS);
	}
}

//----------------------------------------------------------------------
//! @brief	Wi-Fiイベントハンドラ
//! @param	arg			[I]パラメータ
//! @param	eventBase	[I]イベントのベース
//! @param	eventId		[I]イベントID
//! @param	eventData	[I]イベントデータ
//----------------------------------------------------------------------
void HandleWifiEvent(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData)
{
	Rect area = {0, 0, 8, 8};
	Rect textArea = {8, 0, 15 * 4, 8};
	if(eventBase == WIFI_EVENT)
	{
		switch(eventId)
		{
		case WIFI_EVENT_STA_START:					// Stationスタート
			esp_wifi_connect();
			lcd_PutImage(area, &statusIcon[2 * 8], NULL);
			//lcd_Update();		// 別タスク(PerformSntp())で呼ばれるのでコメントアウト
			break;

		case WIFI_EVENT_STA_DISCONNECTED:			// APからStationの断線
			s_isWifiInitialized = 0;
			if(s_retry_num < MAXIMUM_RETRY)
			{
				esp_wifi_connect();
				s_retry_num++;
			}
			else
			{
				lcd_PutImage(area, &statusIcon[1 * 8], NULL);
				//lcd_Update();		// 別タスク(PerformSntp())で呼ばれるのでコメントアウト
				vTaskDelay(2000 / portTICK_RATE_MS);
				esp_wifi_connect();
				lcd_PutImage(area, &statusIcon[2 * 8], NULL);
				//lcd_Update();		// 別タスク(PerformSntp())で呼ばれるのでコメントアウト
				s_retry_num = 0;
			}
			break;

		case WIFI_EVENT_STA_CONNECTED:				// station connected to AP
			lcd_PutImage(area, &statusIcon[0 * 8], NULL);
			//lcd_Update();		// 別タスク(PerformSntp())で呼ばれるのでコメントアウト
			break;

		case WIFI_EVENT_WIFI_READY:					// WiFi ready
		case WIFI_EVENT_SCAN_DONE:					// finish scanning AP
		case WIFI_EVENT_STA_STOP:					// station stop
		case WIFI_EVENT_STA_AUTHMODE_CHANGE:		// the auth mode of AP connected by station changed
		case WIFI_EVENT_STA_BSS_RSSI_LOW:			// AP's RSSI crossed configured threshold
		case WIFI_EVENT_STA_WPS_ER_SUCCESS:			// station wps succeeds in enrollee mode
		case WIFI_EVENT_STA_WPS_ER_FAILED:			// station wps fails in enrollee mode
		case WIFI_EVENT_STA_WPS_ER_TIMEOUT:			// station wps timeout in enrollee mode
		case WIFI_EVENT_STA_WPS_ER_PIN:				// station wps pin code in enrollee mode
		case WIFI_EVENT_AP_START:					// soft-AP start
		case WIFI_EVENT_AP_STOP:					// soft-AP stop
		case WIFI_EVENT_AP_STACONNECTED:			// a station connected to soft-AP
		case WIFI_EVENT_AP_STADISCONNECTED:			// a station disconnected from soft-AP
		case WIFI_EVENT_AP_PROBEREQRECVED:			// Receive probe request packet in soft-AP interface
		default:
			break;
		}
	}
	else if(eventBase == IP_EVENT)
	{
		ip_event_got_ip_t* event;
		switch(eventId)
		{
		case IP_EVENT_STA_GOT_IP:				// Stationが接続したAPからIP取得
			event = (ip_event_got_ip_t*)eventData;
			lcd_Puts(textArea, ip4addr_ntoa(&event->ip_info.ip), Code_Utf8);
			//lcd_Update();		// 別タスク(PerformSntp())で呼ばれるのでコメントアウト
			s_retry_num = 0;
			s_isWifiInitialized = 1;
			break;

		case IP_EVENT_STA_LOST_IP:				// StationがIPを失いIPが0にリセットされた
			lcd_Puts(textArea, "-.-.-.-", Code_Utf8);
			//lcd_Update();		// 別タスク(PerformSntp())で呼ばれるのでコメントアウト
			break;

		case IP_EVENT_AP_STAIPASSIGNED:			// soft-APが接続したStationへIPを割り当てた
		case IP_EVENT_GOT_IP6:					// station / ap / ethernet interface v6IP addr is preferred
		default:
			break;
		}
	}
}

//----------------------------------------------------------------------
//! @brief  Wi-Fiステーションモードで初期化
//! @note	NVSは事前に初期化されていること
//! @return	0=失敗 !0=成功
//----------------------------------------------------------------------
int wifi_Initialize(void)
{
	Rect area = {0, 0, 17 * 4, 8};
	lcd_Puts(area, "□-.-.-.-", Code_Utf8);
	area.x = 8, area.y = 16, area.w = 128, area.h = 16;
	lcd_Puts(area, "Wi-Fiと時刻のテスト", Code_Utf8);
	lcd_Update();

	// Wi-Fi初期化
	esp_netif_init();
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_init_config_t wifiConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));

	// イベントループハンドル登録
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &HandleWifiEvent, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &HandleWifiEvent, NULL));

	// モード設定
	wifi_config_t wifiModeConfig = {0};
	strncpy((char *)wifiModeConfig.sta.ssid, WIFI_SSID, sizeof(wifiModeConfig.sta.ssid));
	strncpy((char *)wifiModeConfig.sta.password, WIFI_PASS, sizeof(wifiModeConfig.sta.password));
	wifiModeConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifiModeConfig));

	// Wi-Fi開始
	ESP_ERROR_CHECK(esp_wifi_start());

	// SNTPサービス開始
	xTaskCreate(PerformSntp, "sntp_task", 2048, NULL, 10, NULL);

	return 1;
}
