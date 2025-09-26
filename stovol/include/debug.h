/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

#ifndef F_OPT_STOVOL_DEBUG_H_
#define F_OPT_STOVOL_DEBUG_H_

/**@file debug.h
 * @brief Debug function declarations for stovol
 * @author kaijun@taifex.com.tw
 */

//--------------------------------------------------------------
// Debug function declarations
//--------------------------------------------------------------

/**
 * @brief (FUT) 印出所有期貨商品的資訊（用於測試和除錯）
 */
void print_fut_prodinfo();

/**
 * @brief (FUT) 印出期貨 PDK 資訊（用於測試和除錯）
 */
void print_fut_pdk_info();

/**
 * @brief 印出SFD相關資訊（用於測試和除錯）
 */
void print_sfd_info();

/**
 * @brief 印出商品統計資訊（用於測試和除錯）
 */
void print_product_stats();

#endif // F_OPT_STOVOL_DEBUG_H_
