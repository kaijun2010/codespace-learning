/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  match_processor.cpp
 * @brief match_p.dat 處理線程實現
 */

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "LoggerProxy.h"
#include "calc_targetF.h"
#include "commlib/inc/tfxlog.h"
#include "env.h" // for build_log_path
#include "llm.h"
#include "match_processor.h"
#include "mtfbuffer.h"
#include "prodinfo.h"
// 調試用巨集: 若需重新啟用 parity debug/diag log，於編譯參數加入 -DENABLE_STOVOL_PARITY_DEBUG=1
#ifndef ENABLE_STOVOL_PARITY_DEBUG
#define ENABLE_STOVOL_PARITY_DEBUG 0
#endif
// 前向宣告：確保在使用前可見 (某些編譯環境下 header 可能未即時刷新)
void output_fut_calc_prices(uint32_t current_id, const std::string &time_str);

// 時間間隔常數定義 - 基於 tag_tprice_header 計數
const uint64_t TPRICE_15MIN_INTERVAL = 7200; // 15min = 7200 tag_tprice_header
const uint64_t TPRICE_1MIN_INTERVAL = 480;   // 1min = 480 tag_tprice_header

// 外部全域變數引用（在 stovol.cpp 中定義）
extern std::atomic<bool> g_fut_thread_running;
extern std::atomic<bool> g_opt_thread_running;
extern std::atomic<bool> g_fut_market_started; // 補上期貨市場開始旗標 extern 宣告
extern std::atomic<bool> g_opt_market_started;
extern std::atomic<bool> g_all_market_closed;

extern std::atomic<uint64_t> g_fut_tprice_header_count;
extern std::atomic<uint64_t> g_fut_last_15min_count;
extern std::atomic<uint64_t> g_fut_last_1min_count;
extern std::atomic<uint32_t> g_fut_current_processing_id;
extern std::atomic<bool> g_fut_is_after_1330;
// 使用既有 OPT 解析出的收盤與 1 分鐘切換時間 (共用 TPC 資訊)
extern int g_opt_tpc_exec_time;    // 收盤時間 HHMMSS
extern int g_opt_1min_switch_time; // 收盤前30分鐘 HHMMSS (已在其他處 extern 一次,
                                   // 這裡保持一致可視重複移除)

extern std::atomic<uint64_t> g_opt_tprice_header_count;
extern std::atomic<uint64_t> g_opt_last_15min_count;
extern std::atomic<uint64_t> g_opt_last_1min_count;
extern std::atomic<uint32_t> g_opt_current_processing_id;
extern std::atomic<bool> g_opt_is_after_1330;
extern int g_opt_1min_switch_time; // 新增：OPT 收盤前30分鐘切換時間 (HHMMSS)
extern MTF_Buffer *g_fut_pMTFBuffer;
extern MTF_Buffer *g_opt_pMTFBuffer;

// 期貨最近月契約相關全域變數
extern NearestMonthMap g_nearest_month_map;
extern std::vector<FutureProductInfo> g_future_products;
extern ProdInfo g_fut_prodbook;
extern bool g_opt_only; // 由 -o 參數控制，只跑選擇權

// 全域計算時點管理器實例
FutCalcPointManager g_fut_calc_point_mgr;
OptCalcPointManager g_opt_calc_point_mgr; // 新增 OPT 計算時點管理器
uint64_t g_fut_1min_counter = 0;          // 13:30 之後 FUT 1min 計算點序號 (1 起算)
uint64_t g_opt_1min_counter = 0;          // 13:30 之後 OPT 1min 計算點序號 (1 起算)

// 外部函數宣告
extern bool check_market_end_fut(); // 在 mtfbuffer.cpp 中定義
extern bool check_market_end_opt(); // 在 mtfbuffer.cpp 中定義

/**
 * @brief 檢查期貨計算時間點 - 基於 tag_tprice_header 計數
 *
 * 當 PROD_OSW_GRP=1 變為 ST_NORMAL(7) 時開始計算
 * 每個 tag_tprice_header 代表 125ms
 * 15min = 900s = 7200 tag_tprice_header
 * 1min = 60s = 480 tag_tprice_header
 * 13:30 = 285min after 08:45 = 285 * 60 * 8 = 136800 tag_tprice_header
 */
void check_fut_calculation_time(uint32_t current_id, const msg_time_t &calc_time,
                                uint32_t tprice_sysorder_id) {
  // 生產模式：使用真實的時間間隔
  const uint64_t PROD_TPRICE_PER_15MIN = TPRICE_15MIN_INTERVAL; // 15min = 7200 tag_tprice_header
  const uint64_t PROD_TPRICE_PER_1MIN = TPRICE_1MIN_INTERVAL;   // 1min = 480 tag_tprice_header

  // 統一使用 tag_begin.SysOrderID (current_id)，不再使用 tag_tprice_header.SysOrderID
  (void)tprice_sysorder_id; // 避免未使用參數警告

  // 如果市場還沒開始，不處理
  if (!g_fut_market_started) {
    return;
  }

  // 取得當前計數 (已在 mtfbuffer.cpp 中累加)
  uint64_t current_count = g_fut_tprice_header_count.load();

  // 轉換傳入的 calc_time 為可讀格式
  char sTimeBuf[TFX::Time::MAX_OUT_LEN];
  memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
  TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &calc_time, sTimeBuf);

  // 新增：若尚未進入 1 分鐘模式，檢查是否已達收盤前30分鐘時間點 (與 OPT 共用衍生時間)
  if (!g_fut_is_after_1330 && g_opt_1min_switch_time > 0) {
    int cur_hms = 0;
    if (strlen(sTimeBuf) >= 8) {
      int hh = (sTimeBuf[0] - '0') * 10 + (sTimeBuf[1] - '0');
      int mm = (sTimeBuf[3] - '0') * 10 + (sTimeBuf[4] - '0');
      int ss = (sTimeBuf[6] - '0') * 10 + (sTimeBuf[7] - '0');
      cur_hms = hh * 10000 + mm * 100 + ss;
    }
    if (cur_hms >= g_opt_1min_switch_time) {
      g_fut_is_after_1330 = true; // 延用旗標語意：已進入1分鐘階段
      g_fut_1min_counter = 0;
      uint64_t cc = g_fut_tprice_header_count.load();
      if (cc >= TPRICE_1MIN_INTERVAL)
        g_fut_last_1min_count = cc - TPRICE_1MIN_INTERVAL;
      else
        g_fut_last_1min_count = 0;
      COUTX << sTimeBuf
            << " FUT 進入 1 分鐘計算階段 (時間達到收盤前30分鐘切換點=" << g_opt_1min_switch_time
            << "), SysOrderId = [" << current_id << "]" << ENDLX;
    }
  }

  // Before 13:30: every 15min (7200 tag_tprice_header each)
  if (!g_fut_is_after_1330) {
    uint64_t elapsed_since_last_15min = current_count - g_fut_last_15min_count;

    if (elapsed_since_last_15min >= PROD_TPRICE_PER_15MIN) {
      // Determine which 15min period (starting from 0 -> displayed as 1,2,3...)
      uint64_t period_number = current_count / PROD_TPRICE_PER_15MIN;

#ifdef DEBUG
      COUTX << sTimeBuf << " FUT 15min calc point(" << period_number
            << ") (tag_tprice_header count = " << current_count << "), tag_begin.SysOrderId = ["
            << current_id << "]" << ENDLX;
#endif

      // 記錄計算時點到管理器 (使用 tag_begin.SysOrderID)
      std::string time_str(sTimeBuf);
      g_fut_calc_point_mgr.add_calc_point(current_id, time_str, calc_time, current_count, true,
                                          period_number);

      g_fut_last_15min_count = current_count;
      g_fut_current_processing_id = current_id;

      // 波動率計算邏輯
      perform_fut_15min_calculation(current_id, current_count);
    }
  }
  // After 13:30: every 1min
  else {
    uint64_t elapsed_since_last_1min = current_count - g_fut_last_1min_count;

    if (elapsed_since_last_1min >= PROD_TPRICE_PER_1MIN) {
      // Determine which 1min period (after 13:30 restart counting)
      g_fut_1min_counter++;

#ifdef DEBUG
      COUTX << sTimeBuf << " FUT 1min calc point(" << g_fut_1min_counter
            << ") (tag_tprice_header count = " << current_count << "), tag_begin.SysOrderId = ["
            << current_id << "]" << ENDLX;
#endif

      // 記錄計算時點到管理器 (使用 tag_begin.SysOrderID)
      std::string time_str(sTimeBuf);
      g_fut_calc_point_mgr.add_calc_point(current_id, time_str, calc_time, current_count, false,
                                          g_fut_1min_counter);

      g_fut_last_1min_count = current_count;
      g_fut_current_processing_id = current_id;

      // 波動率計算邏輯
      perform_fut_1min_calculation(current_id, current_count);
    }
  }
}

/**
 * @brief 檢查期貨市場狀態變化 (市場開始和收盤前30分鐘切換)
 */
void check_fut_market_status(const tag_flow_t &flow_tag) {
  // 若目前沒有啟動 FUT 線程 (例如僅測試 OPT) 則不輸出 FUT 市場相關日誌
  extern std::atomic<bool> g_fut_thread_running;
  if (!g_fut_thread_running.load()) {
    return;
  }
  // 檢查是否為群組1的狀態變更到 ST_NORMAL (市場開始)
  if (flow_tag.grp_no == 1 && flow_tag.TradeStatus == ST_NORMAL) {
    if (!g_fut_market_started) {
      // Market started (PROD_OSW_GRP=1 -> ST_NORMAL) - counters reset in mtfbuffer.cpp
      g_fut_market_started = true;
      g_fut_last_15min_count = 0;
      g_fut_last_1min_count = 0;
      g_fut_is_after_1330 = false;

      // 轉換 tran_time 為可讀格式
      char sTimeBuf[TFX::Time::MAX_OUT_LEN];
      memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
      TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &flow_tag.tran_time, sTimeBuf);

      COUTX << sTimeBuf << " FUT Market started (PROD_OSW_GRP=1 -> ST_NORMAL), SysOrderId = ["
            << flow_tag.SysOrderID << "]" << ENDLX;

#ifdef DEBUG
      // Output first 15min calc point(0) - immediate after market start
      COUTX << sTimeBuf
            << " FUT 15min calc point(0) (tag_tprice_header count = 0), tag_begin.SysOrderId = ["
            << flow_tag.SysOrderID << "]" << ENDLX;
#endif

      // Record initial calc point into manager
      std::string time_str(sTimeBuf);
      g_fut_calc_point_mgr.add_calc_point(flow_tag.SysOrderID, time_str, flow_tag.tran_time, 0,
                                          true, 0);

      // Execute first 15min calculation
      perform_fut_15min_calculation(flow_tag.SysOrderID, 0);

      g_fut_current_processing_id = flow_tag.SysOrderID;
      return;
    }
  }

  // 檢查是否為群組3的狀態變更到 ST_CLOSE (收盤前30分鐘為切換點)
  // (使用時間門檻 g_opt_1min_switch_time 動態切換)

  // 收盤停止條件：若市場已開始，且當前時間 >= 收盤時間 (或維持原事件判斷兼容)
  if (g_fut_market_started) {
    // 轉換 tran_time 為 HHMMSS
    char sTimeBuf[TFX::Time::MAX_OUT_LEN];
    memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
    TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &flow_tag.tran_time, sTimeBuf);
    int cur_hms = 0;
    if (strlen(sTimeBuf) >= 8) {
      int hh = (sTimeBuf[0] - '0') * 10 + (sTimeBuf[1] - '0');
      int mm = (sTimeBuf[3] - '0') * 10 + (sTimeBuf[4] - '0');
      int ss = (sTimeBuf[6] - '0') * 10 + (sTimeBuf[7] - '0');
      cur_hms = hh * 10000 + mm * 100 + ss;
    }
    bool time_reached_close = (g_opt_tpc_exec_time > 0 && cur_hms >= g_opt_tpc_exec_time);
    bool legacy_event_close = (flow_tag.grp_no == 1 && flow_tag.TradeStatus == ST_CLOSE);
    if ((time_reached_close && g_fut_is_after_1330) ||
        (legacy_event_close && g_fut_is_after_1330)) {
      // 轉換 tran_time 為可讀格式
      // 在停止前檢查 1 分鐘計算點數是否需補最後一筆 (若剛好收盤可能還差一筆)
      size_t one_min_points = g_fut_calc_point_mgr.count_1min_points();
      uint64_t current_count = g_fut_tprice_header_count.load();
      // 估算應該有幾筆：從切換時間到收盤時間 (含起始+收盤) =>  (收盤-切換)/60 + 1
      size_t expected = 0;
      if (g_opt_1min_switch_time > 0 && g_opt_tpc_exec_time > g_opt_1min_switch_time) {
        int sh = g_opt_1min_switch_time / 10000;
        int sm = (g_opt_1min_switch_time / 100) % 100;
        int ss = g_opt_1min_switch_time % 100;
        int ch = g_opt_tpc_exec_time / 10000;
        int cm = (g_opt_tpc_exec_time / 100) % 100;
        int cs = g_opt_tpc_exec_time % 100;
        int start_sec = sh * 3600 + sm * 60 + ss;
        int close_sec = ch * 3600 + cm * 60 + cs;
        int diff = close_sec - start_sec;
        if (diff >= 0)
          expected = (diff / 60) + 1; // 每分鐘一筆
      }
      if (expected > 0 && one_min_points < expected) {
        g_fut_1min_counter = one_min_points;
        g_fut_1min_counter++;
        uint32_t point_number = static_cast<uint32_t>(g_fut_1min_counter);
        std::string time_str(sTimeBuf);
        g_fut_calc_point_mgr.add_calc_point(flow_tag.SysOrderID, time_str, flow_tag.tran_time,
                                            current_count, false, point_number);
        perform_fut_1min_calculation(flow_tag.SysOrderID, current_count);
        COUTX << sTimeBuf << " FUT 收盤前補最後一筆 1min 計算點 (" << point_number
              << "), SysOrderId = [" << flow_tag.SysOrderID << "]" << ENDLX;
      }

      COUTX << sTimeBuf << " FUT 收盤停止計算 (時間達到或事件觸發), SysOrderId = ["
            << flow_tag.SysOrderID << "]" << ENDLX;
      g_fut_market_started = false;
      return;
    }
  }
}

/**
 * @brief 取得產品的最佳買價、最佳賣價和買賣中價
 */
void get_product_best_prices(uint32_t pgseq, const std::string &prod_id, int &best_bid,
                             int &best_ask, int &mid_price) {
  best_bid = 0;
  best_ask = 0;
  mid_price = 0;

  if (!g_fut_pMTFBuffer || !g_fut_pMTFBuffer->m_pProdBook) {
    return;
  }

  // 檢查產品序號是否有效
  if (pgseq >= g_fut_pMTFBuffer->m_pProdBook->m_prod.size()) {
    return;
  }

  const Product &product = g_fut_pMTFBuffer->m_pProdBook->m_prod[pgseq];

  // 取得委託簿最佳買價 (m_price_list[0] 是買方)
  // 注意：買方的key是負數，用來讓價格高的排在前面
  int orderbook_bid = 0;
  if (!product.m_price_list[0].empty()) {
    auto it = product.m_price_list[0].begin(); // 取第一個 (key最小，即價格最高)
    orderbook_bid = it->second.m_price;
  }

  // 取得委託簿最佳賣價 (m_price_list[1] 是賣方)
  // 賣方的key是正數，價格低的排在前面
  int orderbook_ask = 0;
  if (!product.m_price_list[1].empty()) {
    auto it = product.m_price_list[1].begin(); // 取第一個 (價格最低)
    orderbook_ask = it->second.m_price;
  }

  // 取得虛擬最佳一檔價格
  int virtual_bid = product.m_best_dprice_list[0].m_price;
  int virtual_ask = product.m_best_dprice_list[1].m_price;

  // 選擇較好的買價 (較高的買價較好)
  best_bid = std::max(orderbook_bid, virtual_bid);

  // 選擇較好的賣價 (較低的賣價較好，但需要非零)
  if (orderbook_ask > 0 && virtual_ask > 0) {
    best_ask = std::min(orderbook_ask, virtual_ask);
  } else if (orderbook_ask > 0) {
    best_ask = orderbook_ask;
  } else if (virtual_ask > 0) {
    best_ask = virtual_ask;
  }

  // 計算買賣中價
  if (best_bid > 0 && best_ask > 0) {
    mid_price = (best_bid + best_ask) / 2;
  }

#ifdef DEBUG
  CINFOX << "產品 " << prod_id << " (PGSEQ=" << pgseq << ") 價格資訊:" << ENDLX;
  CINFOX << "  委託簿買方筆數=" << product.m_price_list[0].size()
         << ", 賣方筆數=" << product.m_price_list[1].size() << ENDLX;
  CINFOX << "  委託簿買價=" << orderbook_bid << ", 虛擬買價=" << virtual_bid
         << " -> 最佳買價=" << best_bid << ENDLX;
  CINFOX << "  委託簿賣價=" << orderbook_ask << ", 虛擬賣價=" << virtual_ask
         << " -> 最佳賣價=" << best_ask << ENDLX;
  CINFOX << "  買賣中價=" << mid_price << ENDLX;

  // 顯示委託簿詳細內容 (修正版)
  if (!product.m_price_list[0].empty()) {
    CINFOX << "  委託簿買方前5檔:" << ENDLX;
    int count = 0;
    for (auto it = product.m_price_list[0].begin();
         it != product.m_price_list[0].end() && count < 5; ++it, ++count) {
      CINFOX << "    Key=" << it->first << ", 價格=" << it->second.m_price
             << ", 口數=" << it->second.m_qty << ENDLX;
    }
  }
  if (!product.m_price_list[1].empty()) {
    CINFOX << "  委託簿賣方前5檔:" << ENDLX;
    int count = 0;
    for (auto it = product.m_price_list[1].begin();
         it != product.m_price_list[1].end() && count < 5; ++it, ++count) {
      CINFOX << "    Key=" << it->first << ", 價格=" << it->second.m_price
             << ", 口數=" << it->second.m_qty << ENDLX;
    }
  }
#else
  (void)prod_id; // 避免 unused parameter 警告
#endif
}

/**
 * @brief 尋找產品代碼對應的產品序號
 */
uint32_t find_product_pgseq_by_id(const std::string &target_prod_id) {
  if (!g_fut_pMTFBuffer || !g_fut_pMTFBuffer->m_pProdBook) {
    return 0;
  }

  for (size_t i = 1; i < g_fut_pMTFBuffer->m_pProdBook->m_prod.size(); i++) {
    const Product &product = g_fut_pMTFBuffer->m_pProdBook->m_prod[i];
    if (target_prod_id == product.m_prod_id) {
      return i;
    }
  }
  return 0;
}

/*
// 暫時註解掉 - 委託簿處理應該是正確的了
void display_fwfi5_prices(uint32_t current_id) {
    static uint32_t fwfi5_pgseq = 0;

    // 首次呼叫時尋找 FWFI5 的產品序號
    if (fwfi5_pgseq == 0) {
        fwfi5_pgseq = find_product_pgseq_by_id("FWFI5");
        if (fwfi5_pgseq == 0) {
            COUTX << "警告: 找不到 FWFI5 產品" << ENDLX;
            return;
        }
#ifdef DEBUG
        CINFOX << "找到 FWFI5 產品序號: " << fwfi5_pgseq << ENDLX;
#endif
    }

    // 特別針對 SysOrderId=53601 做詳細分析
    if (current_id == 53601) {
        COUTX << "=== 特別分析 SysOrderId=53601 時的 FWFI5 委託簿狀態 ===" << ENDLX;

        if (g_fut_pMTFBuffer && g_fut_pMTFBuffer->m_pProdBook &&
            fwfi5_pgseq < g_fut_pMTFBuffer->m_pProdBook->m_prod.size()) {

            const Product& product = g_fut_pMTFBuffer->m_pProdBook->m_prod[fwfi5_pgseq];

            COUTX << "Product: FWFI5 pgseq=" << fwfi5_pgseq << ENDLX;
            COUTX << "委託簿買方(m_price_list[0])筆數: " << product.m_price_list[0].size() << ENDLX;
            COUTX << "委託簿賣方(m_price_list[1])筆數: " << product.m_price_list[1].size() << ENDLX;

            // 顯示所有買方委託
            if (!product.m_price_list[0].empty()) {
                COUTX << "買方委託詳細資訊:" << ENDLX;
                int count = 0;
                for (auto it = product.m_price_list[0].rbegin(); it !=
product.m_price_list[0].rend(); ++it) { count++; COUTX << "  第" << count << "檔: 價格=" <<
it->second.m_price
                          << ", 口數=" << it->second.m_qty
                          << " (key=" << it->first << ")" << ENDLX;
                }
            } else {
                COUTX << "買方委託簿為空" << ENDLX;
            }

            // 顯示所有賣方委託
            if (!product.m_price_list[1].empty()) {
                COUTX << "賣方委託詳細資訊:" << ENDLX;
                int count = 0;
                for (auto it = product.m_price_list[1].begin(); it != product.m_price_list[1].end();
++it) { count++; COUTX << "  第" << count << "檔: 價格=" << it->second.m_price
                          << ", 口數=" << it->second.m_qty
                          << " (key=" << it->first << ")" << ENDLX;
                }
            } else {
                COUTX << "賣方委託簿為空" << ENDLX;
            }

            // 顯示虛擬最佳一檔
            COUTX << "虛擬最佳一檔 - 買價: " << product.m_best_dprice_list[0].m_price
                  << ", 買量: " << product.m_best_dprice_list[0].m_qty << ENDLX;
            COUTX << "虛擬最佳一檔 - 賣價: " << product.m_best_dprice_list[1].m_price
                  << ", 賣量: " << product.m_best_dprice_list[1].m_qty << ENDLX;
        }
        COUTX << "===================================================" << ENDLX;
    }

    int best_bid, best_ask, mid_price;
    get_product_best_prices(fwfi5_pgseq, "FWFI5", best_bid, best_ask, mid_price);

    // 輸出價格資訊
    COUTX << "SysOrderId=" << current_id << " FWFI5 最佳買價=" << best_bid
          << ", 最佳賣價=" << best_ask << ", 買賣中價=" << mid_price << ENDLX;
}
*/

/**
 * @brief 執行期貨15分鐘計算邏輯
 */
void perform_fut_15min_calculation(uint32_t current_id, uint64_t current_count) {
#ifdef DEBUG
  CINFOX << "開始執行期貨15分鐘波動率計算邏輯 - SysOrderId: " << current_id
         << ", Count: " << current_count << ENDLX;
#else
  (void)current_count; // 避免 unused parameter 警告
#endif

  // 顯示 FWFI5 價格資訊
  // display_fwfi5_prices(current_id);

  // 從計算時點管理器中取得時間字串
  const FutCalcPoint *calc_point = g_fut_calc_point_mgr.find_by_sysorder_id(current_id);
  if (calc_point) {
    // 輸出所有期貨最近月契約價格資訊到檔案
    output_fut_calc_prices(current_id, calc_point->time_str);
  }

#ifdef DEBUG
  CINFOX << "期貨15分鐘波動率計算完成" << ENDLX;
#endif
}

/**
 * @brief 執行期貨1分鐘計算邏輯
 */
void perform_fut_1min_calculation(uint32_t current_id, uint64_t current_count) {
#ifdef DEBUG
  CINFOX << "開始執行期貨1分鐘波動率計算邏輯 - SysOrderId: " << current_id
         << ", Count: " << current_count << ENDLX;
#else
  (void)current_count; // 避免 unused parameter 警告
#endif

  // 顯示 FWFI5 價格資訊
  // display_fwfi5_prices(current_id);

  // 從計算時點管理器中取得時間字串
  const FutCalcPoint *calc_point = g_fut_calc_point_mgr.find_by_sysorder_id(current_id);
  if (calc_point) {
    // 輸出所有期貨最近月契約價格資訊到檔案
    output_fut_calc_prices(current_id, calc_point->time_str);
  }

#ifdef DEBUG
  CINFOX << "期貨1分鐘波動率計算完成" << ENDLX;
#endif
}

/**
 * @brief 期貨 match_p.dat 處理線程
 *
 * 架構說明：
 * - 這個程式不像 ftprice_stf 有即時性需求，所有資料都在 match_p.dat 中
 * - 不需要比對 LLM TXRPT 的 SysOrderID，也不需要 ParseUntilID 同步
 * - 採用一次性完整解析策略：程式啟動時讀取全部資料，並基於交易記錄觸發計算
 * - Thread 停止條件只看期貨商品是否全部收盤
 */
void fut_match_processing_thread() {
  CINFOX << "FUT match processing thread started" << ENDLX;

  if (!g_fut_pMTFBuffer) {
    CINFOX << "Error: FUT MTF Buffer is not initialized" << ENDLX;
    g_fut_thread_running = false;
    return;
  }

  g_fut_thread_running = true;

  try {
    // 一次性讀取整個 match_p.dat 檔案的資料
    uint32_t lastid = g_fut_pMTFBuffer->GetLastSysID();
    CINFOX << "FUT match file's last SysOrderId = [" << lastid << "]" << ENDLX;

    if (lastid > 0) {
      CINFOX << "開始完整解析 FUT match_p.dat 資料 (從 SysOrderId 1 到 " << lastid << ")..."
             << ENDLX;

      // 重置到檔案開頭，完整解析所有資料
      g_fut_pMTFBuffer->ResetToBeginning(); // 需要在 MTF_Buffer 中實現這個方法

      // 解析所有交易記錄，不使用 ParseUntilID，而是逐筆處理
      int result = g_fut_pMTFBuffer->ParseAllTransactions(); // 需要在 MTF_Buffer 中實現這個方法

      if (result == 1) {
        CINFOX << "成功解析完整 FUT match_p.dat 資料，共處理到 SysOrderId " << lastid << ENDLX;

        // 檢查最終市場狀態
        tag_flow_t final_flow = g_fut_pMTFBuffer->GetCurrentFlowTag();
        CINFOX << "最終市場狀態：Group=" << (unsigned int)final_flow.grp_no
               << ", TradeStatus=" << (unsigned int)final_flow.TradeStatus << ENDLX;

        // 如果曾經有過市場開啟狀態 (PROD_OSW_GRP=1 -> ST_NORMAL)，顯示統計資訊
        if (g_fut_market_started) {
          CINFOX << "Market trading statistics:" << ENDLX;
          CINFOX << "  Total tag_tprice_header count: " << g_fut_tprice_header_count << ENDLX;
          CINFOX << "  Last 15min calc point: " << g_fut_last_15min_count << ENDLX;
          CINFOX << "  Last 1min calc point: " << g_fut_last_1min_count << ENDLX;
          CINFOX << "  After 13:30: " << (g_fut_is_after_1330 ? "Yes" : "No") << ENDLX;
        } else {
          CINFOX << "Market open state not detected (PROD_OSW_GRP=1 -> ST_NORMAL)" << ENDLX;
        }

      } else {
        CINFOX << "Failed to parse FUT match_p.dat, result = " << result << ENDLX;
      }
    } else {
      CINFOX << "FUT match_p.dat file empty or no valid data" << ENDLX;
    }

    // 檢查期貨商品是否收盤
    if (check_market_end_fut()) {
      CINFOX << "All FUT products closed" << ENDLX;
    } else {
      CINFOX << "Some FUT products still trading" << ENDLX;
    }

  } catch (const std::exception &e) {
    CINFOX << "FUT match processing thread 發生錯誤: " << e.what() << ENDLX;
  } catch (...) {
    CINFOX << "FUT match processing thread 發生未知錯誤" << ENDLX;
  }

  // 處理完成，設定狀態
  g_fut_thread_running = false;
  g_all_market_closed = true;

  // 列印所有FUT計算時點記錄
  g_fut_calc_point_mgr.print_all_points();

  CINFOX << "FUT match processing thread ended" << ENDLX;
}

/**
 * @brief 選擇權 match_p.dat 處理線程
 */
void opt_match_processing_thread() {
  CINFOX << "OPT match processing thread started" << ENDLX;
  if (!g_opt_pMTFBuffer) {
    CINFOX << "Error: OPT MTF Buffer is not initialized" << ENDLX;
    g_opt_thread_running = false;
    return;
  }
  g_opt_thread_running = true;
  try {
    uint32_t lastid = g_opt_pMTFBuffer->GetLastSysID();
    CINFOX << "OPT match file's last SysOrderId = [" << lastid << "]" << ENDLX;
    if (lastid > 0) {
      CINFOX << "開始完整解析 OPT match_p.dat 資料 (從 SysOrderId 1 到 " << lastid << ")..."
             << ENDLX;
      g_opt_pMTFBuffer->ResetToBeginning();
      int result = g_opt_pMTFBuffer->ParseAllTransactions();
      if (result == 1) {
        CINFOX << "成功解析完整 OPT match_p.dat 資料，共處理到 SysOrderId " << lastid << ENDLX;
        tag_flow_t final_flow = g_opt_pMTFBuffer->GetCurrentFlowTag();
        CINFOX << "最終 OPT 市場狀態：Group=" << (unsigned int)final_flow.grp_no
               << ", TradeStatus=" << (unsigned int)final_flow.TradeStatus << ENDLX;
        if (g_opt_market_started) {
          CINFOX << "OPT market trading statistics:" << ENDLX;
          CINFOX << "  Total tag_tprice_header count: " << g_opt_tprice_header_count << ENDLX;
          CINFOX << "  Last 15min calc point: " << g_opt_last_15min_count << ENDLX;
          CINFOX << "  Last 1min calc point: " << g_opt_last_1min_count << ENDLX;
          CINFOX << "  After 13:30: " << (g_opt_is_after_1330 ? "Yes" : "No") << ENDLX;
        } else {
          CINFOX << "OPT market open state not detected (PROD_OSW_GRP=1 -> ST_NORMAL)" << ENDLX;
        }
      } else {
        CINFOX << "Failed to parse OPT match_p.dat, result = " << result << ENDLX;
      }
    } else {
      CINFOX << "OPT match_p.dat file empty or no valid data" << ENDLX;
    }
    if (check_market_end_opt()) {
      CINFOX << "All OPT products closed" << ENDLX;
    } else {
      CINFOX << "Some OPT products still trading" << ENDLX;
    }
  } catch (const std::exception &e) {
    CINFOX << "OPT match processing thread 發生錯誤: " << e.what() << ENDLX;
  } catch (...) {
    CINFOX << "OPT match processing thread 發生未知錯誤" << ENDLX;
  }
  g_opt_thread_running = false;
  g_opt_calc_point_mgr.print_all_points();
  // Parity 與中價輸出已改為於每個計算時點快照，移除舊單次輸出。
  CINFOX << "OPT match processing thread ended" << ENDLX;
}

// =========================================================================
// 新增: 於每個 OPT 計算時點輸出三個快照檔 (追加模式) 並加上 SysOrderId 欄位
//  1) stovol_pc_parity_list.log      =>
//  SysOrderId,PDK,STRIKE,CONTRACT_DATE,P/C,B,S,PROD_ID,PGSEQ,IDX 2) stovol_pc_parity.log =>
//  SysOrderId,PDK,STRIKE,CONTRACT_DATE,CB,CS,PB,PS,MFUTB,MFUTS,IDX 3) stovol_FA_opt_MiddlePrice.log
//  => SysOrderId,PDK,BestBUY,BestSell,BestMiddlePrice
// =========================================================================
static void snapshot_opt_parity(uint32_t sysorder_id) {
  if (!g_opt_pMTFBuffer || !g_opt_pMTFBuffer->m_pProdBook)
    return;
  const auto *pBook = g_opt_pMTFBuffer->m_pProdBook;
  const auto &prodvec = pBook->m_prod;
  const auto &vol_list = pBook->m_opt_vol_pgseq;

  // === 診斷: 統計委託簿非空/空產品數量 ===
  size_t ob_non_empty = 0, ob_empty = 0;
  for (size_t pgseq = 1; pgseq < prodvec.size(); ++pgseq) {
    const Product &p = prodvec[pgseq];
    if (p.m_pdk_kind_id[0] == '\0')
      continue;
    if (!p.m_price_list[0].empty() || !p.m_price_list[1].empty())
      ++ob_non_empty;
    else
      ++ob_empty;
  }
#if ENABLE_STOVOL_PARITY_DEBUG
  try {
    std::ofstream diag(build_log_path("stovol_pc_parity_diag.log"), std::ios::app);
    if (diag.is_open()) {
      diag << "PARITY_SNAPSHOT,SysOrderId=" << sysorder_id << ",OB_NON_EMPTY=" << ob_non_empty
           << ",OB_EMPTY=" << ob_empty << ",VOL_TARGETS=" << vol_list.size() << '\n';
    }
  } catch (...) {
  }
#endif

  // ------------------------------
  // 建立 STC 前綴集合
  // ------------------------------
  std::unordered_set<std::string> stc_prefix_set;
  stc_prefix_set.reserve(vol_list.size());

  std::string ONLY_PREFIX;
  if (g_only_calc_tx) {
    ONLY_PREFIX = "TXO";
  } else {
    ONLY_PREFIX = "STC";
  }

  for (int pgseq : vol_list) {
    if (pgseq <= 0 || (size_t)pgseq >= prodvec.size())
      continue;
    const Product &p = prodvec[pgseq];
    if (p.m_pdk_kind_id[0] == '\0')
      continue;
    std::string prefix(p.m_pdk_kind_id, p.m_pdk_kind_id + 3);
    if (prefix == ONLY_PREFIX) {
      stc_prefix_set.insert(prefix);
    }
  }
  if (stc_prefix_set.empty())
    return; // 無目標不輸出

  // ------------------------------
  // 1) 產生 parity_list (逐商品資料)
  // ------------------------------
  struct ListRow {
    std::string pdk;
    char pc;
    int strike;
    std::string contract_date;
    int bid;
    int ask;
    std::string prod_id;
    int pgseq;
    std::string settle;
    int orig_idx;
  };
  std::vector<ListRow> list_rows;
  list_rows.reserve(1024);
  // --- 新增: 針對特定 PGSEQ 的委託簿 parity debug (環境變數 STOVOL_DEBUG_PGSEQ) ---
  static int debug_pgseq = -2; // -2: 尚未解析, -1: 不啟用, >0: 目標 pgseq
  static int debug_levels = 5; // 預設抓前 5 檔 (買/賣)
  if (debug_pgseq == -2) {
    const char *e = getenv("STOVOL_DEBUG_PGSEQ");
    debug_pgseq = (e && *e) ? atoi(e) : -1;
    const char *lv = getenv("STOVOL_DEBUG_LEVELS");
    if (lv && *lv) {
      int tmp = atoi(lv);
      if (tmp > 0 && tmp <= 10)
        debug_levels = tmp; // 安全限制最大 10 檔
    }
    // 首次啟用時截斷舊檔，避免混淆 (若存在環境變數並且 >0)
    if (debug_pgseq > 0) {
      try {
        std::ofstream ofs(build_log_path("stovol_parity_ob_debug.log"), std::ios::trunc);
      } catch (...) {
      }
    }
  }
  for (size_t pgseq = 1; pgseq < prodvec.size(); ++pgseq) {
    const Product &p = prodvec[pgseq];
    if (p.m_pdk_kind_id[0] == '\0')
      continue;
    std::string prefix(p.m_pdk_kind_id, p.m_pdk_kind_id + 3);
    if (prefix != ONLY_PREFIX)
      continue;
    if (stc_prefix_set.find(prefix) == stc_prefix_set.end())
      continue;
    ListRow r;
    r.pdk = prefix;
    r.pc = (p.m_pc_code[0] ? p.m_pc_code[0] : '?');
    r.strike = p.m_strike_price;
    r.contract_date = p.m_contract_date;
    r.prod_id = p.m_prod_id;
    r.pgseq = p.m_pgseq;
    r.settle = p.m_settle_date;
    r.bid = 0;
    r.ask = 0;
    r.orig_idx = p.m_pdk_orig_idx;
    // 原始買價 / 賣價 (委託簿)
    if (!p.m_price_list[0].empty())
      r.bid = p.m_price_list[0].begin()->second.m_price;
    if (!p.m_price_list[1].empty())
      r.ask = p.m_price_list[1].begin()->second.m_price;
    // fallback1: 使用虛擬最佳價 (derived best) 若委託簿為空
    if (r.bid == 0 && p.m_best_dprice_list[0].m_price > 0)
      r.bid = p.m_best_dprice_list[0].m_price;
    if (r.ask == 0 && p.m_best_dprice_list[1].m_price > 0)
      r.ask = p.m_best_dprice_list[1].m_price;
    // fallback2: 若仍雙邊為 0 但有最近成交價，將 last trade 作為中性價 (bid/ask 同值)
    if (r.bid == 0 && r.ask == 0 && p.m_last_mprice > 0) {
      r.bid = p.m_last_mprice;
      r.ask = p.m_last_mprice;
    }

    // 當前商品若是指定 debug_pgseq，輸出委託簿前 N 檔與計算後 bid/ask
    if (debug_pgseq > 0 && (int)p.m_pgseq == debug_pgseq) {
      try {
        std::ofstream ob(build_log_path("stovol_parity_ob_debug.log"), std::ios::app);
        if (ob.is_open()) {
          // 解析 PROD_ID 中的 base strike (前三碼後的連續數字)
          int base_strike = 0;
          {
            const char *pid = p.m_prod_id;
            int len = (int)strlen(pid);
            int i = 3; // 跳過 KIND
            while (i < len && isdigit((unsigned char)pid[i])) {
              base_strike = base_strike * 10 + (pid[i] - '0');
              ++i;
            }
          }
          int scale_candidates[5] = {1, 10, 100, 1000, 10000};
          int best_scale = 0;
          long long best_val = 0;
          if (base_strike > 0) {
            for (int sc : scale_candidates) {
              long long cand = (long long)base_strike * sc;
              if (best_scale == 0 || llabs((long long)p.m_strike_price - cand) <
                                         llabs((long long)p.m_strike_price - best_val)) {
                best_val = cand;
                best_scale = sc;
              }
            }
          }
          int strike_mismatch = (base_strike > 0 && (int)best_val != p.m_strike_price) ? 1 : 0;
          ob << "SysOrderId=" << sysorder_id << ",PGSEQ=" << p.m_pgseq << ",PDK=" << prefix
             << ",STRIKE=" << p.m_strike_price << ",TopB=" << p.dump_top_levels(0, debug_levels)
             << ",TopS=" << p.dump_top_levels(1, debug_levels) << ",BestBid=" << r.bid
             << ",BestAsk=" << r.ask << ",LastMPrice=" << p.m_last_mprice
             << ",B_SRC=P,A_SRC=P" // 本段簡化: 因為來源在此段尚未細分虛擬/實際 (主程式後續有
                                   // fallback)
             << ",BASE_STRIKE_PARSED=" << base_strike << ",SCALE_GUESS=" << best_scale
             << ",EXPECTED_SCALED=" << best_val << ",STRIKE_MISMATCH=" << strike_mismatch
             << ",PROD_ID=" << p.m_prod_id << ",ADDR=" << &p << '\n';
        }
      } catch (...) {
      }
    }
    list_rows.push_back(r);
  }
  std::sort(list_rows.begin(), list_rows.end(), [](const ListRow &a, const ListRow &b) {
    if (a.pdk != b.pdk)
      return a.pdk < b.pdk;
    if (a.settle != b.settle)
      return a.settle < b.settle;
    if (a.strike != b.strike)
      return a.strike < b.strike;
    return a.pc < b.pc;
  });

  auto append_with_header = [](const std::string &filename, const std::string &header,
                               const std::function<void(std::ofstream &)> &writer) {
    bool need_header = false;
    bool rewrite = false;
    std::string first_line;
    {
      std::ifstream ifs(filename);
      if (!ifs.good() || ifs.peek() == std::ifstream::traits_type::eof()) {
        need_header = true; // 新檔或空檔
      } else {
        std::getline(ifs, first_line);
        // 正規化：允許舊版使用 SysOrderID (大寫 D) 視為與 SysOrderId 等價
        auto normalize = [](std::string s) {
          size_t pos = s.find("SysOrderID");
          if (pos != std::string::npos)
            s.replace(pos, 10, "SysOrderId");
          return s;
        };
        std::string norm_first = normalize(first_line);
        std::string norm_header = normalize(header);
        if (norm_first != norm_header) {
          // schema 不同才重寫: 例如新增/移除欄位 (parity 新增 OPT.PDK_ORIG_IDX 等)
          rewrite = true;
        }
      }
    }
    if (rewrite) {
      std::ofstream truncofs(filename, std::ios::trunc);
      if (truncofs.is_open()) {
        truncofs << header << '\n';
        writer(truncofs);
      }
      return; // 完成 rewrite
    }
    std::ofstream ofs(filename, std::ios::app);
    if (!ofs.is_open())
      return;
    if (need_header)
      ofs << header << '\n';
    writer(ofs);
  };

  append_with_header(build_log_path("stovol_pc_parity_list.log"),
                     "SysOrderId,PDK,STRIKE,CONTRACT_DATE,P/C,B,S,PROD_ID,PGSEQ,IDX",
                     [&](std::ofstream &ofs) {
                       for (auto &r : list_rows) {
                         ofs << sysorder_id << ',' << r.pdk << ',' << r.strike << ','
                             << r.contract_date << ',' << r.pc << ',' << r.bid << ',' << r.ask
                             << ',' << r.prod_id << ',' << r.pgseq << ',' << r.orig_idx << '\n';
                       }
                     });

// ------------------------------
// (Diagnose) 若 bid/ask 皆為 0，輸出額外診斷資訊
// ------------------------------
#if ENABLE_STOVOL_PARITY_DEBUG
  try {
    std::ofstream dbg(build_log_path("stovol_pc_parity_debug.log"), std::ios::app);
    if (dbg.is_open()) {
      int zero_cnt = 0;
      // prefix -> (total rows, rows with any non-zero bid/ask)
      std::unordered_map<std::string, std::pair<int, int>> prefix_stat;
      for (auto &r : list_rows) {
        auto &ps = prefix_stat[r.pdk];
        ps.first += 1; // total
        if (r.bid > 0 || r.ask > 0)
          ps.second += 1;
      }
      for (auto &r : list_rows) {
        if (r.bid == 0 && r.ask == 0) {
          // 重新搜尋對應 Product 以列出委託簿狀況
          const auto *pBook = g_opt_pMTFBuffer && g_opt_pMTFBuffer->m_pProdBook
                                  ? g_opt_pMTFBuffer->m_pProdBook
                                  : nullptr;
          const Product *pprod = nullptr;
          if (pBook) {
            for (size_t pgseq = 1; pgseq < pBook->m_prod.size(); ++pgseq) {
              const Product &p = pBook->m_prod[pgseq];
              if (p.m_pdk_kind_id[0] == '\0')
                continue;
              // 只比對前 3 碼與履約價 / PC
              if (strncmp(p.m_pdk_kind_id, r.pdk.c_str(), 3) == 0 && p.m_strike_price == r.strike &&
                  (p.m_pc_code[0] == r.pc)) {
                pprod = &p;
                break;
              }
            }
          }
          if (pprod) {
            // 收集前 3 檔 (買/賣) 價格, 以及 last trade 價
            std::vector<int> bids, asks;
            bids.reserve(3);
            asks.reserve(3);
            int take = 0;
            for (auto it = pprod->m_price_list[0].begin();
                 it != pprod->m_price_list[0].end() && take < 3; ++it, ++take)
              bids.push_back(it->second.m_price);
            take = 0;
            for (auto it = pprod->m_price_list[1].begin();
                 it != pprod->m_price_list[1].end() && take < 3; ++it, ++take)
              asks.push_back(it->second.m_price);
            dbg << "SysOrderId=" << sysorder_id << ",ZERO_ROW,PDK=" << r.pdk << r.pc
                << ",STRIKE=" << r.strike << ",Settle=" << r.settle << ",OrigIdx=" << r.orig_idx
                << ",BidSz=" << pprod->m_price_list[0].size()
                << ",AskSz=" << pprod->m_price_list[1].size()
                << ",LastMPrice=" << pprod->m_last_mprice << ",SampleBids=";
            if (bids.empty())
              dbg << '-';
            else {
              for (size_t i = 0; i < bids.size(); ++i) {
                if (i)
                  dbg << '|';
                dbg << bids[i];
              }
            }
            dbg << ",SampleAsks=";
            if (asks.empty())
              dbg << '-';
            else {
              for (size_t i = 0; i < asks.size(); ++i) {
                if (i)
                  dbg << '|';
                dbg << asks[i];
              }
            }
            dbg << '\n';
            ++zero_cnt;
          } else {
            dbg << "SysOrderId=" << sysorder_id << ",ZERO_ROW,PDK=" << r.pdk << r.pc
                << ",STRIKE=" << r.strike << ",Settle=" << r.settle << ",OrigIdx=" << r.orig_idx
                << ",ProductNotFound" << '\n';
            ++zero_cnt;
          }
        }
      }
      if (!prefix_stat.empty()) {
        dbg << "SysOrderId=" << sysorder_id << ",PREFIX_SUMMARY";
        for (auto &kv : prefix_stat) {
          dbg << "," << kv.first << "=(" << kv.second.second << '/' << kv.second.first << ')';
        }
        dbg << '\n';
      }
      if (zero_cnt > 0) {
        dbg << "SysOrderId=" << sysorder_id << ",ZERO_ROW_COUNT=" << zero_cnt << '\n';
      }
    }
  } catch (...) {
    // 忽略診斷例外
  }
#endif

  // ------------------------------
  // 2) 產生 merged + MFUT (同 PDK prefix + strike + CONTRACT_DATE 合併 C/P)
  // ------------------------------
  struct MergedRow {
    std::string pdk;
    int strike;
    std::string contract_date;
    int cb = 0, cs = 0, pb = 0, ps = 0;
    std::string call_prod_id, put_prod_id;
    int call_pgseq = 0, put_pgseq = 0;
    int mfutb = 0, mfuts = 0;
    bool has_mfutb = false, has_mfuts = false;
    int orig_idx = 0;
  };
  struct Key {
    std::string pdk;
    int strike;
    std::string contract_date;
  };
  struct KeyLess {
    bool operator()(Key const &a, Key const &b) const {
      if (a.pdk != b.pdk)
        return a.pdk < b.pdk;
      if (a.contract_date != b.contract_date)
        return a.contract_date < b.contract_date;
      return a.strike < b.strike;
    }
  };
  std::map<Key, MergedRow, KeyLess> merged_map;
  for (size_t pgseq = 1; pgseq < prodvec.size(); ++pgseq) {
    const Product &p = prodvec[pgseq];
    if (p.m_pdk_kind_id[0] == '\0')
      continue;
    std::string prefix(p.m_pdk_kind_id, p.m_pdk_kind_id + 3);
    if (prefix != "TXO")
      continue; // 僅保留 TXO
    if (stc_prefix_set.find(prefix) == stc_prefix_set.end())
      continue;
    std::string full_pdk(
        p.m_pdk_kind_id); // 原完整 PDK (含年月/週別)，合併輸出使用 CONTRACT_DATE 區分
    Key key{prefix, p.m_strike_price, p.m_contract_date};
    MergedRow &row = merged_map[key];
    row.pdk = prefix;
    row.strike = p.m_strike_price;
    row.contract_date = p.m_contract_date;
    char pc = (p.m_pc_code[0] ? p.m_pc_code[0] : '?');
    if (pc == 'C') {
      row.call_prod_id = p.m_prod_id;
      row.call_pgseq = p.m_pgseq;
      if (!p.m_price_list[0].empty())
        row.cb = p.m_price_list[0].begin()->second.m_price;
      if (!p.m_price_list[1].empty())
        row.cs = p.m_price_list[1].begin()->second.m_price;
      if (row.cb == 0 && p.m_best_dprice_list[0].m_price > 0)
        row.cb = p.m_best_dprice_list[0].m_price;
      if (row.cs == 0 && p.m_best_dprice_list[1].m_price > 0)
        row.cs = p.m_best_dprice_list[1].m_price;
      if (row.cb == 0 && row.cs == 0 && p.m_last_mprice > 0) {
        row.cb = p.m_last_mprice;
        row.cs = p.m_last_mprice;
      }
      if (row.orig_idx == 0)
        row.orig_idx = p.m_pdk_orig_idx; // 優先取 Call 的 orig_idx
    } else if (pc == 'P') {
      row.put_prod_id = p.m_prod_id;
      row.put_pgseq = p.m_pgseq;
      if (!p.m_price_list[0].empty())
        row.pb = p.m_price_list[0].begin()->second.m_price;
      if (!p.m_price_list[1].empty())
        row.ps = p.m_price_list[1].begin()->second.m_price;
      if (row.pb == 0 && p.m_best_dprice_list[0].m_price > 0)
        row.pb = p.m_best_dprice_list[0].m_price;
      if (row.ps == 0 && p.m_best_dprice_list[1].m_price > 0)
        row.ps = p.m_best_dprice_list[1].m_price;
      if (row.pb == 0 && row.ps == 0 && p.m_last_mprice > 0) {
        row.pb = p.m_last_mprice;
        row.ps = p.m_last_mprice;
      }
      if (row.orig_idx == 0)
        row.orig_idx = p.m_pdk_orig_idx; // 若尚未設定 (無 Call) 則取 Put
    }
  }
  std::vector<MergedRow> merged_rows;
  merged_rows.reserve(merged_map.size());
  for (auto &kv : merged_map) {
    auto &m = kv.second;
    if (m.cb > 0 && m.ps > 0) {
      m.mfutb = m.strike + m.cb - m.ps;
      m.has_mfutb = true;
    }
    if (m.cs > 0 && m.pb > 0) {
      m.mfuts = m.strike + m.cs - m.pb;
      m.has_mfuts = true;
    }
    merged_rows.push_back(m);
  }
  std::sort(merged_rows.begin(), merged_rows.end(), [](const MergedRow &a, const MergedRow &b) {
    if (a.pdk != b.pdk)
      return a.pdk < b.pdk;
    if (a.contract_date != b.contract_date)
      return a.contract_date < b.contract_date;
    return a.strike < b.strike;
  });

  // 需求調整：移除 PROD_ID 欄位 (合併列無單一 PROD_ID)
  append_with_header(build_log_path("stovol_pc_parity.log"),
                     "SysOrderId,PDK,STRIKE,CONTRACT_DATE,CB,CS,PB,PS,MFUTB,MFUTS,IDX",
                     [&](std::ofstream &ofs) {
                       for (auto &r : merged_rows) {
                         ofs << sysorder_id << ',' << r.pdk << ',' << r.strike << ','
                             << r.contract_date << ',' << r.cb << ',' << r.cs << ',' << r.pb << ','
                             << r.ps << ',' << (r.has_mfutb ? std::to_string(r.mfutb) : "0") << ','
                             << (r.has_mfuts ? std::to_string(r.mfuts) : "0") << ',' << r.orig_idx
                             << '\n';
                       }
                     });

  // ------------------------------
  // 4) Fc (PDK_ORIG_IDX) 快照輸出 (每個完整 PDK 僅一行) + 更新 Fc map
  //     Header: SysOrderID,PDK,Fc
  // ------------------------------
  {
    // 聚合同 PDK 單一 orig_idx (若多筆以第一個為主)
    append_with_header(build_log_path("stovol_FC_opt_idx_MiddlePrice.log"), "SysOrderId,PDK,Fc",
                       [&](std::ofstream &ofs) {
                         for (auto &r : merged_rows) {
                           if (!r.pdk.empty()) {
                             int fc = static_cast<int>(get_latest_fc(r.pdk));
                             ofs << sysorder_id << ',' << r.pdk << ',' << fc << '\n';
                             update_fc_snapshot(r.pdk, fc);
                           }
                         }
                       });
  }

  // ------------------------------
  // 3) MiddlePrice 檔案 (需求修改: 依 (PDK, CONTRACT_DATE) 分組)
  // ------------------------------
  struct MidKey {
    std::string pdk;
    std::string contract_date;
  };
  struct MidKeyHash {
    size_t operator()(MidKey const &k) const noexcept {
      return std::hash<std::string>()(k.pdk) ^ (std::hash<std::string>()(k.contract_date) << 1);
    }
  };
  struct MidKeyEq {
    bool operator()(MidKey const &a, MidKey const &b) const noexcept {
      return a.pdk == b.pdk && a.contract_date == b.contract_date;
    }
  };
  std::unordered_map<MidKey, int, MidKeyHash, MidKeyEq> best_buy_cd;  // max MFUTB
  std::unordered_map<MidKey, int, MidKeyHash, MidKeyEq> best_sell_cd; // min MFUTS
  std::unordered_set<std::string> pdk_seen; // 用於後續 update_fa_middle_price (仍以 PDK 匯總)
  for (auto &r : merged_rows) {
    MidKey key{r.pdk, r.contract_date};
    if (r.has_mfutb) {
      auto it = best_buy_cd.find(key);
      if (it == best_buy_cd.end() || r.mfutb > it->second)
        best_buy_cd[key] = r.mfutb;
    }
    if (r.has_mfuts) {
      auto it2 = best_sell_cd.find(key);
      if (it2 == best_sell_cd.end() || r.mfuts < it2->second)
        best_sell_cd[key] = r.mfuts;
    }
    pdk_seen.insert(r.pdk);
  }
  // 收集排序輸出 (穩定性: 先按 PDK 再 CONTRACT_DATE)
  std::vector<MidKey> mid_keys;
  mid_keys.reserve(best_buy_cd.size() + best_sell_cd.size());
  // 需要所有出現過的 (PDK, CONTRACT_DATE) 組合 (任一邊存在即可)
  std::unordered_set<std::string> key_dedup;
  key_dedup.reserve(best_buy_cd.size() + best_sell_cd.size());
  auto add_key = [&](const MidKey &k) {
    std::string comp = k.pdk + "|" + k.contract_date;
    if (!key_dedup.count(comp)) {
      key_dedup.insert(comp);
      mid_keys.push_back(k);
    }
  };
  for (auto &kv : best_buy_cd)
    add_key(kv.first);
  for (auto &kv : best_sell_cd)
    add_key(kv.first);
  std::sort(mid_keys.begin(), mid_keys.end(), [](const MidKey &a, const MidKey &b) {
    if (a.pdk != b.pdk)
      return a.pdk < b.pdk;
    return a.contract_date < b.contract_date;
  });
  append_with_header(build_log_path("stovol_FA_opt_MiddlePrice.log"),
                     "SysOrderId,PDK,CONTRACT_DATE,BestBUY,BestSell,BestMiddlePrice",
                     [&](std::ofstream &ofs) {
                       for (auto &k : mid_keys) {
                         int buy = best_buy_cd.count(k) ? best_buy_cd[k] : 0;
                         int sell = best_sell_cd.count(k) ? best_sell_cd[k] : 0;
                         int mid = (buy > 0 && sell > 0) ? (buy + sell) / 2 : 0;
                         ofs << sysorder_id << ',' << k.pdk << ',' << k.contract_date << ',' << buy
                             << ',' << sell << ',' << mid << '\n';
                       }
                     });
  // 更新 (PDK, CONTRACT_DATE) 細項 Fa 中價 以及 prefix 聚合 (在 update_fa_middle_price 內處理)
  for (auto &k : mid_keys) {
    int buy = best_buy_cd.count(k) ? best_buy_cd[k] : 0;
    int sell = best_sell_cd.count(k) ? best_sell_cd[k] : 0;
    int mid = (buy > 0 && sell > 0) ? (buy + sell) / 2 : 0;
    update_fa_middle_price(k.pdk, k.contract_date, mid);
  }
  // 若某些 PDK 存在但所有 CONTRACT_DATE 都沒有有效 mid，仍需寫入聚合記錄 (0)
  for (auto &pdk : pdk_seen) {
    // 檢查是否至少有一個 contract_date mid 已更新 (存在於 mid_keys)，若無則補 0
    bool has_entry = false;
    for (auto &k : mid_keys) {
      if (k.pdk == pdk) {
        has_entry = true;
        break;
      }
    }
    if (!has_entry)
      update_fa_middle_price(pdk, "", 0);
  }

  CINFOX << "[SNAPSHOT] OPT parity 快照輸出完成 SysOrderId=" << sysorder_id
         << " list_rows=" << list_rows.size() << " merged_rows=" << merged_rows.size() << ENDLX;
}

// ============================================================================
// FutCalcPointManager 實现
// ============================================================================

/**
 * @brief 新增計算時點記錄
 */
void FutCalcPointManager::add_calc_point(uint32_t sysorder_id, const std::string &time_str,
                                         const msg_time_t &calc_time, uint64_t tprice_count,
                                         bool is_15min_point, uint32_t point_number) {
  // 建立新的計算時點記錄
  FutCalcPoint new_point(sysorder_id, time_str, calc_time, tprice_count, is_15min_point,
                         point_number);

  // 加入到向量中
  calc_points.push_back(new_point);

  // 建立 SysOrderId 到 index 的映射
  sysorder_to_index[sysorder_id] = calc_points.size() - 1;

#ifdef DEBUG
  CINFOX << "新增計算時點記錄: SysOrderId=" << sysorder_id << ", Time=" << time_str
         << ", Type=" << (is_15min_point ? "15分鐘" : "1分鐘") << ", Point#=" << point_number
         << ENDLX;
#endif
}

/**
 * @brief 根據 SysOrderId 查找計算時點
 */
const FutCalcPoint *FutCalcPointManager::find_by_sysorder_id(uint32_t sysorder_id) const {
  auto it = sysorder_to_index.find(sysorder_id);
  if (it != sysorder_to_index.end()) {
    return &calc_points[it->second];
  }
  return nullptr;
}

/**
 * @brief 列印所有計算時點 (格式: SysOrderId | HH:MM:SS.sss)
 */
void FutCalcPointManager::print_all_points() const {
  COUTX << "=== FUT 計算時點記錄 ===" << ENDLX;
  COUTX << "SysOrderId | HH:MM:SS.sss | Type | Point# | Count" << ENDLX;
  COUTX << "--------------------------------------------------------" << ENDLX;

  // 同步寫入檔案 (附加) stovol_calc_time.log
  {
    std::ofstream ofs(build_log_path("stovol_calc_time.log"),
                      std::ios::in | std::ios::out | std::ios::app);
    if (ofs.is_open()) {
      // 如果檔案起始尚未寫入 OCF_DATE，需補上一行
      std::ifstream ifs_chk(build_log_path("stovol_calc_time.log"));
      bool has_ocf = false;
      std::string first_line_chk;
      if (ifs_chk.good()) {
        std::getline(ifs_chk, first_line_chk);
        if (first_line_chk.find("g_ocf_date") != std::string::npos ||
            first_line_chk.find("OCF_DATE") != std::string::npos)
          has_ocf = true;
      }
      if (!has_ocf) {
        extern char g_ocf_date[];       // 定義於 stovol.cpp
        extern int g_opt_tpc_exec_time; // 定義於 stovol.cpp
        ofs << "OCF_DATE=" << g_ocf_date << "\n";
        ofs << "OPT_TPC_EXEC_TIME=" << g_opt_tpc_exec_time << "\n";
      }
      ofs << "=== FUT 計算時點記錄 ===\n";
      ofs << "SysOrderId | HH:MM:SS.sss | Type | Point# | Count\n";
      ofs << "--------------------------------------------------------\n";
      for (const auto &point : calc_points) {
        ofs << std::setw(10) << point.sysorder_id << " | " << point.time_str << " | "
            << std::setw(6) << (point.is_15min_point ? "15分鐘" : "1分鐘") << " | " << std::setw(6)
            << point.point_number << " | " << point.tprice_count << "\n";
      }
      ofs << "總計算時點數: " << calc_points.size() << "\n";
      ofs << "==============================\n";
    }
  }

  for (const auto &point : calc_points) {
    COUTX << std::setw(10) << point.sysorder_id << " | " << point.time_str << " | " << std::setw(6)
          << (point.is_15min_point ? "15分鐘" : "1分鐘") << " | " << std::setw(6)
          << point.point_number << " | " << point.tprice_count << ENDLX;
  }

  COUTX << "總計算時點數: " << calc_points.size() << ENDLX;
  COUTX << "==============================" << ENDLX;
}

/**
 * @brief 輸出簡潔格式 (SysOrderId | HH:MM:SS.sss)
 */
void FutCalcPointManager::print_simple_format() const {
  COUTX << "=== FUT 計算時點簡潔記錄 ===" << ENDLX;
  COUTX << "SysOrderId | HH:MM:SS.sss" << ENDLX;
  COUTX << "-------------------------" << ENDLX;

  for (const auto &point : calc_points) {
    COUTX << std::setw(10) << point.sysorder_id << " | " << point.time_str << ENDLX;
  }

  COUTX << "總計算時點數: " << calc_points.size() << ENDLX;
  COUTX << "===========================" << ENDLX;
}

/**
 * @brief 輸出期貨最近月契約價格資訊到檔案
 * @param current_id 當前 SysOrderID
 * @param time_str 時間字串 "HH:MM:SS.sss"
 */
void output_fut_calc_prices(uint32_t current_id, const std::string &time_str) {
  fut_begin_calc_cycle();
  std::string filename = build_log_path("stovol_FB_fut_MiddlePrice.log");

  // 以附加模式開啟檔案
  std::ofstream outfile(filename, std::ios::app);
  if (!outfile.is_open()) {
    CINFOX << "無法開啟檔案: " << filename << ENDLX;
    return;
  }

  // 如果檔案是空的，寫入標題行
  outfile.seekp(0, std::ios::end);
  if (outfile.tellp() == 0) {
    outfile << "SysOrderId,HH:MM:SS.sss,PDK,PRODID,Buy,Sell,MiddlePrice" << std::endl;
  }

  // 遍歷所有最近月契約
  for (const auto &pair : g_nearest_month_map) {
    const std::string &pdk_kind_id = pair.first;
    int pgseq = pair.second;

    // 檢查 pgseq 是否有效
    if (pgseq <= 0 || pgseq >= (int)g_fut_prodbook.m_prod.size()) {
      continue;
    }

    const Product &product = g_fut_prodbook.m_prod[pgseq];

    // 取得最佳買價、賣價
    int best_bid = 0, best_ask = 0, mid_price = 0;

    // 取得委託簿最佳買價 (m_price_list[0] 是買方)
    if (!product.m_price_list[0].empty()) {
      auto it = product.m_price_list[0].begin();
      best_bid = it->second.m_price;
    }

    // 取得委託簿最佳賣價 (m_price_list[1] 是賣方)
    if (!product.m_price_list[1].empty()) {
      auto it = product.m_price_list[1].begin();
      best_ask = it->second.m_price;
    }

    // 取得虛擬最佳一檔價格
    int virtual_bid = product.m_best_dprice_list[0].m_price;
    int virtual_ask = product.m_best_dprice_list[1].m_price;

    // 選擇較好的買價和賣價
    best_bid = std::max(best_bid, virtual_bid);
    if (best_ask > 0 && virtual_ask > 0) {
      best_ask = std::min(best_ask, virtual_ask);
    } else if (virtual_ask > 0) {
      best_ask = virtual_ask;
    }

    // 計算中價
    if (best_bid > 0 && best_ask > 0) {
      mid_price = (best_bid + best_ask) / 2;
    }

    // 輸出到檔案
    outfile << current_id << "," << time_str << "," << pdk_kind_id << "," << product.m_prod_id
            << "," << best_bid << "," << best_ask << "," << mid_price << std::endl;

    // 更新 FUT 最新快照 (供 targetF Fb 使用)
    update_fut_snapshot(pdk_kind_id, mid_price, time_str);
  }

  outfile.close();
  fut_end_calc_cycle();

#ifdef DEBUG
  CINFOX << "已輸出 " << g_nearest_month_map.size() << " 筆期貨價格資料到檔案: " << filename
         << ENDLX;
#endif
}

/**
 * @brief 新增選擇權計算時點記錄
 */
void OptCalcPointManager::add_calc_point(uint32_t sysorder_id, const std::string &time_str,
                                         const msg_time_t &calc_time, uint64_t tprice_count,
                                         bool is_15min_point, uint32_t point_number) {
  if (!g_opt_only) {
    // 原本同步等待 FUT 計算點的邏輯
    size_t opt_points = calc_points.size();
    size_t required = opt_points + 1; // 本次需對齊的 FUT 次數
    size_t spin_cnt = 0;
    const auto wait_start = std::chrono::steady_clock::now();
    const auto TIMEOUT = std::chrono::seconds(5); // 原為10秒，改為5秒
    while (g_fut_calc_point_mgr.get_point_count() < required) {
      auto now = std::chrono::steady_clock::now();
      if ((spin_cnt % 1000) == 0) {
        CINFOX << "[SYNC] OPT SysOrderId=" << sysorder_id
               << " 等待 FUT 計算點 required=" << required
               << " current_fut=" << g_fut_calc_point_mgr.get_point_count() << ENDLX;
      }
      if (now - wait_start > TIMEOUT) {
        CINFOX << "[SYNC-WARN] OPT SysOrderId=" << sysorder_id
               << " 等待 FUT 超過5秒，強制繼續 (required=" << required
               << ", current_fut=" << g_fut_calc_point_mgr.get_point_count() << ")" << ENDLX;
        break;
      }
      ++spin_cnt;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } else {
    // OPT-only 模式：不等待 FUT，同步回合直接推進
    // 可視需要新增 debug 訊息
    // CINFOX << "[MODE] OPT-only: 跳過等待 FUT 計算點 (SysOrderId=" << sysorder_id << ")" << ENDLX;
  }
  OptCalcPoint new_point(sysorder_id, time_str, calc_time, tprice_count, is_15min_point,
                         point_number);
  calc_points.push_back(new_point);
  // 完成一輪 OPT 計算點紀錄 (當前這筆觸發 compute_and_output_targetF 後會使用 FUT 同輪資料)
  ++g_opt_calc_round; // 與 compute_and_output_targetF required 對齊 (1-based)
  sysorder_to_index[sysorder_id] = calc_points.size() - 1;
  // 於新增計算時點後立即產生三份 parity 快照 (追加模式)
  snapshot_opt_parity(sysorder_id);
  // 產生 targetF (依 Fa/Fb/Fc 規則) 並輸出
  compute_and_output_targetF(sysorder_id, time_str);
  // 產生 CAO opt middle price 序列 (含 Time 欄)
  output_opt_middle_price_sequence(sysorder_id, time_str);
}

/**
 * @brief 根據 SysOrderId 查找選擇權計算時點
 */
const OptCalcPoint *OptCalcPointManager::find_by_sysorder_id(uint32_t sysorder_id) const {
  auto it = sysorder_to_index.find(sysorder_id);
  if (it != sysorder_to_index.end())
    return &calc_points[it->second];
  return nullptr;
}

/**
 * @brief 列印所有選擇權計算時點 (格式: SysOrderId | HH:MM:SS.sss)
 */
void OptCalcPointManager::print_all_points() const {
  COUTX << "=== OPT 計算時點記錄 ===" << ENDLX;
  COUTX << "SysOrderId | HH:MM:SS.sss | Type | Point# | Count" << ENDLX;
  COUTX << "--------------------------------------------------------" << ENDLX;
  // 同步寫入檔案
  {
    std::ofstream ofs(build_log_path("stovol_calc_time.log"),
                      std::ios::in | std::ios::out | std::ios::app);
    if (ofs.is_open()) {
      // FUT 區塊若已寫 OCF_DATE 則不重複；僅在檔案第一行無 OCF_DATE 時補寫
      std::ifstream ifs_chk(build_log_path("stovol_calc_time.log"));
      bool has_ocf = false;
      std::string first_line_chk;
      if (ifs_chk.good()) {
        std::getline(ifs_chk, first_line_chk);
        if (first_line_chk.find("g_ocf_date") != std::string::npos ||
            first_line_chk.find("OCF_DATE") != std::string::npos)
          has_ocf = true;
      }
      if (!has_ocf) {
        extern char g_ocf_date[];       // 定義於 stovol.cpp
        extern int g_opt_tpc_exec_time; // 定義於 stovol.cpp
        ofs << "OCF_DATE=" << g_ocf_date << "\n";
        ofs << "OPT_TPC_EXEC_TIME=" << g_opt_tpc_exec_time << "\n";
      }
      ofs << "=== OPT 計算時點記錄 ===\n";
      ofs << "SysOrderId | HH:MM:SS.sss | Type | Point# | Count\n";
      ofs << "--------------------------------------------------------\n";
      for (const auto &p : calc_points) {
        ofs << std::setw(10) << p.sysorder_id << " | " << p.time_str << " | " << std::setw(6)
            << (p.is_15min_point ? "15分鐘" : "1分鐘") << " | " << std::setw(6) << p.point_number
            << " | " << p.tprice_count << "\n";
      }
      ofs << "總計算時點數: " << calc_points.size() << "\n";
      ofs << "==============================\n";
    }
  }
  for (const auto &p : calc_points) {
    COUTX << std::setw(10) << p.sysorder_id << " | " << p.time_str << " | " << std::setw(6)
          << (p.is_15min_point ? "15分鐘" : "1分鐘") << " | " << std::setw(6) << p.point_number
          << " | " << p.tprice_count << ENDLX;
  }
  COUTX << "總計算時點數: " << calc_points.size() << ENDLX;
  COUTX << "==============================" << ENDLX;
}

/**
 * @brief 輸出簡潔格式 (SysOrderId | HH:MM:SS.sss)
 */
void OptCalcPointManager::print_simple_format() const {
  COUTX << "=== OPT 計算時點簡潔記錄 ===" << ENDLX;
  COUTX << "SysOrderId | HH:MM:SS.sss" << ENDLX;
  COUTX << "-------------------------" << ENDLX;
  for (const auto &p : calc_points) {
    COUTX << std::setw(10) << p.sysorder_id << " | " << p.time_str << ENDLX;
  }
  COUTX << "總計算時點數: " << calc_points.size() << ENDLX;
  COUTX << "===========================" << ENDLX;
}

/**
 * @brief 輸出選擇權計算價格資訊到檔案
 * @param current_id 當前 SysOrderID
 * @param time_str 時間字串 "HH:MM:SS.sss"
 */
void output_opt_calc_prices(uint32_t current_id, const std::string &time_str) {
  (void)current_id;
  (void)time_str;
  // 選擇權價格計算邏輯與期貨不同，本階段不輸出任何檔案。
}

void perform_opt_15min_calculation(uint32_t current_id, uint64_t current_count) {
  (void)current_count;
  (void)current_id;
  // 保留計算時點記錄，但不做價格輸出。
}

void perform_opt_1min_calculation(uint32_t current_id, uint64_t current_count) {
  (void)current_count;
  (void)current_id;
  // 保留計算時點記錄，但不做價格輸出。
}

/**
 * @brief 檢查選擇權計算時間點 - 基於 tag_tprice_header 計數
 *
 * 當 PROD_OSW_GRP=1 變為 ST_NORMAL(7) 時開始計算
 * 每個 tag_tprice_header 代表 125ms
 * 15分鐘 = 900秒 = 7200個 tag_tprice_header
 * 1分鐘 = 60秒 = 480個 tag_tprice_header
 * 13:30 = 8:45後285分鐘 = 285 * 60 * 8 = 136800個 tag_tprice_header
 */
void check_opt_calculation_time(uint32_t current_id, const msg_time_t &calc_time,
                                uint32_t tprice_sysorder_id) {
  (void)tprice_sysorder_id;

  // 如果市場還沒開始，不處理
  if (!g_opt_market_started)
    return;

  // 取得當前計數 (已在 mtfbuffer.cpp 中累加)
  uint64_t current_count = g_opt_tprice_header_count.load();

  // 轉換傳入的 calc_time 為可讀格式
  char sTimeBuf[TFX::Time::MAX_OUT_LEN];
  memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
  TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &calc_time, sTimeBuf);

  // 若尚未切換，檢查是否已達收盤前30分鐘切換點 (以 calc_time 轉 HHMMSS 判斷)
  if (!g_opt_is_after_1330 && g_opt_1min_switch_time > 0) {
    int cur_hms = 0;
    if (strlen(sTimeBuf) >= 8) { // 期望格式 HH:MM:SS.mmm 或 HH:MM:SS
      int hh = (sTimeBuf[0] - '0') * 10 + (sTimeBuf[1] - '0');
      int mm = (sTimeBuf[3] - '0') * 10 + (sTimeBuf[4] - '0');
      int ss = (sTimeBuf[6] - '0') * 10 + (sTimeBuf[7] - '0');
      cur_hms = hh * 10000 + mm * 100 + ss;
    }
    if (cur_hms >= g_opt_1min_switch_time) {
      g_opt_is_after_1330 = true; // 重用原旗標 (語意改為：已進入 1 分鐘階段)
      g_opt_1min_counter = 0;
      // 讓下一次檢查立即觸發第一筆 1 分鐘計算：回退一個 interval
      uint64_t current_count_local = g_opt_tprice_header_count.load();
      if (current_count_local >= TPRICE_1MIN_INTERVAL)
        g_opt_last_1min_count = current_count_local - TPRICE_1MIN_INTERVAL;
      else
        g_opt_last_1min_count = 0;

      COUTX << sTimeBuf
            << " OPT 進入 1 分鐘計算階段 (時間達到收盤前30分鐘切換點=" << g_opt_1min_switch_time
            << "), SysOrderId = [" << current_id << "]" << ENDLX;
    }
  }

  // 切換前：每15分鐘計算一次 (每7200個tag_tprice_header)
  if (!g_opt_is_after_1330) {
    uint64_t elapsed = current_count - g_opt_last_15min_count;

    if (elapsed >= TPRICE_15MIN_INTERVAL) {
      // 計算是第幾個15分鐘週期 (從0開始，所以這裡是1, 2, 3...)
      uint64_t period = current_count / TPRICE_15MIN_INTERVAL;

      // 記錄計算時點到管理器 (使用 tag_begin.SysOrderID)
      std::string time_str(sTimeBuf);
      g_opt_calc_point_mgr.add_calc_point(current_id, time_str, calc_time, current_count, true,
                                          period);

      g_opt_last_15min_count = current_count;

      perform_opt_15min_calculation(current_id, current_count);
    }
  }
  // 切換後：每1分鐘計算一次
  else {
    uint64_t elapsed = current_count - g_opt_last_1min_count;

    if (elapsed >= TPRICE_1MIN_INTERVAL) {
      // 計算是第幾個1分鐘週期 (從13:30後重新開始計算)
      g_opt_1min_counter++;

      // 記錄計算時點到管理器 (使用 tag_begin.SysOrderID)
      std::string time_str(sTimeBuf);
      g_opt_calc_point_mgr.add_calc_point(current_id, time_str, calc_time, current_count, false,
                                          g_opt_1min_counter);

      g_opt_last_1min_count = current_count;

      perform_opt_1min_calculation(current_id, current_count);
    }
  }
}

/**
 * @brief 檢查選擇權市場狀態變化 (市場開始和13:30切換)
 */
void check_opt_market_status(const tag_flow_t &flow_tag) {
  // 檢查是否為群組1的狀態變更到 ST_NORMAL (市場開始)
  if (flow_tag.grp_no == 1 && flow_tag.TradeStatus == ST_NORMAL) {
    if (!g_opt_market_started) {
      // 市場開始 (PROD_OSW_GRP=1 -> ST_NORMAL) - 計數重設在 mtfbuffer.cpp 中處理
      g_opt_market_started = true;
      g_opt_last_15min_count = 0;
      g_opt_last_1min_count = 0;
      g_opt_is_after_1330 = false;

      // 轉換 tran_time 為可讀格式
      char sTimeBuf[TFX::Time::MAX_OUT_LEN];
      memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
      TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &flow_tag.tran_time, sTimeBuf);

      COUTX << sTimeBuf << " OPT Market started (PROD_OSW_GRP=1 -> ST_NORMAL), SysOrderId = ["
            << flow_tag.SysOrderID << "]" << ENDLX;

      // 記錄初始計算時點到管理器
      std::string time_str(sTimeBuf);
      g_opt_calc_point_mgr.add_calc_point(flow_tag.SysOrderID, time_str, flow_tag.tran_time, 0,
                                          true, 0);

      // 執行第一次15分鐘計算
      perform_opt_15min_calculation(flow_tag.SysOrderID, 0);

      return;
    }
  }

  // (舊邏輯移除) 原本依靠 OSW_GRP=3/TradeStatus=3 切換，不再使用。

  // 檢查是否為群組1的狀態變更到 ST_CLOSE (13:45結束計算)
  // OSW_GRP=1, TRADESTATUS=3 表示13:45結束計算
  if (flow_tag.grp_no == 1 && flow_tag.TradeStatus == ST_CLOSE && g_opt_market_started &&
      g_opt_is_after_1330) {
    // 轉換 tran_time 為可讀格式
    char sTimeBuf[TFX::Time::MAX_OUT_LEN];
    memset(&sTimeBuf, 0x00, sizeof(sTimeBuf));
    TFX::Time::Format(TFX::Time::FMT_TIME_MS_1, &flow_tag.tran_time, sTimeBuf);

    COUTX << sTimeBuf << " OPT 已過13:45時間點 (OSW=1, TradeStatus=3), 停止計算, SysOrderId = ["
          << flow_tag.SysOrderID << "]" << ENDLX;

    // 設定停止計算的標誌
    g_opt_market_started = false; // 停止後續計算
    return;
  }
}
