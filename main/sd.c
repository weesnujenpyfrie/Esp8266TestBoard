//======================================================================
//! @file   sd.c
//! @brief  SDカードアクセス
//======================================================================
#include <stdint.h>

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "ff.h"

#include "global.h"
#include "sd.h"
#include "setup.h"

//----- 定数 -----
static const char* TAG = "SD";					// ログ用タグ

static const uint8_t noPdrv = 0xff;				// ドライブ番号なし
static const char *basePath = "/sd";			// SDカードのベースパス

//----- メンバ変数 -----
static FATFS *s_fatFs = NULL;					// FatFs登録先
static uint8_t s_pdrv = noPdrv;					// 現在マウントされているドライブ番号
static DSTATUS s_cardStatus;					// カード状態

//----- プロトタイプ宣言 -----
// FatFs要求関数
static DSTATUS Initialize(BYTE pdrv);
static DSTATUS GetStatus(BYTE pdrv);
static DRESULT ReadBlock(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
static DRESULT WriteBlock(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
static DRESULT ControlIo(BYTE pdrv, BYTE cmd, void* buff);

// その他
static void SetNormalSpi(void);																// 通常時SPI設定

//----------------------------------------------------------------------
//! @brief  SD初期設定
//! @return	RET_OK		成功
//! @note   起動時に1回だけ呼ぶ.単一のタスクから呼ぶこと.
//----------------------------------------------------------------------
int sd_Initialize(void)
{
	//----- FATFSにSDカードドライバを接続(登録) -----
	// 未登録ドライブ番号取得
	if(ff_diskio_get_drive(&s_pdrv) != ESP_OK)
	{
		// ドライブ数(マウント数)上限に達している
		return RET_NG;
	}

	// FATFSに関数を接続(登録)
	const ff_diskio_impl_t sdImpl =
	{
		.init = &Initialize,
		.status = &GetStatus,
		.read = &ReadBlock,
		.write = &WriteBlock,
		.ioctl = &ControlIo
	};
	ff_diskio_register(s_pdrv, &sdImpl);

	//----- FATFSをVFSに接続 -----
	const size_t maxFiles = 1;
	char drv[3] = {(char)('0' + s_pdrv), ':', '\0'};
	esp_err_t err = esp_vfs_fat_register(basePath, drv, maxFiles, &s_fatFs);
	if(err == ESP_ERR_INVALID_STATE)
	{
		// 既にVFSに登録済(OK)
	}
	else if(err != ESP_OK)
	{
		goto sd_Initialize_Fail;
	}

	//----- ドライバ初期化 -----
	s_cardStatus = STA_NOINIT;

	return RET_OK;

sd_Initialize_Fail:
	esp_vfs_fat_unregister_path(basePath);
	s_fatFs = NULL;
	ff_diskio_unregister(s_pdrv);
	s_pdrv = noPdrv;

	return RET_NG;
}

//----------------------------------------------------------------------
//! @brief  未初期化状態へ
//! @note	未使用.動作未チェック
//----------------------------------------------------------------------
void sd_Deinitialize(void)
{
	if(s_pdrv == noPdrv)
	{
		// 既に未初期化状態
		return;
	}

	char drv[3] = {(char)('0' + s_pdrv), ':', 0};
	f_unmount(drv);

	s_fatFs = NULL;

	ff_diskio_unregister(s_pdrv);
	s_pdrv = noPdrv;

	esp_vfs_fat_unregister_path(basePath);
}

//----------------------------------------------------------------------
//! @brief  マウント
//! @return	RET_OK		成功
//! @note	ピンが競合しているデバイスの中で一番最初に呼ぶこと.
//----------------------------------------------------------------------
int sd_Mount(void)
{
	//----- 初期化済チェック -----
	if(s_pdrv == noPdrv || s_fatFs == NULL)
	{
		return RET_NG;
	}

	int ret = RET_OK;

	//----- マウント -----
	char drv[3] = {(char)('0' + s_pdrv), ':', '\0'};
	FRESULT res = f_mount(s_fatFs, drv, 1);
	if(res != FR_OK)
	{
		ESP_LOGW(TAG, "failed to mount card (%d)", res);
		ret = RET_NG;
	}

	return ret;
}

//----------------------------------------------------------------------
//! @brief  アンマウント
//! @note	未使用.動作未チェック
//----------------------------------------------------------------------
void sd_Unmount(void)
{
	if (s_pdrv == noPdrv)
	{
		return;
	}
	char drv[3] = {(char)('0' + s_pdrv), ':', 0};
	f_unmount(drv);
}

//----------------------------------------------------------------------
//! @brief  SDカード初期化(FatFs要求)
//! @param	pdrv		[I]ドライブ番号
//! @return	状態 次の値をORでつなげた値 STA_NOINIT, STA_NODISK, STA_PROTECT
//----------------------------------------------------------------------
DSTATUS Initialize(BYTE pdrv)
{
	ESP_LOGI(TAG, "called Initialize");
	return STA_NOINIT;
}

//----------------------------------------------------------------------
//! @brief  ディスクステータス確認(FatFs要求)
//! @param	pdrv		[I]ドライブ番号
//! @return	状態 次の値をORでつなげた値 STA_NOINIT, STA_NODISK, STA_PROTECT
//----------------------------------------------------------------------
DSTATUS GetStatus(BYTE pdrv)
{
	ESP_LOGI(TAG, "called GetStatus");
	return STA_NOINIT;
}

//----------------------------------------------------------------------
//! @brief  セクタ読み込み(FatFs要求)
//! @param	pdrv		[I]ドライブ番号
//! @param	buff		[O]出力バッファ
//! @param	sector		[I]セクタ番号
//! @param	count		[I]ブロック数
//! @return	RES_OK=成功 RES_ERROR=R/Wエラー RES_WRPRT=書込禁止 RES_NOTRDY=未準備 RES_PARERR=パラメータ無効
//----------------------------------------------------------------------
DRESULT ReadBlock(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
	ESP_LOGI(TAG, "called ReadBlock");
	return RES_OK;
}

//----------------------------------------------------------------------
//! @brief  セクタ書き込み(FatFs要求)
//! @param	pdrv		[I]ドライブ番号
//! @param	buff		[I]出力バッファ
//! @param	sector		[I]セクタ番号
//! @param	count		[I]ブロック数
//! @return	RES_OK=成功 RES_ERROR=R/Wエラー RES_WRPRT=書込禁止 RES_NOTRDY=未準備 RES_PARERR=パラメータ無効
//----------------------------------------------------------------------
DRESULT WriteBlock(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
	ESP_LOGI(TAG, "called WriteBlock");
	return RES_OK;
}

//----------------------------------------------------------------------
//! @brief  ディスク情報取得/MISC操作(FatFs要求)
//! @param	pdrv	[I]ドライブ番号
//! @param	cmd		[I]コマンド
//! @param	buff	[IO]パラメータ
//! @return	RES_OK=成功 RES_ERROR=R/Wエラー RES_WRPRT=書込禁止 RES_NOTRDY=未準備 RES_PARERR=パラメータ無効
//----------------------------------------------------------------------
DRESULT ControlIo(BYTE pdrv, BYTE cmd, void* buff)
{
	DRESULT stat = RES_OK;
	switch(cmd)
	{
	// キャッシュとSDの内容を同期させる
	case CTRL_SYNC:
		ESP_LOGI(TAG, "called ControlIo(CTRL_SYNC)");
		break;

	// 使用可能なセクタ数(f_mkfs,f_fdiskで使用)
	case GET_SECTOR_COUNT:
		ESP_LOGI(TAG, "called ControlIo(GET_SECTOR_COUNT)");
		break;

	// セクタサイズ
	case GET_SECTOR_SIZE:
		ESP_LOGI(TAG, "called ControlIo(GET_SECTOR_SIZE)");
		break;
	
	// 消去ブロックサイズ[sector]
	case GET_BLOCK_SIZE:
		ESP_LOGI(TAG, "called ControlIo(GET_BLOCK_SIZE)");
		break;
	
	// 消去可能ブロックの通知
	case CTRL_TRIM:
		ESP_LOGI(TAG, "called ControlIo(CTRL_TRIM)");
		break;
	
	default:
		stat = RES_PARERR;
		break;
	}

	return stat;
}

//----------------------------------------------------------------------
//! @brief  通常時SPI設定
//! @note	set_SetPin()内PinSetting_SdMainの設定値を変えたら
//! 		normalSpiSpeedHzの値も変えること.
//----------------------------------------------------------------------
void SetNormalSpi(void)
{
	//----- SPI速度設定 -----
	set_SetPin(PinSetting_SdMain, NULL);
}
