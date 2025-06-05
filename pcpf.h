#ifndef F_FUT_FTPRICE_PCPF_H_
#define F_FUT_FTPRICE_PCPF_H_

/**@file pcph.h
 * @brief This program is designed to calculate the effective mid-price of PCPF.
 * @author kaijun@taifex.com.tw
 */

//--------------------------------------------------------------------
// forward declare class
//--------------------------------------------------------------------
class PCPF_INFO_T;

//--------------------------------------------------------------------
// PCPF class
//--------------------------------------------------------------------

#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <map>
#include <cmath>
#include <tuple>  // 使用 std::tuple

#define INVALID_PRICE 0x7fffffff

/**
 * @class PCPF_INFO_T
 * @brief 用於接收與計算 PCPF 中價、最佳加權買/賣價的類別。
 *
 * 此類別包含期權產品的基本資訊、最佳買賣價格與數量，以及相關的計算與檢查功能。
 *
 * @details
 * - kind_id: 期權種類代碼，例如 TXU。
 * - settle_date: 結算日期，例如 202505。
 * - seq: 外部傳入的序號。
 * - time: 外部傳入的時間。
 * - pdk_scale: 價格小數點刻度，範圍為 0-10。
 * - price_b: 最佳10檔買價。
 * - qty_b: 最佳10檔買量。
 * - price_s: 最佳10檔賣價。
 * - qty_s: 最佳10檔賣量。
 *
 * 提供以下功能：
 * - 檢查買賣價格排序是否符合規則。
 * - 計算 PCPF 中價、最佳加權買價與最佳加權賣價。
 */
class PCPF_INFO_T {
    public:
        char kind_id[4];     // opt.kind_id  Ex: TXU
        char settle_date[7]; // opt.settle_date Ex: 202505

        uint32_t seq;  // 外部傳來的序號
        uint32_t time; // 外部傳來的時間
        uint32_t pdk_scale; // 正整數，範圍 0-10

        // 最佳10檔買價與數量
        std::array<uint32_t, 10> price_b;
        std::array<uint32_t, 10> qty_b;

        // 最佳10檔賣價與數量
        std::array<uint32_t, 10> price_s;
        std::array<uint32_t, 10> qty_s;

    public:
        PCPF_INFO_T()
            : seq(0), time(0), pdk_scale(0),
              price_b({}), qty_b({}),
              price_s({}), qty_s({}) {
            memset(kind_id, 0, sizeof(kind_id));
            memset(settle_date, 0, sizeof(settle_date));
        }

        PCPF_INFO_T(const std::string& KindId, const std::string& SettleDate,
                    uint32_t Seq, uint32_t Time, uint32_t Pdk_Scale,
                    const std::array<uint32_t, 10>& Px_B, const std::array<uint32_t, 10>& Qty_B,
                    const std::array<uint32_t, 10>& Px_S, const std::array<uint32_t, 10>& Qty_S)
            : seq(Seq), time(Time), pdk_scale(Pdk_Scale),
              price_b(Px_B), qty_b(Qty_B), price_s(Px_S), qty_s(Qty_S) {
            strncpy(kind_id, KindId.c_str(), sizeof(kind_id) - 1);
            kind_id[sizeof(kind_id) - 1] = '\0';
            strncpy(settle_date, SettleDate.c_str(), sizeof(settle_date) - 1);
            settle_date[sizeof(settle_date) - 1] = '\0';
        }


		// 檢查買賣價格排序是否符合規則
        bool check_order_validity() const {
			// 檢查買價是否由高到低排序
			for (size_t i = 0; i < price_b.size() - 1; ++i) {
				if (price_b[i] == INVALID_PRICE) continue;  // 跳過無效價格
				if (price_b[i + 1] == INVALID_PRICE) break; // 如果後面是無效價格，則結束檢查
				if (price_b[i] < price_b[i + 1]) {
					return false;
				}
			}

			// 檢查賣價是否由低到高排序
			for (size_t i = 0; i < price_s.size() - 1; ++i) {
				if (price_s[i] == INVALID_PRICE) continue;  // 跳過無效價格
				if (price_s[i + 1] == INVALID_PRICE) break; // 如果後面是無效價格，則結束檢查
				if (price_s[i] > price_s[i + 1]) {
					return false;
				}
			}

            return true;
        }

        std::tuple<double, double, double> calc_pcpf_middle_price(uint32_t min_total_qty, double bs_filter_threshold) const {
            uint32_t total_price_b = 0, total_qty_b = 0;
            uint32_t total_price_s = 0, total_qty_s = 0;

            for (size_t i = 0; i < 10 && total_qty_b < min_total_qty; ++i) {
                uint32_t take_qty = std::min(min_total_qty - total_qty_b, qty_b[i]);
                total_price_b += price_b[i] * take_qty;
                total_qty_b += take_qty;
            }

            for (size_t i = 0; i < 10 && total_qty_s < min_total_qty; ++i) {
                uint32_t take_qty = std::min(min_total_qty - total_qty_s, qty_s[i]);
                total_price_s += price_s[i] * take_qty;
                total_qty_s += take_qty;
            }

#ifdef DEBUG3
            std::cout << "total_qty_b=" << total_qty_b << " total_qty_s="
                << total_qty_s << " min_total_qty=" << min_total_qty << std::endl;
#endif
            if (total_qty_b < min_total_qty || total_qty_s < min_total_qty) {
                return {INVALID_PRICE, INVALID_PRICE, INVALID_PRICE};
            }

			// 檢查 total_qty_b 和 total_qty_s，防止除 0
			if (total_qty_b == 0 || total_qty_s == 0) {
				return {INVALID_PRICE, INVALID_PRICE, INVALID_PRICE};
			}

            int scale = pow(10, pdk_scale);
            double weighted_avg_b = static_cast<double>(total_price_b) / total_qty_b / scale;
            double weighted_avg_s = static_cast<double>(total_price_s) / total_qty_s / scale;
            // 計算報價寬度濾網: (最佳一檔委託口數加權之委賣價 / 最佳一檔委託口數加權之委買價) -1 <= 0.2%
            double price_ratio = std::abs(((weighted_avg_s / weighted_avg_b) - 1) * 100);
#ifdef DEBUG3
            std::cout << "price_ratio=" << price_ratio << " bs_filter_threshold=" << bs_filter_threshold <<
                " s=" << weighted_avg_s << " b=" << weighted_avg_b << " |((s/b)-1)*100|" << std::endl;
#endif
			if (price_ratio >= bs_filter_threshold) {
				return {INVALID_PRICE, INVALID_PRICE, INVALID_PRICE};
			} else {
				return {round((weighted_avg_b + weighted_avg_s) / 2 * 100) / 100, weighted_avg_b, weighted_avg_s};
			}
        }
    };


#if 0
std::map<std::string, PCPF_INFO_T> g_map_pcpf_orderbook;

void initialize_test_data() {
    std::array<uint32_t, 10> test_prices_b = {1090000, 1080000, 1070000, 1060000, 1050000, 1040000, 1030000, 1020000, 1010000, 1000000};
    std::array<uint32_t, 10> test_qtys_b = {10, 20, 15, 12, 18, 25, 30, 22, 11, 14};

    std::array<uint32_t, 10> test_prices_s = {1000000, 1010000, 1020000, 1030000, 1040000, 1050000, 1060000, 1070000, 1080000, 1090000};
    std::array<uint32_t, 10> test_qtys_s = {10, 23, 13, 12, 11, 25, 30, 22, 11, 14};

    g_map_pcpf_orderbook["MXU202504"] = PCPF_INFO_T("TXU06900U5", "TXU", "202504", 1, 221350, 3, test_prices_b, test_qtys_b, test_prices_s, test_qtys_s);
    g_map_pcpf_orderbook["MXV202504"] = PCPF_INFO_T("TXV07000U5", "TXU", "202504", 2, 221430, 3, test_prices_b, test_qtys_b, test_prices_s, test_qtys_s);
}

int main() {

    std::string key = "MXU202504";
    uint32_t min_total_qty = 32;
    double bs_filter_threshold = 0.1;
    double mid_price, weighted_avg_b, weighted_avg_s;

    if (g_map_pcpf_orderbook.find(key) != g_map_pcpf_orderbook.end()) {
        std::tie(mid_price, weighted_avg_b, weighted_avg_s) =
            g_map_pcpf_orderbook[key].calc_pcpf_middle_price(min_total_qty, bs_filter_threshold);
        std::cout << "Mid Price for " << key << ": " << mid_price << "\n";
    } else {
        std::cout << "Key not found: " << key << "\n";
    }

    return 0;
}
#endif

#endif  // F_FUT_FTPRICE_PCPF_H_
