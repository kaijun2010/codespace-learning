#ifndef OPT_STOVOL_LLM_H
#define OPT_STOVOL_LLM_H

#include "commlib/inc/UnderlyMsg.h"
#include "commlib/inc/llm_comm.h"
#include <byteswap.h>
#include <iostream>
#include <rmmCapi.h>

// 提供給 stovol.cpp 使用的外部物件與 callback 宣告
extern LLMCommRx g_llm_commRx; // 在 llm.cpp 定義

// Callback 函式 (供 LLMCommRx 註冊)
void on_event(const rmmEvent *event_r, void *user);
void on_log_event(const llmLogEvent_t *log_event, void *user);
// 接收現貨『個股價格』(STOCK_TOPIC_NAME) packet handler (公司標準寫法)
void stock_on_packet(rmmPacket *packet, void *user __attribute__((unused)));

// 取得最新的 Fc 價格（現貨指數價格）
double get_latest_fc(const std::string &stock_id);

// 主要處理函式：直接吃 rmmRxMessage (llm.cpp 定義)
void process_stock_message(rmmRxMessage *inMsg);

#endif // OPT_STOVOL_LLM_H
