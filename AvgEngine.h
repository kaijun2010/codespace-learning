/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

#ifndef F_FUT_FTPRICE_AVGENGINE_H_
#define F_FUT_FTPRICE_AVGENGINE_H_

#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "commlib/inc/match_dat.h"

#ifndef INVALID_PRICE
#define INVALID_PRICE 0x7fffffff
#endif

#ifndef MAX_PRICE
#define MAX_PRICE 1000000000 /**< max price 9(6)V9(3) */
#endif

#ifndef MIN_PRICE
#define MIN_PRICE -MAX_PRICE /**< max price 9(6)V9(3) */
#endif

#ifdef USE_SPDLOG
#if FMT_VERSION > 100000
inline auto format_as(AvgTraceLevel t) { return fmt::underlying(t); }
#endif
#endif

namespace calc {

const int BUY_SIDE = 1;
const int SELL_SIDE = 2;

enum class AvgTraceLevel : int { NO_MSG = 0, INFO = 1, DETAIL = 2 };

enum class AvgCalculatorCriteria : int {
    COMPETE_MATCH_PRICE = 'M',
    PREDAY_BASE_PRICE = 'P',
    SETTLE_PRICE = 'S',
    AVG_PRICE = 'V',
    RECENT_PRICE = 'R',
    NOT_SET = 'N',
};

template <typename T>
struct tagPair {
    T begin;
    T end;
    tagPair(T begin, T end);
    bool operator==(const tagPair<T>& rhs) const;
    bool operator!=(const tagPair<T>& rhs) const;
    template <typename U>
    friend std::ostream& operator<<(std::ostream& os, const tagPair<U>& t);
};

typedef tagPair<int32_t> TimePair;
typedef tagPair<uint32_t> SeqPair;

template <typename T>
struct PairCompareLess {
    bool operator()(const tagPair<T>& x, const tagPair<T>& y) const;
};

typedef PairCompareLess<int32_t> TimePairCompareLess;

typedef struct tagTimePrice {
    int32_t time;
    int price;
    tagTimePrice(int32_t time_, int price_) : time(time_), price(price_) {}
} TimePrice;

class RangeTimePrice {
   public:
    // typedef std::list<TimePrice> TimePriceList;

    RangeTimePrice(const TimePair& time_pair, uint32_t beginSysOrderID, uint32_t currentSysOrderID);

    const TimePair& GetRangeTime() const;
    SeqPair GetRangeSeq() const;

    bool Add(TimePrice tp, uint32_t sysOrderID);
    double GetAvgPrice() const;
    bool IsFullFillTo(const msg_time_t& time);
    bool IsAheadOf(const msg_time_t& time);
    bool IsExpire(const TimePair& expire_time_pair);

    void CleanUp();

    /**
     * range time price size, it will be zero after CleanUp
     */
    size_t Size() const;

    std::pair<int, double> GetAvgPair() const;

    std::string Info() const;

   private:
    TimePair m_time_pair;
    uint32_t m_begin_id;
    uint32_t m_stop_id;
    double m_sum;       // 總和
    int m_denominator;  // 分母
    // TimePriceList m_list;
    size_t m_size;
};

class TimePriceCalculator {
    friend class TimePriceCalculatorTest;

   public:
    typedef std::map<TimePair, std::shared_ptr<RangeTimePrice>, TimePairCompareLess> RangeTimePriceBuffer;
    typedef std::list<std::shared_ptr<RangeTimePrice>> RangeTimePriceRecord;
    typedef std::list<int> SidePriceRecord;

    explicit TimePriceCalculator(int32_t pseq);

    /**
     * 設定商品序號
     */
    void SetPseq(int32_t pseq);

    int32_t GetPseq() const;

    /**
     * 設定日盤結算價
     */
    void SetSettlePrice(int settle_price);

    /**
     * 設定開盤基準價
     */
    void SetPredayBasePrice(int preday_base_price);

    /**
     * 設定集合競價完成時間和集合競價價格
     */
    void SetCompleteMatch(const msg_time_t& tran_time, int match_price, int match_qty);

    /**
     * 重新設定日盤結算價並清除集合競價相關狀態
     * @param settle_price
     */
    void Reload(int settle_price, int preday_base_price);

    /**
     * 設定有效固定間隔秒數(預設為5秒)
     */
    void SetInterval(int32_t interval);

    /**
     * 依據回報更新時間價格串列
     * @return 是否符合商品和成交回報定義
     */
    bool AddTimePrice(const tag_rpt_t& tag);

    bool UpdateTimePriceSide(const msg_time_t& trans_time, uint32_t sysOrderID);

    /**
     * 更新買賣方數量供檢核
     */
    void UpdateSummaryCounter(int qty);

    /**
     * 買賣方數量檢核
     * @return successful for true
     */
    bool QtyValidation();

    /**
     * 更新時間價格串列
     * @return 是否符合成交回報定義
     */
    bool UpdateTimePrice(const msg_time_t& trans_time, int price);

    /**
     * 依據傳入時間, 更新簡單算術平均數
     * @param time 時間
     * @param out_criteria 計算使用到那一個規則
     * @return 簡單算術平均數
     */
    double Estimate(const msg_time_t& time, AvgCalculatorCriteria* out_criteria);

    /**
     * 實際有 Refresh 的時間 (取得的時間不會倒退)
     */
    const msg_time_t& GetRefreshTime() const;

    /**
     * 實際有 Refresh 的計數
     */
    uint32_t GetRefreshCounter() const;

    /**
     * 成交計數
     */
    uint32_t GetDealCounter() const;

    /**
     * transaction 的成交計數
     */
    uint32_t GetSummaryCounter() const;

    const msg_time_t& GetLastMtfTime() const;

    void EnabledTrace(AvgTraceLevel level);

    const RangeTimePriceRecord& GetRecord() const;

    char GetCriteria(const AvgCalculatorCriteria* criteria) const;

    void DumpBufferRecord();
    std::string DumpRecordToFile(std::string fileName, std::string extension);

    std::string Summary() const;

   private:
    TimePriceCalculator();

    int32_t m_pseq;
    uint32_t m_id;
    int32_t m_interval;         // 有效固定間隔秒數
    int m_settle_price;         // 日盤結算價
    int m_preday_base_price;    // 開盤基準價(日轉夜基準價)
    int m_compete_match_price;  // 集合競價價格
    double m_recent_avg_price;  // 最近一筆簡單算術平均數
    int m_last_fill_up;
    uint32_t m_compete_match_deal_counter;  // 集合競價成交計數器
    uint32_t m_deal_counter;                // 逐筆撮合成交計數器
    uint32_t m_summary_counter;
    uint32_t m_refresh_counter;  // estimate 計數器 (時間倒退不遞增)
    AvgTraceLevel m_trace;
    msg_time_t m_complete_time;  // 集合競價完成時間
    msg_time_t m_refresh_time;
    msg_time_t m_tran_time;
    msg_time_t m_first_effective_begin;
    msg_time_t m_side_price_record_time;

    RangeTimePriceBuffer m_buffer;
    RangeTimePriceRecord m_record;
    SidePriceRecord m_sell_side_price;
    SidePriceRecord m_buy_side_price;

    void Reset(int32_t pseq);

    TimePair TransferToEffectiveTime(const msg_time_t& time, int32_t offset = 0) const;

    void TransferToEffectiveTime(const msg_time_t& time, msg_time_t* effective_begin, msg_time_t* effective_end) const;

    /**
     * 紀錄 tag_prt 的 tran_time 和 SysOrderID
     */
    void UpdateSysOrderId(const msg_time_t& tran_time, uint32_t sysOrderID);

    void SetCriteria(AvgCalculatorCriteria in, AvgCalculatorCriteria* out) const;

    /**
     * 移除過期區間
     * @param time_pair 最新區間
     */
    void RemoveExpireFromBuffer(const TimePair& curr_time_pair);

    std::shared_ptr<RangeTimePrice> AddBufferRecord(const TimePair& timePair);

    bool FillUp(const msg_time_t& time);

    void RangTimePriceeHasBeenAdded(const msg_time_t& time, const TimePair& time_pair);

    /**
     * @param time 現在時間
     * @return
     */
    RangeTimePriceRecord::reverse_iterator GetRecentPrice(const msg_time_t& time);

    RangeTimePriceRecord::reverse_iterator FindRangeTimePriceRecord(const TimePair& time_pair);

    bool IsInitialized() const;

    void AddTimeSidePrice(const msg_time_t& trans_time, int price, int side, bool multileg);
};

typedef std::shared_ptr<TimePriceCalculator> AvgEngineElement;
typedef std::unordered_map<int, AvgEngineElement> AvgEngineMap;

class AvgEngine {
   public:
    /**
     * 依據商品序號建立 TimePriceCalculator
     * @param pseq
     * @return 建立物件之共用指標
     */
    AvgEngineElement CreateTimePriceCalculator(int32_t pseq);

    /**
     * 依據商品序號取得 TimePriceCalculator
     */
    TimePriceCalculator* GetTimePriceCalculator(int32_t pseq);

    /**
     * 商品序號對應的 TimePriceCalculator 是否存在
     */
    bool IsExist(int32_t pseq) const;

    /**
     * 開啟日誌
     */
    void EnabledTrace(AvgTraceLevel level);

    /**
     * 取得內部 AvgEngineMap
     */
    AvgEngineMap& GetTimePriceCalculator();

    /**
     * 輸出統計資訊
     */
    std::string Summary() const;

    /**
     * 產生今日各個區間的計算結果, 並寫入 $FILENAME_$PGSEQ.$EXTENSION
     * @param prodid_map pseq to prodid
     * @param fileName prefix 路徑
     * @param extension 副檔名
     */
    void DumpRecordToFile(const std::unordered_map<int, std::string>& prodid_map, std::string fileName,
                          std::string extension) const;

   private:
    AvgEngineMap m_calculator_map;
};

}  // namespace calc

#endif  // F_FUT_FTPRICE_AVGENGINE_H_
