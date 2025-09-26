/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  env.cpp
 @brief Environment and global variable handling for stovol
 */

#include "commlib/inc/command.hpp"
#include "commlib/inc/mcc.h"
#include "commlib/inc/tfxlog.h"
#include <iostream>
#include <sstream>
#include <unistd.h> // for getopt

#include "../../inc/stovol_msg.h"
#include "../../inc/stovol_stats.h"
#include "LoggerProxy.h"
#include "env.h"
#include "opt/inc/tradesysmode.h"

// 外部全域變數聲明 (定義在 stovol.cpp)
extern TradeSystemMode g_tsmode;
extern bool g_is_simulate;
extern char g_ProgName[];
extern int g_trade_kind;  // env var TRADE_KIND, 0:日盤, 1:夜盤
extern int g_SYSTEM_TYPE; // OPT:10,11  FUT:20,21
extern bool g_opt_only;   // 只跑選擇權模式 (-o)
extern char g_LOG_path[128];
extern char g_ETL_path[128];
extern char g_ETL_AM_OUT_path[128];
extern char g_NFS_Path[128];
extern char g_NFS_Path_t[128];
extern char g_ETL_DB_SECOND_path[128];
extern bool g_support_mcc;
extern bool g_recover_mode;
extern bool g_close_mode;
extern char g_LLM_ConfigFile[256];
extern char g_fut_DBFile[256];
extern char g_opt_DBFile[256];
extern char g_fut_match_p[256];
extern char g_opt_match_p[256];
extern int g_grpid;
extern stovol_stat_t g_STOVOLStat;
extern void *g_mcc_handle;
extern std::shared_ptr<spdlog::logger> g_spd_logger_stovol;
extern bool g_enable_llm;    // 新增：是否啟動 LLM (-l)
bool g_only_calc_tx = false; // 新增：只計算 TXO 商品 (-t)

// 環境變數取得
void get_env_variable() {
  /* get NFS_PATH_T file path */
  memset(&g_NFS_Path_t, 0x00, sizeof(g_NFS_Path_t));
  if (getenv("NFS_PATH_T") != NULL) {
    safecpy(g_NFS_Path_t, getenv("NFS_PATH_T"));
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_FAIL, "Please check setting of environment variable NFS_PATH_T!");
#endif
    TFX::Log::COUT("Error: Environment variable NFS_PATH_T not defined!");
    exit(1);
  }
  if (getenv("TRADE_KIND") != NULL) {
    g_trade_kind = atoi(getenv("TRADE_KIND"));
    if (g_trade_kind != 0 && g_trade_kind != 1) {
      TFX::Log::COUT("Error: Environment variable TRADE_KIND not defined!");
      exit(1);
    }
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(TRADE_KIND_ENVERR,
                      "Please check setting of environment variable TRADE_KIND!");
#endif
    COUTX << "Error: Environment variable TRADE_KIND not defined!" << ENDLX;
    exit(1);
  }

  /* get LOG_PATH file path */
  memset(&g_LOG_path, 0x00, sizeof(g_LOG_path));
  if (getenv("LOG") != NULL) {
    safecpy(g_LOG_path, getenv("LOG"));
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_FAIL, "Please check setting of environment variable LOG_PATH!");
#endif
    TFX::Log::COUT("Error: Environment variable LOG_PATH not defined!");
    exit(1);
  }

  /* get ETL_PATH file path */
  memset(&g_ETL_path, 0x00, sizeof(g_ETL_path));
  if (getenv("ETL_PATH") != NULL) {
    safecpy(g_ETL_path, getenv("ETL_PATH"));
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_FAIL, "Please check setting of environment variable ETL_PATH!");
#endif
    TFX::Log::COUT("Error: Environment variable ETL_PATH not defined!");
    exit(1);
  }

  /* get NFS_PATH file path */
  memset(&g_NFS_Path, 0x00, sizeof(g_NFS_Path));
  if (getenv("NFS_PATH") != NULL) {
    safecpy(g_NFS_Path, getenv("NFS_PATH"));
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_FAIL, "Please check setting of environment variable NFS_PATH!");
#endif
    TFX::Log::COUT("Error: Environment variable NFS_PATH not defined!");
    exit(1);
  }

  /* get NFS_PATH_T file path */
  memset(&g_NFS_Path_t, 0x00, sizeof(g_NFS_Path_t));
  if (getenv("NFS_PATH_T") != NULL) {
    safecpy(g_NFS_Path_t, getenv("NFS_PATH_T"));
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_FAIL, "Please check setting of environment variable NFS_PATH_T!");
#endif
    TFX::Log::COUT("Error: Environment variable NFS_PATH_T not defined!");
    exit(1);
  }

  /* get ETL_DB_SECOND_PATH file path */
  memset(&g_ETL_DB_SECOND_path, 0x00, sizeof(g_ETL_DB_SECOND_path));
  if (getenv("ETL_DB_SECOND_PATH") != NULL) {
    safecpy(g_ETL_DB_SECOND_path, getenv("ETL_DB_SECOND_PATH"));
  } else {
#ifdef USE_MSG
    TFX::Log::COUTMSG(STOVOL_FAIL, "Please check setting of environment variable "
                                   "ETL_DB_SECOND_PATH!");
#endif
    TFX::Log::COUT("Error: Environment variable ETL_DB_SECOND_PATH not defined!");
    exit(1);
  }
  CINFOX << "Environment variables loaded successfully." << ENDLX;
}

void print_global_variable() {
  // 實作全域變數印出邏輯
  // 例如：印出程式設定、模式等資訊
  std::ostringstream oss;
  oss << "=== Global Variables ===\n"
      << "Program: " << g_ProgName << "\n"
      << "Simulate Mode: " << g_tsmode.getSimulateMode() << "\n"
      << "g_is_simulate: " << g_is_simulate << "\n"
      << "g_support_mcc = " << g_support_mcc << "\n"
      << "g_recover_mode = " << g_recover_mode << "\n"
      << "g_close_mode = " << g_close_mode << "\n"
      << "g_trade_kind = " << g_trade_kind << "\n"
      << "g_LLM_ConfigFile = " << g_LLM_ConfigFile << "\n"
      << "g_opt_DBFile = " << g_opt_DBFile << "\n"
      << "g_opt_match_p = " << g_opt_match_p << "\n"
      << "g_fut_DBFile = " << g_fut_DBFile << "\n"
      << "g_fut_match_p = " << g_fut_match_p << "\n"
      << "g_enable_llm = " << (g_enable_llm ? 1 : 0) << "\n"
      << "g_opt_only = " << (g_opt_only ? 1 : 0) << "\n"
      << "g_only_calc_tx = " << (g_only_calc_tx ? 1 : 0) << "\n"
      << "=========================";

  COUTX << oss.str();
}

void print_usage() {
  COUTX << "Usage: " << g_ProgName
        << " [-drco] <grpid> <LLM_ConfigFile> <opt_DBFile> <fut_DBFile> <opt_match_p> "
           "<fut_match_p> <opt_tpc_file>\n"
        << "       (OPT-only) " << g_ProgName
        << " -o [-drc] <grpid> <LLM_ConfigFile> <opt_DBFile> <opt_match_p> <opt_tpc_file>\n"
        << "Options:\n"
        << "  -d: Disable MCC.\n"
        << "  -r: Recovery mode (盤中重啟).\n"
        << "  -c: Close mode (收盤後重新計算波動率曲線).\n"
        << "  -o: OPT-only mode (不載入/不處理期貨資料).\n"
        << "  -l: 啟動 LLM (現貨資料串接). 預設不啟動。\n"
        << "Parameters:\n"
        << "  grpid: Match group ID (1-9)\n"
        << "  LLM_ConfigFile: LLM configuration file path\n"
        << "  opt_DBFile: Option database file path\n"
        << "  fut_DBFile: Future database file path\n"
        << "  opt_match_p: Option match file path\n"
        << "  fut_match_p: Future match file path\n"
        << "  opt_tpc_file: OPT TPC 檔案 (sqlite 或文字)\n"
        << "  (OPT-only) 不需 fut_DBFile / fut_match_p 兩參數, 仍需 opt_tpc_file\n"
        << "g_is_simulate = " << g_tsmode.getSimulateMode() << std::endl
        << ENDLX;
  // exit(0);
}

void register_mcc() {
  int pgmID;
  int minor_pgmID;
  char tempstr[256];

  pgmID = PGM_ID_STOVOL; // 53

  minor_pgmID = g_grpid * 100 + 1; // 使用固定 instance_id = 1
  memset(&g_STOVOLStat, 0, sizeof(stovol_stat_t));

  g_mcc_handle = hb_open(0, pgmID, minor_pgmID, sizeof(stovol_stat_t));
  if (g_mcc_handle == NULL) {
    sprintf(tempstr, "hb_open(%d, %d, %d, %lu) returned %p.\n", 0, pgmID, minor_pgmID,
            sizeof(stovol_stat_t), (void *)g_mcc_handle);
    std::cout << tempstr << std::endl;
  } else {
    sprintf(tempstr, "Heartbeat registered. hb_open(%d, %d, %d, %lu) returned %p.\n", 0, pgmID,
            minor_pgmID, sizeof(stovol_stat_t), (void *)g_mcc_handle);
    std::cout << tempstr << std::endl;
  }
}

void initialize_SPDLogger() {
  std::string filename = g_LOG_path;
  std::string STOVOL;
  STOVOL = "stovol_stock_spdlog_";
  filename.append("/").append(STOVOL);
  char str_grpid[2 + 1];
  sprintf(str_grpid, "%d", g_grpid);
  filename.append(str_grpid);
  filename.append(".log");
  g_spd_logger_stovol =
      TFX::Log::Logger::Instance().CreateFileLogger("stovol_async", std::move(filename), false);
}

void parse_command_line(int argc, char **argv) {
  int ch;

  while ((ch = getopt(argc, argv, "drcmolt")) != EOF) {
    switch (ch) {
    case 'r': // 盤中重啟
      g_recover_mode = true;
      // 注意：load_stovol_idx() 必須在 g_pdk_hash 準備好後再呼叫！
      break;
    case 'c': // 收盤後取得現貨資料檔後,重新計算波動率曲線
      g_close_mode = true;
      break;
    case 'o': // 只跑選擇權 (OPT-only)
      g_opt_only = true;
      break;
    case 'm': // 啟用 MCC 支援 (原本語義相反，已修正)
      g_support_mcc = true;
      break;
    case 'l': // 啟動 LLM 功能
      g_enable_llm = true;
      break;
    case 't': // 只計算 TXO 商品
      g_only_calc_tx = true;
      break;
    default:
      std::cerr << "Invalid option -" << ch << std::endl;
#ifdef USE_MSG
      TFX::Log::COUTMSG(INVALID_OPTION, "Invalid option.");
#endif
      print_usage();
      exit(1);
    }
  }

  // validate input parameter - stovol 需要的參數數量
  // OPT-only: 需要 <grpid> <LLM_ConfigFile> <opt_DBFile> <opt_match_p> 共 4 個參數
  // Full-mode: 需要 <grpid> <LLM_ConfigFile> <opt_DBFile> <fut_DBFile> <opt_match_p> <fut_match_p>
  // 共 6 個參數
  int expected = g_opt_only ? 5 : 7; // 多一個 opt_tpc_file 參數
  if (!g_close_mode) {
    if (argc != expected + optind) {
      TFX::Log::COUT("Insufficient argument provided!");
      TFX::Log::COUT("Expected {} arguments, got {}", expected + optind, argc);
      print_usage();
      exit(1);
    }
  }

  // get command line param 1 match group id
  g_grpid = (signed char)atoi(argv[optind]);
  if (g_grpid <= 0 || g_grpid > 9) {
    std::cerr << "group id must in range 1-9" << std::endl;
    print_usage();
    exit(1);
  }

  safecpy(g_LLM_ConfigFile, argv[optind + 1]);
  safecpy(g_opt_DBFile, argv[optind + 2]);
  if (!g_opt_only) {
    // full mode
    safecpy(g_fut_DBFile, argv[optind + 3]);
    safecpy(g_opt_match_p, argv[optind + 4]);
    safecpy(g_fut_match_p, argv[optind + 5]);
    safecpy(g_opt_tpc_file, argv[optind + 6]);
  } else {
    // opt-only 模式下, 未提供期貨相關檔案, 清空避免後續誤用
    g_fut_DBFile[0] = '\0';
    g_fut_match_p[0] = '\0';
    safecpy(g_opt_match_p, argv[optind + 3]);
    safecpy(g_opt_tpc_file, argv[optind + 4]);
  }

  CINFOX << "Command line parameters parsed successfully" << ENDLX;
  CINFOX << "g_grpid=" << g_grpid << ENDLX;
  CINFOX << "g_LLM_ConfigFile=" << g_LLM_ConfigFile << ENDLX;
  CINFOX << "g_opt_DBFile=" << g_opt_DBFile << ENDLX;
  if (!g_opt_only)
    CINFOX << "g_fut_DBFile=" << g_fut_DBFile << ENDLX;
  CINFOX << "g_opt_match_p=" << g_opt_match_p << ENDLX;
  if (!g_opt_only)
    CINFOX << "g_fut_match_p=" << g_fut_match_p << ENDLX;
  CINFOX << "g_opt_tpc_file=" << g_opt_tpc_file << ENDLX;
  CINFOX << "g_opt_only=" << (g_opt_only ? 1 : 0) << ENDLX;

  return;
}

// 取得日誌輸出目錄: 優先 STOVOL_LOG_DIR, 若未設定則使用當前目錄.
const char *get_log_dir() {
  static char s_dir[512];
  static bool inited = false;
  if (!inited) {
    const char *env_dir = getenv("LOG"); // 改為使用 LOG 環境變數
    if (env_dir && *env_dir)
      safecpy(s_dir, env_dir);
    else
      safecpy(s_dir, ".");
    inited = true;
  }
  return s_dir;
}

// 建立完整日誌路徑 (非 thread-safe, 呼叫後立即使用)
const char *build_log_path(const char *filename) {
  static char s_path[1024];
  const char *dir = get_log_dir();
  size_t len = strlen(dir);
  if (len + 1 + strlen(filename) >= sizeof(s_path)) {
    // 長度過長, 簡單截斷
    snprintf(s_path, sizeof(s_path), "%s/%s", dir, filename);
  } else {
    sprintf(s_path, "%s/%s", dir, filename);
  }
  return s_path;
}
