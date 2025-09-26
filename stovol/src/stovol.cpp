/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  stovol.cpp
 @brif stovol program
 */
#define APP_VERSION "v2025.10.10"
#include <iostream>
#include <sstream>
#include <thread> // for std::thread (fut/opt processing threads)

#include "../../inc/stovol_stats.h"
#include "LoggerProxy.h"
#include "commlib/inc/UnderlyMsg.h"
#include "commlib/inc/command.hpp"
#include "commlib/inc/llm_comm.h"
#include "commlib/inc/mcc.h"
#include "commlib/inc/safecpy.hpp"
#include "commlib/inc/tfxlog.h"
#include "env.h"
#include "llm.h"       // 新增: LLM callbacks 與 g_llm_commRx 定義
#include "mtfbuffer.h" // 加入 MTF_Buffer 宣告
#include "opt/inc/LogViewer.h"
#include "opt/inc/tradesysmode.h"
#include "prodinfo.h"

// forward declaration for load_stovol_idx
void load_stovol_idx();

#include <map>
#include <sqlite3.h>
#include <string>

#ifdef __GXX_EXPERIMENTAL_CXX0X__
#include <unordered_map>
#else
#ifdef __GNUC__
#include <ext/hash_map>
namespace std {
using __gnu_cxx::hash_map;
}
#else
#include <hash_map>
#endif
#endif

#define STF_TOPIC_NAME "REUTERS.STF.MARKETDATA" // STF類(日), 標的現貨中價 (125ms)

//--------------------------------------------------------------
// global variables
//--------------------------------------------------------------
// 預設不啟用 MCC；需以 -m 明確啟用
bool g_support_mcc = false;
TradeSystemMode g_tsmode;   // 假開obj
bool g_is_simulate = false; // 假開判定
int g_trade_kind = 0;       // env var TRADE_KIND, 0:日盤, 1:夜盤
int g_SYSTEM_TYPE = 0;      // OPT:10,11  FUT:20,21
// 新增：只跑選擇權模式 (命令列 -o)
bool g_opt_only = false;
extern bool g_market_end;
std::shared_ptr<spdlog::logger> g_ActiveLogger;
bool g_recover_mode = false; // 盤中重啟(判斷理論價格的 rule 不同)
bool g_close_mode = false;   // 收盤後取得現貨資料檔後，重新計算
int g_grpid = 0;
char g_LLM_ConfigFile[256];
char g_fut_DBFile[256];
char g_opt_DBFile[256];
char g_fut_match_p[256];
char g_opt_match_p[256];
char g_opt_tpc_file[256];    // 新增：TPC 檔案 (sqlite 或文字)
int g_opt_tpc_exec_time = 0; // 新增：TPC_EXEC_TIME (如 134500 -> 13:45:00)
// 新增：OPT 切換為 1 分鐘計算的時間 (收盤前30分鐘)，格式 HHMMSS (例如 13:15:00 -> 131500)
int g_opt_1min_switch_time = 0;
// 是否啟動 LLM (由 -l 指定, 預設 false)
bool g_enable_llm = false; // callback 與 g_llm_commRx 於 llm.cpp / llm.h 定義
static const char *STOCK_TOPIC_NAME = "UNDERLY.TWSE.STOCK"; // 與 rap030 相同命名

// 前向宣告：計算 OPT 1 分鐘切換時間
static inline void derive_opt_1min_switch_time();

// 全域統計：初始化後的商品數量 (FUT / OPT)
int fut_all_prod_count = 0;
int opt_all_prod_count = 0;

// OCF related global variables
char g_ocf_date[10 + 1];
char g_ocf_prev_date[10 + 1];
char g_ocf_date_slash[11];
uint32_t g_uocf_date = 0;
int g_ocf_mocf_day_count = 0;

//--------------------------------------------------------------
// MCC related variables
//--------------------------------------------------------------
stovol_stat_t g_STOVOLStat;
hb_shared_entry *g_mcc_handle = NULL; // mcc handle (改為正確型別)

//--------------------------------------------------------------
// SPD Logger related variables
//--------------------------------------------------------------
std::shared_ptr<spdlog::logger> g_spd_logger_stovol;

// g_detail_list.clear();

//--------------------------------------------------------------
// LLM related
//--------------------------------------------------------------
// LLMComm comm;
// LLMComm comm_LoggerSender;    // m_LoggerSender
// char g_member_name[64];       // member name
// char g_member_name2[64];      // member name
// rmmTopicT *g_topic_tomatch;   // topic ftprice->match
// rmmTopicT *g_topic_tologger;  // topic ftprice->logger
// int g_msg_received = 0;
// bool g_is_sync_completed = false;
// bool g_has_processed_message = false;
// bool g_is_start_sending_ib = false;
// bool g_REPORT_OUTPUT = false;  // 當收到 MEX, REPORT_OUTPUT 紀錄1秒 //--
// 測試報告使用

//--------------------------------------------------------------
// App data
//--------------------------------------------------------------
LogViewer g_lv_fut, g_lv_opt;
ProdInfo g_fut_prodbook; // storage for fut product information
ProdInfo g_opt_prodbook; // storage for opt product information
char g_ProgName[] = "stovol";
int g_ProductCount = 0;

// 最近月契約資訊
NearestMonthMap g_nearest_month_map; // PDK_KIND_ID -> PGSEQ

// 全域變數：存儲所有期貨商品資訊
std::vector<FutureProductInfo> g_future_products;
MTF_Buffer *g_opt_pMTFBuffer = NULL;
MTF_Buffer *g_fut_pMTFBuffer = NULL;
// transaction buffer (for potential future per-transaction processing like ftprice)
char *g_transaction = NULL;              // data buffer for match transaction
int g_max_transaction_buf_len = 5000000; // initial buffer size (align with ftprice family)

// 撮合處理相關 atomic 狀態變數定義
std::atomic<bool> g_fut_thread_running{false};
std::atomic<bool> g_opt_thread_running{false};
std::atomic<bool> g_fut_market_started{false};
std::atomic<bool> g_opt_market_started{false};
std::atomic<bool> g_all_market_closed{false};

std::atomic<uint64_t> g_fut_tprice_header_count{0};
std::atomic<uint64_t> g_fut_last_15min_count{0};
std::atomic<uint64_t> g_fut_last_1min_count{0};
std::atomic<uint32_t> g_fut_current_processing_id{0};
std::atomic<bool> g_fut_is_after_1330{false};

std::atomic<uint64_t> g_opt_tprice_header_count{0};
std::atomic<uint64_t> g_opt_last_15min_count{0};
std::atomic<uint64_t> g_opt_last_1min_count{0};
std::atomic<uint32_t> g_opt_current_processing_id{0};
std::atomic<bool> g_opt_is_after_1330{false};
char g_LOG_path[128];
char g_ETL_path[128];
char g_ETL_AM_OUT_path[128];
char g_CROSS_NFS_Path[128]; // OPT vix (TPRICE_VIX_Reach_Notice)
char g_NFS_Path[128];
char g_NFS_Path_t[128];
char g_ETL_DB_SECOND_path[128];

//--------------------------------------------------------------
// Function
//--------------------------------------------------------------

// 簡易 MCC 心跳寫入：目前僅遞增 CalcBasePriceCount 並寫出整個 g_STOVOLStat 結構
static inline void write_mcc() {
  if (g_mcc_handle != NULL) {
    hb_write(g_mcc_handle, (char *)&g_STOVOLStat);
  }
}

// 動態調整 g_transaction 緩衝大小 (參考 ftprice 系列用法，預留擴充)
static inline bool resize_transaction_buffer(int new_len) {
  if (new_len <= g_max_transaction_buf_len)
    return true; // 已足夠
  char *new_buf = (char *)realloc(g_transaction, new_len);
  if (!new_buf) {
    CINFOX << "Failed to realloc g_transaction to size=" << new_len << ENDLX;
    return false;
  }
  // 將新增區域清 0
  memset(new_buf + g_max_transaction_buf_len, 0, new_len - g_max_transaction_buf_len);
  g_transaction = new_buf;
  CINFOX << "Resized g_transaction buffer from " << g_max_transaction_buf_len << " to " << new_len
         << ENDLX;
  g_max_transaction_buf_len = new_len;
  return true;
}

void initialize_fut_data() {
  /* Initialize LogViewer */
  g_lv_fut.Initialize(g_fut_DBFile);
  COUT.TPRINT("LogViewer initialize successfully.\n");

  /* Initialize product data */
  int rc = 0;
  COUTX << "<Load PROD>" << ENDLX;

  rc = g_fut_prodbook.load_prod(g_grpid, g_fut_DBFile);
  if (rc != 0) {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "Load product info fail, program will exit!");
#endif
    exit(1);
  }
  COUTX << "</Load PROD>" << ENDLX;
  COUT.TPRINT("Load product info successfully.\n");

  COUTX << "<Load PDK>" << ENDLX;
  rc = g_fut_prodbook.load_pdk(g_grpid, g_fut_DBFile);
  if (rc != 0) {
    COUTX << "Error: Load PDK info fail, program will exit!" << ENDLX;
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "Load PDK info fail, program will exit!");
#endif
    exit(1);
  }
  COUTX << "</Load PDK>" << ENDLX;
  COUT.TPRINT("Load PDK info successfully.\n");

  COUTX << "<Load Nearest Month Contracts>" << ENDLX;
  rc = load_nearest_month_contracts(g_grpid, g_fut_DBFile, g_future_products, g_nearest_month_map);
  if (rc != 0) {
    COUTX << "Error: Load nearest month contracts fail, program will exit!" << ENDLX;
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "Load nearest month contracts fail, program will exit!");
#endif
    exit(1);
  }
  COUTX << "</Load Nearest Month Contracts>" << ENDLX;
  COUT.TPRINT("Load nearest month contracts successfully.\n");

  // 初始化 MTF_Buffer (期貨 / 選擇權)
  if (!g_fut_pMTFBuffer) {
    g_fut_pMTFBuffer = new MTF_Buffer(g_grpid, &g_fut_prodbook, &g_lv_fut, g_fut_match_p);
    CINFOX << "Initialized FUT MTF_Buffer with file: " << g_fut_match_p << ENDLX;
  }
  // 注意：這裡不要提早建立 g_opt_pMTFBuffer，因為選擇權產品資料 (g_opt_prodbook)
  // 尚未載入，提早建立可能造成內部配置/釋放不一致導致 free(): invalid pointer。
  // g_opt_pMTFBuffer 僅在 initialize_opt_data()（完成載入後）建立。

  // 配置 transaction buffer (預留: 若後續需要逐筆處理可直接使用)
  if (!g_transaction) {
    g_transaction = (char *)malloc(g_max_transaction_buf_len);
    if (!g_transaction) {
      COUTX << "Error: Can't allocate memory for g_transaction buffer." << ENDLX;
#ifdef USE_MSG
      TFX::Log::COUTMSG(STOVOL_ERROR, "Error: Can't allocate memory for g_transaction buffer.");
#endif
      exit(1);
    }
    memset(g_transaction, 0, g_max_transaction_buf_len);
    CINFOX << "Allocated g_transaction buffer size=" << g_max_transaction_buf_len << ENDLX;
  }
  // 防止編譯器因目前尚未實際呼叫 resize 而產生未使用警告
  (void)resize_transaction_buffer;

  // 統計期貨商品數
  fut_all_prod_count = static_cast<int>(g_fut_prodbook.m_prod.size());
  COUTX << "fut_all_prod_count = " << fut_all_prod_count << ENDLX;
  if (fut_all_prod_count == 0) {
    COUTX << "Warning: fut_all_prod_count is 0, program will exit!" << ENDLX;
    exit(1);
  }
}

void initialize_opt_data() {
  g_lv_opt.Initialize(g_opt_DBFile);
  COUT.TPRINT("(OPT) LogViewer initialize successfully.\n");
  int rc = 0;
  COUTX << "<Load OPT PROD>" << ENDLX;
  rc = g_opt_prodbook.load_prod_opt(g_grpid, g_opt_DBFile);
  if (rc != 0) {
    COUTX << "Error: (OPT) Load product info fail, program will exit!" << ENDLX;
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "(OPT) Load product info fail, program will exit!");
#endif
    exit(1);
  }
  COUTX << "</Load OPT PROD>" << ENDLX;
  COUT.TPRINT("(OPT) Load product info successfully.\n");

  COUTX << "<Load OPT PDK>" << ENDLX;
  rc = g_opt_prodbook.load_pdk(g_grpid, g_opt_DBFile);
  if (rc != 0) {
    COUTX << "Error: (OPT) Load PDK info fail, program will exit!" << ENDLX;
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_ERROR, "(OPT) Load PDK info fail, program will exit!");
#endif
    exit(1);
  }
  COUTX << "</Load OPT PDK>" << ENDLX;
  COUT.TPRINT("(OPT) Load PDK info successfully.\n");

  if (g_recover_mode) {
    load_stovol_idx();
    CINFOX << "(OPT -r) read stovol_idx.dat finished" << ENDLX;
  }

  // 簡易列印 STC 目標商品統計 (波動率計算標的)
  if (!g_opt_prodbook.m_opt_vol_pgseq.empty()) {
    CINFOX << "(OPT) STC products count=" << g_opt_prodbook.m_opt_vol_pgseq.size() << ENDLX;
    int show = std::min<int>(5, g_opt_prodbook.m_opt_vol_pgseq.size());
    for (int i = 0; i < show; ++i) {
      int pg = g_opt_prodbook.m_opt_vol_pgseq[i];
      if (pg > 0 && pg < (int)g_opt_prodbook.m_prod.size()) {
        CINFOX << "  STC[#" << i << "] pgseq=" << pg
               << " prod_id=" << g_opt_prodbook.m_prod[pg].m_prod_id
               << " pc=" << g_opt_prodbook.m_prod[pg].m_pc_code
               << " strike=" << g_opt_prodbook.m_prod[pg].m_strike_price << ENDLX;
      }
    }
  } else {
    CINFOX << "(OPT) STC products count=0" << ENDLX;
  }

  if (!g_opt_pMTFBuffer) {
    g_opt_pMTFBuffer = new MTF_Buffer(g_grpid, &g_opt_prodbook, &g_lv_opt, g_opt_match_p);
    CINFOX << "Initialized OPT MTF_Buffer with file: " << g_opt_match_p << ENDLX;
  }
  if (!g_transaction) {
    g_transaction = (char *)malloc(g_max_transaction_buf_len);
    if (!g_transaction) {
      COUTX << "Error: (OPT) Can't allocate memory for g_transaction buffer." << ENDLX;
#ifdef USE_MSG
      TFX::Log::COUTMSG(STOVOL_ERROR, "(OPT) Can't allocate memory for g_transaction buffer.");
#endif
      exit(1);
    }
    memset(g_transaction, 0, g_max_transaction_buf_len);
    CINFOX << "(OPT) Allocated g_transaction buffer size=" << g_max_transaction_buf_len << ENDLX;
  }

  // 統計選擇權商品數
  opt_all_prod_count = static_cast<int>(g_opt_prodbook.m_prod.size());
  COUTX << "opt_all_prod_count = " << opt_all_prod_count << ENDLX;
  if (opt_all_prod_count == 0) {
    COUTX << "Warning: opt_all_prod_count is 0, program will exit!" << ENDLX;
    exit(1);
  }
}

// --------------------------------------------------------------
// 讀取 OPT TPC_EXEC_TIME
// --------------------------------------------------------------
static int sqlite_callback_int(void *user, int argc, char **argv, char **colname) {
  (void)argc;
  (void)colname; // 未使用參數抑制警告
  if (argc > 0 && argv[0]) {
    *reinterpret_cast<int *>(user) = atoi(argv[0]);
  }
  return 0;
}

void load_opt_tpc_exec_time() {
  if (g_opt_tpc_file[0] == '\0') {
    CINFOX << "[TPC] 未提供 OPT TPC 檔案參數, g_opt_tpc_exec_time 保持 0" << ENDLX;
    derive_opt_1min_switch_time();
    return;
  }

  // 嘗試以 sqlite 開啟 (若失敗再嘗試純文字解析)
  sqlite3 *dbh = nullptr;
  int rc = sqlite3_open(g_opt_tpc_file, &dbh);
  if (rc == SQLITE_OK) {
    const char *SQL =
        "select TPC_EXEC_TIME from TPC where TPC_OSW_GRP='1' and TPC_OPEN_SWITCH='100' limit 1";
    char *zErrMsg = nullptr;
    int value = 0;
    rc = sqlite3_exec(dbh, SQL, sqlite_callback_int, &value, &zErrMsg);
    if (rc == SQLITE_OK && value > 0) {
      g_opt_tpc_exec_time = value;
      CINFOX << "[TPC] 從 sqlite 載入 TPC_EXEC_TIME=" << g_opt_tpc_exec_time << " ("
             << g_opt_tpc_file << ")" << ENDLX;
      derive_opt_1min_switch_time();
      sqlite3_close(dbh);
      return;
    } else {
      CINFOX << "[TPC][WARN] sqlite 讀取失敗 rc=" << rc << " err=" << (zErrMsg ? zErrMsg : "")
             << ", 將嘗試純文字解析" << ENDLX;
      if (zErrMsg)
        sqlite3_free(zErrMsg);
    }
    sqlite3_close(dbh);
  } else {
    CINFOX << "[TPC][WARN] 無法以 sqlite 開啟檔案: " << g_opt_tpc_file << ", 將嘗試純文字解析"
           << ENDLX;
  }

  // 純文字解析: 尋找第一個 6~8 位數字 (可能格式: TPC_EXEC_TIME=134500 或 CSV 欄位)
  std::ifstream ifs(g_opt_tpc_file);
  if (!ifs.is_open()) {
    CINFOX << "[TPC][ERROR] 開啟 TPC 檔案失敗: " << g_opt_tpc_file << ENDLX;
    return;
  }
  std::string line;
  while (std::getline(ifs, line)) {
    // 去除註解
    auto pos_sharp = line.find('#');
    if (pos_sharp != std::string::npos)
      line = line.substr(0, pos_sharp);
    // Trim
    auto trim = [](std::string &s) {
      while (!s.empty() && isspace((unsigned char)s.front()))
        s.erase(s.begin());
      while (!s.empty() && isspace((unsigned char)s.back()))
        s.pop_back();
    };
    trim(line);
    if (line.empty())
      continue;
    // 尋找數字
    std::string digits;
    for (char c : line) {
      if (isdigit((unsigned char)c))
        digits.push_back(c);
      else if (!digits.empty())
        break;
    }
    if (digits.size() >= 6 && digits.size() <= 8) {
      int val = atoi(digits.c_str());
      if (val > 0) {
        g_opt_tpc_exec_time = val;
        CINFOX << "[TPC] 從文字檔解析 TPC_EXEC_TIME=" << g_opt_tpc_exec_time << " ("
               << g_opt_tpc_file << ")" << ENDLX;
        derive_opt_1min_switch_time();
        return;
      }
    }
  }
  CINFOX << "[TPC][WARN] 未能解析出 TPC_EXEC_TIME, 使用預設 0" << ENDLX;
  derive_opt_1min_switch_time();
}

// 載入 g_opt_tpc_exec_time 後計算 g_opt_1min_switch_time (收盤前30分鐘)
static inline void derive_opt_1min_switch_time() {
  if (g_opt_tpc_exec_time <= 0) {
    g_opt_1min_switch_time = 0;
    CINFOX << "[TPC] g_opt_tpc_exec_time=0, 無法計算 1 分鐘切換時間" << ENDLX;
    return;
  }
  int hh = g_opt_tpc_exec_time / 10000;
  int mm = (g_opt_tpc_exec_time / 100) % 100;
  int ss = g_opt_tpc_exec_time % 100;
  int total_sec = hh * 3600 + mm * 60 + ss;
  int switch_sec = total_sec - 30 * 60; // 收盤前 30 分鐘
  if (switch_sec < 0) {
    g_opt_1min_switch_time = 0; // 不合理，設 0 表示無效
  } else {
    int sh = switch_sec / 3600;
    int sm = (switch_sec % 3600) / 60;
    int ss2 = switch_sec % 60;
    g_opt_1min_switch_time = sh * 10000 + sm * 100 + ss2;
  }
  CINFOX << "[TPC] OPT 1分鐘切換時間 (收盤前30分鐘) = " << g_opt_1min_switch_time
         << " (收盤=" << g_opt_tpc_exec_time << ")" << ENDLX;
}

//--------------------------------------------------------------
// Main function
//--------------------------------------------------------------
int main(int argc, char *argv[]) {
  COUTX << "argc=" << argc << ENDLX;
  COUTX << "argv=" << *argv << ENDLX;
  if (argc == 1) {
    print_usage();
  }
  COUT.SetProgName(argv[0]);
  COUT.SetDuplicateMode(true);
  TFX::Log::Logger::Instance().InitLogger("sto"); // TFX::Log  一定要做 InitLogger
  TFX::Log::Logger::Instance().SetDuplicateMode4COUTMSG(true);
  TFX::Log::COUT("Create Date[{}] Time[{}] Version[{}]", __DATE__, __TIME__, APP_VERSION);
  g_ActiveLogger = TFX::Log::Logger::Instance().GetCoutLogger();
  g_tsmode.checkSimulate();
  g_is_simulate = g_tsmode.getSimulateMode();
  parse_command_line(argc, argv);

  get_env_variable();
  print_global_variable();

  if (g_support_mcc) {
    register_mcc();
  }
  initialize_SPDLogger(); // Initialize SPDLogger

  char cmd[512];
  cmd[0] = 0x0;
  for (int i = 0; i < argc; i++) {
    safecat(cmd, argv[i]);
    strcat(cmd, " ");
  }
  CINFOX << cmd << " Starting..." << ENDLX;

  if (!g_opt_only) {
    CINFOX << "[MODE] Full mode: 初始化期貨與選擇權資料" << ENDLX;
    initialize_fut_data();
  } else {
    CINFOX << "[MODE] OPT-only: 跳過期貨初始化" << ENDLX;
  }
  initialize_opt_data();
  // 讀取 TPC_EXEC_TIME (於 opt 商品載入後即可)
  load_opt_tpc_exec_time();

  // ----------------------------------------------------------
  // 初始化 LLM (若 -l 指定)
  // 參考 rap030.cp 初始化流程: LLMLoadConfig -> LLMParseConfigParameters -> set callbacks ->
  // LLMRxInit -> LLMParseRxTopicParameters -> setRxTopicsCallback -> LLMCreateRxTopics ->
  // JoinMulticastGroup 實作搬移到 llm.cpp, 這裡僅呼叫 API
  // ----------------------------------------------------------
  if (g_enable_llm) {
    CINFOX << "[LLM] 啟動 LLM 現貨資料串接, config=" << g_LLM_ConfigFile << ENDLX;
    const char *memberName = "stovol"; // 參考需求說明指定 memberName
    try {
      g_llm_commRx.LLMLoadConfig(g_LLM_ConfigFile, memberName);
      g_llm_commRx.LLMParseConfigParameters();
      g_llm_commRx.setRxOnEventCallback(on_event);
      g_llm_commRx.setOnLogEventCallback(on_log_event);
      g_llm_commRx.LLMRxInit();
      g_llm_commRx.LLMParseRxTopicParameters();
      g_llm_commRx.setRxTopicsCallback(STOCK_TOPIC_NAME, on_event, stock_on_packet);
      g_llm_commRx.LLMCreateRxTopics();
      g_llm_commRx.JoinMulticastGroup();
      CINFOX << "[LLM] 初始化完成並加入 multicast group" << ENDLX;
    } catch (const std::exception &ex) {
      COUTX << "[LLM][ERROR] 初始化失敗: " << ex.what() << ENDLX;
    }
  } else {
    CINFOX << "[LLM] 未啟動 (未指定 -l)" << ENDLX;
  }

  // 啟動處理 threads
  extern void opt_match_processing_thread();
  extern void fut_match_processing_thread();
  std::thread opt_thread(opt_match_processing_thread);
  std::thread fut_thread; // 預設不啟動, 依模式決定
  if (!g_opt_only) {
    fut_thread = std::thread(fut_match_processing_thread);
  }

  // 若啟用 MCC，啟動心跳監控 thread: 每秒印出 Heard Beat
  std::atomic<bool> heartbeat_running{true};
  std::thread heartbeat_thread;
  if (g_support_mcc) {
    heartbeat_thread = std::thread([&]() {
      while (heartbeat_running.load(std::memory_order_relaxed)) {
        CINFOX << " Heard Beat" << ENDLX;
        // 遞增統計後呼叫 write_mcc()
        g_STOVOLStat.CalcBasePriceCount++;
        write_mcc();
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
  }

  if (opt_thread.joinable())
    opt_thread.join();
  if (fut_thread.joinable())
    fut_thread.join();

  // 停止心跳 thread
  heartbeat_running.store(false, std::memory_order_relaxed);
  if (heartbeat_thread.joinable())
    heartbeat_thread.join();

  CINFOX << (g_opt_only ? "OPT thread finished" : "FUT & OPT thread finished") << ENDLX;

  // 關閉 MCC 心跳
  if (g_mcc_handle != NULL) {
    hb_close(g_mcc_handle);
    g_mcc_handle = NULL;
    CINFOX << "MCC heartbeat closed" << ENDLX;
  }

  // 若有啟動 LLM，進行優雅停用
  if (g_enable_llm) {
    // 目前僅使用接收端 g_llm_commRx，呼叫其停止方法
    if (g_llm_commRx.isRxInit ||
        g_llm_commRx
            .isTierInit) { // 需在 llm_comm.h 中成員為 public 才可；若非 public 可改提供 wrapper
      g_llm_commRx.LLMStop();
      CINFOX << "[LLM] stovol stopped (rx stopped)" << ENDLX;
    } else {
      CINFOX << "[LLM] requested stop but LLM not initialized" << ENDLX;
    }
  }

  return 0;
}

// LLM 回呼實作已移至 llm.cpp
