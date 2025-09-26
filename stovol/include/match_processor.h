/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  match_processor.h
 @brief match_p.dat 處理線程標頭檔
 */

#pragma once

#include "commlib/inc/match_dat.h"
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

/**
 * @brief FUT 計算時點記錄結構
 */
struct FutCalcPoint {
  uint32_t sysorder_id;  // tag_tprice_header.SysOrderId
  std::string time_str;  // 時間字串 "HH:MM:SS.sss"
  msg_time_t calc_time;  // 原始時間結構 (用於後續計算)
  uint64_t tprice_count; // tag_tprice_header 計數
  bool is_15min_point;   // true: 15分鐘計算點, false: 1分鐘計算點
  uint32_t point_number; // 計算點編號 (15分鐘: 0,1,2..., 1分鐘: 1,2,3...)

  FutCalcPoint() : sysorder_id(0), tprice_count(0), is_15min_point(true), point_number(0) {
    memset(&calc_time, 0, sizeof(calc_time));
  }

  FutCalcPoint(uint32_t sys_id, const std::string &time, const msg_time_t &msg_time, uint64_t count,
               bool is_15min, uint32_t point_num)
      : sysorder_id(sys_id), time_str(time), calc_time(msg_time), tprice_count(count),
        is_15min_point(is_15min), point_number(point_num) {}
};

/**
 * @brief FUT 計算時點管理類
 */
class FutCalcPointManager {
private:
  std::vector<FutCalcPoint> calc_points;        // 所有計算時點的向量
  std::map<uint32_t, size_t> sysorder_to_index; // SysOrderId 到 index 的映射

public:
  /**
   * @brief 新增計算時點記錄
   */
  void add_calc_point(uint32_t sysorder_id, const std::string &time_str,
                      const msg_time_t &calc_time, uint64_t tprice_count, bool is_15min_point,
                      uint32_t point_number);

  /**
   * @brief 根據 SysOrderId 查找計算時點
   */
  const FutCalcPoint *find_by_sysorder_id(uint32_t sysorder_id) const;

  /**
   * @brief 獲取所有計算時點
   */
  const std::vector<FutCalcPoint> &get_all_points() const { return calc_points; }

  /**
   * @brief 獲取計算時點總數
   */
  size_t get_point_count() const { return calc_points.size(); }

  /**
   * @brief 回傳已記錄的 1 分鐘計算點數量
   */
  size_t count_1min_points() const {
    size_t c = 0;
    for (auto &pt : calc_points)
      if (!pt.is_15min_point)
        ++c;
    return c;
  }

  /**
   * @brief 列印所有計算時點 (格式: SysOrderId | HH:MM:SS.sss)
   */
  void print_all_points() const;

  /**
   * @brief 輸出簡潔格式 (SysOrderId | HH:MM:SS.sss)
   */
  void print_simple_format() const;

  /**
   * @brief 清空所有記錄
   */
  void clear() {
    calc_points.clear();
    sysorder_to_index.clear();
  }
};

// ---------------- OPT -----------------
struct OptCalcPoint {
  uint32_t sysorder_id;  // tag_tprice_header.SysOrderId
  std::string time_str;  // "HH:MM:SS.sss"
  msg_time_t calc_time;  // 原始時間
  uint64_t tprice_count; // tag_tprice_header 計數
  bool is_15min_point;   // true=15分鐘, false=1分鐘
  uint32_t point_number; // 編號

  OptCalcPoint() : sysorder_id(0), tprice_count(0), is_15min_point(true), point_number(0) {
    memset(&calc_time, 0, sizeof(calc_time));
  }
  OptCalcPoint(uint32_t sys_id, const std::string &time, const msg_time_t &msg_time, uint64_t count,
               bool is_15min, uint32_t num)
      : sysorder_id(sys_id), time_str(time), calc_time(msg_time), tprice_count(count),
        is_15min_point(is_15min), point_number(num) {}
};

class OptCalcPointManager {
private:
  std::vector<OptCalcPoint> calc_points;
  std::map<uint32_t, size_t> sysorder_to_index;

public:
  void add_calc_point(uint32_t sysorder_id, const std::string &time_str,
                      const msg_time_t &calc_time, uint64_t tprice_count, bool is_15min_point,
                      uint32_t point_number);
  const OptCalcPoint *find_by_sysorder_id(uint32_t sysorder_id) const;
  void print_all_points() const;
  void print_simple_format() const;
  const std::vector<OptCalcPoint> &get_all_points() const { return calc_points; }
  void clear() {
    calc_points.clear();
    sysorder_to_index.clear();
  }
};

// 全域計算時點管理器
extern FutCalcPointManager g_fut_calc_point_mgr;
extern OptCalcPointManager g_opt_calc_point_mgr;

// 全域 1 分鐘計數器 (FUT / OPT) - 13:30 後遞增，用於輸出 point_number 與 forced calc point 一致性
extern uint64_t g_fut_1min_counter;
extern uint64_t g_opt_1min_counter;

// 時間間隔常數宣告
extern const uint64_t TPRICE_15MIN_INTERVAL; // 15分鐘 = 7200個 tag_tprice_header
extern const uint64_t TPRICE_1MIN_INTERVAL;  // 1分鐘 = 480個 tag_tprice_header

/**
 * @brief 期貨 match_p.dat 處理線程函數
 */
void fut_match_processing_thread();

/**
 * @brief 選擇權 match_p.dat 處理線程函數
 */
void opt_match_processing_thread();

/**
 * @brief 檢查期貨計算時間點
 * @param current_id 當前 SysOrderID
 * @param calc_time tag_tprice_header 中的 calc_time
 * @param tprice_sysorder_id tag_tprice_header 中的 SysOrderID
 */
void check_fut_calculation_time(uint32_t current_id, const msg_time_t &calc_time,
                                uint32_t tprice_sysorder_id);

/**
 * @brief 檢查期貨市場狀態變化 (市場開始和13:30切換)
 * @param flow_tag tag_flow_t 結構
 */
void check_fut_market_status(const tag_flow_t &flow_tag);

/**
 * @brief 執行期貨15分鐘計算邏輯
 * @param current_id 當前 SysOrderID
 * @param current_count 當前 tag_tprice_header 計數
 */
void perform_fut_15min_calculation(uint32_t current_id, uint64_t current_count);

/**
 * @brief 執行期貨1分鐘計算邏輯
 * @param current_id 當前 SysOrderID
 * @param current_count 當前 tag_tprice_header 計數
 */
void perform_fut_1min_calculation(uint32_t current_id, uint64_t current_count);

// OPT prototypes
void check_opt_calculation_time(uint32_t current_id, const msg_time_t &calc_time,
                                uint32_t tprice_sysorder_id);
void check_opt_market_status(const tag_flow_t &flow_tag);
void perform_opt_15min_calculation(uint32_t current_id, uint64_t current_count);
void perform_opt_1min_calculation(uint32_t current_id, uint64_t current_count);
void output_opt_calc_prices(uint32_t current_id, const std::string &time_str);
