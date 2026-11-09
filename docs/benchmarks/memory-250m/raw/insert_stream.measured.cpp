// Streaming no-reserve insertion: no O(N) input allocation.
#include "bench.h"
#include <cstdio>
#include <new>
#include <limits>
uint64_t permute(uint64_t x) {
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
// Linux VmSize includes all mapped/reserved address space, resident or not.
std::pair<size_t,size_t> virtualMemory() {
    std::ifstream status("/proc/self/status");
    std::string line;
    size_t size_kib=0, peak_kib=0;
    while (std::getline(status,line)) {
        if (line.compare(0,7,"VmSize:")==0) size_kib=std::stoull(line.substr(7));
        if (line.compare(0,7,"VmPeak:")==0) peak_kib=std::stoull(line.substr(7));
    }
    if (!size_kib || !peak_kib) throw std::runtime_error("Cannot read Linux VmSize/VmPeak");
    return {size_kib*1024,peak_kib*1024};
}
int main(int argc, char** argv) {
    const uint64_t n = argc > 1 ? std::stoull(argv[1]) : 500000000;
    const auto baseline_vm = virtualMemory().first;
    const auto baseline = getCurrentRSS();
    auto start = Bench::clock::now();
    uint64_t completed = 0, cursor = 0;
    auto report = [&](const char* status) {
        auto current = getCurrentRSS(), peak = getPeakRSS();
        const auto vm = virtualMemory();
        std::printf("%s,%llu,%.6f,%zu,%zu,%zu,%zu,%zu,%zu\n", status,
            (unsigned long long)completed,
            std::chrono::duration<double>(Bench::clock::now()-start).count(),
            current, peak, baseline, vm.first, vm.second, baseline_vm);
        std::fflush(stdout);
    };
    std::puts("status,inserted,elapsed_s,rss_bytes,peak_bytes,baseline_bytes,vmsize_bytes,vmpeak_bytes,baseline_vmsize_bytes");
    using M = Map<uint64_t,uint64_t>;
    M map;
#ifdef MAP_BENCH_GOOGLE_ADAPTER
    auto& native = static_cast<typename M::native_type&>(map);
#else
    auto& native = map;
#endif
    try {
        while (completed < n) {
            const auto k = permute(cursor++);
            if (k >= (std::numeric_limits<uint64_t>::max)()-1) continue;
            native.insert(typename M::value_type(k,k ^ UINT64_C(0x123456789abcdef0)));
            ++completed;
            if (completed % 10000000 == 0) report("checkpoint");
        }
        if (native.size() != n) return 3;
        report("completed");
    } catch (const std::bad_alloc&) {
        report("allocation_failed");
        // Some native containers do not preserve destructibility after OOM.
        std::_Exit(2);
    }
}
