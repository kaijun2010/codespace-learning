/* Security Level: High */
/* Confidentiality: 2 (Sensitive) */

#include "AvgEngine.h"

#include <assert.h>
#include <string>

#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "../../commlib/inc/BTime.h"
#include "../../commlib/inc/atomic_io.h"

#define LogIf(p, M, ...)                                                 \
    if (static_cast<int>(m_trace) >= static_cast<int>(p)) {              \
        if (p == calc::AvgTraceLevel::INFO) {                            \
            fprintf(stdout, "[%d][AVG][INFO]" M, m_pseq, ##__VA_ARGS__); \
        } else {                                                         \
            fprintf(stdout, "[%d][AVG]" M, m_pseq, ##__VA_ARGS__);       \
        }                                                                \
    }

namespace calc {

using TFX::Time::fmt;
using TFX::Time::GetPartMilliSecond;
using TFX::Time::GetPartNanoSecond;
using TFX::Time::SetTime;
using TFX::Time::ToStr;

using std::endl;
using std::make_pair;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::to_string;
using std::unordered_map;

template <typename T>
tagPair<T>::tagPair(T start, T stop) {
    if (start > stop) {
        // std::swap(start, stop); // 改成這樣會是錯的
        std::swap(begin, end);  // 恢復
    }
    begin = start;
    end = stop;
}

template <typename T>
bool tagPair<T>::operator==(const tagPair<T>& rhs) const {
    return begin == rhs.begin && end == rhs.end;
}

template <typename T>
bool tagPair<T>::operator!=(const tagPair<T>& rhs) const {
    return !(*this == rhs);
}

template <typename U>
std::ostream& operator<<(std::ostream& os, const tagPair<U>& t) {
    os << "[" << t.begin << ", " << t.end << ")";
    return os;
}

template <typename T>
bool PairCompareLess<T>::operator()(const tagPair<T>& x, const tagPair<T>& y) const {
    return x.begin == y.begin ? x.end < y.end : x.begin < y.begin;
}

template struct tagPair<int32_t>;
template struct tagPair<uint32_t>;
template struct PairCompareLess<int32_t>;

RangeTimePrice::RangeTimePrice(const TimePair& time_pair, uint32_t beginSysOrderID, uint32_t currentSysOrderID)
    : m_time_pair(time_pair),
      m_begin_id(beginSysOrderID),
      m_stop_id(currentSysOrderID + 1),
      m_sum(0.0),
      m_denominator(0),
      m_size(0) {}

const TimePair& RangeTimePrice::GetRangeTime() const { return m_time_pair; }

SeqPair RangeTimePrice::GetRangeSeq() const { return SeqPair(m_begin_id, m_stop_id); }

bool RangeTimePrice::Add(TimePrice tp, uint32_t sysOrderID) {
    if (tp.price == INVALID_PRICE) {
        return false;
    }
    // m_list.push_back(tp);
    m_size++;
    m_stop_id = sysOrderID + 1;
    m_sum += tp.price;
    m_denominator++;
    return true;
}

double RangeTimePrice::GetAvgPrice() const { return m_denominator == 0 ? INVALID_PRICE : m_sum / m_denominator; }

bool RangeTimePrice::IsFullFillTo(const msg_time_t& time) {
    if (time.epoch_s >= m_time_pair.begin && time.epoch_s < m_time_pair.end) {
        return true;
    }
    return false;
}

bool RangeTimePrice::IsAheadOf(const msg_time_t& time) {
    if (IsFullFillTo(time)) {
        return true;
    } else if (m_time_pair.begin > time.epoch_s) {
        return true;
    }
    return false;
}

bool RangeTimePrice::IsExpire(const TimePair& expire_time_pair) { return m_time_pair.begin <= expire_time_pair.begin; }

void RangeTimePrice::CleanUp() {
    m_size = 0;
    // m_list.clear();
}

size_t RangeTimePrice::Size() const {
    return m_size;
    // return m_list.size();
}

std::pair<int, double> RangeTimePrice::GetAvgPair() const { return make_pair(m_denominator, m_sum); }

string RangeTimePrice::Info() const {
    stringstream ss;
    ss << "TimePair:" << GetRangeTime();
    if (m_stop_id > 1) {
        ss << ", SeqPair:" << GetRangeSeq();
    }
    ss << std::fixed << ", avg=" << m_sum;
    if (m_denominator != 0) {
        ss << "/" << m_denominator;
    }
    return ss.str();
}

TimePriceCalculator::TimePriceCalculator() { Reset(0); }

TimePriceCalculator::TimePriceCalculator(int32_t pseq) { Reset(pseq); }

void TimePriceCalculator::Reset(int32_t pseq) {
    SetPseq(pseq);
    m_id = 0;
    m_interval = 0;
    m_settle_price = INVALID_PRICE;
    m_preday_base_price = INVALID_PRICE;
    m_compete_match_price = INVALID_PRICE;
    m_recent_avg_price = INVALID_PRICE;
    m_last_fill_up = 0;
    m_compete_match_deal_counter = 0;
    m_deal_counter = 0;
    m_summary_counter = 0;
    m_refresh_counter = 0;
    EnabledTrace(AvgTraceLevel::NO_MSG);
    memset(&m_complete_time, 0, sizeof(msg_time_t));
    memset(&m_refresh_time, 0, sizeof(msg_time_t));
    memset(&m_tran_time, 0, sizeof(msg_time_t));
    memset(&m_first_effective_begin, 0, sizeof(msg_time_t));
    memset(&m_side_price_record_time, 0, sizeof(msg_time_t));
    SetInterval(0);
}

void TimePriceCalculator::SetPseq(int32_t pseq) { m_pseq = pseq; }

int32_t TimePriceCalculator::GetPseq() const { return m_pseq; }

// settle_price 由 ftprice 設定為 開參 (premium)
void TimePriceCalculator::SetSettlePrice(int settle_price) {
    if (m_settle_price != settle_price) {
        LogIf(AvgTraceLevel::INFO, " set settle_price from %d to %d\n", m_settle_price, settle_price);
        m_settle_price = settle_price;
    }
}

void TimePriceCalculator::SetPredayBasePrice(int preday_base_price) {
    if (m_preday_base_price != preday_base_price) {
        LogIf(AvgTraceLevel::INFO, " set preday_base_price from %d to %d\n", m_preday_base_price, preday_base_price);
        m_preday_base_price = preday_base_price;
    }
}

void TimePriceCalculator::SetCompleteMatch(const msg_time_t& tran_time, int match_price, int match_qty) {
    Reload(m_settle_price, m_preday_base_price);

    if (m_compete_match_deal_counter != 0) {
        LogIf(AvgTraceLevel::INFO, "[FLW] %s get compete match deal qty %d\n",
              ToStr(fmt::FMT_TIME_US_1, &tran_time).c_str(), m_compete_match_deal_counter);
    }

    m_complete_time = tran_time;

    // 決定第一個有效區間, 例如 [15:00:00.000000, 15:00:05.000000)
    msg_time_t dummy;
    memset(&dummy, 0, sizeof(msg_time_t));
    TransferToEffectiveTime(m_complete_time, &m_first_effective_begin, &dummy);

    if (match_qty > 0) {
        // 有集合競價價格
        m_compete_match_price = match_price;
        LogIf(AvgTraceLevel::INFO, "[FLW] %s, get compete match price=%d, qty=%d\n",
              ToStr(fmt::FMT_TIME_US_1, &tran_time).c_str(), m_compete_match_price, match_qty);
    } else {
        // 沒有集合競價價格
        LogIf(AvgTraceLevel::INFO, "[FLW] %s, no compete match price=%d, qty=%d\n",
              ToStr(fmt::FMT_TIME_US_1, &tran_time).c_str(), match_price, match_qty);
    }
}

void TimePriceCalculator::Reload(int settle_price, int preday_base_price) {
    m_id = 0;
    SetSettlePrice(settle_price);
    SetPredayBasePrice(preday_base_price);
    m_compete_match_price = INVALID_PRICE;
    m_recent_avg_price = INVALID_PRICE;
    m_last_fill_up = 0;
    m_compete_match_deal_counter = m_deal_counter;
    m_deal_counter = 0;
    m_summary_counter = 0;
    m_refresh_counter = 0;

    memset(&m_complete_time, 0, sizeof(msg_time_t));
    memset(&m_refresh_time, 0, sizeof(msg_time_t));
    memset(&m_tran_time, 0, sizeof(msg_time_t));
    memset(&m_first_effective_begin, 0, sizeof(msg_time_t));
    memset(&m_side_price_record_time, 0, sizeof(msg_time_t));

    m_buffer.clear();
    m_record.clear();
}

void TimePriceCalculator::SetInterval(int32_t interval) { m_interval = interval; }

bool TimePriceCalculator::AddTimePrice(const tag_rpt_t& tag) {
    if (tag.combined_match_type == 0 || tag.combined_match_type == 2) {
        return false;
    } else if (tag.header.ExecType == ORD_INS ||       // 新增
               tag.header.ExecType == ORD_INSMATCH) {  // 新增並成交
        // filter by pseq
        if (tag.ord_data.symbol.pseq1 != m_pseq && tag.ord_data.symbol.pseq2 != m_pseq) {
            return false;
        }

        // std::cout << tag << std::endl;

        if (tag.LastQty == 0) {
            return false;
        }

        // check by status_code
        if (!MATCH_OK(tag.ord_data.status_code)) {
            return false;
        }

        m_deal_counter += tag.LastQty;
        // UpdateSysOrderId(tag.TransactTime, tag.SysOrderID);

        bool multileg = false;
        size_t side = tag.ord_data.Side;
        int price;
        if (tag.ord_data.symbol.pseq2 == 0) {
            price = tag.LastPx;
            AddTimeSidePrice(tag.TransactTime, side, price, multileg);
        } else {
            multileg = true;
            int idx = -1;
            if (tag.ord_data.symbol.pseq1 == m_pseq) {
                if (tag.ord_data.Side == BUY_SIDE) {
                    // 買進組合商品(買遠賣近)
                    side = SELL_SIDE;
                } else {
                    // 賣出組合商品(賣遠買近)
                    side = BUY_SIDE;
                }
                idx = 0;
            } else {  // tag.ord_data.symbol.pseq2 == m_pseq
                if (tag.ord_data.Side == BUY_SIDE) {
                    side = BUY_SIDE;
                } else {
                    side = SELL_SIDE;
                }
                idx = 1;
            }

            price = tag.leg_px[idx];
            AddTimeSidePrice(tag.TransactTime, side, price, multileg);
        }

        return true;
    }

    return false;
}

bool TimePriceCalculator::UpdateTimePriceSide(const msg_time_t& trans_time, uint32_t sysOrderID) {
    bool ret = false;
    if (memcmp(&trans_time, &m_side_price_record_time.epoch_s, sizeof(msg_time_t)) == 0) {
        memset(&m_side_price_record_time, 0, sizeof(msg_time_t));
        if (m_buy_side_price.empty() && m_sell_side_price.empty()) {
            return false;
        }
        UpdateSysOrderId(trans_time, sysOrderID);

        if (m_buy_side_price.empty() || m_sell_side_price.empty()) {
            std::cerr << "[AVG][WARN] side_price size should not be empty" << endl;
            m_buy_side_price.clear();
            m_sell_side_price.clear();
            return false;
        }

        if (m_buy_side_price.size() > 1 || m_sell_side_price.size() > 1) {
            LogIf(AvgTraceLevel::DETAIL, "[RPT] %s, one to many: %zu -> %zu\n",
                  ToStr(fmt::FMT_TIME_US_1, &trans_time).c_str(), m_buy_side_price.size(), m_sell_side_price.size());
        }

        if (m_sell_side_price.size() > m_buy_side_price.size()) {
            for (auto it = m_sell_side_price.begin(); it != m_sell_side_price.end(); ++it) {
                UpdateTimePrice(trans_time, *it);
            }
        } else {
            for (auto it = m_buy_side_price.begin(); it != m_buy_side_price.end(); ++it) {
                UpdateTimePrice(trans_time, *it);
            }
        }
        ret = true;
    }
    m_buy_side_price.clear();
    m_sell_side_price.clear();
    return ret;
}

void TimePriceCalculator::UpdateSummaryCounter(int qty) { m_summary_counter += qty; }

bool TimePriceCalculator::QtyValidation() { return GetDealCounter() == GetSummaryCounter() * 2; }

bool TimePriceCalculator::UpdateTimePrice(const msg_time_t& trans_time, int price) {
    if (!IsInitialized()) {
        return false;
    }

    // 計算有效區間
    TimePair timePair = TransferToEffectiveTime(trans_time);
    RangeTimePriceBuffer::iterator it = m_buffer.find(timePair);

    bool isAdd = false;
    try {
        if (it != m_buffer.end()) {  // 如果 buffer 存在有效區間, 直接加入
            RangeTimePrice& rangeTimePrice = *it->second;
            isAdd = rangeTimePrice.Add(TimePrice(trans_time.epoch_s, price), m_id);
        } else {
            // 如果有效區間在 buffer 沒有找到, 先看是否需要補區間
            FillUp(trans_time);

            // 從 record 去找符合區間
            RangeTimePriceRecord::reverse_iterator findIt = FindRangeTimePriceRecord(timePair);

            if (findIt != m_record.rend()) {  // 如果 record 存在有效區間, 直接加入
                if ((m_refresh_time.epoch_s > trans_time.epoch_s) ||
                    (m_refresh_time.epoch_s == trans_time.epoch_s &&
                     GetPartNanoSecond(m_refresh_time) > GetPartNanoSecond(trans_time))) {
                    LogIf(AvgTraceLevel::INFO, "[RPT] %s, NOTICE! price delay\n",
                          ToStr(fmt::FMT_TIME_US_1, &trans_time).c_str());
                }
                isAdd = (*findIt)->Add(TimePrice(trans_time.epoch_s, price), m_id);
            } else {  // 新增區間並加入
                shared_ptr<RangeTimePrice> rangeTimePrice = AddBufferRecord(timePair);
                RangTimePriceeHasBeenAdded(trans_time, timePair);
                isAdd = rangeTimePrice->Add(TimePrice(trans_time.epoch_s, price), m_id);
            }

            // 從 buffer 移除過期區間
            RemoveExpireFromBuffer(timePair);
        }
    } catch (const std::exception& ex) {
        std::cerr << "[AVG][WARN] " << ex.what() << endl;
    }

    if (isAdd) {
        m_tran_time = trans_time;

        msg_time_t begin;
        SetTime(&begin, timePair.begin, 0);
        msg_time_t end;
        SetTime(&end, timePair.end, 0);
        if (m_id != 0) {
            LogIf(AvgTraceLevel::DETAIL, "[RPT] %s, SysOrderID=%u, price=%d to range [%s, %s)\n",
                  ToStr(fmt::FMT_TIME_US_1, &trans_time).c_str(), m_id, price,
                  ToStr(fmt::FMT_TIME_US_1, &begin).c_str(), ToStr(fmt::FMT_TIME_US_1, &end).c_str());
        } else {
            LogIf(AvgTraceLevel::DETAIL, "[RPT] %s, price=%d to range [%s, %s)\n",
                  ToStr(fmt::FMT_TIME_US_1, &trans_time).c_str(), price, ToStr(fmt::FMT_TIME_US_1, &begin).c_str(),
                  ToStr(fmt::FMT_TIME_US_1, &end).c_str());
        }
    }

    return isAdd;
}

// 計算價格
double TimePriceCalculator::Estimate(const msg_time_t& time, AvgCalculatorCriteria* out_criteria) {
    if (m_tran_time.epoch_s != 0) {
        if ((m_tran_time.epoch_s > time.epoch_s) ||
            (m_tran_time.epoch_s == time.epoch_s && GetPartNanoSecond(m_tran_time) > GetPartNanoSecond(time))) {
            LogIf(AvgTraceLevel::INFO, "[UPD] %s, NOTICE! ahead price\n", ToStr(fmt::FMT_TIME_US_1, &time).c_str());
        }
    }

    double estimate = INVALID_PRICE;
    AvgCalculatorCriteria criteria;

    // 試撮和集合競價有價格 m_first_effective_begin.epoch_s !=0
    if (!IsInitialized()) {
        if (m_preday_base_price != INVALID_PRICE) {
            // 開盤基準價
            SetCriteria(AvgCalculatorCriteria::PREDAY_BASE_PRICE, &criteria);
            estimate = m_preday_base_price;
            SetCriteria(criteria, out_criteria);
        } else {
            // 如果沒有集合競價價格, 沒有開盤基準價, 使用開參 (這理的 SETTLE_PRICE 設定的價格是開參)
            SetCriteria(AvgCalculatorCriteria::SETTLE_PRICE, &criteria);
            estimate = m_settle_price;
            SetCriteria(criteria, out_criteria);
        }
        LogIf(AvgTraceLevel::DETAIL, "[UPD.IsInitialized] %s, estimate=%.2f, criteria=%c\n", ToCStr(fmt::FMT_TIME_US_1, &time), estimate, GetCriteria(&criteria));
        return estimate;
    }

    if (time.epoch_s > m_refresh_time.epoch_s ||
        (time.epoch_s == m_refresh_time.epoch_s && GetPartNanoSecond(time) > GetPartNanoSecond(m_refresh_time))) {
        m_refresh_counter++;
        m_refresh_time = time;
    }

    UpdateTimePrice(time, INVALID_PRICE);

    //std::cout << "TRACE time.epoch_s="<< time.epoch_s << " m_first_effective_begin=" << m_first_effective_begin.epoch_s << std::endl;
    // 開盤前(試撮) 2:50:00 ~ 3:00:00
    if (time.epoch_s < m_first_effective_begin.epoch_s + m_interval) {
        // 開盤 m_effective_interval 秒前,
        // 如果有集合競價價格，使用集合競價價格, 否則使用日盤結算價
        if (m_compete_match_price != INVALID_PRICE) {
            SetCriteria(AvgCalculatorCriteria::COMPETE_MATCH_PRICE, &criteria);
            estimate = m_compete_match_price;
        } else if (m_preday_base_price != INVALID_PRICE) {
            // 開盤基準價
            SetCriteria(AvgCalculatorCriteria::PREDAY_BASE_PRICE, &criteria);
            estimate = m_preday_base_price;
        } else {
            // 開參
            SetCriteria(AvgCalculatorCriteria::SETTLE_PRICE, &criteria);
            estimate = m_settle_price;
        }
    } else {  // 開盤 m_effective_interval 秒後
        // 3:00:05 之後
        // 從 record 找有成交的紀錄
        RangeTimePriceRecord::reverse_iterator findReverseIt = GetRecentPrice(time);
        if (findReverseIt != m_record.rend()) {  // 判斷是否存在紀錄
            RangeTimePrice* recentPrice = findReverseIt->get();
            if (recentPrice->GetRangeTime() == TransferToEffectiveTime(time, -m_interval)) {  // 屬於前一個區間
                SetCriteria(AvgCalculatorCriteria::AVG_PRICE, &criteria);
            } else {  // 屬於其他區間
                SetCriteria(AvgCalculatorCriteria::RECENT_PRICE, &criteria);
            }
            m_recent_avg_price = recentPrice->GetAvgPrice();
            estimate = m_recent_avg_price;
        } else if (m_compete_match_price != INVALID_PRICE) {
            SetCriteria(AvgCalculatorCriteria::COMPETE_MATCH_PRICE, &criteria);
            estimate = m_compete_match_price;
        } else if (m_preday_base_price != INVALID_PRICE) {
            // 開盤基準價
            SetCriteria(AvgCalculatorCriteria::PREDAY_BASE_PRICE, &criteria);
            estimate = m_preday_base_price;
        } else {
            // 開參
            SetCriteria(AvgCalculatorCriteria::SETTLE_PRICE, &criteria);
            estimate = m_settle_price;
        }
    }

    LogIf(AvgTraceLevel::DETAIL, "[UPD] %s, estimate=%.2f, criteria=%c\n", ToCStr(fmt::FMT_TIME_US_1, &time), estimate, GetCriteria(&criteria));
    SetCriteria(criteria, out_criteria);

    return estimate;
}


const msg_time_t& TimePriceCalculator::GetRefreshTime() const { return m_refresh_time; }

uint32_t TimePriceCalculator::GetRefreshCounter() const { return m_refresh_counter; }

uint32_t TimePriceCalculator::GetDealCounter() const { return m_deal_counter; }

uint32_t TimePriceCalculator::GetSummaryCounter() const { return m_summary_counter; }

const msg_time_t& TimePriceCalculator::GetLastMtfTime() const { return m_tran_time; }

void TimePriceCalculator::EnabledTrace(AvgTraceLevel level) { m_trace = level; }

const TimePriceCalculator::RangeTimePriceRecord& TimePriceCalculator::GetRecord() const { return m_record; }

void TimePriceCalculator::DumpBufferRecord() {
    stringstream ss;
    ss << "buffer:" << endl;
    ss << std::fixed;
    for (auto it = m_buffer.rbegin(); it != m_buffer.rend(); ++it) {
        const RangeTimePrice* rangeTimePrice = it->second.get();
        ss << rangeTimePrice->GetRangeTime() << ", " << rangeTimePrice->GetRangeSeq() << ", "
           << rangeTimePrice->GetAvgPair().second << "/" << rangeTimePrice->GetAvgPair().first << endl;
    }

    ss << "record:" << endl;
    for (auto it = m_record.rbegin(); it != m_record.rend(); ++it) {
        const RangeTimePrice* rangeTimePrice = it->get();
        ss << rangeTimePrice->GetRangeTime() << ", " << rangeTimePrice->GetRangeSeq() << ", "
           << rangeTimePrice->GetAvgPair().second << "/" << rangeTimePrice->GetAvgPair().first << endl;
    }
    std::cout << ss.str();
}

string TimePriceCalculator::DumpRecordToFile(string fileName, string extension) {
    string fn = fileName + "." + extension;
    std::fstream fout;
    fout.open(fn, std::ofstream::out | std::ofstream::trunc);
    if (!fout) {
        std::cerr << "[AVG][WARN] open " << fn << "failed" << endl;
        return string();
    }

    AvgTraceLevel backup = m_trace;
    m_trace = AvgTraceLevel::NO_MSG;

    fout << std::fixed;
    for (auto it = m_record.begin(); it != m_record.end(); it++) {
        RangeTimePrice* rangeTimePrice = it->get();
        const TimePair& timePair = rangeTimePrice->GetRangeTime();

        msg_time_t time;
        SetTime(&time, timePair.begin, 0);

        AvgCalculatorCriteria criteria = AvgCalculatorCriteria::NOT_SET;
        double estimate = Estimate(time, &criteria);

        // printf("[%d][AVG][DUMP] %s, estimate=%.2f, criteria=%c\n", m_pseq,
        // ToCStr(fmt::FMT_TIME_US_1, &time), estimate, GetCriteria(&criteria));

        fout << ToStr(fmt::FMT_DATE_TIME_MS_1, &time).c_str() << "| " << estimate << "| " << static_cast<char>(criteria)
             << endl;
        SetTime(&time, timePair.end, 0);
        fout << ToStr(fmt::FMT_DATE_TIME_MS_1, &time).c_str() << "| " << estimate << "| " << static_cast<char>(criteria)
             << endl;
    }

    m_trace = backup;

    fout.close();
    return fn;
}

string TimePriceCalculator::Summary() const {
    stringstream ss;
    ss << "pseq:" << m_pseq << ", ";
    ss << "settle_price:" << m_settle_price << ", ";
    ss << "compete_match_price:" << m_compete_match_price << ", ";
    ss << "deal_counter:" << GetDealCounter() << ", ";
    ss << "summary_counter:" << GetSummaryCounter() << ", ";
    ss << "refresh_counter:" << GetRefreshCounter();

    return ss.str();
}

TimePair TimePriceCalculator::TransferToEffectiveTime(const msg_time_t& time, int32_t offset) const {
    return TimePair(time.epoch_s - time.epoch_s % m_interval + offset,
                    time.epoch_s - time.epoch_s % m_interval + m_interval + offset);
}

void TimePriceCalculator::TransferToEffectiveTime(const msg_time_t& time, msg_time_t* effective_begin,
                                                  msg_time_t* effective_end) const {
    TimePair timePair = TransferToEffectiveTime(time);
    SetTime(effective_begin, timePair.begin, 0);
    SetTime(effective_end, timePair.end, 0);
}

void TimePriceCalculator::UpdateSysOrderId(const msg_time_t& tran_time, uint32_t sysOrderID) {
    m_tran_time = tran_time;
    m_id = sysOrderID;
}

void TimePriceCalculator::SetCriteria(AvgCalculatorCriteria in, AvgCalculatorCriteria* out) const {
    if (out != NULL) {
        *out = in;
    }
}

char TimePriceCalculator::GetCriteria(const AvgCalculatorCriteria* criteria) const {
    if (criteria != NULL) {
        return static_cast<char>(*criteria);
    }
    return static_cast<char>(AvgCalculatorCriteria::NOT_SET);
}

void TimePriceCalculator::RemoveExpireFromBuffer(const TimePair& curr_time_pair) {
    TimePair expireTimePair = curr_time_pair;
    expireTimePair.begin -= m_interval;
    expireTimePair.end -= m_interval;

    // std::cout << "remove " << expireTimePair
    //          << " from buffer(size=" << m_buffer.size() << ")" << endl;

    // DumpBufferRecord();

    RangeTimePriceBuffer::iterator it = m_buffer.begin();
    while (it != m_buffer.end()) {
        if (it->second->IsExpire(expireTimePair)) {
            it->second->CleanUp();
            m_buffer.erase(it++);
        } else {
            ++it;
        }
    }

    // assert(m_buffer.empty() || m_buffer.size() == 1);  // 撮合傳來時間倒退會發生core dump
    // std::cout << "assert(m_buffer.empty() || m_buffer.size() == 1)" << endl;
}

shared_ptr<RangeTimePrice> TimePriceCalculator::AddBufferRecord(const TimePair& timePair) {
    uint32_t beginSysOrderID = 1;
    if (!m_record.empty()) {
        shared_ptr<RangeTimePrice> last = *m_record.rbegin();
        LogIf(AvgTraceLevel::INFO, " %s\n", last->Info().c_str());
        if (last->GetRangeSeq().end > 1) {
            beginSysOrderID = last->GetRangeSeq().end - 1;
        }
    }

    shared_ptr<RangeTimePrice> rangeTimePrice = make_shared<RangeTimePrice>(timePair, beginSysOrderID, m_id);
    m_buffer.insert(make_pair(timePair, rangeTimePrice));
    m_record.push_back(rangeTimePrice);
    return rangeTimePrice;
}

bool TimePriceCalculator::FillUp(const msg_time_t& time) {
    // 取得最後一個區間起始值
    int32_t src = 0;
    if (!m_record.empty()) {
        src = (*m_record.rbegin())->GetRangeTime().begin;
    } else {
        src = m_first_effective_begin.epoch_s;
    }

    // 取得需要補的區間起始值
    int32_t target = TransferToEffectiveTime(time).begin;
    // LogIf(AvgTraceLevel::DETAIL, "m_last_fill_up=%d, src=%d, target=%d\n",
    // m_last_fill_up, src, target);

    // 判斷是否需要補區間
    bool isNeedFillUp = m_last_fill_up > target ? false : true;
    if (!isNeedFillUp) {
        return false;
    } else if (target == 0) {  // 避免沒有初始化的 time 變數
        std::string errorMsg = "fill up to " + ToStr(fmt::FMT_DATE_TIME_MS_1, &time) + " failed, time is not initail";
        throw std::runtime_error(errorMsg);
    }
    m_last_fill_up = target;

    // 避免補太多造成記憶體爆掉
    const int fillUpLimit = 7 * 24 * 60 * 60 / m_interval;
    if ((target - src) / m_interval > fillUpLimit) {
        std::string errorMsg = "fill up to " + ToStr(fmt::FMT_DATE_TIME_MS_1, &time) + " failed, over fillUpLimit";
        throw std::runtime_error(errorMsg);
    }

    // 補區間
    std::function<bool(int32_t, int32_t)> comp = std::less<int32_t>();
    while (comp(src, target)) {  // src < target
        src += m_interval;

        TimePair timePair(src, src + m_interval);
        shared_ptr<RangeTimePrice> rangeTimePrice = AddBufferRecord(timePair);

        // std::cout << "beginSysOrderID=" << beginSysOrderID
        //          << ", currentSysOrderID=" << m_id << endl;

        RangTimePriceeHasBeenAdded(time, timePair);
    }

    RemoveExpireFromBuffer(TransferToEffectiveTime(time));

    return true;
}

void TimePriceCalculator::RangTimePriceeHasBeenAdded(const msg_time_t& time, const TimePair& time_pair) {
    msg_time_t begin;
    TFX::Time::SetTime(&begin, time_pair.begin, 0);
    msg_time_t end;
    TFX::Time::SetTime(&end, time_pair.end, 0);

    string msg = "auto";
    if (time.epoch_s == m_refresh_time.epoch_s && GetPartNanoSecond(time) == GetPartNanoSecond(m_refresh_time)) {
        msg = "fill";
    }

    LogIf(AvgTraceLevel::DETAIL, " %s range [%s, %s)\n", msg.c_str(), ToStr(fmt::FMT_TIME_US_1, &begin).c_str(),
          ToStr(fmt::FMT_TIME_US_1, &end).c_str());
}

TimePriceCalculator::RangeTimePriceRecord::reverse_iterator TimePriceCalculator::GetRecentPrice(
    const msg_time_t& time) {
    if (m_record.empty()) {
        return m_record.rend();
    }

    RangeTimePriceRecord::reverse_iterator it = m_record.rbegin();

    // record 的區間包含傳入時間
    while (!m_record.empty() && (*it)->IsAheadOf(time)) {
        it++;
    }

    // 依據傳入時間, 往前一個區間找價格
    for (; it != m_record.rend(); ++it) {
        RangeTimePrice& rangeTimePrice = **it;
        if (!rangeTimePrice.IsExpire(TimePair(time.epoch_s, 0))) {
            continue;
        }
        if (rangeTimePrice.GetAvgPrice() != INVALID_PRICE) {
            break;
        }
    }

    if (it != m_record.rend()) {
        // std::cout << "recent avg price: " << (*it)->GetRangeTime()
        //          << " " << (*it)->GetAvgPrice() << endl;
    }

    return it;
}

TimePriceCalculator::RangeTimePriceRecord::reverse_iterator TimePriceCalculator::FindRangeTimePriceRecord(
    const TimePair& time_pair) {
    RangeTimePriceRecord::reverse_iterator rit = m_record.rbegin();
    TimePairCompareLess compare;
    while (rit != m_record.rend()) {
        const TimePair targetTimeGroup = (*rit)->GetRangeTime();
        // std::cout << targetTimeGroup << endl;
        if (targetTimeGroup == time_pair) {
            // std::cout << "found" << endl;
            return rit;
        } else if (compare(targetTimeGroup, time_pair)) {
            // std::cout << "compare not found" << endl;
            return m_record.rend();
        }
        rit++;
    }
    // std::cout << "not found" << endl;
    return m_record.rend();
}

bool TimePriceCalculator::IsInitialized() const {
    // 集合競價完成且 interval 有設定
    return m_first_effective_begin.epoch_s != 0 && m_interval != 0;
}

void TimePriceCalculator::AddTimeSidePrice(const msg_time_t& trans_time, int side, int price, bool multileg) {
    if (m_side_price_record_time.epoch_s == 0) {
        m_side_price_record_time = trans_time;
    }

    if (memcmp(&trans_time, &m_side_price_record_time, sizeof(msg_time_t)) == 0) {
        LogIf(AvgTraceLevel::DETAIL, "[RPT][%s]%s %s, price=%d\n", side == BUY_SIDE ? "B" : "S",
              multileg ? "[MULTI]" : "", ToStr(fmt::FMT_TIME_US_1, &trans_time).c_str(), price);
        if (side == BUY_SIDE) {
            m_buy_side_price.push_back(price);
        } else if (side == SELL_SIDE) {
            m_sell_side_price.push_back(price);
        }
    } else {
        std::cerr << "[AVG][WARN] side_price_record_time missing "
                     "UpdateTimePriceSide"
                  << endl;
    }
}

AvgEngineElement AvgEngine::CreateTimePriceCalculator(int32_t pseq) {
    AvgEngineMap::iterator got = m_calculator_map.find(pseq);
    if (got != m_calculator_map.end()) {
        return got->second;
    }
    m_calculator_map[pseq] = make_shared<TimePriceCalculator>(pseq);
    return m_calculator_map[pseq];
}

AvgEngineMap& AvgEngine::GetTimePriceCalculator() { return m_calculator_map; }

TimePriceCalculator* AvgEngine::GetTimePriceCalculator(int32_t pseq) {
    if (IsExist(pseq)) {
        return m_calculator_map[pseq].get();
    }
    return NULL;
}

bool AvgEngine::IsExist(int32_t pseq) const { return m_calculator_map.find(pseq) != m_calculator_map.end(); }

void AvgEngine::EnabledTrace(AvgTraceLevel level) {
    for (auto it = m_calculator_map.begin(); it != m_calculator_map.end(); ++it) {
        it->second->EnabledTrace(level);
    }
}

string AvgEngine::Summary() const {
    stringstream ss;
    for (auto it = m_calculator_map.begin(); it != m_calculator_map.end(); ++it) {
        ss << it->second->Summary() << endl;
    }
    return ss.str();
}

void AvgEngine::DumpRecordToFile(const std::unordered_map<int, std::string>& prodid_map, string fileName,
                                 string extension) const {
    for (auto it = m_calculator_map.begin(); it != m_calculator_map.end(); ++it) {
        TimePriceCalculator* calculator = it->second.get();
        auto findIter = prodid_map.find(calculator->GetPseq());
        if (findIter != prodid_map.end()) {
            calculator->DumpRecordToFile(fileName + "_" + findIter->second, extension);
        }
    }
}

}  // namespace calc
