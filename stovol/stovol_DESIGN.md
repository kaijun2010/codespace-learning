---
title: stovol 系統設計說明文件 [stovol — System Design Document]
STO波動率曲線即時行情建構
author: kaijun@taifex.com
date: 2025/9/26
變更紀錄:
* v0.1（2025-09-26）：草稿
---

======================================================================
1. 簡介（Overview）
======================================================================
- 專案代碼：stovol
- 一句話說明：在交易時段內，依規範以中價與 Black‑Scholes 反解各到期月份/各履約價之隱含波動度，建構 STO 波動率曲線與衍生指標，輸出標準化中介檔供策略、監控與報表使用。

- 背景/脈絡（摘要）：
  - 計算時窗：08:45–13:15 每 15 分鐘一條曲線；13:15–13:45 每 1 分鐘一條曲線(收盤前30分鐘改為1分鐘計算一次)。
  - 標的期貨價格（targetF）取得順序：
    1) 以買賣權平價（put‑call parity）推估的「合成期貨」中價；
    2) 若無法取得，改用最近月期貨最佳買賣報價中價；
    3) 再不行則用現貨最新成交價。
  - 價格基準序列：以「價外」序列為基準；履約價 ≥ targetF 以買權做基準，履約價 < targetF 以賣權做基準。
  - 價格樣本：以「有效買賣中價」作為選擇權價格（最佳買+最佳賣之平均，且買賣差須在規範的寬度倍數內）。
  - IV 計算與檢核：對每個序列以 BS 反解 IV；以各到期的最低波動度序列為基準點，向外檢查「遞增/持平」與「相鄰差距上限」；不符規則者剔除；樣本不足則該時點不輸出曲線（以 0 呈現）。
  - 插補與調整型契約：履約價缺值以線性插補；調整型契約沿用原型契約曲線，同履約價同波動度，無對應時以線性插補，若僅剩調整型則不計算。

- 目標/效益：
  - 產出期限結構與微笑/斜率等關鍵特徵，支援結算、監控與策略信號。
  - 以批次與(盤中即時)並行化流程處理多商品/多到期，確保時效性與可重現性。

- 非目標（Out of Scope）：
  - 前端視覺化 GUI。

- 主要輸入與輸出（簡表）：
  - 輸入：FUT與OPT SQLite 行情/產品定義、期貨/選擇權/現貨報價（含 LLM 現貨來源，期貨撮合資料檔，選擇權撮合資料檔）。
  - 輸出：目標期貨價格、有效中價清單、IV/THEVOL/插補清單、與各類過濾/計算日誌檔。

- 讀者與前置假設：
  - 讀者：量化/交易系統工程師、策略研究員、維運工程師。
  - 前置：具備 C++14 與 SQLite 基礎；已配置 data/config 與可讀行情資料。

（以上簡介來自需求 PDF 與現有設計稿之統整；細節規則與輸出檔名在後續章節精確落地。）

======================================================================
2. 名詞與縮寫（Glossary）
======================================================================

+------------------+----------------------------------------------------------------------------------+
| Key              | Definition                                                                       |
+------------------+----------------------------------------------------------------------------------+
| ImplVol          | 買賣中價隱含波動度                                                                |
| TheVol           | 理論價波動度                                                                      |
| AdjTheVol        | 調整後理論價波動度                                                                |
| SysOrderID       | 計算時撮合序號(FUT/OPT分別記錄)                                                   |
| Time             | 計時時間點(HH:MM:SS.sss)                                                          |
| PROD_ID          | 商品代碼                                                                          |
| PGSEQ            | 商品序號                                                                          |
| PDK              | 契約代碼                                                                          |
| STRIKE           | 履約價                                                                            |
| PDK_PARAM_KEY    | 契約基本資料對照碼 (PDK_PARAM_KEY=TXO/STC) [本需求為計算 STC 類型(對應STF)]       |
| BestBUY          | 最佳買價                                                                          |
| BestSell         | 最佳賣價                                                                          |
| BestMiddlePrice  | 最佳買賣中價(B+S)/2                                                               |
| FA               | 合成期貨中價                                                                      |
| FB               | 近月期貨最佳買賣報價中價                                                          |
| FC               | 現貨最新成交價                                                                    |
+------------------+----------------------------------------------------------------------------------+

======================================================================
3. 需求摘要（Requirements）
======================================================================

3.1 功能性需求（FR）

* 使用OPT/FUT 盤中最佳買賣價，計算調整後理論價波動度。

3.2 非功能性需求（NFR）

* NA。

======================================================================
4. 系統範圍與情境（Scope & Context）
======================================================================

4.1 上下游系統（Context Diagram — 文字化）

- 上游：行情資料檔（FUT/OPT/IDX），對照檔（match_p.dat、tpc.dat 等），靜態參數（config/llm_sirs.ini）
- 本系統：stovol 可執行檔、設定檔、日誌
- 下游：策略模組、報表使用者、監控告警

4.2 輸入/輸出（I/O）

+--------+-------------------------------------------+-------------------------------------------+
| 類型   | 名稱                                      | 說明                                      |
+--------+-------------------------------------------+-------------------------------------------+
| 輸入   | FUT 資料組                                | db_data.dat / DB_ORG / match_p.dat        |
| 輸入   | OPT 資料組                                | db_data.dat / DB_ORG / match_p.dat / tpc.dat |
| 輸入   | config/llm_sirs.ini                       | 參數、清單                                 |
| 輸出   | $oLOG/stovol_*.log                        | 執行與除錯日誌                             |
| 輸出   | out/*.txt / *.csv / 中介檔                | 曲線、節點值、指標（詳見下表「輸出清單」）  |
| 輸出   | 告警（可選）                              | 門檻觸發之通知                             |
+--------+-------------------------------------------+-------------------------------------------+

輸出清單（Output Files）
+--------------------------------------------+------------------------------------------------------------+
| File                                       | 說明                                                       |
+--------------------------------------------+------------------------------------------------------------+
| stovol_opt_prod_info.log                   | OPT 商品資訊記錄檔                                        |
| stovol_pc_parity_list.log                  | 序列價格資料記錄檔                                        |
| stovol_pc_parity.log                       | 序列價格合併資料記錄檔                                    |
| stovol_FA_opt_MiddlePrice.log              | 選擇權合成期貨價格記錄檔                                  |
| stovol_FB_fut_MiddlePrice.log              | 同標的最近月份期貨最佳買賣報價之中價記錄檔                |
| stovol_FC_opt_idx_MiddlePrice.log          | 同標的現貨最新成交價記錄檔                                |
| stovol_targetF.log                         | 標的期貨價格資料檔                                        |
| stovol_optMiddlePrice_list.log             | 中價序列紀錄檔                                            |
| stovol_optMiddlePrice_filter.log           | 中價序列被過濾紀錄檔                                      |
| stovol_calc_time.log                       | FUT 與 OPT 的計算時點記錄檔                                |
| stovol_IMPVOL_list.log                     | 隱含波動度推算資料檔                                      |
| stovol_THEVOL_list.log                     | 理論價波動度資料檔                                        |
| stovol_ADJ_THEVOL_list.log                 | 理論價波動度插補資料檔                                    |
+--------------------------------------------+------------------------------------------------------------+

======================================================================
5. 目錄結構（Repository Layout）
==========================

```
opt/stovol/
  ├─ src/                # C++ 實作（stovol.cpp, ...）
  ├─ include/            # C++ 標頭（*.h）
  ├─ config/             # 設定檔（*.ini/*.json）
  ├─ data/               # 測試資料與 SQLite
  ├─ testsuite/          # 自動化測試腳本與樣本輸入/輸出
  ├─ out/                # 產出物（報表、中介檔）
  ├─ clf100.sh           # clang-format 100 欄位排版工具
  └─ Makefile            # 編譯規則
```

======================================================================
6. 執行流程（High-Level Flow）
======================================================================

* Step 1. 載入設定（環境變數、命令列、config 檔）
* Step 2. 連接資料源（SQLite / 檔案）
* Step 3. 讀取產品定義（PDK/PROD）、合約清單、過濾條件
* Step 4. 讀取行情切片（時間窗、價量、成交/報價）
* Step 5. 計算：

  * 期權理論價、IV/THEVOL、Skew/Smile、Term Structure
  * 曲線擬合（樣條/回歸/平滑）與節點化（Strikes/TTM）
* Step 6. 產出：中介檔/報表/指標；寫入 logs
* Step 7. （可選）告警或推送

======================================================================
7. 介面設計（CLI / Config / API）
======================================================================
7.1 命令列介面（CLI）

+------+---------------------------------------------------------------+------------------------------------------------------+
| 旗標 | 範例                                                          | 說明                                                 |
+------+---------------------------------------------------------------+------------------------------------------------------+
| (無) | stovol <grp> <cfg> <opt_db> <fut_db> <opt_match> <fut_match> <opt_tpc> | 計算 STC（預設模式）     
| -t   | stovol -t <grp> <cfg> <opt_db> <fut_db> <opt_match> <fut_match> <opt_tpc> | 只算 TXO（TXO-only） 
| -o   | stovol -o <grp> <cfg> <opt_db> <fut_db> <opt_match> <fut_match> <opt_tpc> | 只算 TXO 且 只算 OPT
| -r   | stovol -r <grp> <cfg> <opt_db> <fut_db> <opt_match> <fut_match> <opt_tpc> | 重啟/復原(recovery)
| -l   | stovol -l <grp> <cfg> <opt_db> <fut_db> <opt_match> <fut_match> <opt_tpc> | 啟動 LLM 模式     
| -m   | stovol -m <grp> <cfg> <opt_db> <fut_db> <opt_match> <fut_match> <opt_tpc> | 啟動 mcc 模式    
+------+---------------------------------------------------------------+------------------------------------------------------+

參數順序與意義
+-------------+-----------------------------------------------+
| 參數        | 說明                                          |
+-------------+-----------------------------------------------+
| <grp>       | 群組 ID（整數）                               |
| <cfg>       | 設定檔路徑（例：config/llm_sirs.ini）         |
| <opt_db>    | OPT 資料檔（例：data/opt/db_data.datDB_ORG）  |
| <fut_db>    | FUT 資料檔（例：data/fut/db_data.datDB_ORG）  |
| <opt_match> | OPT 對照檔（例：data/opt/match_p.dat）        |
| <fut_match> | FUT 對照檔（例：data/fut/match_p.dat）        |
| <opt_tpc>   | OPT tpc 檔（例：data/opt/tpc.db 或 tpc.dat）  |
+-------------+-----------------------------------------------+

規則
- 模式旗標：-t / -o / -r / -l / -m ；未指定旗標時等同「計算 STC」。

7.2 設定檔（Config）
- 檔案格式：INI/JSON
- 範例片段：

```
[database]
path = /hds1/changer/* , /hds1/changer1/*

[run]
products = CAO, CDO, ...
expiry_filter = active_only
output_dir = out/
```

7.3 程式介面（Headers / Namespaces）
- `include/` 內公共介面


======================================================================
8. 資料模型（Schema / DTO）
======================================================================

8.1 SQLite 表與欄位（若適用）

  *  自行參考 FUT/OPT Table Schema 定義檔

8.2 業務實體（Entities）

* Product, Contract, QuoteSlice, CurveNode, Curve, Indicator, ...（TODO 定義）

======================================================================
9. 核心演算法（Algorithms）
======================================================================

* 期權定價：Black-Scholes
* 反解 IV：二分/牛頓/Brent（容忍誤差、最大迭代）
* 曲線擬合：線性/三次樣條/權重平滑（Outlier 處理）
* 邊界條件：負率、極端價內外；缺值補內插/外推策略
* 精度與效能：浮點穩定、SIMD/並行策略

Ref: STO理論價格波動率曲線計算功能需求(Black-Scholes model):
1.	計算裡選擇權理論價公式
**  理論價格公式 :
**      @call =( N(@d1) * @s * exp(-@q*@t) - N(@d2) * @k * exp(-@r*@t) )/ PDK_multiple
**      @put =( -N(-@d1) * @s * exp(-@q*@t) + N(-@d2) * @k * exp(-@r*@t) )/ PDK_multiple
** 
**      @d1 = (log(@s/@k)+(@r-@q+(@v^2)/2)*@t) / (@v * sqrt(@t))
**      @d2 = @d1 - @v * sqrt(@t)
**
**      @s= floor(@sp * PDK_STOCK_QNTY) + PDK_STOCK_CASH2 + PDK_STOCK_CASH3
            + floor(PDK_STOCK_QNTY3 * case when @sp > PDK_STOCK_PRICE3
                                  then @sp - PDK_STOCK_PRICE3
                                  else 0
                              end)

**      @k= @kp * PDK_multiple

**      N()   = 常態分配函數
**
**      @call  = 買權權利金(理論價格)
**      @put  = 賣權權利金(理論價格)
**      PDK_multiple = 契約乘數
**      @s    = 以標的期貨價格計算之約定標的物價值
**      @sp   = 標的期貨價格
**      @k    = 以履約價格計算之約定標的物價值
**      @kp   = 履約價格
**      PDK_STOCK_QNTY   =約定標的股數
**      PDK_STOCK_CASH2   =現金股利價值
**      PDK_STOCK_CASH3   =截止日後現金增資價值(已確認固定)
**      PDK_STOCK_QNTY3   =尚未截止之現金增資可認購股數
**      PDK_STOCK_PRICE3   =尚未截止之現金增資認購價格
**      @v    = 期貨價格波動率
**      @r    = 無風險利率
**      @q    = 現貨股利率(目前固定為0)
    **      @t    = (結算日期 - 今天交易日期)之天數 / 可交易日

2.	計算選擇權隱含波動度方式
(1) 預設隱含波動(v0)為1, 代入選擇權理論價格計算程式，計算出理論價。
(2) 計算理論價及推算價格(每日結算價)之差距(p0) 
(3) 判斷p0絕對值是否小於0.0001或計算次數小於200
(4) 計算vega值      /****************************************************************
      **  vega 公式
      **  pi() = 3.1415926535897931
      **  @s1  = @s * Exp(-@q * @t)
      **  @d1  = (Log(@s1/@k) + (@r +@v^2/2) * @t) / (@v*@t^0.5)
      **  @n1  = (2*pi())^(-0.5) * Exp(-(d1^2)/2)
      **
  **  @s= floor(@sp * PDK_STOCK_QNTY) + PDK_STOCK_CASH2 + PDK_STOCK_CASH3
            + floor(PDK_STOCK_QNTY3 * case when @sp > PDK_STOCK_PRICE3
                                  then @sp - PDK_STOCK_PRICE3
                                  else 0
                              end)

      **  @k= @kp * PDK_multiple
      **  @s   =以標的期貨價格計算之約定標的物價值
      **  @sp   = 標的期貨價格
      **  @k   =以履約價格計算之約定標的物價值
  **  @kp   = 履約價格
  **      PDK_multiple = 契約乘數
  **      @s    = 以標的期貨價格計算之約定標的物價值
  **      @sp   = 標的期貨價格
  **      @k    = 以履約價格計算之約定標的物價值
  **      @kp   = 履約價格
  **      PDK_STOCK_QNTY   =約定標的股數
  **      PDK_STOCK_CASH2   =現金股利價值
  **      PDK_STOCK_CASH3   =截止日後現金增資價值(已確認固定)
  **      PDK_STOCK_QNTY3   =尚未截止之現金增資可認購股數
      **      PDK_STOCK_PRICE3   =尚未截止之現金增資認購價格
      **  @v0  = 隱含波動率
      **  @r   = 無風險利率
      **  @q   = 現貨股利率(目前固定為0)
      **  @t   = (結算日期 - 今天交易日期)之天數 / 可交易日
      @vega = @n1*@s1*@t^0.5
(5) 以vega再次計算隱含波動度, v0 = v0 - p0 / vega 
(6) 以step(5)之新隱含波動度代入選擇權理論價格計算程式，計算出理論價。
(7) 重覆 (2)~(6)步驟，以理論價及每日結算價差距p0最小時的隱含波動度為最後的隱含波動度



======================================================================
10. 撮合/價差與商規（Business Rules）
======================================================================

* 篩選合約：活躍月、交易時段、成交量門檻
* 價格修整：Tick size（來自 PDK.PRICE_SCALE）
* 指標定義：Skew、Smile、TermSlope、VRP（TODO）

======================================================================
11. 例外處理與驗證（Validation & Error Handling）
========================================

* 參數檢核：必填參數、路徑存在、DB 可開啟
* 資料合理性：時間序單調、價量非負、價差上下界
* 失敗重試：I/O/DB 連線失敗 → 退避重試（次數/間隔）
* 友善訊息：錯誤碼與建議動作

======================================================================
12. 效能與資源（Performance）
======================

* 編譯：`make -j<N>`，建議 N=CPU 核心數（容後微調）
* 格式化：`clf100.sh`（ColumnLimit=100）
* 記憶體：資料窗大小、曲線節點上限
* 平行化：商品/合約層級並行，注意 DB 連線池
* 基準測試：樣本負載、TPS、延遲指標（TODO）

======================================================================
13. 日誌、監控、告警（Logging / Monitoring）
======================================================================

* 日誌等級：ERROR/INFO/DEBUG
* 輸出路徑：`logs/stovol_YYYYMMDD.log`
* 指標：處理筆數、錯誤率、平均反解迭代次數
* 告警：門檻策略（可選）

======================================================================
14. 測試策略（Testing）
======================================================================

* 單元測試：`TEST_MODE` 切換；Catch2/GTest（TODO）
* 端對端：testsuite/testgo.sh 輸入/期望輸出對比
* 回歸資料集：固定樣本檔與核對校驗碼

======================================================================
15. 部署與操作（Deployment & Ops）
======================================================================

* 依賴：glibc、sqlite3、C++14、clang
* 編譯：`make all -j<N> -s`
* 執行：stovol_s, stovol_r, stovol_x
* 任務排程：

======================================================================
16. 安全與權限（Security）
======================================================================


======================================================================
17. 相容性與版本（Compatibility & Versioning）
======================================================================


======================================================================
18. 風險與取捨（Risks & Trade-offs）
======================================================================

* 數值穩定 vs 效能：反解精度/迭代上限取捨
* 擬合平滑 vs 真實度：過度平滑風險
* DB 鎖競爭 vs 並行效率

======================================================================
19. 開放議題（Open Issues）
======================================================================

* OI-001：CLI `-o -a -b` 行為一致性與優先序
* OI-002：IV 反解採用演算法與收斂參數
* OI-003：曲線節點標準化（Δ 或 Moneyness）
* OI-004：日期鍵 6/8 碼一體化 API


======================================================================
A. 附錄：常用指令
======================================================================

* 一鍵格式化：`./clf100.sh`
* 範例執行：

  * `./stovol -o <GRPID> <CFG> <DB> <MATCH> <TPC>`
  * `./stovol -oab <GRPID> <CFG> <DB> <MATCH> <TPC>`
* SQLite：

======================================================================
B. 附錄：錯誤碼
======================================================================
opt/inc/stovol_msg.h
#define INVALID_OPTION      MSG_ERROR,   (PGM_ID_STOVOL*1000+2),  SEV_HIGH,   "Invalid option."
#define FILE_OPEN           MSG_ERROR,   (PGM_ID_STOVOL*1000+4),  SEV_HIGH,   "Open file failed."
#define TRADE_KIND_ENVERR   MSG_ERROR,   (PGM_ID_STOVOL*1000+5),  SEV_HIGH,   "Get TRADE_KIND environment variable failed."
#define NFS_PATH_ENVERR     MSG_ERROR,   (PGM_ID_STOVOL*1000+6),  SEV_HIGH,   "Get NFS_PATH environment variable failed."
#define ORDER_ERROR         MSG_ERROR,   (PGM_ID_STOVOL*1000+7),  SEV_HIGH,   "MTF Order error."
#define TRANS_ERROR         MSG_ERROR,   (PGM_ID_STOVOL*1000+8),  SEV_HIGH,   "MTF Transaction error."
#define TXWindexOTTime      MSG_WARNING, (PGM_ID_STOVOL*1000+9),  SEV_MEDIUM, "No Recv TXWindex Price Over 5min."
#define STAT_CREATE         MSG_ERROR,   (PGM_ID_STOVOL*1000+10), SEV_HIGH,   "Failed to create end stat file."
#define MEM_ALLOC           MSG_ERROR,   (PGM_ID_STOVOL*1000+11), SEV_HIGH,   "Memory allocation error."
#define UNKNOWN_EXCEPTION   MSG_ERROR,   (PGM_ID_STOVOL*1000+12), SEV_HIGH,   "Unknown exception."
#define PRODUCT_TYPE        MSG_ERROR,   (PGM_ID_STOVOL*1000+13), SEV_HIGH,   "Get PRODUCT_TYPE environment variable failed."
#define CALC_FREQUENCY      MSG_ERROR,   (PGM_ID_STOVOL*1000+14), SEV_HIGH,   "Get CALC_FREQUENCY environment variable failed."
#define TXINDEX_TIMES       MSG_ERROR,   (PGM_ID_STOVOL*1000+15), SEV_HIGH,   "Get CHECK_TXWindexPrice_TIMES environment variable failed."
#define STOVOL_FAIL         MSG_ERROR,   (PGM_ID_STOVOL*1000+97), SEV_HIGH,   "Fail."
#define STOVOL_WARN         MSG_ERROR,   (PGM_ID_STOVOL*1000+98), SEV_HIGH,   "Warn."
#define STOVOL_ERROR        MSG_ERROR,   (PGM_ID_STOVOL*1000+99), SEV_HIGH,   "Error."
/*------------------------------------------------------------------------------------------*
