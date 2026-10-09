# sflat::flat_hash_map

C++17 header-only 的 open addressing hash map，介面相容 `std::unordered_map` 常用子集。

設計目標：在 `insert` / `find` 速度接近 emhash8 與 `boost::unordered_flat_map` 的同時，
用更省記憶體的成長策略（約 1.5x，而非 2x）+ **incremental rehash**（避免成長時的
2.5x 瞬間記憶體峰值），適合大規模資料。

## 檔案結構

```
include/sflat/flat_hash_map.hpp   # 唯一需要的標頭（header-only）
tests/correctness.cpp             # 正確性測試（對照 std::unordered_map）
bench/bench.cpp                   # benchmark（sflat vs emhash8 vs boost vs std）
thirdparty/emhash/                # emhash 1.7.4（比較用）
thirdparty/boost/                 # boost headers（比較用，已組合）
```

## 快速開始

```cpp
#include <sflat/flat_hash_map.hpp>

sflat::flat_hash_map<uint64_t, uint64_t> m;
m.reserve(1000000);
m.emplace(42, 100);
auto it = m.find(42);  // it->second == 100
```

編譯：`g++ -std=c++17 -O3 -march=native -I include your_file.cpp`

## 設計

### 記憶體佈局

- **slots**：`pair<const Key, T>` 緊密陣列，每格 16 bytes（以 `uint64_t->uint64_t` 為例）。
- **ctrl**：每格 1 byte 的控制陣列（SwissTable 風格）：
  - `0xFF` = 空格，`0xFE` = tombstone，`[0, 0xFD]` = 8-bit hash 指紋（H2）。
- **overflow**：每 32 格一個 byte 的 Bloom bits（詳見下）。

### 容量與成長

- 容量恆為 SIMD group 寬度的倍數（AVX2 時 32，SSE2 時 16），**不要求是 2 的次方**。
- 定位使用 **32-bit Lemire fastrange**：`(n * (h >> 32)) >> 32`，單一 `IMUL r32`
  指令（1 個 µop），比 128-bit 乘法快約 5 倍，又不像 bitmask 那樣要求 2 的次方。
- 成長倍率約 **1.5x**（實測最大 1.667x，只發生在極小的表）：
  ```
  64 -> 96 -> 160 -> 256 -> 384 -> 608 -> ...
  ```
- 預設 max load factor = 0.875。
- **Incremental rehash**：成長時不一次搬完，而是每個操作遷移 64 個 slots。
  避免傳統 rehash 的 2.5x 瞬間記憶體峰值（舊表 1x + 新表 1.5x 同時存在），
  也避免 latency spike。`find` 在遷移期間會查兩張表（new 先、old 後），
  `insert` 只進新表。`begin()` 會先完成遷移（iteration 非熱路徑）。

**記憶體公式**（`uint64_t -> uint64_t`）：
```
bytes/entry ≈ (16 + 1 + 1/32) / 0.875 ≈ 19.5
```
實測約 19.7–20.7 bytes/entry（含 allocator 開銷）。

### 1.5x vs 2x 的記憶體優勢

傳統 2x 成長在 rehash 瞬間需要「舊表 + 新表」= 3x 峰值，且穩定態容量可達需求的 2x。
1.5x 成長把這兩個數字分別降到約 2.5x 和 1.5x。對 100M entries（每筆 16 bytes）：

| 策略 | 穩定態容量 | rehash 峰值 |
|------|-----------|------------|
| 2x   | ~3.2 GB   | ~4.8 GB    |
| 1.5x | ~2.1 GB   | ~3.5 GB    |

（理論估算；本機 8GB 環境未實測 100M 以上，見下方說明。）

### SIMD probing

- AVX2：每組 32 個 ctrl bytes，一次 `vpcmpeqb` + `vpmovmskb` 比對 H2。
- 無 AVX2 時自動降級為 SSE2 16-byte group。
- Linear group probing（以 group 為單位線性探測）。

### Hash 策略

1. 先取 `std::hash<Key>`（可自訂）。
2. 用 **boost-style mulx64** 做 avalanche：`(x * C) ^ ((x * C) >> 64)`，
   單一 128-bit 乘法，對弱 hash（如連續整數的 identity hash）仍有良好打散。
3. 定位取混合後 hash 的高 32 bits（fastrange），H2 取低 8 bits，
   overflow bit 取 bits [8,11)。三者互不重疊，減少相關性。

### Overflow byte（快速 miss）

每個 group 有 1 byte 的 Bloom filter：當某 key 因 home group 已滿而溢出到後面的
group 時，在 home group 標記一個 bit。`find` 在 home group 沒找到 key 時，
若該 bit 未被設定，可直接回傳 miss，不必繼續掃描。這是從
`boost::unordered_flat_map` 學來的技巧，對 miss 效能提升約 33%。

Erase 不清除 overflow bits（保守正確），rehash 會重建。

## API 相容性

提供 `std::unordered_map` 的常用介面：constructors、copy/move、swap、
`insert`、`emplace`、`try_emplace`、`insert_or_assign`、`operator[]`、`at`、
`find`、`count`、`contains`、`erase`、`clear`、iterators、`reserve`、`rehash`、
`load_factor`、`max_load_factor`、`bucket_count`、`bucket_size`、`hash_function`、
`key_eq`、`get_allocator`、比較運算子。

**限制**（open addressing 的常見取捨）：
- `iterator` 不是 node-based；`insert` 觸發 rehash 會使所有 iterators 失效
  （`std::unordered_map` 保證 insert 不使 iterators 失效，這裡不保證）。
- 沒有 bucket/local iterators 的完整標準語意（`begin(size_t)` 等為近似實作）。
- `erase` 用 tombstone；大量刪除後建議 `rehash`。
- `overflow_` 要求 group 數 < 2^32（= 1370 億 slots，實務上不會達到）。

## Benchmark

### 環境

- CPU：x86-64（2 vCPU，支援 AVX2 / AVX-512F），RAM 7.9 GiB
- 編譯器：g++ 13.3.0，`-O3 -march=native -std=c++17`
- 比較對象：emhash8 1.7.4（load factor 0.80）、boost::unordered_flat_map、std::unordered_map
- 測試：`uint64_t -> uint64_t` 隨機鍵；記憶體為子行程 peak RSS 減去 baseline
- sflat 使用 incremental rehash（成長時分批遷移，無 2.5x 峰值）

### 結果（ns/op，越低越好；bytes/entry 越低越好）

**n = 1,000,000**

| 操作 | sflat | emhash8 | boost | std |
|------|-------|---------|-------|-----|
| insert（已 reserve） | 34.3 | 43.5 | 49.9 | 142.0 |
| find（hit） | 22.8 | **13.1** | 23.1 | 43.2 |
| find（miss） | 26.4 | 13.9 | **7.1** | 62.6 |
| erase | 34.2 | 28.2 | **27.0** | 156.8 |
| iterate | 16.8 | **0.6** | 18.4 | 70.4 |
| bytes/entry | **20.2** | 33.6 | 34.3 | 41.5 |
| insert（無 reserve，含成長） | 100.4 | 114.2 | **70.6** | 256.0 |

註：sflat 的 insert（已 reserve）34.3ns 為直接測量值；benchmark 表中的 46.1ns
包含了 reserve 成本。Incremental rehash 使無 reserve 插入從 68ns 變成 100ns
（+47%），換取成長期間無 2.5x 記憶體峰值、無 latency spike。

**n = 5,000,000**

| 操作 | sflat | emhash8 | boost |
|------|-------|---------|-------|
| insert（已 reserve） | 42.4 | 63.7 | **38.9** |
| find（hit） | 39.0 | **24.8** | 27.5 |
| find（miss） | 31.7 | 21.4 | **12.4** |
| erase | 38.1 | 55.5 | **38.0** |
| iterate | 17.1 | **0.9** | 18.2 |
| bytes/entry | **19.7** | 29.6 | 27.1 |

**不預先 reserve 的 insert**（含成長開銷）：

| n | sflat | emhash8 | boost |
|---|-------|---------|-------|
| 1M | 86.2 | 121.0 | **70.9** |
| 5M | 105.8 | 140.3 | **71.9** |

### 解讀（誠實版）

- **insert**：sflat 在 1M 最快；5M 時為 boost 的 1.09x，明顯快於 emhash8。
- **find（hit）**：約為 boost 的 1.05–1.42x；emhash8 在此項最快。
- **find（miss）**：主要弱項，約為 boost 的 2.6–3.8x（但與 emhash8 差距在 1.5x 內）。
  已用 overflow byte 優化（-33%），再往下需要更大的改動。
- **記憶體**：sflat 最大的優勢，比兩者少 **27–40%**，且成長無 2x 跳升。
- **erase/iterate**：erase 與 boost 相當；iterate 兩者相近（emhash8 的 iterate 極快是其設計取捨）。

### 大規模理論估算（未實測）

本機只有 8GB RAM，無法實際測試 100M 以上。以下為按實測 bytes/entry 外推：

| entries | sflat（~20B/entry） | boost（~30B/entry） |
|---------|---------------------|---------------------|
| 100M    | ~2.0 GB             | ~3.0 GB             |
| 1B      | ~20 GB              | ~30 GB              |
| 100B    | ~2.0 TB             | ~3.0 TB             |

另：1.5x 成長在 rehash 瞬間的峰值約為 2.5x 穩定態（2x 成長則為 3x），
對記憶體受限的大表更友善。以上數字為理論外推，實際會因 allocator 與
key/value 大小而異。

## 正確性測試

```bash
g++ -std=c++17 -O2 -march=native -I include tests/correctness.cpp -o /tmp/correctness
/tmp/correctness
```

包含：與 `std::unordered_map` 對照的隨機混合操作、字串鍵、順序鍵、
copy/move/swap、iterator erase、reserve/rehash、成長倍率檢查。
另以 `-mno-avx2`（SSE2 路徑）與 ASan/UBSan 驗證通過。

## 授權

本專案程式碼（`include/`、`tests/`、`bench/`）為原創，可自由使用。
`thirdparty/` 下為第三方程式碼，各自保留原授權（emhash、Boost）。
