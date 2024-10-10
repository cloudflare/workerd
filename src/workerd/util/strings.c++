#include "strings.h"

namespace workerd {
namespace {
constexpr uint64_t broadcast(uint8_t v) noexcept {
  return 0x101010101010101ull * v;
}

// SWAR routine designed to convert ASCII uppercase letters to lowercase.
// Let's process 8 bytes (64 bits) at a time using a single 64-bit int,
// treating as 8 parallel 8-bit values.
// PS: This will enable the use of auto-vectorization.
constexpr void toLowerAscii(char* input, size_t length) noexcept {
  constexpr const uint64_t broadcast_80 = broadcast(0x80);
  constexpr const uint64_t broadcast_Ap = broadcast(128 - 'A');
  constexpr const uint64_t broadcast_Zp = broadcast(128 - 'Z' - 1);
  size_t i = 0;
  for (; i + 7 < length; i += 8) {
    uint64_t word{};
    memcpy(&word, input + i, sizeof(word));
    word ^= (((word + broadcast_Ap) ^ (word + broadcast_Zp)) & broadcast_80) >> 2;
    memcpy(input + i, &word, sizeof(word));
  }
  if (i < length) {
    uint64_t word{};
    memcpy(&word, input + i, length - i);
    word ^= (((word + broadcast_Ap) ^ (word + broadcast_Zp)) & broadcast_80) >> 2;
    memcpy(input + i, &word, length - i);
  }
}

constexpr void toUpperAscii(char* input, size_t length) noexcept {
  constexpr const uint64_t broadcast_80 = broadcast(0x80);
  constexpr const uint64_t broadcast_ap = broadcast(128 - 'a');
  constexpr const uint64_t broadcast_zp = broadcast(128 - 'z' - 1);
  size_t i = 0;
  for (; i + 7 < length; i += 8) {
    uint64_t word{};
    memcpy(&word, input + i, sizeof(word));
    word ^= (((word + broadcast_ap) ^ (word + broadcast_zp)) & broadcast_80) >> 2;
    memcpy(input + i, &word, sizeof(word));
  }
  if (i < length) {
    uint64_t word{};
    memcpy(&word, input + i, length - i);
    word ^= (((word + broadcast_ap) ^ (word + broadcast_zp)) & broadcast_80) >> 2;
    memcpy(input + i, &word, length - i);
  }
}
}  // namespace

kj::String toLower(kj::String&& str) {
  toLowerAscii(str.begin(), str.size());
  return kj::mv(str);
}

kj::String toUpper(kj::String&& str) {
  toUpperAscii(str.begin(), str.size());
  return kj::mv(str);
}

kj::String toLower(kj::ArrayPtr<const char> ptr) {
  return toLower(kj::str(ptr));
}

kj::ArrayPtr<const char> trimLeadingAndTrailingWhitespace(kj::ArrayPtr<const char> ptr) {
  size_t start = 0;
  auto end = ptr.size();
  while (start < end && isAsciiWhitespace(ptr[start])) {
    start++;
  }
  while (end > start && isAsciiWhitespace(ptr[end - 1])) {
    end--;
  }
  return ptr.slice(start, end).asChars();
}

kj::ArrayPtr<const char> trimTailingWhitespace(kj::ArrayPtr<const char> ptr) {
  auto end = ptr.size();
  while (end > 0 && isAsciiWhitespace(ptr[end - 1])) {
    end--;
  }
  return ptr.first(end).asChars();
}

kj::Array<kj::byte> stripInnerWhitespace(kj::ArrayPtr<kj::byte> input) {
  auto result = kj::heapArray<kj::byte>(input.size());
  size_t len = 0;
  for (const kj::byte c: input) {
    if (!isAsciiWhitespace(c)) {
      result[len++] = c;
    }
  }
  return result.first(len).attach(kj::mv(result));
};

}  // namespace workerd
