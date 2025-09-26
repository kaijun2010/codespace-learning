/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file mtfbuffer.cpp
 * @brief mtfbuffer class implementation for stovol
 * @author kaijun@taifex.com.tw
 *
 * MTF Buffer 實作，用於處理期貨撮合資料 match_p.dat
 * 從 ftprice_stf/mtfbuffer.cpp 移植而來，保留核心功能
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <inttypes.h>
#include <iomanip>
#include <iostream>

#include "LoggerProxy.h"
#include "commlib/inc/atomic_io.h"
#include "commlib/inc/command.hpp"
#include "commlib/inc/match_dat.h"
#include "commlib/inc/safecpy.hpp"
#include "commlib/inc/tfxlog.h"
#include "env.h" // for build_log_path
#include "match_processor.h"
#include "mtfbuffer.h"
#include "opt/inc/LogViewer.h"
#include "prodinfo.h"

// Debug print macros for stovol
#define print_tag_begin(p)                                                                         \
  { COUTX << *((tag_begin_t *)p) << ENDLX; }
#define print_tag_flow(p)                                                                          \
  { COUTX << *((tag_flow_t *)p) << ENDLX; }
#define print_tag_rpt(p)                                                                           \
  { COUTX << *((tag_rpt_t *)p) << ENDLX; }
#define print_tag_market_data(p)                                                                   \
  { COUTX << *((tag_market_data_t *)p) << ENDLX; }
#define print_tag_auto_kill(p)                                                                     \
  { COUTX << *((tag_auto_kill_t *)p) << ENDLX; }
#define print_tag_best_one(p)                                                                      \
  { COUTX << *((tag_best_one_t *)p) << ENDLX; }
#define print_tag_kswitch(p)                                                                       \
  { COUTX << *((tag_kswitch_t *)p) << ENDLX; }
#define print_tag_log(p)                                                                           \
  { COUTX << *((tag_log_t *)p) << ENDLX; }
#define print_tag_in(p)                                                                            \
  { COUTX << *((tag_in_t *)p) << ENDLX; }

#define PATH_DEF_LEN 256
#define SLEEP_MS(milsec) usleep(milsec * 1000)

using namespace std;

// ---------------------------------------------------------------------------
// 報價放寬 (QUOTE_EXTEND) 相關輔助 (參考 ftprice 實作)
// ---------------------------------------------------------------------------
static double g_scaleFactor[] = {1,       0.1,      0.01,      0.001,      0.0001,
                                 0.00001, 0.000001, 0.0000001, 0.00000001, 0.000000001};
static inline double getFactor(int scale) {
  if (scale < 0 || scale > 9) {
    TFX::Log::COUT("Error: Invalid scale value.");
    return 1.0;
  }
  return g_scaleFactor[scale];
}

//--------------------------------------------------------------------
// global variables
//--------------------------------------------------------------------
extern bool g_trace_mode;
extern bool g_recover_mode;
extern bool g_close_mode;
extern bool g_test_mode;
extern int g_grpid;
extern ProdInfo g_fut_prodbook;
extern ProdInfo g_opt_prodbook;

// 期貨時間管理變數
extern std::atomic<bool> g_fut_market_started;
extern std::atomic<uint64_t> g_fut_tprice_header_count;
extern std::atomic<uint64_t> g_fut_last_15min_count;
extern std::atomic<uint64_t> g_fut_last_1min_count;
extern std::atomic<bool> g_fut_is_after_1330;
extern std::atomic<uint32_t> g_fut_current_processing_id;

// 選擇權時間管理變數
extern std::atomic<bool> g_opt_market_started;          // 新增 extern 宣告
extern std::atomic<uint64_t> g_opt_tprice_header_count; // OPT tprice 計數
extern std::atomic<uint64_t> g_opt_last_15min_count;
extern std::atomic<uint64_t> g_opt_last_1min_count;
extern std::atomic<bool> g_opt_is_after_1330;
extern void check_opt_calculation_time(uint32_t sysorder_id, const msg_time_t &calc_time,
                                       uint32_t tprice_sysorder_id);
extern void check_opt_market_status(const tag_flow_t &flow_tag);

// 期貨時間檢查函數
extern void check_fut_calculation_time(uint32_t sysorder_id, const msg_time_t &calc_time,
                                       uint32_t tprice_sysorder_id);

//--------------------------------------------------------------------
// global functions
//--------------------------------------------------------------------
void DumpMD(TFX_Log &Log, char Prefix, tag_market_data_t *p) {
  if (!p)
    return;

  char line_tag;
  switch (p->type) {
  case 1:
    line_tag = 'M';
    break;
  case 2:
    line_tag = 'C';
    break;
  case 3:
    line_tag = 'V';
    break;
  case 4:
    line_tag = 'O';
    break;
  default:
    line_tag = '?';
    break;
  }
  Log.PRINT("  %c %c PGSEQ=%d BS=%d PQ=(%d,%d)\n", Prefix, line_tag, p->pseq, p->Side, p->Price,
            p->Qty);
  return;
}

void DumpIDR(TFX_Log &Log, char Prefix, uint32_t ID, msg_time_t Time, tag_rpt_t *p) {
  char ODR[6];

  if (!p)
    return;
  char line_tag;
  line_tag = p->header.ExecType;

  strncpy(ODR, p->ord_data.order_no, 5);
  ODR[5] = 0;

  char sTimeBuf[TFX::Time::MAX_OUT_LEN];
  memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
  TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &Time, sTimeBuf);
  Log.PRINT("  %c %c %u (%s) [%d:%s] SFCM=%d OQ=%c [%d:%d] RET=%d TYPE=%d "
            "CM_TYPE=%d BS=(%d%d%d) PQ=(%d,%d) PQ1=(%d,%d) "
            "PQ2=(%d,%d)\n",
            Prefix, line_tag, ID, sTimeBuf, p->ord_data.fcm_id, ODR, p->header.source_fcm_id,
            p->ord_data.PositionEffect, p->ord_data.symbol.pseq1, p->ord_data.symbol.pseq2,
            p->ord_data.status_code, p->ord_data.OrdType, p->combined_match_type, p->ord_data.Side,
            p->ord_data.symbol.leg_side[0], p->ord_data.symbol.leg_side[1], p->LastPx, p->LastQty,
            p->leg_px[0], p->leg_qty[0], p->leg_px[1], p->leg_qty[1]);

  return;
}

void DumpSimpleIDR(TFX_Log &Log, char Prefix, uint32_t ID, msg_time_t Time, tag_rpt_t *p) {
  if (!p)
    return;
  char line_tag;
  line_tag = p->header.ExecType;
  char sTimeBuf[TFX::Time::MAX_OUT_LEN];
  memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
  TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &Time, sTimeBuf);
  Log.PRINT("  %c %c %u (%s)\n", Prefix, line_tag, ID, sTimeBuf);
  return;
}

/**
 * @brief 檢查是否全部期貨商品皆已收盤
 *
 * @retval true 全部期貨商品皆為收盤狀態
 * @return false 尚有期貨商品為非收盤狀態
 */
bool check_market_end_fut() {
  vector<Product>::iterator vit;
  for (vit = g_fut_prodbook.m_prod.begin(); vit != g_fut_prodbook.m_prod.end(); ++vit) {
    // 找到任何一個商品未收盤則繼續
    if (vit->m_normal_stage != ST_CLOSE) {
      return false;
    }
  }
  return true;
}

/**
 * @brief 檢查是否全部選擇權商品皆已收盤
 *
 * @retval true 全部選擇權商品皆為收盤狀態
 * @return false 尚有選擇權商品為非收盤狀態
 */
bool check_market_end_opt() {
  vector<Product>::iterator vit;
  for (vit = g_opt_prodbook.m_prod.begin(); vit != g_opt_prodbook.m_prod.end(); ++vit) {
    // 找到任何一個商品未收盤則繼續
    if (vit->m_normal_stage != ST_CLOSE) {
      return false;
    }
  }
  return true;
}

/**
 @brief 依據流程群組更新商品狀態
 */
void update_prod_status_NORMAL(tag_flow_t &tag) {
  uint8_t p_stage;
  int count = 0;
  vector<Product>::iterator vit;
  for (vit = g_fut_prodbook.m_prod.begin(); vit != g_fut_prodbook.m_prod.end(); ++vit) {
    // 記住目前stage
    p_stage = vit->m_normal_stage;
    // 已收盤則skip
    if (p_stage == ST_CLOSE)
      continue;

    /* Normal情況檢查tagflow的OSW_GRP與此商品是否相同 */
    if (vit->m_flow_grp_id == tag.grp_no) {
      count++;
      vit->updateStatus(tag);
      vit->updateProductFlags();
    }

  } // end for each product

  // 如果沒有商品狀態改變，不輸出無意義的訊息
  if (count == 0) {
    return;
  }

#ifdef DEV_TEST_MODE
  printf("%d product's stage changed.\n", count);
  fflush(stdout);
#else
  TFX::Log::COUT("(FUT) {} product's stage changed.", count);
#endif
}

/**
 * @brief 依據流程群組更新(選擇權)商品狀態
 * 與 update_prod_status_NORMAL 類似, 但操作 g_opt_prodbook
 */
void update_opt_prod_status_NORMAL(tag_flow_t &tag) {
  uint8_t p_stage;
  int count = 0;
  vector<Product>::iterator vit;
  for (vit = g_opt_prodbook.m_prod.begin(); vit != g_opt_prodbook.m_prod.end(); ++vit) {
    p_stage = vit->m_normal_stage;
    if (p_stage == ST_CLOSE)
      continue;
    if (vit->m_flow_grp_id == tag.grp_no) {
      count++;
      vit->updateStatus(tag);
      vit->updateProductFlags();
    }
  }
  if (count == 0)
    return;
#ifdef DEV_TEST_MODE
  printf("(OPT) %d product's stage changed.\n", count);
  fflush(stdout);
#else
  TFX::Log::COUT("(OPT) {} product's stage changed.", count);
#endif
}

/*-------------------------------------------------------------------------------------------------------*/
/**
 @brief 依據撮合群組更新商品狀態 0代表全部
 */
void update_prod_status_MATCHGRP(tag_flow_t &tag) {
  // 全部暫停或指定該撮合群組暫停才執行更新
  if (tag.header.tfx_grp_id != 0 && tag.header.tfx_grp_id != g_grpid)
    return;

  uint8_t p_stage;
  int count = 0;
  vector<Product>::iterator vit;
  for (vit = g_fut_prodbook.m_prod.begin(); vit != g_fut_prodbook.m_prod.end(); ++vit) {
    // 記住目前stage
    p_stage = vit->m_normal_stage;
    // 已收盤則skip
    if (p_stage == ST_CLOSE)
      continue;

    count++;
    vit->updateStatus(tag);
    vit->updateProductFlags();
  } // end for each product

  // 如果沒有商品狀態改變，不輸出無意義的訊息
  if (count == 0) {
    return;
  }

#ifdef DEV_TEST_MODE
  printf("%d product's stage changed.\n", count);
  fflush(stdout);
#else
  TFX::Log::COUT("{} product's stage changed.", count);
#endif
}

/*-------------------------------------------------------------------------------------------------------*/
/**
 @brief 依據單一商品序號更新商品狀態
 */
void update_prod_status_PSEQ(tag_flow_t &tag) {
  uint8_t p_stage;

  if (tag.pseq <= 0 || tag.pseq > g_fut_prodbook.m_prod_cnt) {
    TFX::Log::COUT("Error: Invalid tag_flow.pseq value:{}", tag.pseq);
    return;
  }

  // 記住目前stage
  int pseq = tag.pseq;
  p_stage = g_fut_prodbook.m_prod[pseq].m_normal_stage;
  // 已收盤則skip
  if (p_stage == ST_CLOSE)
    return;
  g_fut_prodbook.m_prod[pseq].updateStatus(tag);
  g_fut_prodbook.m_prod[pseq].updateProductFlags();

  // 總是輸出訊息，因為確實有一個商品狀態改變了
#ifdef DEV_TEST_MODE
  printf("1  product's stage changed.\n");
  fflush(stdout);
#else
  TFX::Log::COUT("1  product's stage changed.");
#endif
}

/**
 @brief 依據UNDERLYING_MARKET更新商品狀態
 */
void update_prod_status_UNDERLYING_MAKRET(tag_flow_t &tag) {
  uint8_t p_stage;
  int count = 0;
  vector<Product>::iterator vit;
  for (vit = g_fut_prodbook.m_prod.begin(); vit != g_fut_prodbook.m_prod.end(); ++vit) {
    // 記住目前stage
    p_stage = vit->m_normal_stage;
    // 已收盤則skip
    if (p_stage == ST_CLOSE)
      continue;

    /* Normal情況檢查tagflow的OSW_GRP與此商品是否相同 */
    if (vit->m_underlying_market == tag.pdk_prod_id[0]) {
      count++;
      vit->updateStatus(tag);
      vit->updateProductFlags();
    }

  } // end for each product

  // 如果沒有商品狀態改變，不輸出無意義的訊息
  if (count == 0) {
    return;
  }

#ifdef DEV_TEST_MODE
  printf("%d  MARKET product's stage changed.\n", count);
  fflush(stdout);
#else
  TFX::Log::COUT("{} MARKET product's stage changed.", count);
#endif
}

/**
 @brief 依據單一契約更新商品狀態
 */
void update_prod_status_PDK(tag_flow_t &tag) {
  uint8_t p_stage;
  int count = 0;
  char pdk_kind_id[PDK_LEN + 1];

  memset(&pdk_kind_id, 0x00, sizeof(pdk_kind_id));
  memcpy(pdk_kind_id, tag.pdk_prod_id, PDK_LEN);

  // 對於 stovol，簡化 PDK 處理 - 直接比對所有商品的 pdk_kind_id
  vector<Product>::iterator vit;
  for (vit = g_fut_prodbook.m_prod.begin(); vit != g_fut_prodbook.m_prod.end(); ++vit) {
    // 記住目前stage
    p_stage = vit->m_normal_stage;
    // 已收盤則skip
    if (p_stage == ST_CLOSE)
      continue;

    // 比對 PDK 契約代碼
    if (strncmp(vit->m_pdk_kind_id, pdk_kind_id, PDK_LEN) == 0) {
      count++;
      vit->updateStatus(tag);
      vit->updateProductFlags();
    }
  }

  // 如果沒有商品狀態改變，不輸出無意義的訊息
  if (count == 0) {
    return;
  }

#ifdef DEV_TEST_MODE
  printf("%d  PDK product's stage changed.\n", count);
  fflush(stdout);
#else
  TFX::Log::COUT("{} PDK product's stage changed.", count);
#endif
}

// ================== OPT 專用商品狀態更新 (目前僅必要最小化，若後續需完整邏輯再擴充)
// ==================

//====================================================================
// MTF_Price 實作
//====================================================================

//====================================================================
// MTF_Product 實作
//====================================================================

MTF_Product::MTF_Product() {
  m_PGSEQ = -1;
  m_BuyCnt = 0;
  m_SellCnt = 0;
  m_BuyQty = 0;
  m_SellQty = 0;
  m_LastPrice = 0;
  m_MaxPrice = 0;
  m_MinPrice = 0;

  m_PriceCount = 0;
  m_ListLength = 0;
  m_PriceList = NULL;
}

MTF_Product::~MTF_Product() { FreeAllPrice(); }

MTF_Price *MTF_Product::AllocPrice(void) {
  if (m_PriceCount >= m_ListLength) {
    // 需要擴展 PriceList
    int newLength = m_ListLength + PRICE_BLOCK_SIZE;
    MTF_Price **newList = new MTF_Price *[newLength];

    // 複製舊資料
    if (m_PriceList != NULL) {
      for (int i = 0; i < m_ListLength; i++) {
        newList[i] = m_PriceList[i];
      }
      delete[] m_PriceList;
    }

    // 初始化新空間
    for (int i = m_ListLength; i < newLength; i++) {
      newList[i] = NULL;
    }

    m_PriceList = newList;
    m_ListLength = newLength;
  }

  // 分配新的 MTF_Price
  m_PriceList[m_PriceCount] = new MTF_Price;
  m_PriceList[m_PriceCount]->Price = 0;
  m_PriceList[m_PriceCount]->TotalQty = 0;

  return m_PriceList[m_PriceCount++];
}

int MTF_Product::FreeAllPrice(void) {
  if (m_PriceList != NULL) {
    for (int i = 0; i < m_PriceCount; i++) {
      if (m_PriceList[i] != NULL) {
        delete m_PriceList[i];
        m_PriceList[i] = NULL;
      }
    }
    delete[] m_PriceList;
    m_PriceList = NULL;
  }

  m_PriceCount = 0;
  m_ListLength = 0;
  return 0;
}

//====================================================================
// MTF_Transaction 實作
//====================================================================

MTF_Transaction::MTF_Transaction() {
  m_ID = 0;
  memset(&m_Time, 0, sizeof(m_Time));
  m_BS1 = 0;
  m_BS2 = 0;
  m_PGSEQ1 = -1;
  m_PGSEQ2 = -1;
  m_StatusCode = 0;

  m_ProductCount = 0;
  m_ListLength = 0;
  m_ProductList = NULL;
}

MTF_Transaction::~MTF_Transaction() { FreeAllProd(); }

MTF_Product *MTF_Transaction::AllocProd(void) {
  if (m_ProductCount >= m_ListLength) {
    // 需要擴展 ProductList
    int newLength = m_ListLength + PRODUCT_BLOCK_SIZE;
    MTF_Product **newList = new MTF_Product *[newLength];

    // 複製舊資料
    if (m_ProductList != NULL) {
      for (int i = 0; i < m_ListLength; i++) {
        newList[i] = m_ProductList[i];
      }
      delete[] m_ProductList;
    }

    // 初始化新空間
    for (int i = m_ListLength; i < newLength; i++) {
      newList[i] = NULL;
    }

    m_ProductList = newList;
    m_ListLength = newLength;
  }

  // 分配新的 MTF_Product
  m_ProductList[m_ProductCount] = new MTF_Product;
  return m_ProductList[m_ProductCount++];
}

int MTF_Transaction::FreeLastProd(void) {
  if (m_ProductCount > 0) {
    m_ProductCount--;
    if (m_ProductList[m_ProductCount] != NULL) {
      delete m_ProductList[m_ProductCount];
      m_ProductList[m_ProductCount] = NULL;
    }
  }
  return 0;
}

int MTF_Transaction::FreeAllProd(void) {
  if (m_ProductList != NULL) {
    for (int i = 0; i < m_ProductCount; i++) {
      if (m_ProductList[i] != NULL) {
        delete m_ProductList[i];
        m_ProductList[i] = NULL;
      }
    }
    delete[] m_ProductList;
    m_ProductList = NULL;
  }

  m_ProductCount = 0;
  m_ListLength = 0;
  return 0;
}

void MTF_Transaction::RegisterProdPrice(int PGSEQ, int Price, MTF_Product **ppProduct,
                                        MTF_Price **ppPrice) {
  // 尋找或創建 Product
  MTF_Product *pProduct = NULL;
  for (int i = 0; i < m_ProductCount; i++) {
    if (m_ProductList[i]->m_PGSEQ == PGSEQ) {
      pProduct = m_ProductList[i];
      break;
    }
  }

  if (pProduct == NULL) {
    pProduct = AllocProd();
    pProduct->m_PGSEQ = PGSEQ;
  }

  // 尋找或創建 Price
  MTF_Price *pPrice = NULL;
  for (int i = 0; i < pProduct->m_PriceCount; i++) {
    if (pProduct->m_PriceList[i]->Price == Price) {
      pPrice = pProduct->m_PriceList[i];
      break;
    }
  }

  if (pPrice == NULL) {
    pPrice = pProduct->AllocPrice();
    pPrice->Price = Price;
  }

  if (ppProduct)
    *ppProduct = pProduct;
  if (ppPrice)
    *ppPrice = pPrice;
}

int MTF_Transaction::AddOrder(tag_rpt_t *pRPT) {
  int i, N, Input, Skip;
  int PGSEQ[2], Price[2], Qty[2];
  int BS[2];
  MTF_Product *pProduct;
  MTF_Price *pPrice;

  /* reset input status */
  Input = 0;

  /*** single order ***/
  if (pRPT->ord_data.symbol.pseq2 == 0) {
    N = 1;
    PGSEQ[0] = pRPT->ord_data.symbol.pseq1;
    Price[0] = pRPT->LastPx;
    BS[0] = pRPT->ord_data.Side;
    Qty[0] = pRPT->LastQty;
#ifdef DEBUG2
    // 前置顯示交易時間 (HH:MM:SS.mmm) 取自 transaction m_Time (對應 tag_begin.tran_time)
    char sTimeBuf[TFX::Time::MAX_OUT_LEN];
    memset(sTimeBuf, 0, sizeof(sTimeBuf));
    TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &m_Time, sTimeBuf);
    COUTX << sTimeBuf << " ADDORDER: PSEQ=" << PGSEQ[0] << " BS=" << BS[0] << " PRICE=" << Price[0]
          << " QTY=" << Qty[0] << ENDLX;
#endif

    /* check whether this order is the input order or not */
    // 與begin裡面的SysOrderId相等則為新進單
    if (pRPT->SysOrderID == m_ID) {
      Input = 1;
#ifdef DEBUG2
      COUTX << "ADDORDER: Input = 1" << ENDLX;
#endif
    }

    RegisterProdPrice(PGSEQ[0], Price[0], &pProduct, &pPrice);
    pProduct->m_LastPrice = Price[0];
  }
  /*** combined order - 複式商品處理，stovol 不需要但保留架構 ***/
  else {
    /* check whether this combined order is the input order or not */
    if (pRPT->SysOrderID == m_ID)
      Input = 1;

    // 對於 stovol，我們跳過複式商品處理
    // COUTX << "ADDORDER: Combined order detected (SysOrderID=" << pRPT->SysOrderID
    //     << ", PSEQ1=" << pRPT->ord_data.symbol.pseq1 << ", PSEQ2=" << pRPT->ord_data.symbol.pseq2
    //     << "), skipping..." << ENDLX;
    return 0;
  }

  /*** strict check ***/
  Skip = 0;
  for (i = 0; i < N; i++) {
    if (BS[i] != 1 && BS[i] != 2) {
      COUTX << "Error: Unknown BS_CODE! Matched I-Order skipped." << ENDLX;
      Skip = 1;
    }
    if (Qty[i] <= 0) {
      COUTX << "Error: Invalid Qty! Matched I-Order skipped." << ENDLX;
      Skip = 1;
    }
  }

  if (Skip == 1)
    return -1;

  // accumulate this pIDR info into corresponding MTF_Product and MTF_Price entry
  for (i = 0; i < N; i++) {
    RegisterProdPrice(PGSEQ[i], Price[i], &pProduct, &pPrice);

    // buy side
    if (BS[i] == 1) {
      if (Input != 1)
        pProduct->m_BuyCnt++;

      pPrice->TotalQty += Qty[i];
      pProduct->m_BuyQty += Qty[i];
    }
    // sell side
    else if (BS[i] == 2) {
      if (Input != 1)
        pProduct->m_SellCnt++;

      pPrice->TotalQty += Qty[i];
      pProduct->m_SellQty += Qty[i];
    }

    pProduct->m_LastPrice = Price[i];
    if (pProduct->m_MaxPrice < Price[i])
      pProduct->m_MaxPrice = Price[i];
    if (pProduct->m_MinPrice > Price[i])
      pProduct->m_MinPrice = Price[i];
  }

  return N;
}

int MTF_Transaction::Finalize(void) {
  // 完成交易處理
  COUTX << "Transaction " << m_ID << " finalized with " << m_ProductCount << " products" << ENDLX;
  return 0;
}

//====================================================================
// MTF_Buffer 實作
//====================================================================

MTF_Buffer::MTF_Buffer(int grpid, ProdInfo *pInfo, LogViewer *pLv, char *melog) {
  m_pProdBook = pInfo;
  m_pLogViewer = pLv;
  m_grpid = grpid;
  m_ListLength = TRANSACTION_BLOCK_SIZE;
  m_TransactionCount = 0;
  m_TransactionList = NULL;
  m_bIsInCompete = false;
  m_CurrentID = 0;
  m_LastSysOrderID = 0;
  memset(&m_flowtag, 0, sizeof(m_flowtag));
  safecpy(m_melogger, melog);
  TFX::Time::SetTime(&m_CurrentTransTime, 0, 0);

  /* open match_p.dat */
  m_handle = m_atm.Open(melog);
  if (m_handle < 0) {
    COUTX << "Error: unable to open file: " << melog << ENDLX;
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "unable to open file: {}", melog);
#endif
    exit(1);
  }

  m_TransactionList = new MTF_Transaction *[m_ListLength];
  for (int i = 0; i < m_ListLength; i++) {
    m_TransactionList[i] = new MTF_Transaction;
  }
}

MTF_Buffer::~MTF_Buffer() {
  for (int i = 0; i < m_ListLength; i++) {
    delete m_TransactionList[i];
  }
  delete[] m_TransactionList;
}

MTF_Transaction *MTF_Buffer::AllocTrans(void) {
  /*
   *  m_TransactionList[] may not be large enough,
   *  if so, enlarge m_TransactionList[] by TRANSACTION_BLOCK_SIZE entries
   * each time
   */
  if (m_TransactionCount == m_ListLength) {
    /* Enlarge m_TransactionList[] */
    // COUTX << "Enlarge Transaction List from " << m_ListLength
    //       << " to " << m_ListLength + TRANSACTION_BLOCK_SIZE << ENDLX;
    m_ListLength += TRANSACTION_BLOCK_SIZE;
    MTF_Transaction **NewList = new MTF_Transaction *[m_ListLength];
    memcpy(NewList, m_TransactionList, m_TransactionCount * sizeof(MTF_Transaction *));
    delete[] m_TransactionList;
    m_TransactionList = NewList;

    for (int i = m_TransactionCount; i < m_ListLength; i++) {
      m_TransactionList[i] = new MTF_Transaction;
    }
  }
  m_TransactionList[m_TransactionCount]->m_ID = 0;
  TFX::Time::SetTime(&m_TransactionList[m_TransactionCount]->m_Time, 0, 0);
  m_TransactionList[m_TransactionCount]->m_BS1 = 0;
  m_TransactionList[m_TransactionCount]->m_BS2 = 0;
  m_TransactionList[m_TransactionCount]->m_PGSEQ1 = 0;
  m_TransactionList[m_TransactionCount]->m_PGSEQ2 = 0;
  m_TransactionList[m_TransactionCount]->m_ProductCount = 0;

  ++m_TransactionCount;

  return m_TransactionList[m_TransactionCount - 1];
}

int MTF_Buffer::FreeLastTrans() {
  m_TransactionCount--;
  m_TransactionList[m_TransactionCount]->FreeAllProd();

  return 0;
}

int MTF_Buffer::FreeAllTrans() {
  for (int i = 0; i < m_TransactionCount; i++) {
    m_TransactionList[i]->FreeAllProd();
  }
  m_TransactionCount = 0;

  return 0;
}

int MTF_Buffer::GetFileSize(off_t &size) {
  struct stat st;
  if (stat(m_melogger, &st) != 0) {
    COUTX << "Error: failed to get file size for " << m_melogger << ENDLX;
    return -1;
  }

  size = st.st_size;
  return 0;
}

/*
 * @brief 解析交易記錄直到指定的 SysOrderID
 * @param ID 指定的 SysOrderID
 * @param mode 1: File mode => 讀到與ID相同的該筆transaction並進行處理
 *             2: LLM  mode => 讀到與ID相同的該筆transaction但不處理最後一筆transaction
 * @return 1 成功
 *         0 失敗
 *        -1 讀取錯誤
 */
int MTF_Buffer::ParseUntilID(uint32_t ID, int mode) {
  char *p; // p will be pointer to transaction
  int rc;
  int count = 0;
  uint32_t read_id;

  COUTX << "ParseUntilID: target ID=" << ID << " mode=" << mode << ENDLX;

  /*
   * 讀取新的交易記錄並逐一處理
   * 直到找到指定的 ID 或發生錯誤
   */
  while (1) {
    while ((p = m_atm.ReadTran())) {
      count++;
      read_id = ((tag_begin_t *)p)->SysOrderID;

      if (read_id == ID && mode == 2)
        return 1;

      /* 解析交易記錄 */
      rc = ParseTransaction(p, -1, 0);
      if (rc == 1) {
        // 遇到流程標籤，進行狀態切換
        tag_flow_t ftag = GetCurrentFlowTag();
        change_stage_stovol(ftag, (m_pProdBook == &g_fut_prodbook) ? "FUT" : "OPT");
      }

      if (read_id == ID)
        return 1;
    }

    // 讀取更多資料
    int len = m_atm.Read();
    if (len == 0) {
      // 沒有資料或交易記錄不完整
      // 等待 100ms 後繼續讀取
      usleep(100 * 1000); // 100ms
      continue;
    } else if (len < 0) {
      COUTX << "Error: atomic_io Read() failed, return code=" << len << ENDLX;
      TFX::Log::COUTMSG(STOVOL_ERROR, "atomic_io Read() failed, return code={}", len);
      return -1;
    }
  }

  return 0;
}

/*
 * @brief 處理流程狀態變更 (簡化版本，適用於 stovol)
 * @param tag 流程標籤
 */
void MTF_Buffer::change_stage_stovol(tag_flow_t tag, const char *prod_type) {
  // 將 msg_time_t 轉換成可讀時間格式
  char sTimeBuf[TFX::Time::MAX_OUT_LEN];
  time_t epoch_time = tag.tran_time.epoch_s;
  struct tm *tm_info = localtime(&epoch_time);
  uint32_t ms = tag.tran_time.ns / 1000000; // 將 ns 轉換成 ms (取前三位)

  snprintf(sTimeBuf, sizeof(sTimeBuf), "%02d:%02d:%02d.%03d", tm_info->tm_hour, tm_info->tm_min,
           tm_info->tm_sec, ms);

  // 先輸出通用的 FLOW_GROUP 資訊（適用於所有 tag_type）
  // 過濾掉 TRADESTATUS:101 的輸出（太多不必要的資訊）
  if (tag.TradeStatus != 101) {
    CINFOX << "FLOW_GROUP:" << (unsigned int)tag.grp_no
           << " TRADESTATUS:" << (unsigned int)tag.TradeStatus << " TIME:" << sTimeBuf
           << " tag_type:" << (unsigned int)tag.tag_type << ENDLX;
  }

  bool isFutBook = (prod_type && prod_type[0] == 'F');
  // 依據 tag_flow 種類更新商品狀態 (FUT / OPT 分離)
  switch (tag.tag_type) {
  case TAG_TYPE_NORMAL: // 1
    if (isFutBook)
      update_prod_status_NORMAL(tag);
    else
      update_opt_prod_status_NORMAL(tag);

    // 收盤處理
    if (tag.TradeStatus == ST_CLOSE) {
      CINFOX << "Market close detected for group " << (unsigned int)tag.grp_no << ENDLX;
      // TODO: 可以在這裡添加收盤後的處理邏輯
    }
    break;

  case TAG_TYPE_HALT: // 2
    // 全部暫停或指定該撮合群組暫停才執行更新
    if (tag.header.tfx_grp_id != 0 && tag.header.tfx_grp_id != g_grpid)
      return;
    if (isFutBook)
      update_prod_status_MATCHGRP(tag);

    if (tag.TradeStatus == ST_BREAK) {
      CINFOX << "Trading halted" << ENDLX;
      // TODO: 暫停交易的處理
    }

    if (tag.TradeStatus == ST_RECV) {
      CINFOX << "Trading resumed" << ENDLX;
      // TODO: 恢復交易的處理
    }
    break;

  case TAG_TYPE_PRICE_LIMIT: // 3
    if (tag.grp_no == 0) {
      CINFOX << "PRICE_LIMIT: pdk_prod_id=" << tag.pdk_prod_id
             << " STAGE:" << (unsigned int)tag.stage << ENDLX;
      if (tag.pdk_prod_id[0] == 0) {
        CINFOX << " pseq=" << (int)tag.pseq << ENDLX;
        if (isFutBook)
          update_prod_status_PSEQ(tag);
      } else {
        // PDK 更新
        CINFOX << " pdk_prod_id=" << tag.pdk_prod_id << ENDLX;
        if (isFutBook)
          update_prod_status_PDK(tag);
      }
    }
    break;

  case TAG_TYPE_PRICE_STABLE: // 4
    CINFOX << "PRICE_STABLE: pseq=" << tag.pseq << " TRADESTATUS:" << (unsigned int)tag.TradeStatus
           << ENDLX;
    break;

  case TAG_TYPE_NEWS_HALT: // 5
    if (tag.grp_no == 0) {
      CINFOX << "NEWS_HALT: grp_no=" << (unsigned int)tag.grp_no
             << " TRADESTATUS:" << (unsigned int)tag.TradeStatus << ENDLX;
      if (tag.pdk_prod_id[0] == 0) {
        if (isFutBook)
          update_prod_status_PSEQ(tag);
      } else {
        if (isFutBook)
          update_prod_status_PDK(tag);
      }
    } else {
      CINFOX << "NEWS_HALT: grp_no=" << (unsigned int)tag.grp_no
             << " TRADESTATUS:" << (unsigned int)tag.TradeStatus << ENDLX;
      if (isFutBook)
        update_prod_status_UNDERLYING_MAKRET(tag);
    }
    break;

  case TAG_TYPE_MARKET_HALT: // 6
    if (tag.grp_no == 0) {
      CINFOX << "MARKET_HALT: grp_no=" << (unsigned int)tag.grp_no
             << " TRADESTATUS:" << (unsigned int)tag.TradeStatus << ENDLX;
      if (tag.pdk_prod_id[0] == 0) {
        if (isFutBook)
          update_prod_status_PSEQ(tag);
      } else {
        if (isFutBook)
          update_prod_status_PDK(tag);
      }
    } else {
      if (isFutBook)
        update_prod_status_UNDERLYING_MAKRET(tag);
    }
    break;

  case TAG_TYPE_DATA_NOT_READY: // 7
    CINFOX << "DATA_NOT_READY: grp_no=" << (unsigned int)tag.grp_no
           << " TRADESTATUS:" << (unsigned int)tag.TradeStatus << ENDLX;
    break;

  case TAG_TYPE_DPL: // 8
    CINFOX << "DPL: Dynamic Price Limit detected" << ENDLX;
    break;

  case TAG_TYPE_OI_HALT: // 9
    CINFOX << "OI_HALT: Open Interest halt detected" << ENDLX;
    break;

  default:
    CINFOX << "Unknown tag_type: " << (unsigned int)tag.tag_type << ENDLX;
    break;
  }
}

/*
 * 解析單一交易記錄
 *
 * stovol 8:45~13:30 每15分鐘計算一次價格，13:30~13:45 每1分鐘計算一次價格
 * 因此只需讀取 match_p.dat 檔案即可
 * 不需要訂閱 LLM 即時資料流
 *
 * LLM 設計說明：
 * - LLM 是非常快速的資料傳輸通道
 * - 下游程式可訂閱需要的 LLM 通道取得即時資料
 * - melogger 程式訂閱 LLM 撮合通道，寫入 match_p.dat 檔案
 * - 非即時程式（如 stovol）只需讀取 match_p.dat 即可
 * - 需要極速交易的程式才需直接訂閱 LLM 撮合通道
 *
 * @param p 指向交易記錄開始位置的指標
 * @param buflen 緩衝區長度（buflen==-1 表示從 match_p.dat 檔案讀取）
 * @param Output 輸出控制參數（未使用）
 * @return 0  正常處理
 *         -1 跳過此筆記錄
 *         1  遇到流程標籤
 */
int MTF_Buffer::ParseTransaction(char *p, int buflen, int Output) {
  (void)Output; // 避免未使用參數警告
  int skip = 0;
  int Ret;
  int isFlow = 0;
  int version = 0;
  MTF_Transaction *pTransaction;
  int len = 0;

  // keep the input order's information
  pTransaction = AllocTrans();

  version = m_atm.GetVersion();
  // trick, the TAG_END == '\0'
  while ((len < buflen && *p) || (buflen == -1 && *p)) {
    switch (*p) {
    case TAG_BEGIN:
      parse_begin((tag_begin_t *)(void *)p, pTransaction);
      break;
    case TAG_FLOW:
      isFlow = parse_flow(version, (tag_flow_t *)(void *)p);
      if (isFlow) {
        // 只在對應的產品類型 buffer 上觸發對應市場狀態檢查，避免 FUT 與 OPT 線程互相重複呼叫
        if (m_pProdBook == &g_fut_prodbook) {
          check_fut_market_status(*(tag_flow_t *)p);
        } else if (m_pProdBook == &g_opt_prodbook) {
          check_opt_market_status(*(tag_flow_t *)p);
        }
      }
      break;
    case TAG_RPT: {
      tag_rpt_t *pRPT = (tag_rpt_t *)(void *)p;
      uint8_t RetCode = pRPT->ord_data.status_code;

      if ((pRPT->header.ExecType == ORD_INS && pRPT->combined_match_type == 0) ||
          pRPT->header.ExecType == ORD_INSMATCH) {
        // order reply success - 對於 stovol，只處理單式商品
        if (MATCH_OK(RetCode)) {
          // 跳過複式商品檢查，stovol 只處理單式商品
        }
      }

      // 處理成交回報
      if ((pRPT->header.ExecType == ORD_INS && pRPT->combined_match_type != 0) ||
          pRPT->header.ExecType == ORD_INSMATCH) {
        if (MATCH_OK(RetCode) && pRPT->LastQty != 0) {
          Ret = pTransaction->AddOrder(pRPT); // 成交回報

          /* fatal order error occurred */
          if (Ret < -1)
            skip = 1;

        } else
          Ret = 0;

      } else if (pRPT->header.ExecType == ORD_INS && pRPT->combined_match_type == 0) {
        // 新單未成交回報 - stovol 也處理但不統計
        if (MATCH_OK(RetCode) && pRPT->LastQty == 0) {
          // 記錄但不需要特殊統計
        }
      }

    } break;
    case TAG_MARKET_DATA:
      parse_market_data((tag_market_data_t *)(void *)p);
      break;
    case TAG_BEST_ONE: {
      // stovol 不需要處理試撮價格，先略過
    } break;
    case TAG_IN:
      break;
    case TAG_AUTO_KILL:
      break;
    case TAG_KSWITCH:
      break;
    case TAG_LOG: {
      // 造市者盤中調整措施 (僅以 tag_log_t 包裝於 command 中)
      if (((tag_log_t *)p)->sub_type == LOG_SUBTYPE_QUOTE_EXTEND) {
        process_log_quote_extend((tag_log_t *)(void *)p);
      }
    } break;
    case TAG_TPRICE_HEADER: // 34
      // 僅於各自產品 Buffer 中累計與檢查，避免兩條 thread 重複觸發 FUT 計算造成檔案 I/O 競態
      if (m_pProdBook == &g_fut_prodbook && g_fut_market_started) {
        g_fut_tprice_header_count.fetch_add(1);
        tag_tprice_header_t *tprice_header = (tag_tprice_header_t *)p;
        check_fut_calculation_time(m_CurrentID, tprice_header->calc_time,
                                   tprice_header->SysOrderID);
      }
      if (m_pProdBook == &g_opt_prodbook && g_opt_market_started) { // OPT 計數與檢查
        g_opt_tprice_header_count.fetch_add(1);
        tag_tprice_header_t *tprice_header = (tag_tprice_header_t *)p;
        check_opt_calculation_time(m_CurrentID, tprice_header->calc_time,
                                   tprice_header->SysOrderID);
      }
      break;
    case TAG_TPRICE_STF_HDR: // 35
      // stovol 不需要處理 tprice stf header，跳過
      break;
    case TAG_LIST_TYPE: // 36
      // stovol 不需要處理 list type，跳過
      break;
    case TAG_MEX_HDR: // 26
      // stovol 不需要處理 MEX header，跳過
      break;
    case TAG_MTPR_HEADER: // 27
      // stovol 不需要處理 MTPR header，跳過
      break;
    case TAG_MTPR: // 28
      // stovol 不需要處理 MTPR，跳過
      break;
    case TAG_TPRICE_STF_HDR_20201123: // 29
      // stovol 不需要處理舊版 STF header，跳過
      break;
    case TAG_TPRICE_STF: // 30
      // stovol 不需要處理 STF，跳過
      break;
    case TAG_PACK_HDR: // 31
      // stovol 不需要處理 pack header，跳過
      break;
    default: // skip other tags
#ifdef DEV_TEST_MODE
      COUTX << "Error: unknown tag, " << (int)*p << ENDLX;
#else
      TFX::Log::COUT("Error: unknown tag, {}", (int)*p);
#endif
      skip = 1;
    }

    if (buflen != -1) {
      int tag_size = m_atm.GetTagSize(p);
      if (tag_size <= 0) {
        COUTX << "Err:" << __LINE__ << " GetTagSize fail, tag=" << uint32_t(*p) << ENDLX;
        return 1;
      }
      len += tag_size;
    }

    int tag_size = m_atm.GetTagSize(p);
    if (tag_size <= 0) {
      COUTX << "Err:" << __LINE__ << " GetTagSize fail, tag=" << uint32_t(*p) << ENDLX;
      return 1;
    }
    p += tag_size; // input from match_p.dat
  }

  if (isFlow == 1) {
    FreeLastTrans();
    return 1;
  }

  if (skip == 1) {
    FreeLastTrans();
    return -1;
  }

  /* Update all price, and SummarizeTransactionList() */
  SummarizeTransactionList();

  return 0;
}

void MTF_Buffer::parse_begin(tag_begin_t *p, MTF_Transaction *t) {
  if (!p || !t)
    return;

  t->m_ID = p->SysOrderID;
  t->m_Time = p->tran_time;
  m_CurrentID = p->SysOrderID;
  m_CurrentTransTime = p->tran_time;

  // COUTX << "Transaction begin: ID=" << p->SysOrderID << ENDLX;
}

/**@brief parse flow block
 * @param version 版本號
 * @param p ptr to first byte in block
 */
int MTF_Buffer::parse_flow(uint8_t version, tag_flow_t *p) {
  // 判斷此tag是否屬於同一個taifex group
  if (m_grpid != p->header.tfx_grp_id) {
    TFX::Log::COUT("TPC message tpc_seq={} tfx_grp_id different", p->tpc_seq);
    return 0;
  }

  /* 讀取到flow tag暫時先把tag的內容記下來
   * 由進行切換stage的動作
   */
  // copy tag_flow_t here
  switch (version) {
  case ATOMIC_VERSION: {
    memcpy((void *)&m_flowtag, (void *)p, sizeof(tag_flow_t));

    // 已在 ParseTransaction 內依 m_pProdBook 判斷呼叫，這裡移除避免 OPT-only 時出現 FUT 日誌
  } break;

  default:
    COUTX << "Err: not implement version:" << version << ENDLX;
    // exit(1); // LLM 不要直接exit,會造成行為異常
  }

  return 1;
}

// ==============================================================================================
// 盤中報價放寬: LOG_SUBTYPE_QUOTE_EXTEND 處理 (參考 ftprice 版本移植, 簡化)
// ==============================================================================================
void MTF_Buffer::process_log_quote_extend(tag_log_t *p_log) {
  if (!p_log)
    return;

  tag_flow_t *p_flow_tag = (tag_flow_t *)&p_log->command; // command 區內嵌 tag_flow_t

  // 僅支援 grp_no==0, pdk_prod_id[0]!=0 (PDK 層級調整)
  if (p_flow_tag->grp_no != 0) {
    TFX::Log::COUTMSG(STOVOL_ERROR, "Err: tag_flow grp_no!=0. QUOTE_EXTEND tag_log tpc_seq={}",
                      p_log->tpc_seq);
    return;
  } else if (p_flow_tag->pdk_prod_id[0] != 0) {
    // 更新報價價差(口數)倍數 (不能回縮問題由撮合處理)
    UpdateSLTSpreadQty(p_flow_tag);
  } else {
    TFX::Log::COUTMSG(
        STOVOL_ERROR,
        "Err: tag_flow grp_no==0 && pdk_prod_id[0]==0. QUOTE_EXTEND tag_log tpc_seq={}",
        p_log->tpc_seq);
  }
}

void MTF_Buffer::UpdateSLTSpreadQty(tag_flow_t *p_tag) {
  if (!p_tag || !m_pProdBook)
    return;

  char pdk_kind_id[PDK_LEN + 1];
  memset(&pdk_kind_id, 0x00, sizeof(pdk_kind_id));
  memcpy(pdk_kind_id, p_tag->pdk_prod_id, PDK_LEN);

  double factor = getFactor(GetSubSecond(p_tag->receive_time)); // 與 ftprice 一致

  for (auto &prod : m_pProdBook->m_prod) {
    char pdk_key[PDK_LEN + 1];
    memset(&pdk_key, 0x00, sizeof(pdk_key));
    safecpy(pdk_key, prod.m_pdk_param_key); // stovol 使用 m_pdk_param_key (與 ftprice 同欄位)

    if (strncmp(pdk_key, pdk_kind_id, PDK_LEN) == 0) {
      double slt_spread = prod.m_slt_spread * (p_tag->break_time.epoch_s * factor); // 報價價差倍數
      int acc_qnty = prod.m_acc_qnty * (p_tag->receive_time.epoch_s * factor); // 報價口數倍數

      if (acc_qnty == 0) {
        prod.m_acc_qnty = 1; // 若相乘結果小於 1 口，強制改為 1 口
      } else {
        prod.m_slt_spread = slt_spread;
        prod.m_acc_qnty = acc_qnty;
        // CINFOX << "UpdateSLTSpreadQty OK! prod_id(" << prod.m_pgseq << ")=" << prod.m_prod_id
        //<< " m_slt_spread=" << prod.m_slt_spread << " m_acc_qnty=" << prod.m_acc_qnty
        //<< ENDLX;
      }
    }
  }
}

void MTF_Buffer::parse_market_data(tag_market_data_t *p) {
  if (!p)
    return;

  switch (p->type) {
  case 1: // M
  case 2: // C
    UpdateOrderBook(p);
    break;
  case 3: // V
    UpdateBestDPrice(p);
    break;
  case 4: // O
    break;
  }
}

uint32_t MTF_Buffer::GetLastSysID() {
  // if MELog not exist, return
  AtomicIO InFile;
  int h;
  uint32_t id = 0;

  h = InFile.Open(m_melogger);
  if (h < 0) {
    COUTX << "Error: unable to open file: " << m_melogger << ENDLX;
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "unable to open file: {}", m_melogger);
#endif
    exit(1);
  }

  tag_begin_t *p_begin = (tag_begin_t *)InFile.JumpOffsetByID(ATOMIC_MAX_ID);
  if (p_begin) {
    id = p_begin->SysOrderID;
  }
  InFile.Close();
  return id;
}

void MTF_Buffer::Purge(void) {
  FreeAllTrans();
  m_CurrentID = 0;
  m_LastSysOrderID = 0;
  memset(&m_CurrentTransTime, 0, sizeof(m_CurrentTransTime));

  // COUTX << "MTF_Buffer purged" << ENDLX;
}

void MTF_Buffer::SummarizeTransactionList() {
  int i, j;
  MTF_Transaction *pTrans;
  MTF_Product *pProduct;

  for (i = 0; i < m_TransactionCount; i++) {
    pTrans = m_TransactionList[i];
    for (j = 0; j < pTrans->m_ProductCount; j++) {
      pProduct = pTrans->m_ProductList[j];
      Summarize(pTrans, pProduct);
    } // end for
  } // end for
  Purge();
}

//===================================================================================================================

/*
 * @brief 將交易-商品統計資料匯總到 ProdBook 中
 * @param pTrans 交易物件指標
 * @param pProduct 商品物件指標
 * @return 0:成功
 */
int MTF_Buffer::Summarize(MTF_Transaction *pTrans, MTF_Product *pProduct) {
  int PGSEQ = pProduct->m_PGSEQ;

  /* update summary data to prodBook - 只處理單式商品 */
  if (m_pProdBook && PGSEQ >= 0 && PGSEQ < (int)m_pProdBook->m_prod.size()) {
    /* 更新成交資料 */
    m_pProdBook->m_prod[PGSEQ].m_last_mtime = pTrans->m_Time;
    m_pProdBook->m_prod[PGSEQ].m_status_code = pTrans->m_StatusCode;
    m_pProdBook->m_prod[PGSEQ].m_last_mprice = pProduct->m_LastPrice;
    m_pProdBook->m_prod[PGSEQ].m_last_mqty = pProduct->m_BuyQty;

    /* 更新開盤價 - 在集合競價時才更新(NA) */
  }

  return 0;
}

//===================================================================================================================

/*
 * @brief Update order book of product pgseq in product book
 * @param p pointer to market_data block
 */
void MTF_Buffer::UpdateOrderBook(tag_market_data_t *p) {
  if (p->Side != 1 && p->Side != 2) {
    /* Error */
    COUTX << "Invalid BS code in TAG_MARKET_DATA : " << p->Side << ENDLX;
    return;
  }

  /* update 委託 到product book中 - 只處理單式商品 */
  if (p->type == 1) {
    // 單式商品 - 檢查邊界並更新委託簿
    if (m_pProdBook && p->pseq >= 0 && p->pseq < (int)m_pProdBook->m_prod.size()) {
      // 實際更新委託簿
      Product &product = m_pProdBook->m_prod[p->pseq];

#if ENABLE_STOVOL_PARITY_DEBUG
      // === 診斷：統計委託簿更新 (條件編譯) ===
      static std::unordered_map<int, uint64_t> s_update_cnt; // key: pgseq
      static std::unordered_set<int> s_first_logged;         // 已記錄第一筆者
      uint64_t &uc = s_update_cnt[p->pseq];
      ++uc;
      if (!s_first_logged.count(p->pseq)) {
        s_first_logged.insert(p->pseq);
        try {
          std::ofstream diag(build_log_path("stovol_pc_parity_diag.log"), std::ios::app);
          if (diag.is_open()) {
            diag << "FIRST_ORDERBOOK_UPDATE,SysOrderId=" << m_CurrentID << ",PGSEQ=" << p->pseq
                 << ",Side=" << (p->Side == 1 ? "B" : "S") << ",Price=" << p->Price
                 << ",Qty=" << p->Qty << '\n';
          }
        } catch (...) {
        }
      }
      if ((uc & 0x3FF) == 0) { // 每 1024 次寫一次累積統計 (避免 I/O 過多)
        try {
          std::ofstream diag(build_log_path("stovol_pc_parity_diag.log"), std::ios::app);
          if (diag.is_open()) {
            diag << "ORDERBOOK_UPDATE_STAT,SysOrderId=" << m_CurrentID << ",PGSEQ=" << p->pseq
                 << ",Count=" << uc << '\n';
          }
        } catch (...) {
        }
      }
#endif

      // 將 Side 轉換為 bs (0=買, 1=賣) 並計算數量變化
      int bs = (p->Side == 1) ? 0 : 1; // Side: 1=買, 2=賣 -> bs: 0=買, 1=賣
      int qty_changed = p->Qty; // 假設是新增數量，實際上可能需要更複雜的邏輯

      // 調用 update_price_list 方法
      product.update_price_list(bs, p->Price, qty_changed);

#ifdef DEBUG
      // 將 Side 轉換為可讀字符串
      const char *side_str = "Unknown";
      if (p->Side == 1) {
        side_str = "Buy";
      } else if (p->Side == 2) {
        side_str = "Sell";
      }

      COUTX << "SysOrderID=" << m_CurrentID << " Update OrderBook: pseq=" << p->pseq
            << " Side=" << side_str << " Price=" << p->Price << " Qty=" << p->Qty << ENDLX;
#endif
    }
  }
  // 忽略複式商品 (type == 2)

  return;
}

/*
 * @brief Update best derived price/qty of product pseq in product book
 * @param p pointer to MTF tag_market_data_t record
 */
void MTF_Buffer::UpdateBestDPrice(tag_market_data_t *p) {
  int bs;
  int price;
  int qty;

  price = p->Price;
  qty = p->Qty;
  /* do some rationality check */
  if (qty < 0) {
    COUTX << "Invalid QTY in TAG_MARKET_DATA : " << p->Qty << ENDLX;
    return;
  } else if (qty == 0) {
    /* if qty=0 force to set price=0 */
    price = 0;
  }
  if (p->Side == 1)
    bs = 0;
  else if (p->Side == 2)
    bs = 1;
  else {
    /* Error */
    COUTX << "Invalid BS code in TAG_MARKET_DATA : " << p->Side << ENDLX;
    return;
  }

  /* update data 到 product book中 - 只處理單式商品 */
  if (m_pProdBook && p->pseq >= 0 && p->pseq < (int)m_pProdBook->m_prod.size()) {
    m_pProdBook->m_prod[p->pseq].m_best_dprice_list[bs].m_price = price;
    m_pProdBook->m_prod[p->pseq].m_best_dprice_list[bs].m_qty = qty;
  }

  return;
}

/**
 * @brief 重置到檔案開頭 (stovol專用)
 *
 * 重新初始化 AtomicIO 物件，從 match_p.dat 檔案開頭開始讀取
 * 適用於需要完整重新解析整個檔案的情況
 */
void MTF_Buffer::ResetToBeginning() {
  CINFOX << "重置 MTF Buffer 到檔案開頭..." << ENDLX;

  // 清理現有的交易記錄
  FreeAllTrans();

  // 重新初始化 AtomicIO 物件
  m_atm.Reset();

  // 重置內部狀態
  m_CurrentID = 0;
  m_LastSysOrderID = 0;
  memset(&m_CurrentTransTime, 0, sizeof(msg_time_t));
  memset(&m_flowtag, 0, sizeof(tag_flow_t));

  CINFOX << "MTF Buffer 已重置到檔案開頭" << ENDLX;
}

/**
 * @brief 完整解析所有交易記錄 (stovol專用)
 *
 * 從檔案開頭開始，解析整個 match_p.dat 檔案中的所有交易記錄
 * 適用於非即時處理，需要完整分析歷史資料的場景
 *
 * @return 1 成功解析完成
 *         0 解析失敗
 *        -1 讀取錯誤
 */
int MTF_Buffer::ParseAllTransactions() {
  char *p; // 指向交易記錄的指標
  int rc;
  int total_count = 0;
  int success_count = 0;
  int flow_count = 0;
  uint32_t first_id = 0;
  uint32_t last_id = 0;

  CINFOX << "開始完整解析 match_p.dat 檔案..." << ENDLX;

  /*
   * 逐一讀取並處理所有交易記錄
   */
  while (1) {
    while ((p = m_atm.ReadTran())) {
      total_count++;
      uint32_t current_id = ((tag_begin_t *)p)->SysOrderID;

      // 記錄第一筆和最後一筆 ID
      if (first_id == 0) {
        first_id = current_id;
      }
      last_id = current_id;

      /* 解析交易記錄 */
      rc = ParseTransaction(p, -1, 0);
      if (rc == 1) {
        // 遇到流程標籤，進行狀態切換
        flow_count++;
        tag_flow_t ftag = GetCurrentFlowTag();
        change_stage_stovol(ftag, (m_pProdBook == &g_fut_prodbook) ? "FUT" : "OPT");

        // 如果檢測到市場開始狀態，依據當前 buffer 類型設定 FUT 或 OPT 旗標
        if (ftag.grp_no == 1 && ftag.TradeStatus == ST_NORMAL) {
          if (m_pProdBook == &g_fut_prodbook && !g_fut_market_started) {
            g_fut_market_started = true;
            g_fut_tprice_header_count = 0;
            g_fut_last_15min_count = 0;
            g_fut_last_1min_count = 0;
            g_fut_is_after_1330 = false;
            g_fut_current_processing_id = current_id;
            CINFOX << "FUT 偵測到市場開始狀態 (PROD_OSW_GRP=1 -> ST_NORMAL), SysOrderId = ["
                   << current_id << "]" << ENDLX;
          } else if (m_pProdBook == &g_opt_prodbook && !g_opt_market_started) {
            g_opt_market_started = true;
            g_opt_tprice_header_count = 0;
            g_opt_last_15min_count = 0;
            g_opt_last_1min_count = 0;
            g_opt_is_after_1330 = false;
            CINFOX << "OPT 偵測到市場開始狀態 (PROD_OSW_GRP=1 -> ST_NORMAL), SysOrderId = ["
                   << current_id << "]" << ENDLX;
          }
        }

        success_count++;
      } else if (rc == 0) {
        success_count++;
      }
      // rc == -1 表示跳過該筆記錄，不計入成功數

      // 每處理 1000 筆記錄顯示一次進度
      if (total_count % 1000 == 0) {
#ifdef DEBUG
        CINFOX << "已處理 " << total_count << " 筆交易記錄, 當前 SysOrderId = [" << current_id
               << "]" << ENDLX;
#endif
      }
    }

    // 嘗試讀取更多資料
    int len = m_atm.Read();
    if (len == 0) {
      // 已經讀取完整個檔案
      CINFOX << "已讀取完整個 match_p.dat 檔案" << ENDLX;
      break;
    } else if (len < 0) {
      CINFOX << "讀取 match_p.dat 檔案時發生錯誤, return code=" << len << ENDLX;
      TFX::Log::COUTMSG(STOVOL_ERROR, "atomic_io Read() failed, return code={}", len);
      return -1;
    }
  }

  // 顯示解析統計
  CINFOX << "完整解析統計：" << ENDLX;
  CINFOX << "  總交易記錄數：" << total_count << " 筆" << ENDLX;
  CINFOX << "  成功處理數：" << success_count << " 筆" << ENDLX;
  CINFOX << "  流程標籤數：" << flow_count << " 個" << ENDLX;
  CINFOX << "  SysOrderId 範圍：[" << first_id << "] ~ [" << last_id << "]" << ENDLX;

  // 更新最後處理的 ID
  m_LastSysOrderID = last_id;

  return 1;
}
