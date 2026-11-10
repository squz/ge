// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Android footprint: Pss from /proc/self/smaps_rollup (proportional, so
// shared pages are not counted in full). If that file is missing or has
// no Pss line, RSS from /proc/self/statm. There is no kernel peak; the
// shared tracker keeps the max sample.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

#include "MemoryFootprint_internal.h"

namespace ge {
namespace {

uint64_t readPssBytes() {
    std::ifstream in("/proc/self/smaps_rollup");
    if (!in) return 0;
    std::string line;
    while (std::getline(in, line)) {
        unsigned long long kb = 0;
        if (std::sscanf(line.c_str(), "Pss: %llu", &kb) == 1)
            return static_cast<uint64_t>(kb) * 1024ull;
    }
    return 0;
}

uint64_t readStatmRssBytes() {
    std::ifstream in("/proc/self/statm");
    if (!in) return 0;
    unsigned long long sizePages = 0;
    unsigned long long rssPages = 0;
    if (!(in >> sizePages >> rssPages)) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return 0;
    return rssPages * static_cast<unsigned long long>(page);
}

}  // namespace

FootprintSample platformFootprintSample() {
    FootprintSample s;
    s.current = readPssBytes();
    if (s.current == 0)
        s.current = readStatmRssBytes();
    return s;
}

}  // namespace ge
