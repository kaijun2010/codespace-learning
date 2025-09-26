/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

/**@file  env.h
 @brief Environment and global variable handling for stovol
 */

#ifndef OPT_STOVOL_ENV_H
#define OPT_STOVOL_ENV_H

/**
 * @brief 取得環境變數
 * 實作環境變數取得邏輯，例如：取得交易模式、日誌等級等環境設定
 */
void get_env_variable();

/**
 * @brief 印出全域變數
 * 實作全域變數印出邏輯，例如：印出程式設定、模式等資訊
 */
void print_global_variable();

/**
 * @brief 印出程式使用說明
 * 顯示程式的命令列參數和使用方式
 */
void print_usage();

/**
 * @brief 註冊 MCC (Message Control Center)
 * 初始化 MCC 連線和設定心跳機制
 */
void register_mcc();

/**
 * @brief 初始化 SPD Logger
 * 建立 stovol 專用的 spdlog 檔案記錄器
 */
void initialize_SPDLogger();

/**
 * @brief 解析命令列參數
 * 處理程式啟動時的命令列選項和參數
 */
void parse_command_line(int argc, char **argv);

// 新增：只計算 TXO 商品 (-t)
extern bool g_only_calc_tx;

// 啟動 LLM 功能旗標 (命令列 -l 啟動, 預設關閉)
extern bool g_enable_llm;

// 新增：TPC 收盤時間相關全域 (於 stovol.cpp 定義)
extern int g_opt_tpc_exec_time;  // 例如 134500 -> 13:45:00
extern char g_opt_tpc_file[256]; // TPC 檔案路徑
void load_opt_tpc_exec_time();   // 載入函式 (stovol.cpp 實作)

// 取得日誌輸出目錄 (若環境變數 STOVOL_LOG_DIR 存在則使用, 否則使用當前工作目錄".")
const char *get_log_dir();
// 依據檔名產生含路徑完整檔案路徑 (內部使用 static buffer, 非執行緒安全)
const char *build_log_path(const char *filename);

// ------------------------------------------------------------
// 產品 / 盤別 判斷巨集 (需在 stovol.cpp 定義對應全域變數)
// g_product_type[0] 期待為 'F' (Futures) 或 'O' (Options)
extern char g_product_type[]; // 若尚未定義請於 stovol.cpp 補上實際內容來源
extern int g_trade_kind;      // 0: 日盤, 1: 夜盤 (stovol.cpp 已有定義)

#define IS_FUTURES() (g_product_type[0] == 'F')
#define IS_OPTIONS() (g_product_type[0] == 'O')
#define IS_DAY() (g_trade_kind == 0)
#define IS_NIGHT() (g_trade_kind == 1)

#endif // OPT_STOVOL_ENV_H
