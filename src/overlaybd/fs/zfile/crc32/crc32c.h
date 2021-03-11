#include <stdint.h>
#include <string>

#pragma once

namespace FileSystem {

namespace crc32 {

extern uint32_t crc32c(const void *data, size_t nbytes);
extern uint32_t crc32c(const std::string &text);

extern uint32_t crc32c_extend(const void *data, size_t nbytes, uint32_t crc);
extern uint32_t crc32c_extend(const std::string &text, uint32_t crc);

namespace testing {
extern uint32_t crc32c_slow(const void *data, size_t nbytes, uint32_t crc);
extern uint32_t crc32c_fast(const void *data, size_t nbytes, uint32_t crc);
} // namespace testing

} // namespace crc32

} // namespace FileSystem
