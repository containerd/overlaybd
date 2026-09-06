/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

// Cross-validation of the aarch64 inner-search kernels (NEON, SVE) against
// an independent scalar reference implemented here in the test. Kernel level
// only: randomized + boundary inputs on single nodes (16x u32 / 8x u64).
// Tree-level integration is exercised on real hardware via the dispatch
// tier log (see docs/lsmt_arm64_sve_code.md for the full story).
#include <gtest/gtest.h>
#include <cstdint>

#if defined(__aarch64__)
#include <sys/auxv.h>

// NEON is architecturally mandatory on aarch64. The SVE kernels live in
// the separately-compiled SVE TU and are callable only when that TU was
// built (see lsmt/CMakeLists.txt for the toolchain probe).
extern "C" uint32_t lsmt_neon_inner_search_u32(const uint32_t *base, uint32_t x);
extern "C" uint32_t lsmt_neon_inner_search_u64(const uint64_t *base, uint64_t x);
#if defined(OVERLAYBD_ENABLE_SVE)
extern "C" uint32_t lsmt_sve_inner_search_u32(const uint32_t *base, uint32_t x);
extern "C" uint32_t lsmt_sve_inner_search_u64(const uint64_t *base, uint64_t x);
#endif

namespace {

// Independent scalar reference: number of keys <= x in a sorted node.
// Written from the spec here rather than shared with production code, so a
// bug shared between production implementations cannot hide the mismatch.
uint32_t ref32(const uint32_t *k, uint32_t x) {
    uint32_t m = 0;
    for (int i = 0; i < 16; i++)
        m |= ((k[i] <= x) << i);
    return __builtin_popcount(m);
}
uint32_t ref64(const uint64_t *k, uint64_t x) {
    uint32_t m = 0;
    for (int i = 0; i < 8; i++)
        m |= ((k[i] <= x) << i);
    return __builtin_popcount(m);
}

// Fixed-seed xorshift64: any failure is reproducible.
struct Rng {
    uint64_t s = 0x243F6A8885A308D3ULL;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

constexpr uint32_t BX32[] = {0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu};
constexpr uint64_t BX64[] = {0, 1, 0x7fffffffffffffffUL, 0x8000000000000000UL,
                             UINT64_MAX};
constexpr int kTrials = 4096;

bool sve_available() { return getauxval(AT_HWCAP) & (1UL << 22); }

// Generates ascending nodes and mixed queries (boundary / exact-key /
// random); returns the number of mismatches between `fn` and the reference.
template <typename Fn>
int mismatches_u32(Fn &&fn) {
    Rng rng;
    int bad = 0;
    for (int t = 0; t < kTrials; t++) {
        uint32_t k[16];
        uint32_t v = (uint32_t)rng.next();
        for (auto &kk : k) {
            v += 1 + (uint32_t)(rng.next() % 1000);
            kk = v;
        }
        uint32_t x;
        if ((t & 15) == 0)
            x = BX32[(t >> 4) % 5]; // boundary values
        else if ((t & 3) == 0)
            x = k[rng.next() & 15]; // exact-key hits
        else
            x = (uint32_t)rng.next(); // random
        bad += (fn(k, x) != ref32(k, x));
    }
    return bad;
}
template <typename Fn>
int mismatches_u64(Fn &&fn) {
    Rng rng;
    int bad = 0;
    for (int t = 0; t < kTrials; t++) {
        uint64_t k[8];
        uint64_t v = rng.next();
        for (auto &kk : k) {
            v += 1 + (rng.next() % 1000);
            kk = v;
        }
        uint64_t x;
        if ((t & 15) == 0)
            x = BX64[(t >> 4) % 5];
        else if ((t & 3) == 0)
            x = k[rng.next() & 7];
        else
            x = rng.next();
        bad += (fn(k, x) != ref64(k, x));
    }
    return bad;
}

} // namespace

// NEON runs on every aarch64 machine, no gating needed.
TEST(inner_search, neon_vs_reference) {
    EXPECT_EQ(0, mismatches_u32([](const uint32_t *k,
                                   uint32_t x) { return lsmt_neon_inner_search_u32(k, x); }));
    EXPECT_EQ(0, mismatches_u64([](const uint64_t *k,
                                   uint64_t x) { return lsmt_neon_inner_search_u64(k, x); }));
}

#if defined(OVERLAYBD_ENABLE_SVE)
// SVE kernels fault on hardware without SVE, so skip (not fail) when the
// machine does not report HWCAP_SVE.
TEST(inner_search, sve_vs_reference) {
    if (!sve_available())
        GTEST_SKIP() << "SVE not available on this machine";
    EXPECT_EQ(0, mismatches_u32([](const uint32_t *k,
                                   uint32_t x) { return lsmt_sve_inner_search_u32(k, x); }));
    EXPECT_EQ(0, mismatches_u64([](const uint64_t *k,
                                   uint64_t x) { return lsmt_sve_inner_search_u64(k, x); }));
}
#endif

#else // !__aarch64__

// Keep a visible test on non-aarch64 CI runners, where the aarch64 kernels
// are not part of the build.
TEST(inner_search, aarch64_only) {
    GTEST_SKIP() << "aarch64 only";
}

#endif // __aarch64__
