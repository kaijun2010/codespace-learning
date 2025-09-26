/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

#ifndef F_OPT_STOVOL_PRODINFO_H_
#define F_OPT_STOVOL_PRODINFO_H_

/**@file prodinfo.h
 * @brief main definition of product data structure
 * @author kaijun@taifex.com.tw
 */

#include <cstring>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../inc/stovol_msg.h"
#include "commlib/inc/atomic_io.h"
#include "commlib/inc/match_dat.h"
#include "commlib/inc/tmp.h"
#include "opt/inc/ProdConv.h"

#ifdef __GXX_EXPERIMENTAL_CXX0X__
#include <unordered_map>
#else
#ifdef __GNUC__
#include <ext/hash_map>
#include <ext/hash_set>
namespace std {
using __gnu_cxx::hash;
using __gnu_cxx::hash_map;
using __gnu_cxx::hash_set;
} // namespace std
#else
#include <hash_map>
#endif
#endif

#define INVALID_PRICE 0x7fffffff
#define RANDOM_MAX 0x7fffffff
#define DEFAULT_SEQ -0x7fffffff
#define FIRST_PSEQ 1

#define PDK_LEN 3            /**< PDK_KIND_ID length */
#define PROD_LEN 10          /**< PROD_ID length */
#define COMBINED_PROD_LEN 20 /**< combined order id length */
#define MAX_PRICE 1000000000 /**< max price 9(6)V9(3) */
#define MIN_PRICE -MAX_PRICE /**< max price 9(6)V9(3) */
#define DECPOINTDIGIT 5      /**< double 精度取小數點後5位 */
#define ERR_EXIT(e, msg) err_exit(e, (msg), reinterpret_cast<char *>(__FILE__), __LINE__)
extern void err_exit(int, int, int, char *, std::string s, char *src_file, int line_no);

//--------------------------------------------------------------------
// forward declare class
//--------------------------------------------------------------------
class PriceNode;
class Product;
// class CProduct;
class ProdInfo;
struct Put;
class Pdk;

typedef std::map<int, PriceNode> pr_map_t;
typedef std::map<int, int> st_rule_map_t; // <Rule,Counter>
#ifdef __GXX_EXPERIMENTAL_CXX0X__
typedef std::unordered_map<int, std::string> pgseq_id_map_t;
typedef std::unordered_map<std::string, int> SPREAD_CODE_HM; // pdk, pgseq
#else
typedef std::hash_map<int, std::string> pgseq_id_map_t;
typedef std::hash_map<std::string, int> SPREAD_CODE_HM; // pdk, pgseq
#endif

//--------------------------------------------------------------------
// PriceNode class
//--------------------------------------------------------------------
/**@brief keep price qty */
class PriceNode {
public:
  int m_price;
  int m_qty;
  PriceNode() : m_price(0), m_qty(0) {}
  bool operator==(const PriceNode &rhs) const {
    return m_price == rhs.m_price && m_qty == rhs.m_qty;
  }
  bool operator!=(const PriceNode &rhs) const { return !(*this == rhs); }
};

//--------------------------------------------------------------------
// Pdk class
//--------------------------------------------------------------------
/**@brief contrustor of Pdk
 * @param param_key ptr to PDK_KIND_ID
 */
class Pdk {
public:
  char m_kind_id[PDK_LEN + 1]; // PDK_KIND_ID
  char m_subtype;              // PDK_EXPIRY_TYPE
  int m_price_scale;           // PDK_PRICE_SCALE
  int m_scale;                 // power(10, m_price_scale)
  int m_tick_size;             // Tick size
  int m_underlying_market;     // Underlying market
  std::list<Put> m_put;

  Pdk() { memset(this, 0, sizeof(*this)); }
  explicit Pdk(char *kind_id);
  char *getKey() { return m_kind_id; }
  int getTick(double price) const;
  int getTickPrice(double price, int scale) const;
};

struct eqstr_mat {
  bool operator()(const char *s1, const char *s2) const { return strcmp(s1, s2) == 0; }
};

struct hash_func {
  size_t operator()(const char *str) const {
    size_t hash = 5381;
    int c;

    while ((c = *str++))
      hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
  }
};

#ifndef STOVOL_PDK_T_DEFINED
#define STOVOL_PDK_T_DEFINED
// 簡化版 pdk_t (供 LLM 現價與 seq 更新使用，不影響既有 Pdk* 結構)
struct pdk_t {
  char kind_id[PDK_LEN + 1]; // PDK_KIND_ID
  char stock_id[6 + 1];      // PDK_STOCK_ID
  int orig_prod_idx;         // PDK_ORIG_PROD_IDX
  int m_price_scale;         // PDK_PRICE_SCALE
  int m_scale;               // 10 ^ m_price_scale
  uint64_t seq;              // 最新序號
  pdk_t() : orig_prod_idx(0), m_price_scale(0), m_scale(1), seq(0) {
    stock_id[0] = 0;
    kind_id[0] = 0;
  }
};
#endif

// 原本 m_PutMap 使用的雜湊 (key -> Pdk*) 保持不變
#ifdef __GXX_EXPERIMENTAL_CXX0X__
struct pdk_hash_t : std::unordered_map<std::string, Pdk *, std::hash<std::string>> {};
#else
struct pdk_hash_t : hash_map<string, Pdk *, hash<string>> {};
#endif
typedef pdk_hash_t::iterator pdk_hash_it_t; // for Pdk* map (m_PutMap)

// 簡化 pdk_t 的雜湊，使用 PDK_KIND_ID(3) 為 key
#ifdef __GXX_EXPERIMENTAL_CXX0X__
typedef std::unordered_map<std::string, pdk_t, std::hash<std::string>> pdk_kind_map_t;
#else
typedef hash_map<std::string, pdk_t, hash<string>> pdk_kind_map_t;
#endif
extern pdk_kind_map_t g_pdk_hash; // 以 PDK_KIND_ID 為 key 的簡化 pdk_t 資料
extern pthread_mutex_t g_pdk_hash_mutex;

/**
 * @brief single product data structure
 */
class Product {
public:
  char m_prod_id[COMBINED_PROD_LEN + 1]; /**< 商品代碼 */
  short m_pgseq;                         /**< 商品序號 */
  char m_pdk_kind_id[PDK_LEN + 1];       /**< 契約代碼 (與 m_pdk_param_key 不同) */

  msg_time_t m_last_mtime; /**< 最近成交時間 */
  int m_last_mprice;       /**< 最近成交價 */
  int m_last_mqty;         /**< 最近成交量 */
  int m_prod_premium;      /**< 開參 */

  int m_status_code; /**< 狀態碼 */
  int m_flow_grp_id; /**< 一般交易OPEN SWITCH群組代號 */
  int m_status;
  int m_normal_stage;                 /**< normal TradeStatus of this product */
  int m_sub_stage[TAG_TYPE_MAX + 1];  /**< record stage of each TAG_TYPE of this product */
  int m_sub_status[TAG_TYPE_MAX + 1]; /**< record TradeStatus of each TAG_TYPE of this product */
  pr_map_t m_price_list[2];           /**< 委託簿 */
  PriceNode m_best_dprice_list[2];    /**< 虛擬最佳一檔 */
  msg_time_t m_simu_time;             /**< 最近一次模擬試撮時間 */
  bool m_bIsInCompete;                /**< 是否於收集合競價  ST_OPEN */
  bool m_bIsRecvStage;                /**< 是否於收單階段 ST_RECV */
  bool m_bIsInNormal;                 /**< 是否於開盤階段, ST_NORMAL */
  char m_subtype; /**< 契約類型 B:公債 C:商品(黃金,源油) E:匯率 I:指數, S:股票 */
  char m_underlying_market; /**< 1:上市、2:上櫃、3:日本、4:印度、5:美國、6:英國 */

  // Additional fields needed for database loading
  int m_group_id; /**< 群組代碼 */
  // (移除未使用欄位 m_opening_date, m_settlement_date)

  char m_end_date[10 + 1];    /**< 最後交易日 */
  char m_settle_date[6 + 1];  /**< 交割年月 */
  int m_high_limit;           /**< 漲停價 */
  int m_low_limit;            /**< 跌停價 */
  char m_pdk_stock_id[6 + 1]; /**< (2330) 台積電*/
  int m_pdk_orig_idx; /**< PDK原始產品索引 (期貨為0，選擇權從PDK_ORIG_PROD_IDX撈取) */
  // ===== 新增: 契約到期月份日 (PROD_CONTRACT_DATE，格式 yyyymm 或 yyyymmdd 依資料庫) =====
  char m_contract_date[8 + 1];

  // ===== OPT 專用或共用延伸欄位 =====
  char m_pc_code[2];  /**< 選擇權買賣權別: 'C'  or 'P'; FUT 置空 */
  int m_strike_price; /**< 履約價 (依 PDK_PRICE_SCALE 轉換後的整數價) */
  // N: 標準型, O: 調整型 (除權、除息等)；若未撈取則為空字串
  char m_pdk_on_code[2 + 1]; // N: 標準型, O: 調整型 (除權、除息等)；若未撈取則為空字串
  char m_pdk_market_close[2 + 1]; // PDK_MARKET_CLOSE (收盤時間類別碼)

  // ===== 新增: PROD_SPREAD_CODE (商品價差代碼) =====
  int m_spread_code;        /**< 對應資料庫欄位 PROD_SPREAD_CODE; 若未撈取則為 0 */
  char m_pdk_param_key[16]; // PDK_PARAM_KEY (例如 STC*, 用於辨識波動計算分組)
  // ===== 隱含波動度所需新增欄位 =====
  double m_p03_compound; /**< 無風險利率 (P03_COMPOUND) */
  double m_impvol;       /**< 隱含波動度 (計算結果) */
  int m_remaining_days;  /**< 剩餘交易天數 (含今日或不含? 依 SQL 結果) */

  // ===== 新增: SLT 相關欄位 (需求) =====
  double m_slt_spread; /**< 報價價差放寬倍數 (DB SLT_SPREAD * scale 後四捨五入整數存於 double) */
  int m_acc_qnty; /**< 最小累積口數 (DB SLT_VALID_QNTY) */

  // ===== 新增: DSPAR 參數 (結算價豁免天數) =====
  double m_dspar_exempt_days; /**< 來源: DSPAR.DSPAR_EXEMPT_DAYS (DSPAR_TYPE='C') */

  // ===== 新增: DSLMT 參數 (依 KIND+MONTH 對應) =====
  double m_gap_max; /**< DSLMT_GAP_MAX 對應 (預設 0) */

  // ===== 新增: PDK 價格刻度資訊 =====
  int m_price_scale; /**< PDK_PRICE_SCALE */
  int m_scale;       /**< 10^m_price_scale */

  // Constructor to initialize all members to zero
  Product() { memset(this, 0, sizeof(*this)); }

  // 方法宣告
  int update_price_list(int bs, int price, int qty_changed);
  void updateStatus(tag_flow_t &tag);
  void updateProductFlags();
  // Debug: 傾印 Top N 價階 (買:side=0 / 賣:side=1)，格式 B1:price@qty,... 或 S1:price@qty,...
  std::string dump_top_levels(int side, int N) const;

  friend std::ostream &operator<<(std::ostream &os, const Product &prod);
};

/**
 * @brief Store all products' information
 */
class ProdInfo {
public:
  std::vector<Product> m_prod;
  pgseq_id_map_t m_prodid_map;
  int m_prod_cnt;
  CPdkMap m_pdk_map;
  pdk_hash_t m_PutMap;

  ~ProdInfo();
  int load_prod(int grpid, const char *pszFilename);
  int load_prod_opt(int grpid, const char *pszFilename); // OPT 專用載入 (簡化 SQL)
  int load_pdk(int grpid, const char *pszFilename);
  bool isSingleProduct(int pgseq);
  Pdk *GetPdkPut(std::string kindID) const;
  int GetTick(std::string kindID) const;
  std::vector<Product> m_vprod; // 虛擬期
  // ===== OPT 波動率計算相關 =====
  std::vector<int> m_opt_vol_pgseq; /**< 需計算波動率的商品 pgseq (PDK_PARAM_KEY='STC') */
  std::unordered_set<int> m_opt_vol_set; /**< 快速查詢用 (pgseq) */
  bool isOptVolTarget(int pgseq) const { return m_opt_vol_set.find(pgseq) != m_opt_vol_set.end(); }
  // ===== 利率與剩餘天數快取 =====
  std::unordered_map<std::string, double> m_p03_rate_map;    /**< key: KINDID+CONTRACT_EDATE */
  std::unordered_map<std::string, int> m_remaining_days_map; /**< key: KINDID+CONTRACT_EDATE */
  friend std::ostream &operator<<(std::ostream &os, const ProdInfo &obj);
};

//--------------------------------------------------------------------
// 期貨最近月契約相關定義
//--------------------------------------------------------------------
/**
 * @brief 期貨商品資訊結構
 */
struct FutureProductInfo {
  std::string pdk_kind_id;
  std::string prod_id;
  int pgseq;
  std::string settle_date;

  FutureProductInfo(const std::string &kind_id = "", const std::string &prod_id = "", int seq = 0,
                    const std::string &date = "")
      : pdk_kind_id(kind_id), prod_id(prod_id), pgseq(seq), settle_date(date) {}
};

#ifdef __GXX_EXPERIMENTAL_CXX0X__
typedef std::unordered_map<std::string, int> NearestMonthMap; // PDK_KIND_ID -> PGSEQ
#else
typedef std::hash_map<std::string, int> NearestMonthMap; // PDK_KIND_ID -> PGSEQ
#endif

// 函數宣告
extern "C" {
int callback_future_products(void *ptr, int argc, char **argv, char **azColName);
}
int load_nearest_month_contracts(int grpid, const char *pszFilename,
                                 std::vector<FutureProductInfo> &future_products,
                                 NearestMonthMap &nearest_month_map);

struct Put {
  int m_max;  // PUT_MAX
  int m_min;  // PUT_MIN
  int m_unit; // PUT_UNIT
};

#endif // F_OPT_STOVOL_PRODINFO_H_
