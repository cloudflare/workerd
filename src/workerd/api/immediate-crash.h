// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#if defined(__GNUC__) || defined(__clang__)

#if defined(__x86_64__) || defined(__i386__)

#define TRAP_SEQUENCE1_() asm volatile("int3")
#define TRAP_SEQUENCE2_() asm volatile("ud2")

#elif defined(__arm__)

#define TRAP_SEQUENCE1_() asm volatile("bkpt #0")
#define TRAP_SEQUENCE2_() asm volatile("udf #0")

#elif defined(__aarch64__)

#define TRAP_SEQUENCE1_() asm volatile("brk #0")
#define TRAP_SEQUENCE2_() asm volatile("hlt #0")

#elif defined(__powerpc64__)

#define TRAP_SEQUENCE1_() asm volatile(".4byte 0x7D821008")
#define TRAP_SEQUENCE2_() asm volatile("")

#elif defined(__s390x__)

#define TRAP_SEQUENCE1_() asm volatile(".2byte 0x0001")
#define TRAP_SEQUENCE2_() asm volatile("")

#else

#define TRAP_SEQUENCE1_() __builtin_trap()
#define TRAP_SEQUENCE2_() asm volatile("")

#endif  // Architecture check

#elif defined(_MSC_VER)

#if defined(_M_X64) || defined(_M_IX86)

#define TRAP_SEQUENCE1_() __debugbreak()
#define TRAP_SEQUENCE2_()

#elif defined(_M_ARM64)

#define TRAP_SEQUENCE1_() __debugbreak()
#define TRAP_SEQUENCE2_()

#else

#error No supported trap sequence for this MSVC architecture!

#endif  // _MSC_VER architecture check

#else

#error No supported compiler!

#endif  // Compiler check

#define TRAP_SEQUENCE_()                                                                           \
  do {                                                                                             \
    TRAP_SEQUENCE1_();                                                                             \
    TRAP_SEQUENCE2_();                                                                             \
  } while (false)

// Wrapping the trap sequence to allow its use inside constexpr functions.
#if defined(__GNUC__) || defined(__clang__)

#define WRAPPED_TRAP_SEQUENCE_()                                                                   \
  do {                                                                                             \
    [] { TRAP_SEQUENCE_(); }();                                                                    \
  } while (false)

#else

#define WRAPPED_TRAP_SEQUENCE_() TRAP_SEQUENCE_()

#endif  // Compiler check for wrapping

#if defined(__clang__) || defined(__GNUC__)

// __builtin_unreachable() hints to the compiler that this code path is not reachable.
#define IMMEDIATE_CRASH()                                                                          \
  ({                                                                                               \
    WRAPPED_TRAP_SEQUENCE_();                                                                      \
    __builtin_unreachable();                                                                       \
  })

#else

#define IMMEDIATE_CRASH() WRAPPED_TRAP_SEQUENCE_()

#endif  // __clang__ or __GNUC__
