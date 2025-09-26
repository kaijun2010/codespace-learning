/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  prodinfo.cpp
 * @brief implement product info for stovol
 * @author kaijun@taifex.com.tw
 */

#include "prodinfo.h"
// 定義全域 PDK 簡化雜湊 (key=PDK_KIND_ID, value=pdk_t)
pdk_kind_map_t g_pdk_hash;
#include "LoggerProxy.h"
#include "env.h" // for build_log_path

#include <sqlite3.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "../../inc/stovol_msg.h"
#include "commlib/inc/command.hpp"
#include "commlib/inc/match_dat.h"
#include "commlib/inc/safecpy.hpp"
#include "commlib/inc/tfxlog.h"

using namespace std;

//--------------------------------------------------------------------
// global variables
//--------------------------------------------------------------------
extern int g_grpid;
extern char g_ocf_date[10 + 1];
extern char g_ocf_prev_date[10 + 1];
extern char g_ocf_date_slash[11];
extern uint32_t g_uocf_date;
extern int g_ocf_mocf_day_count;

//--------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------
static inline char *StringCast(char *p) { return p ? p : (char *)""; }

// adjust2: 四捨五入到整數
static inline int adjust2(double f) { return f >= 0 ? (int)(f + 0.5) : (int)(f - 0.5); }

// sqlite3_exec_printf: Execute SQL with printf-style formatting
static int sqlite3_exec_printf(sqlite3 *ldb, const char *fmt, sqlite3_callback locallback,
                               void *context, char **errmsg, ...) {
  va_list args;
  va_start(args, errmsg);
  char *query = sqlite3_vmprintf(fmt, args);
#ifdef DEBUG_SQLITE
  cout << "query = " << query << endl;
#endif
  int res = sqlite3_exec(ldb, query, locallback, context, errmsg);
  sqlite3_free(query);
  va_end(args);
  return res;
}

//--------------------------------------------------------------------
// SQLite callback functions
//--------------------------------------------------------------------
#ifdef DEBUG_SQLITE
static int callback_OCF(void *ptr, int argc, char **argv, char **azColName)
#else
static int callback_OCF(void *ptr, __attribute__((unused)) int argc, char **argv,
                        __attribute__((unused)) char **azColName)
#endif
{
  int ocf_date = atoi(argv[0]);
  g_ocf_mocf_day_count = atoi(argv[1]);
  safecpy(g_ocf_date_slash, StringCast(argv[2]));
  safecpy(g_ocf_prev_date, StringCast(argv[3])); // 前一交易日

  sprintf((char *)ptr, "%04d%02d%02d%c", ocf_date / 10000, ocf_date % 10000 / 100, ocf_date % 100,
          '\0');

  g_uocf_date = atoi(g_ocf_date);

#ifdef DEBUG_SQLITE
  cout << "callback_OCF " << argc << " col: ";
  for (int i = 0; i < argc; i++)
    cout << azColName[i] << "=" << (argv[i] ? argv[i] : "NULL") << "   ";
  cout << endl;
  cout << "g_ocf_date = " << g_ocf_date << endl;
  cout << "g_ocf_mocf_day_count = " << g_ocf_mocf_day_count << endl;
  cout << "g_ocf_prev_date = " << g_ocf_prev_date << endl;
#endif
  return 0;
}

#ifdef DEBUG_SQLITE
static int callback_PROD(void *ptr, int argc, char **argv, char **azColName)
#else
static int callback_PROD(void *ptr, __attribute__((unused)) int argc, char **argv,
                         __attribute__((unused)) char **azColName)
#endif
{
  static int seq = 0; // 商品序號累加器
  Product prod;       // 使用預設建構函數，不需要 memset

  ProdInfo *pProdInfo = (ProdInfo *)ptr;

  // PROD_ID
  safecpy(prod.m_prod_id, StringCast(argv[0]));
  strncpy(prod.m_pdk_kind_id, prod.m_prod_id, 3);

  // PROD_PGSEQ
  if (++seq != atoi(argv[1])) {
    TFX::Log::COUT("Err: prod pgseq error.");
    return -1;
  }
  prod.m_pgseq = seq;

  // PROD_OSW_GRP or PGRP_BTRD_OSW_GRP
  prod.m_flow_grp_id = atoi(argv[2]);

  // Store additional database fields
  prod.m_group_id = g_grpid;

  // PDK_PRICE_SCALE
  int pdk_scale = atoi(argv[4]);
  int scale = 1;
  for (int i = 0; i < pdk_scale; i++)
    scale *= 10;

  // Premium and limits
  if (argv[5])
    prod.m_prod_premium = adjust2(atof(argv[5]) * scale); // PROD_PREMIUM
  if (argv[6]) {
    // PDK_ORIG_PROD_IDX - FUT 不需要存現貨資料, 由OPT處理
  }

  // End date
  safecpy(prod.m_end_date, StringCast(argv[7]));

  // Price limits
  if (argv[8])
    prod.m_high_limit = adjust2(atof(argv[8]) * scale); // 漲停
  if (argv[9])
    prod.m_low_limit = adjust2(atof(argv[9]) * scale); // 跌停

  // Near month pgseq
  if (argv[10]) {
    // m_near_pgseq = atoi(argv[10]) - 可以加到 Product 結構中
  }

  // PDK fields
  safecpy(prod.m_pdk_stock_id, StringCast(argv[11])); // PDK_STOCK_ID
  if (argv[12])
    prod.m_subtype = argv[12][0]; // PDK_EXPIRY_TYPE
  if (argv[15])
    prod.m_underlying_market = argv[15][0]; // PDK_UNDERLYING_MARKET

  /* put prod to vector */
  // insert first dummy prod because first pseq is 1
  if (pProdInfo->m_prod.size() == 0) {
    pProdInfo->m_prod.push_back(prod);
    pProdInfo->m_prod[0].m_normal_stage = 0; // ST_CLOSE equivalent
  }
  pProdInfo->m_prod.push_back(prod);
  pProdInfo->m_prodid_map[prod.m_pgseq] = prod.m_prod_id;
  pProdInfo->m_prod_cnt++;

#ifdef DEBUG_SQLITE
  cout << "callback_PROD: " << argc << " col: ";
  for (int i = 0; i < argc; i++)
    cout << azColName[i] << "=" << (argv[i] ? argv[i] : "NULL") << "   ";
  cout << endl;
#endif

  return 0;
}

// ---------------- OPT 專用商品載入 Callback ----------------
// SQL: select PROD_ID, PROD_PGSEQ, PROD_SETTLE_DATE, PROD_STRIKE_PRICE, PROD_PC_CODE, PROD_PREMIUM,
// PROD_OSW_GRP,
//      PDK_KIND_ID, PDK_STOCK_ID, ifnull(PDK_PRICE_SCALE, -1), PDK_PARAM_KEY,
//      ifnull(PDK_ORIG_PROD_IDX, 0)
static int callback_PROD_OPT(void *ptr, int argc, char **argv, char **azColName) {
  // 期權商品 SQL 欄位說明 (最新):
  //  0 PROD_ID
  //  1 PROD_PGSEQ
  //  2 PROD_SETTLE_DATE
  //  3 PROD_STRIKE_PRICE
  //  4 PROD_PC_CODE
  //  5 PROD_PREMIUM
  //  6 PROD_OSW_GRP
  //  7 PDK_KIND_ID
  //  8 PDK_STOCK_ID
  //  9 PDK_PRICE_SCALE
  // 10 PDK_PARAM_KEY
  // 11 PDK_ORIG_PROD_IDX
  // 12 PDK_EXPIRY_TYPE (ifnull(...,''))
  // 13 PDK_UNDERLYING_MARKET (ifnull(...,0))
  // 14 PROD_END_DATE
  // 15 PDK_ON_CODE (新增: 標準型(N)/調整型(O))
  // 16 PROD_CONTRACT_DATE (新增: yyyymm 或 yyyymmdd)
  // 17 PROD_SPREAD_CODE (月序)
  // 18: PDK_MARKET_CLOSE (收盤時間類別碼)

  if (argc < 12)
    return 0; // 基本保護
  static int seq_opt = 0;
  Product prod; // 預設清 0
  ProdInfo *pProdInfo = (ProdInfo *)ptr;

  // 0: PROD_ID
  safecpy(prod.m_prod_id, StringCast(argv[0]));
  strncpy(prod.m_pdk_kind_id, prod.m_prod_id, 3);

  // 1: PROD_PGSEQ (仍以資料庫值為主, 也可用 auto 增量)
  int db_pgseq = atoi(argv[1]);
  if (++seq_opt != db_pgseq) {
    // 若不希望嚴格檢查，可改為 seq_opt = db_pgseq;
    seq_opt = db_pgseq; // 放寬規則: 直接使用 DB PGSEQ
  }
  prod.m_pgseq = db_pgseq;

  // 2: PROD_SETTLE_DATE -> 存到 m_settle_date (長度 6)
  if (argv[2]) {
    strncpy(prod.m_settle_date, argv[2], sizeof(prod.m_settle_date) - 1);
  }

  // 3: PROD_STRIKE_PRICE (double -> int by scale later)
  double strike = argv[3] ? atof(argv[3]) : 0.0;

  // 4: PROD_PC_CODE
  if (argv[4]) {
    prod.m_pc_code[0] = argv[4][0];
    prod.m_pc_code[1] = '\0';
  }

  // 5: PROD_PREMIUM
  double premium = argv[5] ? atof(argv[5]) : 0.0;

  // 6: PROD_OSW_GRP
  prod.m_flow_grp_id = argv[6] ? atoi(argv[6]) : 0;
  prod.m_group_id = g_grpid;

  // 7: PDK_KIND_ID (已由 substr 取得, 可比對)
  // 8: PDK_STOCK_ID -> m_pdk_stock_id
  safecpy(prod.m_pdk_stock_id, StringCast(argv[8]));

  // 9: PDK_PRICE_SCALE
  int pdk_scale = argv[9] ? atoi(argv[9]) : -1;
  prod.m_price_scale = (pdk_scale > 0 ? pdk_scale : 0);
  // 10: PDK_PARAM_KEY 用來判斷是否 STC
  const char *param_key = argv[10] ? argv[10] : "";
  if (*param_key) {
    safecpy(prod.m_pdk_param_key, param_key);
  }
  // 11: PDK_ORIG_PROD_IDX (已不使用)

  int scale = 1;
  if (pdk_scale > 0) {
    for (int i = 0; i < pdk_scale; ++i)
      scale *= 10;
  }
  prod.m_scale = scale; // 即使 pdk_scale <=0 也保留為1
  if (pdk_scale > 0) {
    prod.m_prod_premium = adjust2(premium * scale);
    prod.m_strike_price = adjust2(strike * scale);
    // prod.m_pdk_orig_idx 實際上不使用由  g_pdk_hash 接現貨實際 Update PDK_ORIG_PROD_IDX
    // prod.m_pdk_orig_idx = adjust2(pdk_orig_idx_raw * scale); // 處理 PDK_ORIG_PROD_IDX
  } else {
    // prod.m_pdk_orig_idx = 0; // 無效 scale 時設為 0
  }
  prod.m_pdk_orig_idx = 0; // 此處給0，價格由 g_pdk_hash[key].orig_prod_idx 取出。

  prod.m_subtype = argv[12][0];                        // 12:PDK_EXPIRY_TYPE
  prod.m_underlying_market = argv[13][0];              // 13:PDK_UNDERLYING_MARKET
  safecpy(prod.m_end_date, StringCast(argv[14]));      // 14:PROD_END_DATE -> m_end_date
  safecpy(prod.m_pdk_on_code, StringCast(argv[15]));   // 15:PDK_ON_CODE -> m_pdk_on_code
  safecpy(prod.m_contract_date, StringCast(argv[16])); // 16:PROD_CONTRACT_DATE -> m_contract_date
  prod.m_spread_code = atoi(argv[17]);                 // 17:PROD_SPREAD_CODE
  safecpy(prod.m_pdk_market_close, StringCast(argv[18])); // 18:PDK_MARKET_CLOSE(收盤時間類別碼)

  // push
  if (pProdInfo->m_prod.empty()) {
    // 建立真正空白 dummy (pgseq=0) 以確保後續 m_prod[pgseq] 對齊, 不複製第一筆商品內容
    Product dummy; // 預設全 0
    dummy.m_pgseq = 0;
    dummy.m_normal_stage = 0; // ST_CLOSE
    safecpy(dummy.m_prod_id, "DUMMY");
    pProdInfo->m_prod.push_back(dummy);
  }
  // 期望: 現在 vector.size()==pgseq, 這樣 push_back 後新商品會位於 index==pgseq
  if ((int)pProdInfo->m_prod.size() != prod.m_pgseq) {
    // 若不一致, 代表 PGSEQ 可能跳號; 先補齊中間空洞 (放 GAP 標記)
    while ((int)pProdInfo->m_prod.size() < prod.m_pgseq) {
      Product gap;
      gap.m_pgseq = pProdInfo->m_prod.size();
      safecpy(gap.m_prod_id, "GAP");
      pProdInfo->m_prod.push_back(gap);
    }
  }
  pProdInfo->m_prod.push_back(prod); // push 後 index == prod.m_pgseq (若前述條件成立)
  // 防禦: 檢查剛插入商品是否位於期望位置
  if ((int)pProdInfo->m_prod.size() - 1 != prod.m_pgseq) {
    try {
      std::ofstream warn(build_log_path("stovol_pgseq_align_warn.log"), std::ios::app);
      if (warn.is_open()) {
        warn << "PGSEQ_ALIGN_WARN OPT DB_PGSEQ=" << prod.m_pgseq
             << " VEC_SIZE=" << pProdInfo->m_prod.size()
             << " LAST_INDEX=" << (pProdInfo->m_prod.size() - 1) << '\n';
      }
    } catch (...) {
    }
  }
  pProdInfo->m_prodid_map[prod.m_pgseq] = prod.m_prod_id;
  pProdInfo->m_prod_cnt++;

  if (param_key[0]) {
    // 只判斷前三碼即可，無需處理空白或長度
    bool is_target = false;
    if (g_only_calc_tx) {
      is_target = (strncmp(param_key, "TXO", 3) == 0);
    } else {
      is_target = (strncmp(param_key, "STC", 3) == 0);
    }
    if (is_target) {
      pProdInfo->m_opt_vol_pgseq.push_back(prod.m_pgseq);
      pProdInfo->m_opt_vol_set.insert(prod.m_pgseq);
    }
  }

#ifdef DEBUG_SQLITE
  cout << "callback_OPT_PROD: " << argc << " col: ";
  for (int i = 0; i < argc; i++)
    cout << azColName[i] << "=" << (argv[i] ? argv[i] : "NULL") << "   ";
  cout << endl;
#endif

  return 0;
}

int ProdInfo::load_prod_opt(int grpid, const char *pszFilename) {
  static char *zErrMsg = 0;
  int rc = 0;
  sqlite3 *dbh;
  const char *SQL;

  rc = sqlite3_open_v2(pszFilename, &dbh, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_open_v2() return: " << rc << endl;
    cout << "Can't open SQLite DB database: " << sqlite3_errmsg(dbh) << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "(OPT) fetching load_prod_opt data error.");
    return -1;
  }

  const char *SQLBegin = "BEGIN;";
  sqlite3_exec(dbh, SQLBegin, NULL, 0, &zErrMsg); // 容錯忽略錯誤

  // 清空原資料
  m_prod.clear();
  m_prodid_map.clear();
  m_prod_cnt = 0;
  m_p03_rate_map.clear();
  m_remaining_days_map.clear();
  // 讀取 DSPAR_EXEMPT_DAYS 參數 (DSPAR_TYPE='C')
  double dspar_exempt_days = 0.0;
  {
    const char *SQL_DSPAR = "select DSPAR_EXEMPT_DAYS from DSPAR where DSPAR_TYPE = 'C'";
    struct LocalCtxDSPAR {
      double *pval;
    } lctxDSPAR{&dspar_exempt_days};
    auto callback_dspar = [](void *ptr, int argc, char **argv, char **) -> int {
      if (argc >= 1 && argv[0]) {
        auto *ctx = static_cast<LocalCtxDSPAR *>(ptr);
        *(ctx->pval) = atof(argv[0]);
      }
      return 0;
    };
    rc = sqlite3_exec(dbh, SQL_DSPAR, callback_dspar, &lctxDSPAR, &zErrMsg);
    if (rc != SQLITE_OK) {
      cout << "sqlite3_exec() DSPAR return: " << rc << " ErrorMsg: " << (zErrMsg ? zErrMsg : "")
           << endl;
      if (zErrMsg) {
        sqlite3_free(zErrMsg);
        zErrMsg = nullptr;
      }
      dspar_exempt_days = 0.0; // fallback
    }
  }

  //  先載入 P03 利率
  const char *SQL_P03 =
      "select distinct substr(PROD_ID, 1, 3) as KIND_ID, PROD_CONTRACT_DATE, PROD_END_DATE, "
      "P03_COMPOUND "
      "from PROD join P03 on substr(PROD_ID, 1, 3) = P03_PROD_ID "
      "where PROD_EXPIRE_CODE <> 'Y' and PROD_END_DATE = P03_DELIVERY_DATE "
      "order by KIND_ID, PROD_CONTRACT_DATE";

  // callback: KIND_ID, PROD_CONTRACT_DATE, PROD_END_DATE, P03_COMPOUND
  struct LocalCtxP03 {
    ProdInfo *p;
  } lctxP03{this};
  auto callback_p03 = [](void *ptr, int argc, char **argv, char **) -> int {
    if (argc < 4)
      return 0;
    auto *ctx = static_cast<LocalCtxP03 *>(ptr);
    const char *kind = argv[0] ? argv[0] : "";
    const char *settle = argv[1] ? argv[1] : ""; // 例如 202509
    // argv[2] = PROD_END_DATE (目前不使用, 如未來需要可再取用)
    double rate = argv[3] ? atof(argv[3]) : 0.0;
    if (strlen(kind) >= 3 && strlen(settle) >= 6) {
      char key_buf[32];
      snprintf(key_buf, sizeof(key_buf), "%3.3s_%.*s", kind, (int)strlen(settle), settle);
      ctx->p->m_p03_rate_map[key_buf] = rate;
    }
    return 0;
  };
  rc = sqlite3_exec(dbh, SQL_P03, callback_p03, &lctxP03, &zErrMsg);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    // 不立即 return, 但清除錯誤
    sqlite3_free(zErrMsg);
    zErrMsg = nullptr;
  }

  // 載入剩餘交易天數
  const char *SQL_REMAIN =
      "select distinct substr(PROD_ID, 1, 3) as KIND_ID, PROD_CONTRACT_DATE, "
      "count(PROD_ID) - 1 as REMAINING_TRADE_DAYS "
      "from PROD,OCF,MOCF "
      "where PROD_EXPIRE_CODE <> 'Y' "
      "and MOCF_OPEN_CODE = 'Y' and MOCF_DATE  between OCF_DATE and PROD_END_DATE "
      "group by PROD_ID, PROD_CONTRACT_DATE order by PROD_CONTRACT_DATE";
  struct LocalCtxRemain {
    ProdInfo *p;
  } lctxRemain{this};
  auto callback_remaining_trade_days = [](void *ptr, int argc, char **argv, char **) -> int {
    if (argc < 3)
      return 0;
    auto *ctx = static_cast<LocalCtxRemain *>(ptr);
    const char *kind = argv[0] ? argv[0] : "";
    const char *contract_date = argv[1] ? argv[1] : "";
    int days = argv[2] ? atoi(argv[2]) : 0;
    if (strlen(kind) >= 3 && strlen(contract_date) >= 6) {
      char key_buf[32];
      snprintf(key_buf, sizeof(key_buf), "%3.3s_%.*s", kind, (int)strlen(contract_date),
               contract_date);
      // COUTX << "REMAINING_TRADE_DAYS(key)=" << key_buf << ENDLX;
      ctx->p->m_remaining_days_map[key_buf] = days;
    }
    return 0;
  };
  rc = sqlite3_exec(dbh, SQL_REMAIN, callback_remaining_trade_days, &lctxRemain, &zErrMsg);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    sqlite3_free(zErrMsg);
    zErrMsg = nullptr;
  }
  // (移至產品載入後) DSLMT 需匹配已載入商品, 不能過早撈取
  // OPT 專用 SQL, (不含當日到期契約及調整型契約)
  SQL = (char *)"select PROD_ID, PROD_PGSEQ, PROD_SETTLE_DATE, PROD_STRIKE_PRICE, PROD_PC_CODE, "
                "PROD_PREMIUM, PROD_OSW_GRP, PDK_KIND_ID, PDK_STOCK_ID, ifnull(PDK_PRICE_SCALE, "
                "-1), PDK_PARAM_KEY,ifnull(PDK_ORIG_PROD_IDX, 0), "
                "ifnull(PDK.PDK_EXPIRY_TYPE,''), ifnull(PDK.PDK_UNDERLYING_MARKET,0), "
                "PROD_END_DATE, PDK_ON_CODE, ifnull(PROD_CONTRACT_DATE,''), PROD_SPREAD_CODE, "
                "PDK_MARKET_CLOSE "
                "from PROD, PDK "
                "where PDK_KIND_ID = substr (PROD_ID,1,3) "
                "and PROD_GRP_ID = %d and PROD_EXPIRE_CODE != 'Y' "
                "order by PROD_GRP_ID, PROD_PGSEQ ";
  // PDK_ON_CODE=N/O (標準型/調整型)
  // 撈全部, callback 內再依 PDK_PARAM_KEY == 'STC' 收集

  rc = sqlite3_exec_printf(dbh, SQL, callback_PROD_OPT, this, &zErrMsg, grpid);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "(OPT) fetching product(callback_PROD_OPT) data error.");
    return -1;
  }

  // 產品已載入後再撈取 DSLMT (KIND, MONTH->GAP_MAX)
  {
    const char *SQL_DSLMT = "select DSLMT_KIND_ID, DSLMT_MONTH, DSLMT_GAP_MAX "
                            "from DSLMT order by DSLMT_KIND_ID, DSLMT_MONTH";
    struct LocalCtxDSLMT {
      ProdInfo *p;
    } lctxDsl{this};
    auto callback_dslmt = [](void *ptr, int argc, char **argv, char **) -> int {
      if (argc < 3)
        return 0;
      auto *ctx = static_cast<LocalCtxDSLMT *>(ptr);
      const char *kind = argv[0] ? argv[0] : "";     // DSLMT_KIND_ID
      int month = argv[1] ? atoi(argv[1]) : 0;       // DSLMT_MONTH -> spread code
      double gapMax = argv[2] ? atof(argv[2]) : 0.0; // DSLMT_GAP_MAX (百分比值, 需 /100)
      if (!*kind)
        return 0;
      for (size_t i = 1; i < ctx->p->m_prod.size(); ++i) { // skip dummy index0
        Product &pr = ctx->p->m_prod[i];
        if (strncmp(pr.m_pdk_kind_id, kind, 3) == 0 && pr.m_spread_code == month) {
          pr.m_gap_max = gapMax / 100.0; // 轉換為比率 (例如 2 => 0.02)
        }
      }
      return 0;
    };
    int rc_dsl = sqlite3_exec(dbh, SQL_DSLMT, callback_dslmt, &lctxDsl, &zErrMsg);
    if (rc_dsl != SQLITE_OK) {
      cout << "sqlite3_exec() DSLMT return: " << rc_dsl << " ErrorMsg: " << (zErrMsg ? zErrMsg : "")
           << endl;
      if (zErrMsg) {
        sqlite3_free(zErrMsg);
        zErrMsg = nullptr;
      }
    }
  }

  // === 在商品全部載入後再批次填入利率 / 剩餘天數與初始化隱含波動度 ===
  for (size_t i = 1; i < m_prod.size(); ++i) { // index 0 為 dummy
    Product &prod = m_prod[i];
    if (prod.m_settle_date[0] == '\0')
      continue; // 無 settle 資料略過
    char key_buf[32];
    snprintf(key_buf, sizeof(key_buf), "%3.3s_%.*s", prod.m_pdk_kind_id,
             (int)strlen(prod.m_settle_date), prod.m_settle_date);
    auto rit = m_p03_rate_map.find(key_buf);
    if (rit != m_p03_rate_map.end())
      prod.m_p03_compound = rit->second;
    auto dit = m_remaining_days_map.find(key_buf);
    if (dit != m_remaining_days_map.end())
      prod.m_remaining_days = dit->second;
    // 確保初值
    if (prod.m_impvol == 0.0)
      prod.m_impvol = 0.0;                        // 之後計算再覆寫
    prod.m_dspar_exempt_days = dspar_exempt_days; // 全部商品共用此參數
  }

  // === 需求: 載入 SLT (m_slt_spread, m_acc_qnty) ===
  // 參考 ftprice 寫法: select a.PROD_ID, a.PROD_PGSEQ, ifnull(c.SLT_SPREAD,1),
  // ifnull(c.SLT_VALID_QNTY,1) 需乘上 scale: scale = 10^(PDK_PRICE_SCALE)
  const char *SQL_SLT =
      "select a.PROD_ID, a.PROD_PGSEQ, ifnull(c.SLT_SPREAD,1), ifnull(c.SLT_VALID_QNTY,1) "
      "from PROD a  "
      "left join PDK b on  b.PDK_KIND_ID = substr(a.PROD_ID,1,3)  "
      "left join SLT c on  c.SLT_KIND_ID = substr(a.PROD_ID,1,3) and c.SLT_SPREAD_LONG = "
      "a.PROD_SPREAD_CODE  "
      "where a.PROD_GRP_ID = %d and a.PROD_EXPIRE_CODE != 'Y'";

  struct LocalCtxSLT {
    ProdInfo *p;
  } lctxSLT{this};
  auto callback_SLT = [](void *ptr, int argc, char **argv, char **) -> int {
    if (argc < 4)
      return 0; // 預期 4 欄
    auto *ctx = static_cast<LocalCtxSLT *>(ptr);
    int pgseq = argv[1] ? atoi(argv[1]) : 0;
    if (pgseq <= 0 || pgseq >= (int)ctx->p->m_prod.size())
      return 0; // 防禦
    Product &prod = ctx->p->m_prod[pgseq];
    int scale = (prod.m_scale > 0 ? prod.m_scale : 1);
    double spread_raw = argv[2] ? atof(argv[2]) : 1.0;                              // 原始 DB 值
    int spread_scaled = (int)(spread_raw * scale + (spread_raw >= 0 ? 0.5 : -0.5)); // 四捨五入
    prod.m_slt_spread = static_cast<double>(spread_scaled);
    prod.m_acc_qnty = argv[3] ? atoi(argv[3]) : 1;
    if (prod.m_acc_qnty <= 0)
      prod.m_acc_qnty = 1; // 防呆
                           // #ifdef DEBUG_SQLITE
    std::cout << "SLT_CB pgseq=" << pgseq << " prod_id=" << prod.m_prod_id
              << " raw_spread=" << spread_raw << " scale=" << scale
              << " slt_spread(stored)=" << prod.m_slt_spread << " acc_qnty=" << prod.m_acc_qnty
              << std::endl;
    // #endif
    return 0;
  };
  rc = sqlite3_exec_printf(dbh, SQL_SLT, callback_SLT, &lctxSLT, &zErrMsg, grpid);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() (SLT) return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    if (zErrMsg) {
      sqlite3_free(zErrMsg);
      zErrMsg = nullptr;
    }
  }

  const char *SQLEnd = "END;";
  sqlite3_exec(dbh, SQLEnd, NULL, 0, &zErrMsg); // 容錯忽略錯誤
  sqlite3_close(dbh);
  cout << "(OPT) Successfully loaded " << m_prod_cnt << " opt products" << endl;

  // 產出 stovol_opt_prod_info.log 以檢視所有商品欄位
  {
    std::ofstream ofs(build_log_path("stovol_opt_prod_info.log"), std::ios::out | std::ios::trunc);
    if (ofs.is_open()) {
      ofs << "# Dump all option products after load_prod_opt()\n";
      ofs << "# total=" << m_prod_cnt << " (含 dummy? index0 is dummy)\n";
      for (size_t i = 0; i < m_prod.size(); ++i) {
        const Product &p = m_prod[i];
        char show_subtype = p.m_subtype ? p.m_subtype : '.';
        ofs << std::fixed << "SEQ=" << i << " PROD_ID=" << p.m_prod_id
            << " KIND=" << p.m_pdk_kind_id
            << " PDK_PARAM_KEY=" << (p.m_pdk_param_key[0] ? p.m_pdk_param_key : "")
            << " PDK_ON_CODE=" << (p.m_pdk_on_code[0] ? p.m_pdk_on_code : "")
            << " SETTLE_DATE=" << p.m_settle_date << " END_DATE=" << p.m_end_date
            << " P03_RATE=" << std::setprecision(6) << p.m_p03_compound
            << " REMAIN_DAYS=" << p.m_remaining_days << " PC=" << p.m_pc_code
            << " STRIKE(int)=" << p.m_strike_price << " PROD_PREMIUM=" << p.m_prod_premium
            << " PDK_ORIG_IDX=" << p.m_pdk_orig_idx << " PDK_STOCK_ID=" << p.m_pdk_stock_id
            << " CONTRACT_DATE=" << p.m_contract_date << " PRICE_SCALE=" << p.m_price_scale
            << " SCALE=" << p.m_scale << " SLT_SPREAD=" << p.m_slt_spread
            << " ACC_QNTY=" << p.m_acc_qnty << " SUBTYPE=" << show_subtype
            << " SPREAD=" << p.m_spread_code << std::setprecision(2) << " GAP_MAX=" << p.m_gap_max
            << " exempt_days=" << p.m_dspar_exempt_days << '\n';
      }
    }
  }
  return 0;
}

#ifdef DEBUG_SQLITE
static int callback_PDK(void *ptr, int argc, char **argv, char **azColName)
#else
static int callback_PDK(void *ptr, __attribute__((unused)) int argc, char **argv,
                        __attribute__((unused)) char **azColName)
#endif
{
  // SQL: select PDK_KIND_ID, PDK_STOCK_ID, PDK_ORIG_PROD_IDX, PDK_PRICE_SCALE from PDK ...
  // argv[0]: PDK_KIND_ID
  // argv[1]: PDK_STOCK_ID
  // argv[2]: PDK_ORIG_PROD_IDX
  // argv[3]: PDK_PRICE_SCALE
  ProdInfo *pProdInfo = (ProdInfo *)ptr;

  char key[PDK_LEN + 1] = {0};
  if (argv[0]) {
    strncpy(key, argv[0], PDK_LEN);
    key[PDK_LEN] = 0;
  }

  pdk_hash_it_t it = pProdInfo->m_PutMap.find(key);
  Pdk *pPdk;
  if (it == pProdInfo->m_PutMap.end()) {
    pPdk = new Pdk(key);
    // PDK_STOCK_ID
    // Pdk 結構無 m_stock_id，若需存放 STOCK_ID 可自訂欄位，暫以 m_kind_id 取代
    if (argv[1])
      safecpy(pPdk->m_kind_id, argv[1]);
    // PDK_PRICE_SCALE
    if (argv[3]) {
      pPdk->m_price_scale = atoi(argv[3]);
      pPdk->m_scale = 1;
      for (int i = 0; i < pPdk->m_price_scale; i++)
        pPdk->m_scale *= 10;
    } else {
      pPdk->m_price_scale = 0;
      pPdk->m_scale = 1;
    }
    pProdInfo->m_PutMap[pPdk->getKey()] = pPdk;
  } else {
    pPdk = it->second;
    // 更新 stock_id/scale 如有新值
    if (argv[1] && argv[1][0])
      safecpy(pPdk->m_kind_id, argv[1]);
    if (argv[3]) {
      int new_price_scale = atoi(argv[3]);
      if (new_price_scale > 0 && pPdk->m_price_scale != new_price_scale) {
        pPdk->m_price_scale = new_price_scale;
        pPdk->m_scale = 1;
        for (int i = 0; i < pPdk->m_price_scale; ++i)
          pPdk->m_scale *= 10;
      }
    }
  }

#ifdef DEBUG_SQLITE
  cout << "callback_PDK " << argc << " col: ";
  for (int i = 0; i < argc; i++)
    cout << azColName[i] << "=" << (argv[i] ? argv[i] : "NULL") << "   ";
  cout << endl;
#endif

  // 另外填入 g_pdk_hash (使用簡版 pdk_t, key=PDK_KIND_ID)
  if (argv[0]) {
    std::string k(argv[0], PDK_LEN);
    // printf("[callback_PDK] build g_pdk_hash key='%s'\n", k.c_str());
    auto git = g_pdk_hash.find(k);
    if (git == g_pdk_hash.end()) {
      pdk_t obj;
      safecpy(obj.kind_id, k.c_str());
      if (argv[1])
        safecpy(obj.stock_id, argv[1]); // PDK_STOCK_ID
      // 新增: orig_prod_idx 欄位，需乘上 m_scale
      obj.m_price_scale = 0;
      obj.m_scale = 1;
      if (argv[3]) {
        obj.m_price_scale = atoi(argv[3]);
        obj.m_scale = 1;
        for (int i = 0; i < obj.m_price_scale; ++i)
          obj.m_scale *= 10;
      }
      obj.orig_prod_idx = 0;
      if (argv[2])
        obj.orig_prod_idx = atof(argv[2]) * obj.m_scale;
      g_pdk_hash[k] = obj;
    } else {
      if (argv[1] && argv[1][0])
        safecpy(git->second.stock_id, argv[1]);
      if (argv[3]) {
        int new_price_scale = atoi(argv[3]);
        if (new_price_scale > 0 && git->second.m_price_scale != new_price_scale) {
          git->second.m_price_scale = new_price_scale;
          git->second.m_scale = 1;
          for (int i = 0; i < git->second.m_price_scale; ++i)
            git->second.m_scale *= 10;
        }
      }
    }
  }
  return 0;
}

int callbackEx_PUT(__attribute__((unused)) void *ptr, int argc, char **argv,
                   __attribute__((unused)) char **azColName) {

  char key[PDK_LEN + 1]; // PDK_KIND_ID
  memcpy(key, argv[0], PDK_LEN);
  key[PDK_LEN] = 0;
  ProdInfo *pProdInfo = (ProdInfo *)ptr;

  pdk_hash_it_t it = pProdInfo->m_PutMap.find(key);
  Pdk *pPdk;
  if (it == pProdInfo->m_PutMap.end()) {
    pPdk = new Pdk(key);
    pPdk->m_price_scale = atoi(argv[4]);
    pPdk->m_scale = 1;
    for (int i = 0; i < pPdk->m_price_scale; i++)
      pPdk->m_scale *= 10;
    pProdInfo->m_PutMap[pPdk->getKey()] = pPdk;
  } else {
    pPdk = it->second;
  }

  if (pPdk->m_price_scale == -1) {
    cerr << "Err: pdk scale error for pdk[" << argv[0] << "]" << endl;
    return SQLITE_ABORT;
  }

  Put put;
  put.m_max = adjust2(atof(argv[1]) * pPdk->m_scale);
  put.m_min = adjust2(atof(argv[2]) * pPdk->m_scale);
  put.m_unit = adjust2(atof(argv[3]) * pPdk->m_scale);
  pPdk->m_put.push_back(put);

#ifdef DEBUG_SQLITE
  stringstream ss;
  ss << "callbackEx_PUT: ";
  for (int i = 0; i < argc; i++) {
    ss << azColName[i] << "=" << (argv[i] ? argv[i] : "NULL") << " ";
  }
  cout << ss.str() << endl;
#endif

  return 0;
}

//--------------------------------------------------------------------
// ProdInfo class implementation
//--------------------------------------------------------------------
ProdInfo::~ProdInfo() {
  // Clean up dynamically allocated Pdk objects
  for (auto it = m_PutMap.begin(); it != m_PutMap.end(); ++it) {
    delete it->second;
  }
  m_PutMap.clear();
}

//--------------------------------------------------------------------
// Pdk class implementation
//--------------------------------------------------------------------
Pdk::Pdk(char *kind_id) {
  memset(m_kind_id, 0, sizeof(m_kind_id));
  memcpy(m_kind_id, kind_id, PDK_LEN);
  m_price_scale = -1;
  m_scale = 0;
}

/**
 * @brief Load product information from SQLite database
 * @param grpid Group ID
 * @param pszFilename Database file path
 * @return 0 on success, non-zero on error
 */
int ProdInfo::load_prod(int grpid, const char *pszFilename) {
  static char *zErrMsg = 0;
  int rc = 0;
  sqlite3 *dbh;
  const char *SQL;

#ifdef DEBUG_SQLITE
  cout << "Loading SQLite file(" << pszFilename << ")" << endl;
#endif
  /* open sqlite DB */
  rc = sqlite3_open_v2(pszFilename, &dbh, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_open_v2() return: " << rc << endl;
    cout << "Can't open SQLite DB database: " << sqlite3_errmsg(dbh) << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "fetching load_prod data error.");
    return -1;
  }

  const char *SQLBegin = "BEGIN;";
  if (sqlite3_exec(dbh, SQLBegin, NULL, 0, &zErrMsg) != SQLITE_OK) {
    cout << "SQL = BEGIN; ErrorMsg: " << zErrMsg << endl;
  }

  /* OCF day */
  SQL =
      (char *)"select OCF_DATE, OCF_MOCF_DAY_COUNT,  "
              "substr(OCF_DATE,0,5) || '/' || substr(OCF_DATE,5,2) || '/' || substr(OCF_DATE,7,2), "
              "OCF_PREV_DATE "
              "from OCF ";

  rc = sqlite3_exec_printf(dbh, SQL, callback_OCF, &g_ocf_date, &zErrMsg);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "fetching OCF data error.");
    return -1;
  }

  // Clear existing products
  m_prod.clear();
  m_prodid_map.clear();
  m_prod_cnt = 0;

  /* 商品撈取 */
  /* prepare SQL statement */
  // 找出該商品最近月契約, 但股票型, 或調整型 可能沒有最近月 PROD_SPREAD_CODE
  // 沒有=1的, 先用 0 代替,因為目前不會算到
  // 週到期的契約要特別處理
  SQL = (char *)"select PROD_ID, PROD_PGSEQ, PROD_OSW_GRP, ifnull(PROD_SPREAD_CODE,1), "
                "ifnull(PDK_PRICE_SCALE, -1), "
                "PROD_PREMIUM, ifnull(PDK_ORIG_PROD_IDX, 0), PROD_END_DATE, "
                "ifnull(PRODLMT_RAISE_PRICE,2147483647) , ifnull(PRODLMT_FALL_PRICE,2147483647), "
                "case PDK_EXPIRY_TYPE when 'S'  then "
                " ifnull ((select PROD_PGSEQ from PROD where substr (PROD_ID,1,3) = "
                "PDK.PDK_KIND_ID and PROD_SPREAD_CODE = 1 and PROD_EXPIRE_CODE != 'Y'),0) "
                "else "
                " ifnull ((select PROD_PGSEQ from (select * from PROD,PDK where PDK_KIND_ID = "
                "substr (PROD_ID,1,3) and PDK_EXPIRY_TYPE ='S')T  "
                " where substr (T.PROD_ID,1,2) = substr (PDK.PDK_KIND_ID,1,2) and "
                "T.PROD_SPREAD_CODE = 1 and T.PROD_EXPIRE_CODE != 'Y'),0)  "
                "end, "
                "PDK.PDK_STOCK_ID, PDK.PDK_EXPIRY_TYPE, PDK.PDK_PARAM_KEY, PDK.PDK_MARKET_CLOSE, "
                "PDK.PDK_SUBTYPE, ifnull(PDK.PDK_UNDERLYING_MARKET,0), PROD_SETTLE_PRICE "
                "from PROD, PDK "
                "LEFT JOIN  PRODLMT ON PROD_ID = PRODLMT_PROD_ID and PRODLMT_PLIMIT_CODE = 1 "
                "where  PDK_KIND_ID = substr(PROD_ID,1,3) "
                "and PROD_GRP_ID = %d and PROD_EXPIRE_CODE != 'Y' "
                "order by PROD_GRP_ID, PROD_PGSEQ, PROD_SPREAD_CODE";

  /* execute the SQL statement */
  rc = sqlite3_exec_printf(dbh, SQL, callback_PROD, this, &zErrMsg, grpid);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "fetching product(callback_PROD) data error.");
    return -1;
  }

  /* PUT */
  SQL = (char *)" select PDK_KIND_ID, PUT_MAX, PUT_MIN, PUT_UNIT "
                ", ifnull(PDK_PRICE_SCALE, -1) PDK_PRICE_SCALE "
                " from PDK, PUT "
                " where PDK_PARAM_KEY=PUT_KIND_ID "
                " order by PDK_KIND_ID, PUT_MAX";
  /* execute the SQL statement */
  rc = sqlite3_exec_printf(dbh, SQL, callbackEx_PUT, this, &zErrMsg);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "fetching PUT data error.");
    return -1;
  }

  const char *SQLEnd = "END;";
  if (sqlite3_exec(dbh, SQLEnd, NULL, 0, &zErrMsg) != SQLITE_OK) {
    cout << "SQL = END; ErrorMsg: " << zErrMsg << endl;
  }

  sqlite3_close(dbh);

  cout << "Successfully loaded " << m_prod_cnt << " products" << endl;
  return 0;
}

/**
 * @brief Load PDK information from SQLite database
 * @param grpid Group ID
 * @param pszFilename Database file path
 * @return 0 on success, non-zero on error
 */
int ProdInfo::load_pdk(int grpid, const char *pszFilename) {
  static char *zErrMsg = 0;
  int rc = 0;
  sqlite3 *dbh;
  const char *SQL;

#ifdef DEBUG_SQLITE
  cout << "Loading SQLite file(" << pszFilename << ")" << endl;
#endif
  /* open sqlite DB */
  rc = sqlite3_open_v2(pszFilename, &dbh, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    cout << "sqlite3_open_v2() return: " << rc << endl;
    cout << "Can't open SQLite DB database: " << sqlite3_errmsg(dbh) << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "fetching load_pdk data error.");
    return -1;
  }

  const char *SQLBegin = "BEGIN;";
  if (sqlite3_exec(dbh, SQLBegin, NULL, 0, &zErrMsg) != SQLITE_OK) {
    cout << "SQL = BEGIN; ErrorMsg: " << zErrMsg << endl;
  }

  // Clear existing PDK data (using PUT map for now)
  for (auto it = m_PutMap.begin(); it != m_PutMap.end(); ++it) {
    delete it->second;
  }
  m_PutMap.clear();

  /* PDK data */
  SQL = (char *)"select PDK_KIND_ID, PDK_STOCK_ID, PDK_ORIG_PROD_IDX, PDK_PRICE_SCALE "
                "from PDK "
                "where PDK_EXPIRE_CODE <> 'Y' "
                "order by PDK_KIND_ID";

  /* execute the SQL statement */
  if (grpid == 0) {
    rc = sqlite3_exec(dbh, SQL, callback_PDK, this, &zErrMsg);
  } else {
    rc = sqlite3_exec_printf(dbh, SQL, callback_PDK, this, &zErrMsg, grpid);
  }

  if (rc != SQLITE_OK) {
    cout << "sqlite3_exec() return: " << rc << endl;
    cout << "ErrorMsg: " << zErrMsg << endl;
    sqlite3_close(dbh);
    TFX::Log::COUTMSG(STOVOL_ERROR, "fetching PDK data error.");
    return -1;
  }

  const char *SQLEnd = "END;";
  if (sqlite3_exec(dbh, SQLEnd, NULL, 0, &zErrMsg) != SQLITE_OK) {
    cout << "SQL = END; ErrorMsg: " << zErrMsg << endl;
  }

  sqlite3_close(dbh);

  cout << "Successfully loaded PDK data" << endl;
  return 0;
}

/**
 * @brief Check if a product is single product
 * @param pgseq Product sequence number
 * @return true if single product, false otherwise
 */
bool ProdInfo::isSingleProduct(int /*pgseq*/) {
  // Simple implementation - can be enhanced based on requirements
  return true;
}

/**
 * @brief Get PDK Put information
 * @param kindID Kind ID
 * @return Pointer to Pdk object or NULL
 */
Pdk *ProdInfo::GetPdkPut(std::string kindID) const {
  auto it = m_PutMap.find(kindID.c_str());
  if (it != m_PutMap.end()) {
    return it->second;
  }
  return NULL;
}

/**
 * @brief update price list of this product
 * @param bs buy/sell code, 0:buy 1:sell
 * @param price price
 * @param qty_changed the number of quantity changed
 * @return result, 1: success <0: error
 */
int Product::update_price_list(int bs, int price, int qty_changed) {
  int price_sign = 1;
  int k;

  if (bs == 0)
    price_sign = -1;
  else if (bs == 1)
    price_sign = 1;
  else
    return -1;
  k = price * price_sign;
  pr_map_t::iterator lb = m_price_list[bs].lower_bound(k);
  if (lb == m_price_list[bs].end() || (m_price_list[bs].key_comp()(k, lb->first))) {
    /* check quantity integrity */
    if (qty_changed <= 0) {
      // cout << "Error: Attempt to insert a new PriceNode with illegal " "quantity = " <<
      // qty_changed << endl;
      CINFOX << "Attempt to insert a new PriceNode with illegal quantity = " << qty_changed
             << ENDLX;
      return -1;
    }
    /* case of not found. we must insert a new node */
    PriceNode pnode;
    pnode.m_price = price;
    pnode.m_qty = qty_changed;
    lb = m_price_list[bs].insert(lb, pr_map_t::value_type(k, pnode));
  } else {
    /* case of found. update the node */
    lb->second.m_qty += qty_changed;
    int newqty = lb->second.m_qty;
    /* check quantity integrity
       if == 0 , then delete node
       if <0,  error something wrong. */
    if (newqty <= 0) {
      m_price_list[bs].erase(lb);
    }
    if (newqty < 0) {
      // cout << "Error: Illegal quantity after update. Qty = " << newqty << endl;
      CINFOX << "Illegal quantity after update. Qty = " << newqty << ENDLX;
      return -1;
    }
  }
  return 1;
}

/**
 * @brief 更新商品狀態
 * @param tag 流程標籤
 */
void Product::updateStatus(tag_flow_t &tag) {
  // 基本實作：更新商品的狀態相關欄位
  m_status_code = tag.TradeStatus;
  m_flow_grp_id = tag.grp_no; // 使用 grp_no 而不是 PROD_OSW_GRP

  // 根據不同狀態設定對應的旗標
  switch (tag.TradeStatus) {
  case 1: // ST_RECV
    m_bIsRecvStage = true;
    m_bIsInNormal = false;
    m_bIsInCompete = false;
    break;
  case 2: // ST_OPEN
    m_bIsInCompete = true;
    m_bIsRecvStage = false;
    m_bIsInNormal = false;
    break;
  case 3: // ST_NORMAL
    m_bIsInNormal = true;
    m_bIsRecvStage = false;
    m_bIsInCompete = false;
    break;
  default:
    m_bIsRecvStage = false;
    m_bIsInNormal = false;
    m_bIsInCompete = false;
    break;
  }
}

/**
 * @brief 更新商品旗標
 */
void Product::updateProductFlags() {
  switch (m_status) {
  case ST_NORMAL:
  case ST_OPEN_F:
    // 集合競價結束, 逐筆撮合開始
    m_bIsRecvStage = false;
    break;
  case ST_RECV:
    // 收單期間
    m_bIsRecvStage = true;
    break;
  case ST_BREAK:
    // 暫停交易
    m_bIsRecvStage = false;
    break;
  case ST_CLOSE:
    // 收盤
    m_bIsRecvStage = false;
    break;
  default:
    // 其他階段對商品flag沒有影響
    break;
  }
  return;
}

/**
 * @brief Get tick size for a kind ID
 * @param kindID Kind ID
 * @return Tick size or 0 if not found
 */
int ProdInfo::GetTick(std::string kindID) const {
  Pdk *pdk = GetPdkPut(kindID);
  if (pdk) {
    return pdk->m_tick_size;
  }
  return 0;
}

/**
 * @brief Dump top N levels of order book side to a string (for debugging)
 * @param side 0=buy 1=sell
 * @param N number of levels
 * @return formatted string: B1:price@qty,B2:price@qty,... 或 S1:...
 */
std::string Product::dump_top_levels(int side, int N) const {
  std::ostringstream oss;
  if (side != 0 && side != 1)
    return oss.str();
  int count = 0;
  if (side == 0) { // buy side
    for (auto it = m_price_list[0].begin(); it != m_price_list[0].end() && count < N;
         ++it, ++count) {
      if (count)
        oss << ",";
      oss << "B" << (count + 1) << ':' << it->second.m_price << '@' << it->second.m_qty;
    }
  } else { // sell side
    for (auto it = m_price_list[1].begin(); it != m_price_list[1].end() && count < N;
         ++it, ++count) {
      if (count)
        oss << ",";
      oss << "S" << (count + 1) << ':' << it->second.m_price << '@' << it->second.m_qty;
    }
  }
  return oss.str();
}

/**
 * @brief Stream output operator for ProdInfo
 */
std::ostream &operator<<(std::ostream &os, const ProdInfo &obj) {
  os << "ProdInfo: " << obj.m_prod_cnt << " products loaded";
  return os;
}

//--------------------------------------------------------------------
// 期貨最近月契約相關函數實作
//--------------------------------------------------------------------

/**
 * @brief SQLite callback 函數：載入期貨商品資訊
 */
extern "C" int callback_future_products(void *ptr, int argc, char **argv,
                                        __attribute__((unused)) char **azColName) {
  if (argc < 4)
    return -1;

  std::vector<FutureProductInfo> *pVector = static_cast<std::vector<FutureProductInfo> *>(ptr);

  std::string pdk_kind_id = argv[0] ? argv[0] : "";
  std::string prod_id = argv[1] ? argv[1] : "";
  int pgseq = argv[2] ? atoi(argv[2]) : 0;
  std::string settle_date = argv[3] ? argv[3] : "";

  if (!pdk_kind_id.empty() && pgseq > 0) {
    pVector->push_back(FutureProductInfo(pdk_kind_id, prod_id, pgseq, settle_date));

#ifdef DEBUG_SQLITE
    cout << "FUTProduct: " << pdk_kind_id << "," << prod_id << "," << pgseq << "," << settle_date
         << endl;
#endif
  }

  return 0;
}

/**
 * @brief 載入期貨最近月契約資訊
 * @param grpid 群組ID
 * @param pszFilename 資料庫檔案路徑
 * @param future_products 存儲所有期貨商品資訊的向量
 * @param nearest_month_map 存儲最近月契約對應的 map
 * @return 0 成功，非零失敗
 */
int load_nearest_month_contracts(int grpid, const char *pszFilename,
                                 std::vector<FutureProductInfo> &future_products,
                                 NearestMonthMap &nearest_month_map) {
  sqlite3 *dbh;
  char *zErrMsg = nullptr;
  int rc = 0;

  // 開啟資料庫
  rc = sqlite3_open_v2(pszFilename, &dbh, SQLITE_OPEN_READONLY, nullptr);
  if (rc != SQLITE_OK) {
    cout << "Error: Cannot open SQLite DB: " << sqlite3_errmsg(dbh) << endl;
    sqlite3_close(dbh);
    return -1;
  }

  // 簡單的 SQL：撈出所有符合條件的(期貨商品) DB: fut
  char sql_buf[1024];
  if (g_only_calc_tx) {
    // only TXF
    snprintf(sql_buf, sizeof(sql_buf),
             "SELECT PDK.PDK_KIND_ID, PROD.PROD_ID, PROD.PROD_PGSEQ, PROD.PROD_SETTLE_DATE "
             "FROM PROD "
             "JOIN PDK ON PDK.PDK_KIND_ID = substr(PROD.PROD_ID,1,3) "
             "WHERE PROD.PROD_GRP_ID = %d "
             "AND PROD.PROD_EXPIRE_CODE != 'Y' "
             "AND PDK.PDK_SUBTYPE = 'I' "
             "AND PDK.PDK_PARAM_KEY = 'TXF' "
             "AND PROD.PROD_ID like 'TXF%%' "
             "ORDER BY PDK.PDK_KIND_ID, PROD.PROD_SETTLE_DATE;",
             grpid);
  } else {
    // STF
    snprintf(sql_buf, sizeof(sql_buf),
             "SELECT PDK.PDK_KIND_ID, PROD.PROD_ID, PROD.PROD_PGSEQ, PROD.PROD_SETTLE_DATE "
             "FROM PROD "
             "JOIN PDK ON PDK.PDK_KIND_ID = substr(PROD.PROD_ID,1,3) "
             "WHERE PROD.PROD_GRP_ID = %d "
             "AND PROD.PROD_EXPIRE_CODE != 'Y' "
             "AND PDK.PDK_SUBTYPE = 'S' "
             "AND PDK.PDK_PARAM_KEY = 'STF' "
             "ORDER BY PDK.PDK_KIND_ID, PROD.PROD_SETTLE_DATE;",
             grpid);
  }

  // 清空之前的資料
  future_products.clear();

  // 執行 SQL
  rc = sqlite3_exec(dbh, sql_buf, callback_future_products, &future_products, &zErrMsg);

  if (rc != SQLITE_OK) {
    cout << "Error executing future products SQL: " << zErrMsg << endl;
    sqlite3_free(zErrMsg);
    sqlite3_close(dbh);
    return -1;
  }

  sqlite3_close(dbh);

  // 用程式邏輯找出每個商品的最近月契約
  nearest_month_map.clear();
  std::string current_kind = "";

  // 先計算會顯示多少筆最近月契約
  std::set<std::string> unique_kinds;
  for (const auto &product : future_products) {
    unique_kinds.insert(product.pdk_kind_id);
  }

  cout << "\nFutures Nearest Month Contract Table (PDK, PROD_ID, PROD_PGSEQ) ["
       << unique_kinds.size() << "]" << endl;
  cout << "==============================================================================" << endl;

  for (const auto &product : future_products) {
    if (product.pdk_kind_id != current_kind) {
      // 新的商品類別，這是最近月（因為已經按結算日期排序）
      nearest_month_map[product.pdk_kind_id] = product.pgseq;
      current_kind = product.pdk_kind_id;

      cout << product.pdk_kind_id << " " << product.prod_id << " pgseq:" << product.pgseq
           << " (settle: " << product.settle_date << ")" << endl;
    }
  }

  cout << "==============================================================================" << endl;
  cout << "Loaded " << nearest_month_map.size() << " nearest month contracts from "
       << future_products.size() << " total products." << endl;

  return 0;
}

// 盤中重啟時讀取 stovol_idx.dat 並更新 g_pdk_hash
void load_stovol_idx() {
  extern char g_NFS_Path_t[];
  std::string filename = std::string(g_NFS_Path_t) + "/stovol_idx.dat";
  std::ifstream infile(filename);
  if (!infile) {
    std::cerr << "[load_stovol_idx.dat] Cannot open " << filename << std::endl;
    return;
  }
  std::string line;
  while (std::getline(infile, line)) {
    std::istringstream iss(line);
    std::string key_str, dummy, price_str;
    // 解析: key_str(PDK/STO等) dummy(原本stock_id,不再用) price_str(現貨價字串)
    if (!(iss >> key_str >> dummy >> price_str))
      continue;
    // 移除 price_str 內的逗號
    price_str.erase(std::remove(price_str.begin(), price_str.end(), ','), price_str.end());
    double price = atof(price_str.c_str());
    auto it = g_pdk_hash.find(key_str);
    if (it != g_pdk_hash.end()) {
      int new_idx = static_cast<int>(price * it->second.m_scale + 0.5); // 四捨五入
      printf("[load_stovol_idx] key='%s' new_idx=%d\n", key_str.c_str(), new_idx);
      it->second.orig_prod_idx = new_idx;
    } else {
      printf("[load_stovol_idx] key='%s' not found in g_pdk_hash!\n", key_str.c_str());
    }
  }
  infile.close();
}
