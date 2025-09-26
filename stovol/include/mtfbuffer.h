/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

#ifndef OPT_STOVOL_INCLUDE_MTFBUFFER_H_
#define OPT_STOVOL_INCLUDE_MTFBUFFER_H_

/**@file mtfbuffer.h
 * @brief mtfbuffer class definition for stovol
 * @author kaijun@taifex.com.tw
 *
 * 簡化版 MTF Buffer 用於處理期貨撮合資料 match_p.dat
 * 主要功能：
 * 1. 讀取 match_p.dat 檔案
 * 2. 解析撮合資料
 * 3. 處理商品開收盤狀態
 * 4. 更新價格/口數資訊
 */

#include <string>
#include <vector>

#include "../include/LoggerProxy.h"
#include "../include/prodinfo.h"
#include "commlib/inc/atomic_io.h"
#include "commlib/inc/match_dat.h"

// Forward declaration
class LogViewer;

extern bool g_trace_mode;

// 各種List每次增長的長度
#define PRODUCT_BLOCK_SIZE 3
#define PRICE_BLOCK_SIZE 4
#define TRANSACTION_BLOCK_SIZE 500

// 移除 PRODUCT_TYPE 依賴：若需判斷，請使用實際資料來源或 thread 角色而非環境變數

// 狀態判斷巨集
#define IS_BREAK_TAG(p)                                                                            \
  ((p)->tag_type == TAG_TYPE_HALT || (p)->tag_type == TAG_TYPE_NEWS_HALT ||                        \
   (p)->tag_type == TAG_TYPE_MARKET_HALT || (p)->tag_type == TAG_TYPE_DATA_NOT_READY ||            \
   (p)->tag_type == TAG_TYPE_OI_HALT)
#define IS_NORMAL_TAG(p) ((p)->tag_type == TAG_TYPE_NORMAL)

#ifdef __CYGWIN__
#pragma pack(push, 1)
#else
#ifdef __sparc
#pragma pack(1)
#else
#pragma pack(push)
#pragma pack(1)
#endif
#endif

/*
 * @brief Product-Price statistics collected from matched MTF orders.
 * 用於統計一個商品在單一撮合交易中的價格資訊
 */
typedef struct {
  int Price;    // 成交價格
  int TotalQty; // 此價格的總成交量
} MTF_Price;

/*
 * @brief Used to keep statistics of one product in one MTF transaction.
 * 用於統計一個商品在單一撮合交易中的資訊
 */
class MTF_Product {
public:
  int m_PGSEQ; // 商品序號

  int m_BuyCnt;    // 買進筆數
  int m_SellCnt;   // 賣出筆數
  int m_BuyQty;    // 買進總量
  int m_SellQty;   // 賣出總量
  int m_LastPrice; // 最後成交價
  int m_MaxPrice;  // 最高價
  int m_MinPrice;  // 最低價

  int m_PriceCount;
  int m_ListLength;
  MTF_Price **m_PriceList;

  MTF_Product();
  ~MTF_Product();

  MTF_Price *AllocPrice(void);
  int FreeAllPrice(void);
};

/*
 * @brief MTF Transaction.
 * 用於保存一個完整撮合交易的所有資訊
 */
class MTF_Transaction {
public:
  uint32_t m_ID;     // 交易序號 (撮合系統指定)
  msg_time_t m_Time; // 交易時間 (撮合系統記錄)
  int m_BS1;         // 第一腳買賣別
  int m_BS2;         // 第二腳買賣別
  int m_PGSEQ1;      // 第一腳商品序號
  int m_PGSEQ2;      // 第二腳商品序號
  int m_StatusCode;  // 狀態碼

  int m_ProductCount;
  int m_ListLength;
  MTF_Product **m_ProductList;

  MTF_Transaction();
  ~MTF_Transaction();

  MTF_Product *AllocProd(void);
  int FreeLastProd(void);
  int FreeAllProd(void);

  void RegisterProdPrice(int PGSEQ, int Price, MTF_Product **ppProduct, MTF_Price **ppPrice);
  int AddOrder(tag_rpt_t *pRPT);
  int Finalize(void);
};

/*
 * @brief MTF Buffer 主類別
 * 用於處理 match_p.dat 檔案的讀取和解析
 */
class MTF_Buffer {
public:
  AtomicIO m_atm;       // 原子化檔案讀取物件
  int m_handle;         // 檔案控制代碼
  tag_flow_t m_flowtag; // 當前流程標籤
  char m_melogger[128]; // match_p.dat 檔案路徑
  int m_grpid;          // 群組ID

  int m_ListLength;
  int m_TransactionCount;
  MTF_Transaction **m_TransactionList;

  // 商品資訊
  ProdInfo *m_pProdBook;   // 商品資訊物件指標
  LogViewer *m_pLogViewer; // LogViewer 物件指標

  // 狀態管理
  bool m_bIsInCompete;           // 是否在集合競價中
  msg_time_t m_CurrentTransTime; // 當前交易時間
  uint32_t m_CurrentID;          // 當前交易ID
  uint32_t m_LastSysOrderID;     // 最後處理的系統單號

  // 建構/解構函式
  MTF_Buffer(int grpid, ProdInfo *pInfo, LogViewer *pLv, char *melog);
  ~MTF_Buffer();

  // Transaction 管理函式
  MTF_Transaction *AllocTrans(void);
  int FreeLastTrans(void);
  int FreeAllTrans(void);

  // 檔案操作
  int GetFileSize(off_t &size); // 取得檔案大小

  // 解析功能
  int ParseUntilID(uint32_t LastID, int mode);           // 解析到指定ID (-r 重啟模式用)
  int ParseTransaction(char *p, int buflen, int Output); // 解析單一交易
  int ParseAllTransactions(); // 完整解析所有交易記錄 (stovol專用)
  void ResetToBeginning();    // 重置到檔案開頭 (stovol專用)

  // 標籤處理
  void parse_begin(tag_begin_t *p, MTF_Transaction *t);
  int parse_flow(uint8_t version, tag_flow_t *p);
  void parse_market_data(tag_market_data_t *p);

  // 市場資料更新函式 (從 ftprice_stf 移植)
  void UpdateOrderBook(tag_market_data_t *p);
  void UpdateBestDPrice(tag_market_data_t *p);

  // 盤中報價放寬 (QUOTE_EXTEND) 功能
  void process_log_quote_extend(tag_log_t *p); // 處理 LOG_SUBTYPE_QUOTE_EXTEND 封包
  void UpdateSLTSpreadQty(tag_flow_t *p_tag);  // 更新商品報價價差/口數倍數

  // 狀態查詢
  tag_flow_t GetCurrentFlowTag() { return m_flowtag; }
  uint32_t GetLastSysID();
  void change_stage_stovol(tag_flow_t tag, const char *prod_type);

  // 清理和重置
  void Purge(void);
  void SummarizeTransactionList();

  // 統計功能
  int Summarize(MTF_Transaction *pTrans, MTF_Product *pProduct);
};

// 取得 sub-second (與 ftprice 家族一致)
inline uint16_t GetSubSecond(const msg_time_t &p) { return static_cast<uint16_t>(p.ns); }

#pragma pack()

#endif // OPT_STOVOL_INCLUDE_MTFBUFFER_H_
