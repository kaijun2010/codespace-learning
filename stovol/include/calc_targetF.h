#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 計算 targetF 規則 (恢復三層 Fa/Fb/Fc):
//  Fa: stovol_Fa_MiddlePrice.log 的 BestMiddlePrice (>0)
//  Fb: 同標的最近月份期貨最佳買賣中價 (FUT 最新 MiddlePrice) (>0)
//  Fc: 現貨(指數)價格 => 以 OPT 端載入的 m_pdk_orig_idx (PDK_ORIG_IDX) 做為靜態現貨價
//  (後續可替換即時 feed) 取價順序: Fa > Fb > Fc > 0 (皆為 0 則 targetF=0, rule=F0) rule 輸出: FA /
//  FB / FC / F0 輸出檔案: stovol_targetF.log (SysOrderID,Time,PDK,Fa,Fb,Fc,targetF,rule) 時序: 於
//  OPT calc point 觸發；需確保 FUT 至少已有一次計算 (Fb)；Fc 為靜態或後續更新。

struct FutPricePair {
  int middle_price = 0;
};
struct FutPricePoint {
  int time_ms;
  int price;
}; // 單一期貨計算點 (當日毫秒 + 價格)

// FUT 最近一次計算點: key=PDK (pdk_kind_id), value= MiddlePrice
extern std::unordered_map<std::string, FutPricePair> g_fut_latest_price_map; // 最新值 (保留兼容)
extern std::unordered_map<std::string, std::vector<FutPricePoint>>
    g_fut_price_history_map; // 歷史序列 (遞增 time_ms)
extern std::vector<std::unordered_map<std::string, int>>
    g_fut_midprice_snapshots;   // 每次 FUT 計算完整快照 (Fb round 對齊)
extern size_t g_fut_calc_round; // 已完成的 FUT 計算次數 (1-based push)
extern size_t g_opt_calc_round; // 已完成的 OPT 計算次數
// 最近一次各 OPT kind 的 targetF (option scale)
extern std::unordered_map<std::string, int> g_opt_targetF_map;
extern std::mutex g_opt_targetF_mtx;
// 新增: (PDK|CONTRACT_DATE) -> targetF 映射，供 optMiddlePrice_list 依合約月份引用
extern std::unordered_map<std::string, int> g_opt_targetF_cd_map; // key = PDK|CONTRACT_DATE
extern std::mutex g_fut_latest_price_mtx;

// Fa: 由 stovol_FA_opt_MiddlePrice 快照更新:
//  1) g_fa_middle_price_map: key=PDK (prefix, 例如 TXO) -> 聚合後的 BestMiddlePrice (保留舊行為)
//  2) g_fa_middle_price_cd_map: key="PDK|CONTRACT_DATE" -> 對應合約月份的 BestMiddlePrice
extern std::unordered_map<std::string, int> g_fa_middle_price_map;
extern std::unordered_map<std::string, int> g_fa_middle_price_cd_map;
extern std::mutex g_fa_middle_price_mtx; // 與 contract_date map 共用同一把鎖

// Fc: 現貨指數 (PDK_ORIG_IDX) 快照 (key=PDK, value=orig_idx)
extern std::unordered_map<std::string, int> g_fc_spot_idx_map;
extern std::mutex g_fc_spot_idx_mtx;

// 計算所有 (PDK, CONTRACT_DATE) 的 targetF (於 OPT calc point) 並追加輸出，每個組合一行。
// Header: SysOrderID,Time,PDK,CONTRACT_DATE,Fa,Fb,Fc,targetF,rule
void compute_and_output_targetF(uint32_t opt_sysorder_id, const std::string &time_str);

// 更新 Fa 單一 (PDK, CONTRACT_DATE) 中價；同時更新 prefix 聚合 (若傳入 contract_date
// 空字串，僅更新聚合)
void update_fa_middle_price(const std::string &pdk, const std::string &contract_date,
                            int middle_price);

// FUT 計算完成時對每個 PDK 呼叫 (middle_price 為 FUT scale 整數, time_str: HH:MM:SS.sss)
void update_fut_snapshot(const std::string &pdk, int middle_price, const std::string &time_str);

// FUT 計算批次開始/結束（由 output_fut_calc_prices 呼叫）
void fut_begin_calc_cycle();
void fut_end_calc_cycle();

// 針對單一 kind (目前僅 DLO) 依 targetF 產生選擇權有效買賣中價序列
void output_opt_middle_price_sequence(uint32_t opt_sysorder_id, const std::string &time_str);

// OPT parity 快照時更新 Fc (orig index / spot)
void update_fc_snapshot(const std::string &pdk, int orig_idx);
