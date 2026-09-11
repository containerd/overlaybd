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

// SVE1 implementation of the LSMT B+tree inner search.
//
// This TU is compiled with -march=armv8.2-a+sve (SVE1, NOT +sve2).
// armv8.2-a is the minimal architecture baseline that can carry the
// +sve extension -- a permission floor for code generation, not a
// per-generation target: SVE hardware is at least ARMv8.2-A, newer
// generations run this same binary unchanged, and runtime dispatch
// (HWCAP_SVE) decides whether this path is taken at all.
// SVE1 code is a strict architectural subset of SVE2, so the same
// code serves SVE1 and SVE2-capable hardware. Never compile this TU
// with +sve2 -- hardware without SVE2 would fault with SIGILL on
// SVE2 instructions.
#include <cstdint>
#include <arm_sve.h>

extern "C" uint32_t lsmt_sve_inner_search_u32(const uint32_t *base, uint32_t x) {
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < 16;) {
        svbool_t pg = svwhilelt_b32((uint64_t)i, (uint64_t)16);
        svuint32_t d = svld1_u32(pg, base + i);
        svbool_t c = svcmple_u32(pg, d, svdup_u32(x));
        cnt += (uint32_t)svcntp_b32(pg, c);
        i += svcntw();
    }
    return cnt;
}

extern "C" uint32_t lsmt_sve_inner_search_u64(const uint64_t *base, uint64_t x) {
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < 8;) {
        svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)8);
        svuint64_t d = svld1_u64(pg, base + i);
        svbool_t c = svcmple_u64(pg, d, svdup_u64(x));
        cnt += (uint32_t)svcntp_b64(pg, c);
        i += svcntd();
    }
    return cnt;
}

extern "C" uint32_t lsmt_sve_vl_bytes() {
    return svcntb();
}
