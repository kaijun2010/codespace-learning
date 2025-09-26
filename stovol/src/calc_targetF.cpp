#include "calc_targetF.h"
#include <algorithm> // for std::sort
#include <cctype>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

// 明確引用當前模組的 prodinfo.h，避免與其他 opt/inc 下同名檔案衝突
#include "../include/prodinfo.h" // 取得 Product / ProdInfo 定義 (含 m_slt_spread)
#include "BlackScholes.h"        // 隱含波動度計算
#include "env.h"                 // for build_log_path
#include <iomanip>

extern int g_ocf_mocf_day_count; // 交易日分母 (由 OCF 載入)

std::unordered_map<std::string, FutPricePair> g_fut_latest_price_map;
std::unordered_map<std::string, std::vector<FutPricePoint>> g_fut_price_history_map; // 依時間遞增
std::vector<std::unordered_map<std::string, int>> g_fut_midprice_snapshots; // round snapshots
size_t g_fut_calc_round = 0;                                                // FUT 已完成輪次
size_t g_opt_calc_round = 0;                                                // OPT 已完成輪次
std::mutex g_fut_latest_price_mtx;
std::unordered_map<std::string, int> g_fa_middle_price_map;    // prefix 聚合 (舊)
std::unordered_map<std::string, int> g_fa_middle_price_cd_map; // key = PDK|CONTRACT_DATE
std::mutex g_fa_middle_price_mtx;
std::unordered_map<std::string, int> g_fc_spot_idx_map; // Fc (PDK_ORIG_IDX as spot)
std::mutex g_fc_spot_idx_mtx;

namespace {
std::mutex g_targetF_file_mtx;

static void append_with_header(const std::string &filename, const std::string &header,
                               const std::string &line) {
  bool need_header = false;
  bool rewrite = false;
  std::string first_line;
  {
    std::ifstream ifs(filename);
    if (!ifs.good() || ifs.peek() == std::ifstream::traits_type::eof()) {
      need_header = true;
    } else {
      std::getline(ifs, first_line);
      // 舊版 header 僅有 SysOrderID,Time,PDK,targetF,rule ; 新版需要含 Fa,Fb,Fc
      if (first_line.find("SysOrderID") == std::string::npos ||
          first_line.find(",Fa,Fb,Fc,targetF,rule") == std::string::npos) {
        rewrite = true; // schema 變更
      }
    }
  }
  if (rewrite) {
    std::ofstream ofs(filename, std::ios::trunc);
    if (!ofs.is_open())
      return;
    ofs << header << '\n';
    ofs << line << '\n';
    return;
  }
  std::ofstream ofs(filename, std::ios::app);
  if (!ofs.is_open())
    return;
  if (need_header)
    ofs << header << '\n';
  ofs << line << '\n';
}
} // namespace

void update_fa_middle_price(const std::string &pdk, const std::string &contract_date,
                            int middle_price) {
  if (pdk.empty())
    return;
  std::lock_guard<std::mutex> lk(g_fa_middle_price_mtx);
  // 更新 (PDK, CONTRACT_DATE) 明細
  if (!contract_date.empty()) {
    std::string key = pdk + '|' + contract_date;
    g_fa_middle_price_cd_map[key] = middle_price;
  }
  // 若 middle_price > 0，更新聚合 (第一個有價者或覆蓋為最新有效價)；若 0 則不覆蓋已有正值
  if (middle_price > 0) {
    g_fa_middle_price_map[pdk] = middle_price;
  } else if (!contract_date.empty() && !g_fa_middle_price_map.count(pdk)) {
    // 若尚未有聚合值，允許寫入 0 以保證存在 (保持舊行為兼容)
    g_fa_middle_price_map[pdk] = 0;
  }
}

static int parse_time_ms(const std::string &time_str) {
  // 格式 HH:MM:SS.sss -> 轉換成 (HH*3600+MM*60+SS)*1000 + sss
  if (time_str.size() < 12)
    return -1;
  int HH = 0, MM = 0, SS = 0, ms = 0;
  // 粗略解析 (不嚴格檢查字元)
  HH = (time_str[0] - '0') * 10 + (time_str[1] - '0');
  MM = (time_str[3] - '0') * 10 + (time_str[4] - '0');
  SS = (time_str[6] - '0') * 10 + (time_str[7] - '0');
  ms = (time_str[9] - '0') * 100 + (time_str[10] - '0') * 10 + (time_str[11] - '0');
  return ((HH * 3600 + MM * 60 + SS) * 1000) + ms;
}

void update_fut_snapshot(const std::string &pdk, int middle_price, const std::string &time_str) {
  if (pdk.empty())
    return;
  int tms = parse_time_ms(time_str);
  if (tms < 0)
    return; // 無效時間格式
  std::lock_guard<std::mutex> lk(g_fut_latest_price_mtx);
  g_fut_latest_price_map[pdk] = FutPricePair{middle_price};
  std::vector<FutPricePoint> &vec = g_fut_price_history_map[pdk];
  if (!vec.empty() && vec.back().time_ms == tms) {
    vec.back().price = middle_price; // 同一時間點覆蓋
  } else if (vec.empty() || vec.back().time_ms < tms) {
    vec.push_back(FutPricePoint{tms, middle_price});
    // 防止無界成長，可選擇裁剪 (僅保留最近 N 筆) 例如 2000 筆
    if (vec.size() > 4000)
      vec.erase(vec.begin(), vec.begin() + (vec.size() - 3000));
  } else {
    // 逆序時間點 (理論不應發生) - 採插入維持排序
    std::vector<FutPricePoint>::iterator it = vec.end();
    while (it != vec.begin() && (it - 1)->time_ms > tms)
      --it;
    if (it != vec.end() && it != vec.begin() && (it - 1)->time_ms == tms) {
      (it - 1)->price = middle_price;
    } else {
      vec.insert(it, FutPricePoint{tms, middle_price});
    }
  }
}

void update_fc_snapshot(const std::string &pdk, int orig_idx) {
  if (pdk.empty())
    return;
  std::lock_guard<std::mutex> lk(g_fc_spot_idx_mtx);
  g_fc_spot_idx_map[pdk] = orig_idx;
}

// 只輸出屬於 OPT STC 產品 (於 load_prod_opt 中以 PDK_PARAM_KEY 前綴 STC 收集的 m_opt_vol_pgseq)
// STC 產品集合以其前 3 碼 kind id 區分 (與 parity 輸出一致)
extern ProdInfo g_opt_prodbook;                         // 宣告於 stovol.cpp
extern ProdInfo g_fut_prodbook;                         // 期貨商品資訊 (取得 FUT scale 用)
std::unordered_map<std::string, int> g_opt_targetF_map; // 最新 targetF (option scale)
std::mutex g_opt_targetF_mtx;
std::unordered_map<std::string, int> g_opt_targetF_cd_map; // (PDK|CONTRACT_DATE) -> targetF

void compute_and_output_targetF(uint32_t opt_sysorder_id, const std::string &time_str) {
  // 使用當前已累計的 OPT 計算輪次 (g_opt_calc_round 為 1-based 已在 add_calc_point 遞增)
  size_t fut_round_to_use = g_opt_calc_round; // 1-based
  if (fut_round_to_use == 0)
    return; // 尚未有有效輪次
  if (g_fut_calc_round < fut_round_to_use) {
    // 防禦: FUT 輪次不足 (理論不應發生，因前置等待機制)，直接放棄此次輸出
    return;
  }
  // 快照複製 (避免鎖時間過長)
  std::unordered_map<std::string, int> fa_prefix_copy;
  std::unordered_map<std::string, int> fa_cd_copy;
  {
    std::lock_guard<std::mutex> lk(g_fa_middle_price_mtx);
    fa_prefix_copy = g_fa_middle_price_map;
    fa_cd_copy = g_fa_middle_price_cd_map;
  }
  std::unordered_map<std::string, FutPricePair> fut_copy;
  std::unordered_map<std::string, std::vector<FutPricePoint>> fut_hist_copy;
  {
    std::lock_guard<std::mutex> lk(g_fut_latest_price_mtx);
    fut_copy = g_fut_latest_price_map;
    fut_hist_copy = g_fut_price_history_map;
  }
  std::unordered_map<std::string, int> fc_copy;
  {
    std::lock_guard<std::mutex> lk(g_fc_spot_idx_mtx);
    fc_copy = g_fc_spot_idx_map;
  }
  if (fa_prefix_copy.empty() && fa_cd_copy.empty() && fut_copy.empty() && fc_copy.empty())
    return; // 無任何來源資料

  // 建立 STC kind prefix 集合 (前 3 碼)
  std::unordered_set<std::string> stc_prefix;
  stc_prefix.reserve(g_opt_prodbook.m_opt_vol_pgseq.size());
  for (int pgseq : g_opt_prodbook.m_opt_vol_pgseq) {
    if (pgseq <= 0 || pgseq >= (int)g_opt_prodbook.m_prod.size())
      continue;
    const Product &p = g_opt_prodbook.m_prod[pgseq];
    if (p.m_pdk_kind_id[0] == '\0')
      continue;
    stc_prefix.emplace(std::string(p.m_pdk_kind_id, p.m_pdk_kind_id + 3));
  }
  if (stc_prefix.empty())
    return; // 沒有 STC 目標

  // 只輸出 STC Option kind 本身 (第三碼是 'O')，FB 從對應期貨 kind (第三碼改成 'F') 取值
  std::lock_guard<std::mutex> lk(g_targetF_file_mtx);
  // 收集每個 Option kind prefix 對應的所有 CONTRACT_DATE (來自產品表)
  struct CDKey {
    std::string pdk;
    std::string contract_date;
  };
  std::vector<CDKey> cd_keys;
  cd_keys.reserve(g_opt_prodbook.m_prod.size());
  {
    for (size_t i = 1; i < g_opt_prodbook.m_prod.size(); ++i) {
      const Product &p = g_opt_prodbook.m_prod[i];
      if (p.m_pdk_kind_id[0] == '\0')
        continue;
      std::string prefix(p.m_pdk_kind_id, p.m_pdk_kind_id + 3);
      if (stc_prefix.find(prefix) == stc_prefix.end())
        continue;
      if (prefix.size() < 3 || prefix[2] != 'O')
        continue;
      std::string cd = p.m_contract_date;
      if (cd.empty())
        continue; // 若無 CONTRACT_DATE 略過
      cd_keys.push_back(CDKey{prefix, cd});
    }
    // 去重 (以排序 + unique)
    std::sort(cd_keys.begin(), cd_keys.end(), [](const CDKey &a, const CDKey &b) {
      if (a.pdk != b.pdk)
        return a.pdk < b.pdk;
      return a.contract_date < b.contract_date;
    });
    cd_keys.erase(std::unique(cd_keys.begin(), cd_keys.end(),
                              [](const CDKey &a, const CDKey &b) {
                                return a.pdk == b.pdk && a.contract_date == b.contract_date;
                              }),
                  cd_keys.end());
  }
  // 若沒有任何 contract_date，退回原行為僅 prefix 輸出 (兼容)
  if (cd_keys.empty()) {
    cd_keys.reserve(stc_prefix.size());
    for (auto &opt_kind : stc_prefix)
      if (opt_kind.size() >= 3 && opt_kind[2] == 'O')
        cd_keys.push_back(CDKey{opt_kind, ""});
  }

  for (const auto &ck : cd_keys) {
    const std::string &opt_kind = ck.pdk;
    const std::string &contract_date = ck.contract_date; // 可能為空
    // 取得 (PDK, CONTRACT_DATE) 專屬 Fa；若該合約有記錄但為 0，視為沒有有效
    // Fa，不得回退使用其他合約月份的聚合價
    int fa = 0;
    bool has_cd_entry = false;
    if (!contract_date.empty()) {
      std::string key = opt_kind + '|' + contract_date;
      auto itfa = fa_cd_copy.find(key);
      if (itfa != fa_cd_copy.end()) {
        fa = itfa->second;
        has_cd_entry = true;
      }
    }
    // 不再對特定 contract_date 使用 prefix 聚合值；prefix 聚合僅供其它舊模組使用
    int fc = 0;
    {
      auto it = fc_copy.find(opt_kind);
      if (it != fc_copy.end())
        fc = it->second;
    }
    std::string fut_kind = opt_kind.substr(0, 2) + 'F';
    int fb_raw = 0;
    {
      if (fut_round_to_use >= 1 && fut_round_to_use <= g_fut_midprice_snapshots.size()) {
        const auto &snap = g_fut_midprice_snapshots[fut_round_to_use - 1];
        auto itfb = snap.find(fut_kind);
        if (itfb != snap.end())
          fb_raw = itfb->second;
      }
    }
    int fb = fb_raw;
    int targetF = 0;
    const char *rule = "F0";
    if (has_cd_entry && fa > 0) {
      targetF = fa;
      rule = "FA";
    } else if (fb > 0) {
      targetF = fb;
      rule = "FB";
    } else if (fc > 0) {
      targetF = fc;
      rule = "FC";
    }
    std::string line = std::to_string(opt_sysorder_id) + "," + time_str + "," + opt_kind + "," +
                       contract_date + "," + std::to_string(fa) + "," + std::to_string(fb) + "," +
                       std::to_string(fc) + "," + std::to_string(targetF) + "," + rule;
    append_with_header(build_log_path("stovol_targetF.log"),
                       "SysOrderID,Time,PDK,CONTRACT_DATE,Fa,Fb,Fc,targetF,rule", line);
    if (targetF > 0) {
      std::lock_guard<std::mutex> lk(g_opt_targetF_mtx);
      g_opt_targetF_map[opt_kind] = targetF; // prefix 最新
      if (!contract_date.empty()) {
        g_opt_targetF_cd_map[opt_kind + '|' + contract_date] = targetF;
      }
    }
  }
}

// FUT 計算一輪開始: 可選擇預留空間 (目前不需)
void fut_begin_calc_cycle() {
  // no-op
}

// FUT 計算一輪結束: 將最新 g_fut_latest_price_map 拷貝為一個輪次快照
void fut_end_calc_cycle() {
  std::lock_guard<std::mutex> lk(g_fut_latest_price_mtx);
  std::unordered_map<std::string, int> snap;
  snap.reserve(g_fut_latest_price_map.size());
  for (std::unordered_map<std::string, FutPricePair>::const_iterator it =
           g_fut_latest_price_map.begin();
       it != g_fut_latest_price_map.end(); ++it) {
    snap[it->first] = it->second.middle_price;
  }
  g_fut_midprice_snapshots.push_back(snap);
  g_fut_calc_round = g_fut_midprice_snapshots.size();
}

// 產出 optMiddlePrice_list.log，加入 Time 欄位 (第二欄) 格式 HH:MM:SS.sss
void output_opt_middle_price_sequence(uint32_t opt_sysorder_id, const std::string &time_str) {

  std::string focus_param_key;
  if (g_only_calc_tx) {
    focus_param_key = "TXO";
  } else {
    focus_param_key = "STC";
  }

  const char *fname =
      build_log_path("stovol_optMiddlePrice_list.log"); // 既有檔名 (新增 CONTRACT_DATE, EM 欄位)
  bool need_header = false;
  bool rewrite = false;
  std::string first_line;
  {
    std::ifstream ifs(fname);
    if (!ifs.good() || ifs.peek() == std::ifstream::traits_type::eof()) {
      need_header = true;
    } else {
      std::getline(ifs, first_line);
      if (first_line.find("CONTRACT_DATE") == std::string::npos)
        rewrite = true; // header 升級 (加入 CONTRACT_DATE)
    }
  }
  if (rewrite) {
    // 重新寫檔 (舊資料不再保留；如需保留可改成備份)
    std::ofstream ofs(fname, std::ios::trunc);
    if (!ofs.is_open())
      return;
    ofs << "SYSORDER_ID,Time,PDK,CONTRACT_DATE,PROD_ID,PROD_STRIKE_PRICE,PC,B,S,M,EM,targetF"
        << '\n';
    // 不在此 return，仍續往下做一次正常流程 (避免第一筆遺漏)
  }
  std::ofstream ofs(fname, std::ios::app);
  if (!ofs.is_open())
    return;
  if (need_header && !rewrite)
    ofs << "SYSORDER_ID,Time,PDK,CONTRACT_DATE,PROD_ID,PROD_STRIKE_PRICE,PC,B,S,M,EM,targetF"
        << '\n';

  // 方向過濾：
  //  a. 若 strike >= targetF -> 只輸出 CALL (C)
  //  b. 若 strike <  targetF -> 只輸出 PUT  (P)
  // 並依 strike 由小到大排序 (便於閱讀)，同 strike 不分先後。
  struct Row {
    int strike;
    std::string prodid;
    std::string pdk;
    std::string contract_date;
    char cp;
    int bid;
    int ask;
    int mid;
    short pgseq;
    double rate;
    int remaining_days; // for implied vol
  };
  std::vector<Row> rows;
  rows.reserve(64);
  const ProdInfo &P = g_opt_prodbook;
  for (size_t i = 1; i < P.m_prod.size(); ++i) {
    const Product &prod = P.m_prod[i];
    if (strncmp(prod.m_pdk_param_key, focus_param_key.c_str(), 3) != 0)
      continue;
    char cp = prod.m_pc_code[0];
    if (cp != 'C' && cp != 'P')
      continue;
    int strike = prod.m_strike_price;
    std::string contract_date(prod.m_contract_date);
    int bid = 0, ask = 0;
    // 買價應取最高價：m_price_list[0] 假設以價格升序儲存，使用 rbegin()
    if (!prod.m_price_list[0].empty())
      bid = prod.m_price_list[0].rbegin()->second.m_price;
    // derived best 與原始最高價擇高
    bid = std::max(bid, prod.m_best_dprice_list[0].m_price);
    if (!prod.m_price_list[1].empty())
      ask = prod.m_price_list[1].begin()->second.m_price;
    int vask = prod.m_best_dprice_list[1].m_price;
    if (ask > 0 && vask > 0)
      ask = std::min(ask, vask);
    else if (ask == 0 && vask > 0)
      ask = vask;
    // mid 僅在雙邊都有價時計算，否則為 0 (避免 200,0,200 這類情形誤導)
    int mid;
    if (bid > 0 && ask > 0)
      mid = (bid + ask) / 2;
    else
      mid = 0;
    rows.push_back(Row{strike, prod.m_prod_id,
                       std::string(prod.m_pdk_kind_id, prod.m_pdk_kind_id + 3), contract_date, cp,
                       (bid > 0 ? bid : 0), (ask > 0 ? ask : 0), mid, prod.m_pgseq,
                       prod.m_p03_compound, prod.m_remaining_days});
  }
  if (rows.empty()) {
    if (g_only_calc_tx) {
      ofs << opt_sysorder_id << ',' << time_str << ',' << focus_param_key
          << ",,TXO-NOQUOTE,0,.,0,0,0,0,0" << '\n';
    } else {
      ofs << opt_sysorder_id << ',' << time_str << ',' << focus_param_key
          << ",,STC-NOQUOTE,0,.,0,0,0,0,0" << '\n';
    }
    return;
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row &a, const Row &b) { return a.strike < b.strike; });
  // 同步建立/附加 stovol_IMPVOL_list.log (僅含 EM>0 列)
  const char *em_fname = build_log_path("stovol_IMPVOL_list.log");
  bool need_em_header = false;
  {
    std::ifstream eifs(em_fname);
    if (!eifs.good() || eifs.peek() == std::ifstream::traits_type::eof())
      need_em_header = true;
  }
  std::ofstream emofs(em_fname, std::ios::app);
  if (emofs.is_open() && need_em_header)
    emofs << "SysOrderID,Time,PDK,CONTRACT_DATE,PROD_ID,PSEQ,PC,EMP,ImplVol" << '\n';

  // 新增: THEVOL 檔案改寫邏輯
  // 需求: 以 (PDK, CONTRACT_DATE) + STRIKE 排序，並補齊缺失的履約價 (ImplVol,TheVol=0)
  // 策略:
  //  1. 先暫存本次可計算出 IV 的資料 (sigma) 於 key=(PDK|CONTRACT_DATE) -> map<int,strikeSigma>
  //  2. 計算每個 key 的最小正向 strike 差值 min_step (若無法計算, 使用 100000 作為預設步長)
  //  3. 依 minStrike..maxStrike 以 step 遞增補齊，缺口輸出 0
  //  4. SysOrderID,Time 與同批計算保持一致 (使用函式參數 opt_sysorder_id/time_str)
  //  5. 僅在整批處理完 rows 後一次性寫入檔案 (減少 I/O 雜訊，確保排序)
  struct TheVolEntry {
    double implvol; // 已計算出的隱含波動度 (sigma)
  };
  // 暫存: key -> ordered strikes (使用 std::map 以利後續推導步長和遍歷)
  std::unordered_map<std::string, std::map<int, TheVolEntry>>
      thevol_bucket; // key = PDK|CONTRACT_DATE
  thevol_bucket.reserve(32);

  // 新增: 準備 filter 檔案 (被 S-B > m_slt_spread 排除者)
  const char *filter_fname = build_log_path("stovol_optMiddlePrice_filter.log");
  bool need_filter_header = false;
  {
    std::ifstream fifs(filter_fname);
    if (!fifs.good() || fifs.peek() == std::ifstream::traits_type::eof())
      need_filter_header = true;
  }
  std::ofstream fofs(filter_fname, std::ios::app);
  if (fofs.is_open() && need_filter_header) {
    fofs << "SYSORDER_ID,Time,PDK,CONTRACT_DATE,PROD_ID,PROD_STRIKE_PRICE,PC,B,S,M,EM,SLT,targetF"
         << '\n';
  }

  for (size_t i = 0; i < rows.size(); ++i) {
    const Row &r = rows[i];
    // 取得商品 slt_spread: 透過 pgseq 反查 product (若合法)。若無資料或 <=0 則當作無限制。
    double slt_spread = 0.0;
    if (r.pgseq > 0 && r.pgseq < (int)g_opt_prodbook.m_prod.size()) {
      slt_spread =
          g_opt_prodbook.m_prod[r.pgseq].m_slt_spread; // 已為 option scale 的價差 (整數價位)
    }
    int spread = 0;
    if (r.bid > 0 && r.ask > 0)
      spread = r.ask - r.bid;
    bool spread_ok = true;
    if (spread > 0 && slt_spread > 0.0) {
      // slt_spread 來源以 double 存, 先轉成整數比較 (已以 scale 放大後整數存入 double)
      int allowed = (int)(slt_spread + 0.5);
      spread_ok = (spread <= allowed);
    }
    int em = 0;
    if (r.bid > 0 && r.ask > 0 && spread_ok) {
      em = r.mid; // 中價有效
    }

    // 依 (PDK, CONTRACT_DATE) 找對應 targetF (計算一次供後續使用)
    int row_targetF = 0;
    {
      std::lock_guard<std::mutex> lk(g_opt_targetF_mtx);
      if (!r.contract_date.empty()) {
        auto it = g_opt_targetF_cd_map.find(r.pdk + '|' + r.contract_date);
        if (it != g_opt_targetF_cd_map.end())
          row_targetF = it->second;
      }
      if (row_targetF <= 0) {
        auto itp = g_opt_targetF_map.find(r.pdk);
        if (itp != g_opt_targetF_map.end())
          row_targetF = itp->second; // fallback (理論上已細化)
      }
    }
    // 方向過濾改為：若 row_targetF>0 則 strike >= targetF 只輸出 C，strike < targetF 只輸出 P
    if (row_targetF > 0) {
      bool need_call = (r.strike >= row_targetF);
      bool need_put = (r.strike < row_targetF);
      if ((need_call && r.cp != 'C') || (need_put && r.cp != 'P'))
        continue; // 跳過不符合方向的列
    }
    ofs << opt_sysorder_id << ',' << time_str << ',' << r.pdk << ',' << r.contract_date << ','
        << r.prodid << ',' << r.strike << ',' << r.cp << ',' << r.bid << ',' << r.ask << ','
        << r.mid << ',' << em << ',' << row_targetF << '\n';

    // 被過濾 (spread 不符) 或 單邊缺價 (但題意僅要求 spread filter) -> 只對 spread 不符寫 filter 檔
    if ((r.bid > 0 && r.ask > 0) && !spread_ok && fofs.is_open()) {
      int allowed = (int)(slt_spread + 0.5);
      // 重新取得 row_targetF 與上方一致 (可考慮重構, 目前為簡化保持)
      int row_targetF2 = 0;
      {
        std::lock_guard<std::mutex> lk(g_opt_targetF_mtx);
        if (!r.contract_date.empty()) {
          auto it = g_opt_targetF_cd_map.find(r.pdk + '|' + r.contract_date);
          if (it != g_opt_targetF_cd_map.end())
            row_targetF2 = it->second;
        }
        if (row_targetF2 <= 0) {
          auto itp = g_opt_targetF_map.find(r.pdk);
          if (itp != g_opt_targetF_map.end())
            row_targetF2 = itp->second;
        }
      }
      fofs << opt_sysorder_id << ',' << time_str << ',' << r.pdk << ',' << r.contract_date << ','
           << r.prodid << ',' << r.strike << ',' << r.cp << ',' << r.bid << ',' << r.ask << ','
           << r.mid << ',' << 0 << ',' << allowed << ',' << row_targetF2 << '\n';
    }

    if (em > 0 && emofs.is_open()) {
      // 以行對應 targetF 計算隱含波動度 (若 0 則稍後 sigma 保持 0)
      int row_targetF_iv = 0;
      {
        std::lock_guard<std::mutex> lk(g_opt_targetF_mtx);
        if (!r.contract_date.empty()) {
          auto it = g_opt_targetF_cd_map.find(r.pdk + '|' + r.contract_date);
          if (it != g_opt_targetF_cd_map.end())
            row_targetF_iv = it->second;
        }
        if (row_targetF_iv <= 0) {
          auto itp = g_opt_targetF_map.find(r.pdk);
          if (itp != g_opt_targetF_map.end())
            row_targetF_iv = itp->second;
        }
      }
      double S = static_cast<double>(row_targetF_iv);
      double K = static_cast<double>(r.strike);
      double R = r.rate; // 已為年化利率值 (假設資料庫即為此)
      double T = 0.0;
      if (g_ocf_mocf_day_count > 0 && r.remaining_days > 0) {
        T = static_cast<double>(r.remaining_days) / static_cast<double>(g_ocf_mocf_day_count);
      }
      double P = static_cast<double>(em);
      double sigma = 0.0;
      if (S > 0 && K > 0 && P > 0 && T > 0 && R >= 0.0) {
        // 根據買賣權別呼叫對應函數
        if (r.cp == 'P')
          sigma = NTPutIV(S, K, R, T, P, nullptr);
        else if (r.cp == 'C')
          sigma = NTCallIV(S, K, R, T, P, nullptr);
        // 合理性過濾: 若回傳 NaN/inf 或異常數值, 設為 0
        if (!(sigma >= 0.0 && sigma < 5.0))
          sigma = 0.0; // 波動度通常 <500%
      }
      // 更新 Product.m_impvol (若 pgseq 合法)
      if (r.pgseq > 0 && r.pgseq < (int)g_opt_prodbook.m_prod.size()) {
        g_opt_prodbook.m_prod[r.pgseq].m_impvol = sigma;
      }
      emofs << opt_sysorder_id << ',' << time_str << ',' << r.pdk << ',' << r.contract_date << ','
            << r.prodid << ',' << r.pgseq << ',' << r.cp << ',' << em << ',' << std::fixed
            << std::setprecision(6) << sigma << '\n';
      if (sigma > 0.0) {
        // 暫存本筆 (僅保存有 IV 者; 補齊時再填 0)
        std::string key = r.pdk + '|' + r.contract_date;
        thevol_bucket[key][r.strike] = TheVolEntry{sigma};
      }
    }
  }

  // ====== THEVOL 檔案輸出階段 ======
  {
    const char *thevol_fname = build_log_path("stovol_THEVOL_list.log");
    bool need_thevol_header = false;
    {
      std::ifstream tvifs(thevol_fname);
      if (!tvifs.good() || tvifs.peek() == std::ifstream::traits_type::eof())
        need_thevol_header = true;
    }
    std::ofstream thevofs(thevol_fname, std::ios::app);
    if (!thevofs.is_open()) {
      return; // 無法寫 THEVOL 就結束 (不影響其它檔案)
    }
    if (need_thevol_header) {
      thevofs << "SysOrderID,Time,PDK,CONTRACT_DATE,STRIKE,ImplVol,TheVol" << '\n';
    }
    // 為後續 AdjTheVol 線性插補，同步收集每個 key 的 (strike -> ImplVol, TheVol)
    struct StrikeVolPair {
      double impl;
      double theo;
    };
    std::unordered_map<std::string, std::map<int, StrikeVolPair>> adj_bucket; // key -> ordered map
    adj_bucket.reserve(64);
    // 為了輸出『所有』履約價(即使 ImplVol=0)，需先建立每個 key 的完整 strike 清單。
    // 收集本批處理涉及的 (PDK|CONTRACT_DATE) 以及與 focus kind 對應產品中的所有 strike。
    std::unordered_map<std::string, std::vector<int>> all_strikes_map; // key -> strikes
    all_strikes_map.reserve(64);
    for (const auto &prod : g_opt_prodbook.m_prod) {
      if (prod.m_prod_id[0] == '\0')
        continue;
      if (strncmp(prod.m_pdk_param_key, focus_param_key.c_str(), 3) != 0)
        continue;
      std::string key =
          std::string(prod.m_pdk_kind_id, prod.m_pdk_kind_id + 3) + '|' + prod.m_contract_date;
      if (prod.m_contract_date[0] == '\0')
        continue; // 沒有 CONTRACT_DATE 不納入 (與前段一致)
      if (prod.m_strike_price <= 0)
        continue;
      all_strikes_map[key].push_back(prod.m_strike_price);
    }
    // also include keys from thevol_bucket (防止可能存在有 implvol
    // 但產品集合尚未收集完全的極端情況)
    for (auto &kv : thevol_bucket) {
      if (!all_strikes_map.count(kv.first)) {
        // 若沒在產品中找到, 使用已有 implvol strikes
        for (auto &p : kv.second)
          all_strikes_map[kv.first].push_back(p.first);
      }
    }
    // 逐 key 輸出
    for (auto &kvAll : all_strikes_map) {
      const std::string &key = kvAll.first;
      size_t pos = key.find('|');
      std::string pdk = (pos == std::string::npos ? key : key.substr(0, pos));
      std::string cdate = (pos == std::string::npos ? std::string() : key.substr(pos + 1));
      std::vector<int> &strikes = kvAll.second;
      if (strikes.empty())
        continue;
      std::sort(strikes.begin(), strikes.end());
      strikes.erase(std::unique(strikes.begin(), strikes.end()), strikes.end());

      // 建立 implvol 查詢表 (可能無此 key -> 空)
      auto itBucket = thevol_bucket.find(key);
      std::unordered_map<int, double> impl_map; // strike -> implvol
      if (itBucket != thevol_bucket.end()) {
        impl_map.reserve(itBucket->second.size());
        for (auto &p : itBucket->second)
          impl_map[p.first] = p.second.implvol;
      }

      // 準備 Product 映射 (strike->Product*) 取得 gap_max / exempt_days / remaining_days
      std::unordered_map<int, const Product *> prod_map; // strike -> product
      prod_map.reserve(strikes.size());
      for (const auto &prod : g_opt_prodbook.m_prod) {
        if (prod.m_prod_id[0] == '\0')
          continue;
        if (strncmp(prod.m_pdk_kind_id, pdk.c_str(), 3) != 0)
          continue;
        if (cdate.size() && strncmp(prod.m_contract_date, cdate.c_str(), cdate.size()) != 0)
          continue;
        if (prod.m_strike_price <= 0)
          continue;
        prod_map[prod.m_strike_price] = &prod;
      }

      // 尋找 baseline:
      //  1. 以『正的最小 ImplVol』為候選集合
      //  2. 若候選多於 1 且存在 targetF，挑選 |K-targetF| 最小者
      //  3. 若距離仍相同 (對稱於 targetF)，依規則: Call 偏好較大履約價；Put 偏好較小履約價。
      //     目前 THEVOL 演算法僅允許單一 baseline，故在最終 tie 時採『較大履約價』，
      //     (若後續需對 C/P 分別 baseline，可再擴充)。
      int baseline_strike = -1;
      double baseline_impl = 0.0;
      // 收集所有 (最小正 impl) 候選
      double min_impl_val = 0.0;
      std::vector<int> min_impl_strikes;
      min_impl_strikes.reserve(strikes.size());
      for (int K : strikes) {
        auto itv = impl_map.find(K);
        if (itv == impl_map.end())
          continue;
        double v = itv->second;
        if (v <= 0.0)
          continue;
        if (min_impl_strikes.empty() || v + 1e-12 < min_impl_val) { // 新的更小值
          min_impl_val = v;
          min_impl_strikes.clear();
          min_impl_strikes.push_back(K);
        } else if (std::fabs(v - min_impl_val) <= 1e-12) {
          min_impl_strikes.push_back(K);
        }
      }

      // 取得 (PDK, CONTRACT_DATE) 對應 targetF (option scale)
      int key_targetF = 0;
      {
        std::lock_guard<std::mutex> lk(g_opt_targetF_mtx);
        if (!cdate.empty()) {
          auto ittf = g_opt_targetF_cd_map.find(pdk + '|' + cdate);
          if (ittf != g_opt_targetF_cd_map.end())
            key_targetF = ittf->second;
        }
        if (key_targetF <= 0) {
          auto ittf2 = g_opt_targetF_map.find(pdk);
          if (ittf2 != g_opt_targetF_map.end())
            key_targetF = ittf2->second;
        }
      }

      if (min_impl_strikes.empty()) {
        // 沒有任何正的 implvol -> 舊行為: 使用第一個 strike 作為 baseline，impl=0
        baseline_strike = strikes.front();
        baseline_impl = 0.0;
      } else if (min_impl_strikes.size() == 1 || key_targetF <= 0) {
        // 只有一個候選 或 沒 targetF 可參考 -> 任取 (第一個) 即原本最小 impl 邏輯
        baseline_strike = min_impl_strikes.front();
        baseline_impl = min_impl_val;
      } else {
        // 多個候選且有 targetF -> 先依距離過濾
        int bestK = min_impl_strikes.front();
        int bestDist = std::abs(bestK - key_targetF);
        for (size_t i = 1; i < min_impl_strikes.size(); ++i) {
          int K = min_impl_strikes[i];
          int d = std::abs(K - key_targetF);
          if (d < bestDist) {
            bestK = K;
            bestDist = d;
          } else if (d == bestDist) {
            // 距離相同 -> tie-break: 選較大 strike (Call 偏好)；
            // 若未來需針對 Put 方向選較小，可在此再加參數控制。
            if (K > bestK)
              bestK = K;
          }
        }
        baseline_strike = bestK;
        baseline_impl = min_impl_val;
      }

      // 取得群組 gap_max / exempt_days (用 baseline 或第一筆產品)
      double group_gap_max = 0.0;
      double group_exempt_days = 0.0;
      {
        const Product *bp = nullptr;
        auto pit = prod_map.find(baseline_strike);
        if (pit != prod_map.end())
          bp = pit->second;
        else if (!prod_map.empty())
          bp = prod_map.begin()->second;
        if (bp) {
          group_gap_max = bp->m_gap_max;
          group_exempt_days = bp->m_dspar_exempt_days;
        }
      }

      // THEVOL 計算結果
      std::unordered_map<int, double> thevol_map;
      thevol_map.reserve(strikes.size());
      thevol_map[baseline_strike] = baseline_impl; // 基準點直接採用 impl

      auto get_meta = [&prod_map](int strike, int &remain_days, double &gap_max, double &exempt) {
        auto it = prod_map.find(strike);
        if (it != prod_map.end()) {
          remain_days = it->second->m_remaining_days;
          gap_max = it->second->m_gap_max;
          exempt = it->second->m_dspar_exempt_days;
        } else {
          remain_days = 0;
        }
      };

      // 向右 (遞增 strike) 掃描
      double prev_impl_accepted = baseline_impl;
      int gap_missing = 0; // 連續空資料數
      for (size_t idx = 0; idx < strikes.size(); ++idx) {
        int K = strikes[idx];
        if (K == baseline_strike) {
          // baseline 已處理
          continue;
        }
        if (K < baseline_strike) // 先略過左側, 右側稍後處理
          continue;
        double impl = 0.0;
        auto itv = impl_map.find(K);
        if (itv != impl_map.end())
          impl = itv->second;
        int remain_days = 0;
        double gap_max_local = group_gap_max;
        double exempt_local = group_exempt_days;
        get_meta(K, remain_days, gap_max_local, exempt_local);
        if (gap_max_local <= 0.0)
          gap_max_local = group_gap_max;
        bool exempt_check =
            (remain_days > 0 &&
             remain_days < (int)(exempt_local > 0.0 ? exempt_local : group_exempt_days));

        if (impl <= 0.0) { // 空資料
          gap_missing++;
          continue; // 之後仍要輸出, 但 THEVOL = 0 (預設)
        }
        if (impl + 1e-12 < prev_impl_accepted) {
          // 遞減 -> 不接受, gap_missing 不變
          continue;
        }
        bool pass = true;
        if (!exempt_check && gap_max_local > 0.0 && prev_impl_accepted > 0.0) {
          double diff = impl - prev_impl_accepted;
          double allowed = gap_max_local * (gap_missing + 1);
          if (diff - allowed > 1e-12)
            pass = false;
        }
        if (pass) {
          thevol_map[K] = impl;
          prev_impl_accepted = impl;
          gap_missing = 0;
        }
        // 不通過 -> 不寫入 (保留 0)
      }

      // 向左 (遞減 strike) 掃描
      prev_impl_accepted = baseline_impl;
      gap_missing = 0;
      for (size_t r = strikes.size(); r-- > 0;) {
        int K = strikes[r];
        if (K == baseline_strike)
          continue;
        if (K > baseline_strike) // 已處理右側
          continue;
        double impl = 0.0;
        auto itv = impl_map.find(K);
        if (itv != impl_map.end())
          impl = itv->second;
        int remain_days = 0;
        double gap_max_local = group_gap_max;
        double exempt_local = group_exempt_days;
        get_meta(K, remain_days, gap_max_local, exempt_local);
        if (gap_max_local <= 0.0)
          gap_max_local = group_gap_max;
        bool exempt_check =
            (remain_days > 0 &&
             remain_days < (int)(exempt_local > 0.0 ? exempt_local : group_exempt_days));

        if (impl <= 0.0) {
          gap_missing++;
          continue;
        }
        if (impl + 1e-12 < prev_impl_accepted) {
          continue; // 遞減，不接受
        }
        bool pass = true;
        if (!exempt_check && gap_max_local > 0.0 && prev_impl_accepted > 0.0) {
          double diff = impl - prev_impl_accepted;
          double allowed = gap_max_local * (gap_missing + 1);
          if (diff - allowed > 1e-12)
            pass = false;
        }
        if (pass) {
          thevol_map[K] = impl;
          prev_impl_accepted = impl;
          gap_missing = 0;
        }
      }

      // 最終輸出 (所有 strikes)
      for (int K : strikes) {
        double impl = 0.0;
        auto itv = impl_map.find(K);
        if (itv != impl_map.end())
          impl = itv->second;
        double thev = 0.0;
        auto itT = thevol_map.find(K);
        if (itT != thevol_map.end())
          thev = itT->second;
        thevofs << opt_sysorder_id << ',' << time_str << ',' << pdk << ',' << cdate << ',' << K
                << ',' << std::fixed << std::setprecision(6) << impl << ',' << std::setprecision(6)
                << thev << '\n';
        // 收集以供 AdjTheVol
        std::string kkey = pdk + '|' + cdate;
        adj_bucket[kkey][K] = StrikeVolPair{impl, thev};
      }
    }

    // ====== 產出 AdjTheVol (線性插補) ======
    const char *adj_fname = build_log_path("stovol_ADJ_THEVOL_list.log");
    bool need_adj_header = false;
    {
      std::ifstream aifs(adj_fname);
      if (!aifs.good() || aifs.peek() == std::ifstream::traits_type::eof())
        need_adj_header = true;
    }
    std::ofstream adjofs(adj_fname, std::ios::app);
    if (adjofs.is_open()) {
      if (need_adj_header) {
        adjofs << "SysOrderID,Time,PDK,CONTRACT_DATE,STRIKE,ImplVol,TheVol,AdjTheVol" << '\n';
      }
      for (auto &kv : adj_bucket) {
        const std::string &kkey = kv.first;
        size_t pos = kkey.find('|');
        std::string pdk = (pos == std::string::npos ? kkey : kkey.substr(0, pos));
        std::string cdate = (pos == std::string::npos ? std::string() : kkey.substr(pos + 1));
        auto &omap = kv.second; // ordered by strike
        if (omap.empty())
          continue;
        // 預先收集所有有 theVol>0 的節點作為 dataPoints
        struct Pt {
          int x;
          double y;
        };
        std::vector<Pt> pts;
        pts.reserve(omap.size());
        for (auto &p : omap) {
          if (p.second.theo > 0.0)
            pts.push_back(Pt{p.first, p.second.theo});
        }
        // 若所有 TheVol 為 0，則改用 ImplVol>0 作為節點；仍找不到則全部 Adj=0。
        if (pts.empty()) {
          for (auto &p : omap) {
            if (p.second.impl > 0.0)
              pts.push_back(Pt{p.first, p.second.impl});
          }
        }
        std::sort(pts.begin(), pts.end(), [](const Pt &a, const Pt &b) { return a.x < b.x; });
        for (auto it = omap.begin(); it != omap.end(); ++it) {
          int K = it->first;
          double impl = it->second.impl;
          double thev = it->second.theo;
          double adj = thev;   // 預設: 若已有 thev>0 直接使用
          if (!(thev > 0.0)) { // 需要插補
            if (pts.empty()) {
              adj = 0.0; // 無任何參考點
            } else if (K <= pts.front().x) {
              adj = pts.front().y; // 單邊 - 左側
            } else if (K >= pts.back().x) {
              adj = pts.back().y; // 單邊 - 右側
            } else {
              // 線性插補: 找第一個 x > K
              size_t idx = 0;
              while (idx < pts.size() && pts[idx].x < K)
                ++idx;
              if (idx == 0 || idx >= pts.size()) {
                adj = pts.front().y; // safety
              } else {
                int x2 = pts[idx].x;
                double y2 = pts[idx].y;
                int x1 = pts[idx - 1].x;
                double y1 = pts[idx - 1].y;
                if (x2 != x1) {
                  adj = y1 + (double)(K - x1) * (y2 - y1) / (double)(x2 - x1);
                } else {
                  adj = y1; // 重複點 (理論不會)
                }
              }
            }
          }
          adjofs << opt_sysorder_id << ',' << time_str << ',' << pdk << ',' << cdate << ',' << K
                 << ',' << std::fixed << std::setprecision(6) << impl << ',' << std::setprecision(6)
                 << thev << ',' << std::setprecision(6) << adj << '\n';
        }
      }
    }
  }
}
