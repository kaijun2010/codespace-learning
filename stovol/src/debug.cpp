/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  debug.cpp
 * @brief Debug functions for stovol
 * @author kaijun@taifex.com.tw
 */

#include "LoggerProxy.h"
#include "commlib/inc/command.hpp"
#include "commlib/inc/tfxlog.h"
#include "prodinfo.h"
#include <iostream>

//--------------------------------------------------------------
// External global variables
//--------------------------------------------------------------
extern ProdInfo g_fut_prodbook; // storage for fut product information
extern ProdInfo g_opt_prodbook; // storage for opt product information

//--------------------------------------------------------------
// Debug functions
//--------------------------------------------------------------

/**
 * @brief (FUT) 印出所有期貨商品的資訊（用於測試和除錯）
 */
void print_fut_prodinfo() {
  // 測試程式：印出所有商品的資訊
  COUTX << "=== 商品資訊測試 ===" << ENDLX;
  COUTX << "總共載入商品數量: " << g_fut_prodbook.m_prod_cnt << ENDLX;
  COUTX << "商品向量大小: " << g_fut_prodbook.m_prod.size() << ENDLX;
  COUTX << ENDLX;

  // 印出表頭
  COUTX << "序號\tPROD_ID\t\tPGSEQ\tSETTLE_DATE\tHIGH_LIMIT\tLOW_LIMIT\tPDK_STOCK_ID" << ENDLX;
  COUTX << "============================================================================" << ENDLX;

  // 遍歷所有商品
  for (size_t i = 0; i < g_fut_prodbook.m_prod.size(); i++) {
    const Product &prod = g_fut_prodbook.m_prod[i];

    // 跳過序號為0的dummy商品
    if (prod.m_pgseq == 0)
      continue;

    COUTX << i << "\t" << prod.m_prod_id << "\t\t" << prod.m_pgseq << "\t" << prod.m_settle_date
          << "\t\t" << prod.m_high_limit << "\t\t" << prod.m_low_limit << "\t\t"
          << prod.m_pdk_stock_id << ENDLX;
  }

  COUTX << "============================================================================" << ENDLX;
  COUTX << "=== 測試完成 ===" << ENDLX;
  COUTX << ENDLX;

  // 另外測試幾個特定商品的詳細資訊
  if (g_fut_prodbook.m_prod.size() > 3) {
    COUTX << "=== 特定商品詳細資訊 ===" << ENDLX;
    const Product &test_prod = g_fut_prodbook.m_prod[3];
    COUTX << "第3個商品詳細資訊:" << ENDLX;
    COUTX << "  PROD_ID: " << test_prod.m_prod_id << ENDLX;
    COUTX << "  PGSEQ: " << test_prod.m_pgseq << ENDLX;
    COUTX << "  PDK_KIND_ID: " << test_prod.m_pdk_kind_id << ENDLX;
    COUTX << "  SETTLE_DATE: " << test_prod.m_settle_date << ENDLX;
    COUTX << "  HIGH_LIMIT: " << test_prod.m_high_limit << ENDLX;
    COUTX << "  LOW_LIMIT: " << test_prod.m_low_limit << ENDLX;
    COUTX << "  PDK_STOCK_ID: " << test_prod.m_pdk_stock_id << ENDLX;
    COUTX << "  SUBTYPE: " << test_prod.m_subtype << ENDLX;
    COUTX << "  UNDERLYING_MARKET: " << (int)test_prod.m_underlying_market << ENDLX;
    COUTX << "=== 詳細資訊完成 ===" << ENDLX;
  }
}

/**
 * @brief (FUT) 印出期貨 PDK 資訊（用於測試和除錯）
 */
void print_fut_pdk_info() {
  COUTX << "=== PDK 資訊測試 ===" << ENDLX;
  COUTX << "PDK Map 大小: " << g_fut_prodbook.m_PutMap.size() << ENDLX;
  COUTX << ENDLX;

  // 印出表頭
  COUTX << "PDK_KIND_ID\tUNDERLYING_ID\tSCALE\tPRICE_SCALE\tSUBTYPE\tUNDERLYING_MARKET" << ENDLX;
  COUTX << "=================================================================" << ENDLX;

  // 遍歷所有PDK
  for (auto it = g_fut_prodbook.m_PutMap.begin(); it != g_fut_prodbook.m_PutMap.end(); ++it) {
    const Pdk *pdk = it->second;
    if (pdk) {
      COUTX << pdk->m_kind_id << "\t\t" << pdk->m_underlying_id << "\t\t" << pdk->m_scale << "\t"
            << pdk->m_price_scale << "\t\t" << pdk->m_subtype << "\t"
            << (int)pdk->m_underlying_market << ENDLX;
    }
  }

  COUTX << "=================================================================" << ENDLX;
  COUTX << "=== PDK 測試完成 ===" << ENDLX;
  COUTX << ENDLX;
}

/**
 * @brief 印出PDK_ORIG_IDX相關資訊（用於測試和除錯）
 */
void print_sfd_info() {
  COUTX << "=== PDK_ORIG_IDX 資訊測試 ===" << ENDLX;

  // 印出表頭
  COUTX << "PROD_ID\t\tPGSEQ\tPDK_STOCK_ID\tPDK_ORIG_IDX" << ENDLX;
  COUTX << "=============================================" << ENDLX;

  // 只印出有IDX資料的商品
  for (size_t i = 0; i < g_fut_prodbook.m_prod.size(); i++) {
    const Product &prod = g_fut_prodbook.m_prod[i];

    // 跳過序號為0的dummy商品，且只印出有PDK_ORIG_IDX資料的
    if (prod.m_pgseq == 0)
      continue;
    if (prod.m_pdk_orig_idx == 0 || prod.m_pdk_orig_idx == INVALID_PRICE)
      continue;

    COUTX << prod.m_prod_id << "\t\t" << prod.m_pgseq << "\t" << prod.m_pdk_stock_id << "\t\t"
          << prod.m_pdk_orig_idx << ENDLX;
  }

  COUTX << "=============================================" << ENDLX;
  COUTX << "=== PDK_ORIG_IDX 測試完成 ===" << ENDLX;
  COUTX << ENDLX;
}

/**
 * @brief 印出商品統計資訊（用於測試和除錯）
 */
void print_product_stats() {
  COUTX << "=== 商品統計資訊 ===" << ENDLX;

  int total_products = 0;
  int products_with_sfd = 0;
  int products_with_stock_id = 0;

  for (size_t i = 0; i < g_fut_prodbook.m_prod.size(); i++) {
    const Product &prod = g_fut_prodbook.m_prod[i];

    if (prod.m_pgseq == 0)
      continue; // 跳過dummy商品

    total_products++;

    if (prod.m_pdk_orig_idx != 0 && prod.m_pdk_orig_idx != INVALID_PRICE) {
      products_with_sfd++;
    }

    if (strlen(prod.m_pdk_stock_id) > 0) {
      products_with_stock_id++;
    }
  }

  COUTX << "總商品數量: " << total_products << ENDLX;
  COUTX << "有PDK_ORIG_IDX資料的商品: " << products_with_sfd << ENDLX;
  COUTX << "有PDK_STOCK_ID的商品: " << products_with_stock_id << ENDLX;
  COUTX << "PDK數量: " << g_fut_prodbook.m_PutMap.size() << ENDLX;
  COUTX << "=== 統計完成 ===" << ENDLX;
  COUTX << ENDLX;
}
