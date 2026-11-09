# 250M 不 reserve insert：VmSize 與 RSS

uint64_t → uint64_t；唯一 key 即時生成，不配置輸入陣列；不 reserve、不 find。六種 map 共用 std::hash、相同輸入順序、CPU affinity、原有版本與編譯設定；每種一個獨立程序。目標 250,000,000 entries，完成 2/6。

**本表為整個程序用量，沒有扣除 baseline**（baseline 仍保存於原始 CSV）。VmSize 是當下整個虛擬位址空間，包含預留、尚未 resident 的映射；RSS 是當下常駐頁。VmPeak 是程序歷來最大 VmSize，RSS peak 是 getrusage 與已監控 VmHWM 的最大值，兩個峰值不一定發生於同一時刻。單位 GiB = 2^30 bytes。

## 完成目標：最後一筆 insert 後、map 仍存活

| Map | Entries | VmSize GiB | RSS GiB | VmPeak GiB | RSS peak GiB |
|---|---:|---:|---:|---:|---:|
| sflat | 250,000,000 | 4.822 | 4.818 | 8.035 | 5.031 |
| Google sparse | 250,000,000 | 4.298 | 4.294 | 4.298 | 4.294 |

## 未完成目標：監控發出 SIGKILL 前的同一個取樣

| Map | 最後確認筆數下界 | 停止時 VmSize GiB | 停止時 RSS GiB | 已觀測 VmPeak GiB | 已觀測 RSS peak GiB |
|---|---:|---:|---:|---:|---:|
| Boost flat | ≥220,000,000 | 12.006 | 6.964 | ≥12.006 | ≥6.964 |
| Ankerl dense | ≥210,000,000 | 8.131 | 6.965 | ≥8.131 | ≥6.965 |
| Abseil flat | ≥230,000,000 | 12.756 | 6.967 | ≥12.756 | ≥6.967 |
| Google dense | ≥130,000,000 | 12.006 | 6.964 | ≥12.006 | ≥6.964 |

未完成者以上用量是停止取樣，不能當成 250M 的用量，也不能當成完成 250M 所需峰值；最後確認筆數來自每 10M checkpoint，停止當時實際筆數可能更高。取樣與 SIGKILL 之間仍可能有少量配置，因此未完成者 peak 是下界。

RLIMIT_AS soft/hard 均為 unlimited，沒有設定虛擬位址限制。每約 5 ms 監控實際 RSS；程序 RSS 預算 7 GiB、整個環境 memory.current 保護線 7.5 GiB，環境硬上限 8 GiB，無 swap。總用量包含其他程序與 page cache，可能先觸發總用量保護。這是取樣監控，並非 Linux RLIMIT_RSS 硬限制。

完成者的 VmSize/VmPeak 從程序內 /proc/self/status 讀取，RSS 從 smaps_rollup 讀取；未完成者的 VmSize/RSS 同取自外部監控的 /proc/PID/status。兩種 RSS kernel accounting 可能有少量差異。

```sh
python3 tools/run_insert_stream.py --entries 250000000 --limit-gib 0 --rss-limit-gib 7 --cgroup-limit-gib 7.5 --out results-flat64-250m-vm-rss
python3 tools/summarize_insert_vm_rss.py results-flat64-250m-vm-rss
```

原始 CSV、停止時快照、觀測峰值、metadata、source/dependency/binary SHA、測量用 C++ source snapshot、前後 OOM event counters 均保留。這是單次記憶體測試；elapsed 包含即時 key 產生、checkpoint 與監控成本，不應直接與先前純 insert timing 排名混用。
