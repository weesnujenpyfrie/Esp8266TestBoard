//======================================================================
//! @file   sd.c
//! @brief  SDカードアクセス
//======================================================================
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "ff.h"
#include "driver/spi.h"
#include "driver/gpio.h"

#include "global.h"
#include "sd.h"
#include "setup.h"

//----- 定義 -----
#define CALC_CMD_CRC 0		// CMD転送時のCRCを計算するか 0=計算しない
#define CALC_RW_CRC  0		// DATA転送時のCRCを計算するか 0=計算しない
typedef enum {InitType_SdVer2, InitType_SdVer1, InitType_MmcVer3} InitType_t;						// 初期化タイプ
typedef enum {Card_SdVer2Block, Card_SdVer2Byte, Card_SdVer1, Card_MmcVer3, Card_Unknown} Card_t;	// SDカード種別
typedef enum {Reg_Csd, Reg_Cid, Reg_Status} Register_t;												// レジスタ指定

//----- 定数 -----
static const char* TAG = "SD";					// ログ用タグ

static const int timeOutMs = 500;				// タイムアウト時間[ms]
static const int bitPerByte = 8;				// 1byteあたりのbit数
static const int bytePerSector = 512;			// 1セクタあたりのバイト数
static const int maxSpiTransferSize = 64;		// 最大SPI転送サイズ
static const uint8_t noPdrv = 0xff;				// ドライブ番号なし
static const char *basePath = "/sd";			// SDカードのベースパス

// レスポンス
static const uint8_t r1Invalid = 0x80;			// R1 無効なレスポンス
static const uint8_t r1NoError = 0x00;			// R1 エラーなし
static const uint8_t r1InitIdle = 0x01;			// R1 初期化中アイドル状態
static const uint8_t r1bBusy = 0;				// R1b ビジー状態
static const uint32_t r3Ccs = 0x40000000UL;		// R3 カード容量ステータス

//----- メンバ変数 -----
static FATFS *s_fatFs = NULL;					// FatFs登録先
static uint8_t s_pdrv = noPdrv;					// 現在マウントされているドライブ番号
static Card_t s_cardType;						// カードタイプ
static DSTATUS s_cardStatus;					// カード状態
static uint32_t s_allocationUnitSize;			// カードのアロケーションユニットサイズ[sector]
static uint32_t s_cardSize;						// カードの容量[sector]

//----- プロトタイプ宣言 -----
// FatFs要求関数
static DSTATUS Initialize(BYTE pdrv);
static DSTATUS GetStatus(BYTE pdrv);
static DRESULT ReadBlock(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
static DRESULT WriteBlock(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
static DRESULT ControlIo(BYTE pdrv, BYTE cmd, void* buff);

// その他
static uint32_t GetRegValue(uint8_t *data, int msb, int lsb);								// レジスタ値取得
static int ReadRegister(uint32_t *buff, Register_t reg);									// レジスタ読込
static int InitSdCom(InitType_t type);														// SD通信初期化
static uint8_t SendCom(uint8_t command, uint32_t param, uint32_t *addRes, int csControl);	// コマンド送信
static uint8_t WaitRes(uint8_t continueValue);												// レスポンス待ち
static void CallBackTimer(TimerHandle_t timer);
static inline void SetRxMode(void);															// 受信モード(MOSIピンをH出力固定にする)
static inline void SetTxMode(void);															// 送信モード(MOSIピンをMOSI機能にする)
static inline void StartCommunication(void);												// 通信開始(CSピンをLにする)
static void StopCommunication(void);														// 通信停止(CSピンをHにする、MISOラインSD側をHi-Zにする)
static void SetNormalSpi(void);																// 通常時SPI設定
#if CALC_CMD_CRC
static uint8_t CalcCrc7(uint8_t *buf, int length);											// CRC7計算
#endif
#if CALC_RW_CRC
static uint16_t CalcCrc16(uint8_t *buf, int length);										// CRC16計算
#endif

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
	//----- カード初期化完了か -----
	if((s_cardStatus & STA_NOINIT) == 0)
	{
		return 0;
	}

	set_TakeCommunicationMutex();

	//----- SPI設定 -----
	set_SetPin(PinSetting_SdMount, NULL);

	//----- 10byte FF 送信 -----
	spi_trans_t trans = {0};				// 送受信用の入れ物
	uint32_t waitData = 0xffffffffUL;		// 初めに74clock以上Hを出力するためのデータ
	trans.cmd = (uint16_t *)&waitData;
	trans.addr = &waitData;
	trans.mosi = &waitData;
	trans.bits.cmd = 2 * bitPerByte;
	trans.bits.addr = 4 * bitPerByte;
	trans.bits.mosi = 4 * bitPerByte;
	trans.bits.miso = 0;
	set_SetSpiTransFlag(0);
	spi_trans(HSPI_HOST, &trans);
	set_WaitSpiTrans();

	//----- SDカード初期化コマンド送信 & 識別 -----
	uint32_t res;							// 戻り値
	s_cardType = Card_Unknown;

	// CMD00 送信
	if(SendCom(0, 0x00000000UL, NULL, 1) == r1InitIdle)
	{
		// CMD08 送信
		if(SendCom(8, 0x000001aaUL, &res, 1) == r1InitIdle)
		{
			// R7確認
			if((res & 0x00000fffUL) == 0x000001aaUL)
			{
				if(InitSdCom(InitType_SdVer2) == RET_OK)
				{
					// SD Ver2
					// CMD58 送信
					if(SendCom(58, 0x00000000UL, &res, 1) == r1NoError)
					{
						// R3確認
						s_cardType = (res & r3Ccs) ? Card_SdVer2Block : Card_SdVer2Byte;
					}
				}
			}
		}
		else if(InitSdCom(InitType_SdVer1) == RET_OK)
		{
			// SD Ver 1
			s_cardType = Card_SdVer1;
		}
		else if(InitSdCom(InitType_MmcVer3) == RET_OK)
		{
			// MMC Ver3
			s_cardType = Card_MmcVer3;
		}
	}
	switch(s_cardType)
	{
	case Card_SdVer2Byte:
	case Card_SdVer1:
	case Card_MmcVer3:
		if(SendCom(16, bytePerSector, NULL, 1) != r1NoError)
		{
			s_cardType = Card_Unknown;
		}
		break;
	case Card_SdVer2Block:
	case Card_Unknown:
	default:
		break;
	}

	//----- カード情報収集 -----
	// 2つの値をCSDレジスタから取得する
	// * カードサイズ[sector]
	// * アロケーションユニットサイズ(ブロック消去単位)[sector]
	SetNormalSpi();

	union
	{
		uint32_t u32[4];
		uint8_t u8[16];
	} info;
	if(ReadRegister(info.u32, Reg_Csd) != RET_OK)
	{
		s_cardType = Card_Unknown;
	}

	if(s_cardType != Card_Unknown)
	{
		uint8_t csdVer = GetRegValue(info.u8, 127, 126);
		if(csdVer == 0)
		{
			// CSD Ver.1
			// (card size) = (C_SIZE+1) x 2^(C_SIZEMULTI + 2 + READ_BL_LEN)
			// (card sector count) = (card size) / 512 = (card size) >> 9
			s_cardSize = GetRegValue(info.u8, 49, 47) + 2;				// C_SIZE_MULTI [49:47] + 2
			s_cardSize += GetRegValue(info.u8, 83, 80);					// READ_BL_LEN [83:80]
			s_cardSize = 1 << (s_cardSize - 9);							// セクタサイズ512(=2^9)で割る
			s_cardSize *= GetRegValue(info.u8, 73, 62) + 1;				// C_SIZE [73:62] + 1

			// (erase block size) = SECTOR_SIZE + 1
			s_allocationUnitSize = GetRegValue(info.u8, 45, 39) + 1;	// SECTOR_SIZE [45:39]
		}
		else if(csdVer == 1)
		{
			// CSD Ver.2
			// (card size) = (C_SIZE+1) x 512KiB
			// (card sector count) = (card size) / 512
			s_cardSize = GetRegValue(info.u8, 69, 48) + 1;				// C_SIZE [69:48] + 1
			s_cardSize <<= 10;											// x(512KiB / 512) = x1024

			if(ReadRegister(info.u32, Reg_Status) != RET_OK)
			{
				s_cardType = Card_Unknown;
			}
			else
			{
				// (erase block size) = 表の通り
				const uint32_t auSizeTable[16] = {0, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 24576, 32768, 49152, 65536, 131072};
				int auSizeTableIndex = GetRegValue(info.u8, 47, 44);	// AU_SIZE [47:44]
				s_allocationUnitSize = auSizeTable[auSizeTableIndex];
			}
		}
		else
		{
			s_cardType = Card_Unknown;
		}
	}

	set_GiveCommunicationMutex();

	//----- テスト表示 -----
	switch(s_cardType)
	{
	case Card_SdVer2Block:	ESP_LOGI(TAG, "SD v2 block");	break;
	case Card_SdVer2Byte:	ESP_LOGI(TAG, "SD v2 byte");	break;
	case Card_SdVer1:		ESP_LOGI(TAG, "SD v1");			break;
	case Card_MmcVer3:		ESP_LOGI(TAG, "MMC v3");		break;
	case Card_Unknown:
	default:				ESP_LOGI(TAG, "Unknown");		break;
	}
	if(s_cardType != Card_Unknown)
	{
		ESP_LOGI(TAG, "card size=%u erase unit=%u", s_cardSize, s_allocationUnitSize);
	}

	s_cardStatus = (s_cardType != Card_Unknown) ? 0 : STA_NOINIT;

	return s_cardStatus;
}

//----------------------------------------------------------------------
//! @brief  ディスクステータス確認(FatFs要求)
//! @param	pdrv		[I]ドライブ番号
//! @return	状態 次の値をORでつなげた値 STA_NOINIT, STA_NODISK, STA_PROTECT
//----------------------------------------------------------------------
DSTATUS GetStatus(BYTE pdrv)
{
	DSTATUS stat = s_cardStatus;

	if(pdrv != s_pdrv || pdrv == noPdrv)
	{
		stat = STA_NOINIT;
	}
	return stat;
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
	DRESULT res = RES_OK;

	//----- 準備 -----
	if((s_cardStatus & STA_NOINIT) != 0)
	{
		return RES_NOTRDY;
	}

	set_TakeCommunicationMutex();
	SetNormalSpi();

	//----- コマンド転送 -----
	switch(s_cardType)
	{
	case Card_SdVer2Byte:	// Byte access
	case Card_SdVer1:		// Byte access
	case Card_MmcVer3:		// Byte access
		sector *= bytePerSector;
		break;
	case Card_SdVer2Block:	// Block access
	case Card_Unknown:		// -
	default:				// -
		break;
	}

	StartCommunication();

	if(SendCom((count >= 2) ? 18 : 17, sector, NULL, 0) != r1NoError)
	{
		res = RES_ERROR;
		goto sd_Read_End;
	}

	//----- データ受信 -----
	SetRxMode();

	const uint8_t dataDummy = 0xff;					// ダミーデータ
	const uint8_t startDataBlockToken = 0xfe;		// スタートデータブロックトークン

	spi_trans_t trans = {0};		// 送受信用の入れ物
	union
	{
		uint32_t u32;
		uint8_t u8[4];
	} misoData;						// 実際受信するデータ
	uint32_t crc;					// CRC
#if CALC_RW_CRC
	uint32_t calcCrc;				// CRC
#endif

	uint32_t index = 0;
	trans.bits.val = 0;
	for(int packet = 0; packet < count; packet++)
	{
		// データトークン待ち
		if(WaitRes(dataDummy) != startDataBlockToken)
		{
			res = RES_ERROR;
			break;
		}

		// データ読み込み(4byte境界まで)
		int align = 3 - (((int)buff + 3) % 4);	// 4byte境界までに必要なバイト数 buff=0,1,2,3 -> align=0,3,2,1
		if(align != 0)
		{
			trans.bits.miso = align * bitPerByte;
			trans.miso = &misoData.u32;
			spi_trans(HSPI_HOST, &trans);
			for(int i = 0; i < align; i++)
			{
				buff[index] = misoData.u8[i];
				index++;
			}
		}

		// データ読み込み(残りの分)
		int remain;
		trans.bits.miso = maxSpiTransferSize * bitPerByte;
		for(int i = align; i < bytePerSector; )
		{
			remain = bytePerSector - i;
			if(remain < 4)
			{
				trans.bits.miso = remain * bitPerByte;
				trans.miso = &misoData.u32;
				spi_trans(HSPI_HOST, &trans);
				for(int j = 0; j < remain; j++)
				{
					buff[index] = misoData.u8[j];
					index++;
					i++;
				}
			}
			else
			{
				if(remain < maxSpiTransferSize)
				{
					// 4byteずつ読み込むが、align!=0の場合最後のデータはbuff[]からはみ出て書き換えてしまう.
					// なので4byte境界で止めておいて、残り1～3byteは下のif(remain < 4)で読み込む.
					trans.bits.miso = (remain & ~3) * bitPerByte;
					i += remain & ~3;
				}
				else
				{
					i += maxSpiTransferSize;
				}
				trans.miso = (uint32_t *)&buff[index];
				spi_trans(HSPI_HOST, &trans);
				index += trans.bits.miso / bitPerByte;
			}
		}

		// CRC取得
		trans.bits.miso = 2 * bitPerByte;
		trans.miso = &crc;
		spi_trans(HSPI_HOST, &trans);

#if CALC_RW_CRC
		crc = ((crc >> 8) & 0xff) | ((crc << 8) & 0xff00);
		calcCrc = CalcCrc16(&buff[packet * bytePerSector], bytePerSector);
		if(crc != calcCrc)
		{
			res = RES_ERROR;
			break;
		}
#endif
	}

	//----- データストップ -----
	if(count >= 2)
	{
		// CMD12送信
		SetTxMode();
		if(SendCom(12, 0x00000000UL, NULL, 0) != r1NoError)
		{
			res = RES_ERROR;
		}
	}

sd_Read_End:
	SetTxMode();
	StopCommunication();
	set_GiveCommunicationMutex();

	return res;
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
	const int minMultiBlockAccessCount = 2;		// マルチブロックアクセスを行う回数(この回数以上の時マルチブロックアクセスを行う)

	DRESULT res = RES_ERROR;

	//----- 準備 -----
	if((s_cardStatus & STA_NOINIT) != 0)
	{
		return RES_NOTRDY;
	}

	switch(s_cardType)
	{
	case Card_SdVer2Byte:	// Byte access
	case Card_SdVer1:		// Byte access
	case Card_MmcVer3:		sector *= bytePerSector;	break;	// Byte access
	case Card_SdVer2Block:	// Block access
	case Card_Unknown:		// -
	default:											break;	// Block access
	}

	set_TakeCommunicationMutex();
	SetNormalSpi();
	StartCommunication();

	//----- コマンド転送(マルチブロックライトの場合) -----
	if(count >= minMultiBlockAccessCount)
	{
		// 事前消去ブロック数の指定 (MMC=CMD23, SDC=ACMD23)
		if(s_cardType != Card_MmcVer3)
		{
			if(SendCom(55, 0x00000000UL, NULL, 0) != r1NoError)
			{
				res = RES_ERROR;
				goto sd_Write_End;
			}
		}
		if(SendCom(23, count, NULL, 0) != r1NoError)
		{
			res = RES_ERROR;
			goto sd_Write_End;
		}
		// ライトコマンド発行
		if(SendCom(25, sector, NULL, 0) != r1NoError)
		{
			res = RES_ERROR;
			goto sd_Write_End;
		}
	}

	//---- データ転送 ----
	const uint8_t rdAccepted = 0x05;					// データレスポンス(OK)
	const uint8_t dataDummy = 0xff;						// ダミーデータ
	const uint8_t startDataBlockTokenCmd24 = 0xfe;		// スタートデータブロックトークン
	const uint8_t startDataBlockTokenCmd25 = 0xfc;		// スタートデータブロックトークン
	const uint8_t stopDataBlockTokenCmd25 = 0xfd;		// ストップトークン

	uint32_t crc;					// CRC
	uint16_t commandData;			// SPI転送コマンドデータ
	uint32_t addressData;			// SPI転送アドレスデータ
	uint32_t index = 0;				// 入力データの読込位置

	spi_trans_t trans = {0};
	trans.bits.val = 0;
	trans.cmd = &commandData;
	trans.addr = &addressData;
	for(int packet = 0; packet < count; packet++)
	{
		// コマンド送信(シングルブロックライトの場合)
		if(count < minMultiBlockAccessCount)
		{
			if(SendCom(24, sector, NULL, 0) != r1NoError)
			{
				res = RES_ERROR;
				goto sd_Write_End;
			}
		}

		// ブロックトークンと4byte境界までのデータを転送
		commandData = dataDummy;		// 1byte以上空けることになっているので0xffを送る
		commandData |= ((count >= minMultiBlockAccessCount) ? startDataBlockTokenCmd25 : startDataBlockTokenCmd24) << bitPerByte;
		for(int i = 0; i < 4; i++)
		{
			addressData <<= bitPerByte;
			addressData |= buff[i];
		}
		int align = 3 - (((int)buff + 3) % 4);	// 4byte境界までに必要なバイト数 buff=0,1,2,3 -> align=0,3,2,1
		index += align;
		trans.bits.val = 0;
		trans.bits.cmd = 2 * bitPerByte;
		trans.bits.addr = align * bitPerByte;
		set_SetSpiTransFlag(0);
		spi_trans(HSPI_HOST, &trans);
		set_WaitSpiTrans();

		// 残りのデータを転送
		int remain;
		trans.bits.mosi = maxSpiTransferSize * bitPerByte;
		for(int i = align; i < bytePerSector; )
		{
			remain = bytePerSector - i;
			switch(remain)
			{
			case 1:
				commandData = buff[index];
				trans.bits.cmd = 1 * bitPerByte;
				trans.bits.addr = 0 * bitPerByte;
				index += 1;
				i += 1;
				remain = 0;
				break;
			case 2:
				commandData = buff[index] | (buff[index + 1] << bitPerByte);
				trans.bits.cmd = 2 * bitPerByte;
				trans.bits.addr = 0 * bitPerByte;
				index += 2;
				i += 2;
				remain = 0;
				break;
			case 3:
				commandData = buff[index] | (buff[index + 1] << bitPerByte);
				trans.bits.cmd = 2 * bitPerByte;
				addressData = buff[index + 2] << (3 * bitPerByte);
				trans.bits.addr = 1 * bitPerByte;
				index += 3;
				i += 3;
				remain = 0;
				break;
			case 4:
			default:
				commandData = buff[index] | (buff[index + 1] << bitPerByte);
				trans.bits.cmd = 2 * bitPerByte;
				addressData = (buff[index + 2] << (3 * bitPerByte)) | (buff[index + 3] << (2 * bitPerByte));
				trans.bits.addr = 2 * bitPerByte;
				index += 4;
				i += 4;
				remain -= 4;
				break;
			}

			trans.mosi = (uint32_t *)&buff[index];
			if(remain < maxSpiTransferSize)
			{
				trans.bits.mosi = remain * bitPerByte;
				i += remain;
				index += remain;
			}
			else
			{
				i += maxSpiTransferSize;
				index += maxSpiTransferSize;
			}

			set_SetSpiTransFlag(0);
			spi_trans(HSPI_HOST, &trans);
			set_WaitSpiTrans();
		}

		//----- CRC転送 -----
#if CALC_RW_CRC
		crc = CalcCrc16(buff, bytePerSector);
#else
		crc = 0;
#endif
		commandData = (uint16_t)((crc << bitPerByte) & 0xff00) | ((crc >> bitPerByte) & 0x00ff);
		trans.bits.val = 0;
		trans.bits.cmd = 2 * bitPerByte;
		set_SetSpiTransFlag(0);
		spi_trans(HSPI_HOST, &trans);
		set_WaitSpiTrans();

		//----- SD側データ受信待ち -----
		SetRxMode();
		// データレスポンス待ち
		res = WaitRes(dataDummy) & 0x1f;
		if(res != rdAccepted)
		{
			SetTxMode();
			goto sd_Write_End;
		}

		// SD側データ書き込み待ち
		if(WaitRes(0x00) == r1Invalid)
		{
			SetTxMode();
			goto sd_Write_End;
		}

		SetTxMode();
	}

	//----- ストップトークン転送(マルチブロックライトの場合) -----
	if(count >= minMultiBlockAccessCount)
	{
		trans.bits.val = 0;
		trans.bits.cmd = 2 * bitPerByte;
		commandData = stopDataBlockTokenCmd25;
		commandData |= dataDummy << bitPerByte;		// token送信後の1byteを無視すべく8クロック入れるためのダミー
		set_SetSpiTransFlag(0);
		spi_trans(HSPI_HOST, &trans);
		set_WaitSpiTrans();

		SetRxMode();
		if(WaitRes(0x00) == r1Invalid)
		{
			SetTxMode();
			goto sd_Write_End;
		}
		SetTxMode();
	}

	res = RES_OK;

sd_Write_End:
	StopCommunication();
	set_GiveCommunicationMutex();

	return res;
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
	if((s_cardStatus & STA_NOINIT) != 0)
	{
		return RES_NOTRDY;
	}

	DRESULT stat = RES_OK;
	switch(cmd)
	{
	// キャッシュとSDの内容を同期させる
	case CTRL_SYNC:
		// 遅延書き込みしないのでスルー
		break;

	// 使用可能なセクタ数(f_mkfs,f_fdiskで使用)
	case GET_SECTOR_COUNT:
		*(DWORD *)buff = s_cardSize;
		break;

	// セクタサイズ
	case GET_SECTOR_SIZE:
		*(WORD *)buff = (WORD)bytePerSector;
		break;
	
	// 消去ブロックサイズ[sector]
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = s_allocationUnitSize;
		break;
	
	// 消去可能ブロックの通知
	case CTRL_TRIM:
		// FF_USE_TRIM = 0 なので実装不要
		// 実装するなら以下SDカードコマンドを使用して削除する
		// CMD32 消去開始ブロック指定
		// CMD33 消去終了ブロック指定
		// CMD38 指定範囲のブロック消去
		break;
	
	default:
		stat = RES_PARERR;
		break;
	}

	return stat;
}

//----------------------------------------------------------------------
//! @brief  レジスタ値取得
//! @param	data	[I]レジスタデータ(16byte)
//! @param	msb		[I]抜き出したいデータMSB
//! @param	lsb		[I]抜き出したいデータLSB
//! @return	[msb:lsb]レジスタ値
//----------------------------------------------------------------------
uint32_t GetRegValue(uint8_t *data, int msb, int lsb)
{
	uint32_t value = 0;

	// [msb:lsb]に該当するdata[]をvalueに連結代入
	// data[]はSPI通信でlittle endianで取り込んでいる一方で送られてくるデータはbig endian
	int lsbyte = 15 - (lsb / bitPerByte);
	int msbyte = 15 - (msb / bitPerByte);
	for(int i = msbyte; i <= lsbyte ; i++)
	{
		value <<= bitPerByte;
		value |= data[i];
	}

	// LSBが0bit位置となるようずらす
	value >>= lsb % bitPerByte;
	// データ長(msb - lsb + 1)分マスクをかける
	value &= (1UL << (msb - lsb + 1)) - 1;

	return value;
}

//----------------------------------------------------------------------
//! @brief  レジスタ読込
//! @param	buff		[O]出力バッファ
//! @param	reg			[I]レジスタ
//! @return	RET_OK=成功 RET_NG=エラー
//----------------------------------------------------------------------
int ReadRegister(uint32_t *buff, Register_t reg)
{
	int ret = RET_NG;

	//----- コマンド転送 -----
	uint8_t command;
	switch(reg)
	{
	case Reg_Csd:		command = 9;	break;
	case Reg_Cid:		command = 10;	break;
	case Reg_Status:	command = 13;	break;
	default:							goto sd_ReadRegister_End;
	}

	StartCommunication();

	uint8_t r1;
	if(reg == Reg_Status)
	{
		if((r1 = SendCom(55, 0x00000000UL, NULL, 0)) != r1NoError)
		{
			goto sd_ReadRegister_End;
		}
	}
	uint32_t r2 = 0;
	if((r1 = SendCom(command, 0x00000000UL, &r2, 0)) != r1NoError)
	{
		goto sd_ReadRegister_End;
	}
	if(reg == Reg_Status && r2 != 0)
	{
		goto sd_ReadRegister_End;
	}

	//----- データ読み込み -----
	// データトークン待ち
	const uint8_t dataDummy = 0xff;					// ダミーデータ
	const uint8_t startDataBlockToken = 0xfe;		// スタートデータブロックトークン
	SetRxMode();

	if(WaitRes(dataDummy) != startDataBlockToken)
	{
		goto sd_ReadRegister_End;
	}

	// データ読み込み
	spi_trans_t trans = {0};
	trans.bits.val = 0;
	trans.bits.miso = 16 * bitPerByte;
	trans.miso = buff;
	spi_trans(HSPI_HOST, &trans);

	if(reg == Reg_Status)
	{
		// SD_STATUSの場合 残りの64-16=48byte読み捨て
		uint32_t dummy[48 / sizeof(uint32_t)];
		trans.bits.miso = 48 * bitPerByte;
		trans.miso = dummy;
		spi_trans(HSPI_HOST, &trans);
	}

	// CRC
	uint32_t crc;					// CRC
	trans.bits.miso = 2 * bitPerByte;
	trans.miso = &crc;
	spi_trans(HSPI_HOST, &trans);
#if CALC_RW_CRC
	crc = ((crc >> 8) & 0xff) | ((crc << 8) & 0xff00);
	uint32_t calcCrc = CalcCrc16(buff, bytePerSector);
	if(crc != calcCrc)
	{
		goto sd_ReadRegister_End;
	}
#endif

	ret = RET_OK;

sd_ReadRegister_End:
	SetTxMode();
	StopCommunication();
	return ret;
}

//----------------------------------------------------------------------
//! @brief  SD初期化コマンド送信
//! @param  type	[I]初期化タイプ
//! @return RET_NG=エラー, RET_OK=成功
//----------------------------------------------------------------------
int InitSdCom(InitType_t type)
{
	const int isSd = (type == InitType_SdVer2) || (type == InitType_SdVer1);
	uint8_t cmd = isSd ? 41 : 1;
	uint32_t param = (type == InitType_SdVer2) ? 0x40000000UL : 0x00000000UL;

	uint8_t res1;
	int ret = RET_NG;

	int status = 0;
	TimerHandle_t timer = xTimerCreate("SdTimer", pdMS_TO_TICKS(timeOutMs), pdFALSE, &status, &CallBackTimer);
	xTimerStart(timer, 0);
	while(status == 0)
	{
		if(isSd)
		{
			// CMD55 送信
			if((SendCom(55, 0x00000000UL, NULL, 1) & ~r1InitIdle) != r1NoError)
			{
				break;
			}
		}
		// CMD01/ACMD41 送信
		res1 = SendCom(cmd, param, NULL, 1);

		if(res1 == r1NoError)
		{
			ret = RET_OK;
			break;
		}
		if(res1 != r1InitIdle)
		{
			break;
		}
	}

	return ret;
}

//----------------------------------------------------------------------
//! @brief  SDコマンド送信
//! @param  command			[I]コマンド番号
//! @param  param			[I]パラメータ
//! @param  addRes			[O]R2, R3, R7 レスポンスデータ
//! @param  cdControl		[I]!0=CSピンH/L制御する
//! @return R1レスポンスデータ, r1Invalid=タイムアウト
//----------------------------------------------------------------------
uint8_t SendCom(uint8_t command, uint32_t param, uint32_t *addRes, int csControl)
{
	const uint8_t dataDummy = 0xff;		// Wait用.
	union transData
	{
		uint32_t val;
		uint8_t val8[4];
	} txData, rxData;				// 転送データ / 受信データ
	uint16_t commandData;			// 実際送信するコマンドのデータ
	uint8_t ret;					// 戻り値

	//----- 転送設定 -----
	spi_trans_t trans = {0};
	trans.cmd = &commandData;
	trans.addr = &param;
	trans.mosi = &txData.val;
	trans.miso = &rxData.val;

	//----- コマンド転送 -----
	command &= 0x3f;
	commandData = dataDummy;		// CSピン変化後1bit(SDC)/1byte(MMC)分のクロックを入れないとカードのピン設定が反映されない.
	commandData |= (command | 0x40) << bitPerByte;

// CRCを真面目に計算するとき
#if CALC_CMD_CRC
	uint8_t crcSrc[5];
	crcSrc[0] = command | 0x40;
	crcSrc[1] = (param >> 24) & 0xff;
	crcSrc[2] = (param >> 16) & 0xff;
	crcSrc[3] = (param >> 8) & 0xff;
	crcSrc[4] = param & 0xff;
	txData.val8[0] = CalcCrc7(crcSrc, 5);
// CRCを真面目に計算しない場合(動作は高速)
#else
	// CRC必須コマンド(CMD00,CMD08)のみCRCを付加する
	switch(command)
	{
	case 0:		txData.val8[0] = (0x4a << 1) | 0x01;	break;
	case 8:		txData.val8[0] = (0x43 << 1) | 0x01;	break;
	default:	txData.val8[0] = (0x00 << 1) | 0x01;	break;
	}
#endif
	trans.bits.cmd = 2 * bitPerByte;
	trans.bits.addr = 4 * bitPerByte;
	trans.bits.mosi = 1 * bitPerByte;
	trans.bits.miso = 0;

	if(csControl)
	{
		StartCommunication();
	}
	set_SetSpiTransFlag(0);
	spi_trans(HSPI_HOST, &trans);
	set_WaitSpiTrans();

	//----- レスポンスR1受信 -----
	SetRxMode();

	// CMD12の場合は1byte無視する
	if(command == 12)
	{
		trans.bits.val = 0;
		trans.bits.miso = 1 * bitPerByte;
		spi_trans(HSPI_HOST, &trans);
	}

	ret = WaitRes(dataDummy);
	if(ret == r1Invalid)
	{
		goto sendCom_End;
	}

	//----- レスポンスR2, R3, R7受信 -----
	int res1b = 0;					// R1bレスポンス
	int count;
	switch(command)
	{
	case 8:		// R7
	case 58:	count = 4;				break;	// R7, R3
	case 13:	count = 1;				break;	// R2 (ACMD13も含む)
	case 12:	// R1b
	case 28:	// R1b
	case 29:	// R1b
	case 38:	count = 0;	res1b = 1;	break;	// R1b
	default:	count = 0;				break;	// R1
	}

	if(res1b)
	{
		// R1bレスポンスビジー状態待ち
		if(WaitRes(r1bBusy) == r1Invalid)
		{
			ret = r1Invalid;
			goto sendCom_End;
		}
	}
	else if(count != 0)
	{
		trans.bits.val = 0;
		trans.bits.miso = count * bitPerByte;
		spi_trans(HSPI_HOST, &trans);

		if(addRes != NULL)
		{
			*addRes = 0UL;
			for(int i = 0; i < count; i++)
			{
				*addRes <<= bitPerByte;
				*addRes |= rxData.val8[i];
			}
		}
	}

sendCom_End:
	//----- 終了処理 -----
	SetTxMode();
	if(csControl)
	{
		StopCommunication();
	}

	return ret;
}

//----------------------------------------------------------------------
//! @brief  レスポンス待ち
//! @param  continueValue	[I]継続条件値
//! @return R1レスポンス, r1Invalid=タイムアウト
//----------------------------------------------------------------------
uint8_t WaitRes(uint8_t continueValue)
{
	int ret = r1Invalid;
	uint32_t rxData = 0;

	spi_trans_t trans = {0};
	trans.miso = &rxData;
	trans.bits.val = 0;
	trans.bits.miso = 1 * bitPerByte;

	int status = 0;
	TimerHandle_t timer = xTimerCreate("SdTimer", pdMS_TO_TICKS(timeOutMs), pdFALSE, &status, &CallBackTimer);
	xTimerStart(timer, 0);
	while(status == 0)
	{
		spi_trans(HSPI_HOST, &trans);
		rxData &= 0xff;
		if(rxData != continueValue)
		{
			ret = (uint8_t)rxData;
			break;
		}
	}

	return ret;
}

//----------------------------------------------------------------------
//! @brief  タイマーコールバック関数.呼ばれたことを通知する.
//! @param  timer	[I]タイマーハンドラ
//----------------------------------------------------------------------
void CallBackTimer(TimerHandle_t timer)
{
	int *value = (int *)pvTimerGetTimerID(timer);
	*value = 1;
}

//----------------------------------------------------------------------
//! @brief	受信モード設定
//----------------------------------------------------------------------
inline void SetRxMode(void)
{
	// SDカードでは受信時、ダミーデータとして0xffを送信する必要があるが
	// ESP8266のSPIでは受信時は任意データの送信は不可能で0x00固定値しか送信できない
	// (ユーザーマニュアルでSPIは「半二重通信のみ」とあるがこの現象を意味するものと思われる).
	// そこで一旦SOUTピンの機能をGPIOに変更後、Hを出力して受信を行う.
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13);	// GPIO13に変更
	gpio_set_level(GPIO_MOSI_NUM, 1);						// MOSIポート = H
}

//----------------------------------------------------------------------
//! @brief	送信モード設定
//----------------------------------------------------------------------
inline void SetTxMode(void)
{
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_HSPID_MOSI);	// GPIO13をMOSIに変更
}

//----------------------------------------------------------------------
//! @brief	通信開始
//----------------------------------------------------------------------
inline void StartCommunication(void)
{
	gpio_set_level(GPIO_SDCS_NUM, 0);
}

//----------------------------------------------------------------------
//! @brief	通信停止
//----------------------------------------------------------------------
void StopCommunication(void)
{
	uint16_t dataDummy = 0x00ff;
	spi_trans_t trans = {0};
	trans.cmd = &dataDummy;
	trans.bits.cmd = 1 * bitPerByte;

	// データ転送待ちしてCS=H
	set_WaitSpiTrans();
	gpio_set_level(GPIO_SDCS_NUM, 1);

	// 通常のSPIデバイスと違ってCS=H後MOSIピンをHi-Zにするには1byte(MMC)/1bit(SDC)のクロックが必要.
	// ここでは1byte分のクロックを送信する.
	set_SetSpiTransFlag(0);
	spi_trans(HSPI_HOST, &trans);
	set_WaitSpiTrans();
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

//----------------------------------------------------------------------
//! @brief  CRC7計算
//! @param  buf		[I]計算対象データ
//! @param  length	[I]データ長 (>=1)
//! @return CRC7計算結果
//----------------------------------------------------------------------
#if CALC_CMD_CRC
uint8_t CalcCrc7(uint8_t *buf, int length)
{
	const uint16_t mod = 0x8900;		// x^7 + x^3 + x^0

	int index, digit;
	int count = bitPerByte;
	int lastLength = length + 1;
	uint16_t quotient = buf[0] << bitPerByte;

	for(index = 1; index < lastLength; index++)
	{
		if(index < length)
		{
			quotient |= buf[index];
		}
		else if(index == lastLength - 1)
		{
			count = 7;
		}
		for(digit = 0; digit < count; digit++)
		{
			quotient <<= 1;
			if(quotient & 0x8000)
			{
				quotient ^= mod;
			}
		}
	}

	return (quotient >> 8) & 0xff;
}
#endif

//----------------------------------------------------------------------
//! @brief  CRC16計算
//! @param  buf		[I]計算対象データ
//! @param  length	[I]データ長 (>=1)
//! @return CRC16計算結果
//----------------------------------------------------------------------
#if CALC_RW_CRC
uint16_t CalcCrc16(uint8_t *buf, int length)
{
	const uint32_t mod = 0x01102100UL;	// x^16 + x^12 + x^5 + x^0

	int index, digit;
	int count = bitPerByte;
	int lastLength = length + 3;
	uint32_t quotient = buf[0] << bitPerByte;

	for(index = 1; index < lastLength; index++)
	{
		if(index < length)
		{
			quotient |= buf[index];
		}
		else if(index == lastLength - 1)
		{
			count = 0;
		}
		for(digit = 0; digit < count; digit++)
		{
			quotient <<= 1;
			if(quotient & 0x01000000UL)
			{
				quotient ^= mod;
			}
		}
	}

	return (quotient >> 8) & 0xffff;
}
#endif
