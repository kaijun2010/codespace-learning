#include "llm.h"
#include "commlib/inc/safecpy.hpp"
#include "env.h"      // for IS_OPTIONS / IS_FUTURES macros
#include "prodinfo.h" // for pdk_t / g_pdk_hash

#include <ctime>
#include <memory>
#include <pthread.h>
#include <spdlog/logger.h>
#include <string>

// 來自 env.cpp 的 log 物件 extern 宣告
extern std::shared_ptr<spdlog::logger> g_spd_logger_stovol;

LLMCommRx g_llm_commRx; // 定義全域 LLM 接收物件

// 若主程式未定義 g_product_type，這裡提供預設 (可由 parse_command_line 覆寫)
#ifndef HAVE_G_PRODUCT_TYPE
char g_product_type[] = "O"; // 預設 Options，實務可依參數更新
#endif

void on_event(const rmmEvent *event_r, void *user) {
  (void)user;
  const char *evtType = nullptr;
  const char *log_level = nullptr;
  g_llm_commRx.parseRmmEvent(event_r, &evtType, &log_level);
  std::cout << g_llm_commRx.getTime() << " [LLM." << (log_level ? log_level : "")
            << "] on_event :evtType=" << (evtType ? evtType : "") << "|evtTypeID=" << event_r->type
            << "|T=" << (event_r->topic_name ? event_r->topic_name : "")
            << "|S=" << (event_r->source_addr ? event_r->source_addr : "") << "|P=" << event_r->port
            << "|Param0="
            << (event_r->event_params[0] ? (char *)event_r->event_params[0] : (char *)"")
            << std::endl;
}

void on_log_event(const llmLogEvent_t *log_event, void *user) {
  (void)user;
  struct tm pt_buf;
  struct tm *pt = &pt_buf;
  char timeStr[64];
  time_t timeInSec = (time_t)(log_event->timestamp / 1000);
  localtime_r(&timeInSec, pt);
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d.%03d", pt->tm_hour, pt->tm_min, pt->tm_sec,
           (int)(log_event->timestamp % 1000));
  std::cout << timeStr << " [LLM.] on_log_event : msg_key=" << log_event->msg_key
            << "|log_level=" << log_event->log_level << "|module=" << log_event->module
            << "|Param0="
            << (log_event->event_params[0] ? (char *)log_event->event_params[0] : (char *)"")
            << std::endl;
}

static inline int64_t ntohd64(int64_t v) { return (int64_t)bswap_64((uint64_t)v); }
// 公司既有程式常用 ntohd，這裡提供與 ntohd64 等價的包裝以避免未定義錯誤
static inline int64_t ntohd(int64_t v) { return ntohd64(v); }

// LLM 訊息處理互斥鎖 (避免多執行緒同時修改共享資料結構)
pthread_mutex_t g_process_message = PTHREAD_MUTEX_INITIALIZER;

// 取得最新的 Fc 價格（即 Product 的 m_pdk_orig_idx）
double get_latest_fc(const std::string &stock_id) {
  auto it = g_pdk_hash.find(stock_id);
  if (it != g_pdk_hash.end()) {
    return it->second.orig_prod_idx;
  }
  return 0.0; // 查無此商品時回傳 0 或其他預設值
}

// 範例用法：
// std::string stock_id(pdk_stock_id, sizeof(pdk_t::stock_id));
// double fc = get_latest_fc(stock_id);

// ------------------------------------------------------------
// @brief Callback used to process stock message.
// @param inMsg : LLM message
// ------------------------------------------------------------
void process_stock_message(rmmRxMessage *inMsg) {
  UnderlyMsg_T umsg;
  int64_t price;

  // rmm is not thread safe, use mutex to prevent Mex and LLM upadte same pdk
  pthread_mutex_lock(&g_process_message);

  memcpy(&umsg, inMsg->msg_buf, inMsg->msg_len);
  umsg.seq = ntohd(umsg.seq);
  umsg.time = ntohd(umsg.time);
  price = ntohd(*(int64_t *)&umsg.price);
  memcpy(&umsg.price, &price, sizeof(umsg.price));

  /* use product type to filter the LLM message */
  uint8_t mask;
  if (IS_OPTIONS()) {
    mask = OPTIONS_MASK;
  } else {
    mask = FUTURES_MASK;
  }
  if (!(umsg.productType & mask)) {
    pthread_mutex_unlock(&g_process_message);
    return;
  }

  std::string key(umsg.sid, sizeof(pdk_t::stock_id));
  auto pdk_it = g_pdk_hash.find(key);
  if (pdk_it != g_pdk_hash.end()) {
    if (umsg.seq > pdk_it->second.seq) {
      pdk_it->second.seq = umsg.seq;
      // LLM 現貨價格乘上 m_scale 存入 orig_prod_idx
      pdk_it->second.orig_prod_idx =
          static_cast<int>(umsg.price * pdk_it->second.m_scale + (umsg.price >= 0 ? 0.5 : -0.5));
      printf("020 msg stock_id=%s Seq=%ld price=%f\n", umsg.sid, umsg.seq, umsg.price);
      char tmpstring[256];
      // 取得現在時間並格式化為 HHMMSS
      time_t now = time(NULL);
      struct tm tstruct;
      localtime_r(&now, &tstruct);
      char timestr[16];
      snprintf(timestr, sizeof(timestr), "%02d%02d%02d", tstruct.tm_hour, tstruct.tm_min,
               tstruct.tm_sec);
      snprintf(tmpstring, sizeof(tmpstring), "[%s] sid:%s seq:%lu time:%lu price:%.3f", timestr,
               umsg.sid, umsg.seq, umsg.time, umsg.price);
      g_spd_logger_stovol->info("{}", tmpstring); // SPDLogger
    }
  }
  pthread_mutex_unlock(&g_process_message);
}

void stock_on_packet(rmmPacket *packet, void *user __attribute__((unused))) {
  int i = 0;
  for (i = 0; i < packet->num_msgs; i++) {
    process_stock_message(&packet->msgs_info[i]);
  }

  return;
}
