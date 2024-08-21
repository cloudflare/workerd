// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Some macro metaprogramming helpers.

// =======================================================================================
// TODO(cleanup): Move these macros to libkj.

#define JSG_STRING_LITERAL_(...) #__VA_ARGS__
#define JSG_STRING_LITERAL(...) JSG_STRING_LITERAL_(__VA_ARGS__)
// JSG_STRING_LITERAL(foo, bar) expands to the string literal: "foo, bar"

#define JSG_EXPAND(...) __VA_ARGS__
// Identity macro. Often useful in macro hacking.

#define JSG_IF_NONEMPTY_(                                                                          \
    dummy, part1, part2, part3, part4, part5, part6, part7, part8, result, ...)                    \
  JSG_EXPAND result
#define JSG_IF_NONEMPTY_2_(...) dummy, ##__VA_ARGS__
#define JSG_IF_NONEMPTY_3_(...) JSG_IF_NONEMPTY_(__VA_ARGS__)
#define JSG_IF_NONEMPTY(arg, ...)                                                                  \
  JSG_IF_NONEMPTY_3_(JSG_IF_NONEMPTY_2_(arg), (__VA_ARGS__), (__VA_ARGS__), (__VA_ARGS__),         \
      (__VA_ARGS__), (__VA_ARGS__), (__VA_ARGS__), (__VA_ARGS__), (__VA_ARGS__), ())

#define JSG_CAT(a, b) a##b
// Paste two preprocessor tokens together. Useful in macro hacking.

// If the first argument is empty, expands to nothing.
//
// If the first argument is not empty, expands to the remaining arguments.
//
// So:
//    JSG_IF_NONEMPTY(, foo, bar) ->
//    JSG_IF_NONEMPTY(x, foo, bar) -> foo, bar
//
// (We support multiple "arguments" because often the output needs to contain commas, e.g. because
// it's a template type and the preprocessor doesn't recognize <...> as a grouping.)

#define JSG_FOR_EACH_(op, param, A1, A2, A3, A4, A5, A6, A7, A8, B1, B2, B3, B4, B5, B6, B7, B8,   \
    C1, C2, C3, C4, C5, C6, C7, C8, D1, D2, D3, D4, D5, D6, D7, D8, E1, E2, E3, E4, E5, E6, E7,    \
    E8, F1, F2, F3, F4, F5, F6, F7, F8, sentinel, ...)                                             \
  JSG_IF_NONEMPTY(A1, op(param, A1))                                                               \
  JSG_IF_NONEMPTY(A2, , op(param, A2))                                                             \
  JSG_IF_NONEMPTY(A3, , op(param, A3))                                                             \
  JSG_IF_NONEMPTY(A4, , op(param, A4))                                                             \
  JSG_IF_NONEMPTY(A5, , op(param, A5))                                                             \
  JSG_IF_NONEMPTY(A6, , op(param, A6))                                                             \
  JSG_IF_NONEMPTY(A7, , op(param, A7))                                                             \
  JSG_IF_NONEMPTY(A8, , op(param, A8))                                                             \
  JSG_IF_NONEMPTY(B1, , op(param, B1))                                                             \
  JSG_IF_NONEMPTY(B2, , op(param, B2))                                                             \
  JSG_IF_NONEMPTY(B3, , op(param, B3))                                                             \
  JSG_IF_NONEMPTY(B4, , op(param, B4))                                                             \
  JSG_IF_NONEMPTY(B5, , op(param, B5))                                                             \
  JSG_IF_NONEMPTY(B6, , op(param, B6))                                                             \
  JSG_IF_NONEMPTY(B7, , op(param, B7))                                                             \
  JSG_IF_NONEMPTY(B8, , op(param, B8))                                                             \
  JSG_IF_NONEMPTY(C1, , op(param, C1))                                                             \
  JSG_IF_NONEMPTY(C2, , op(param, C2))                                                             \
  JSG_IF_NONEMPTY(C3, , op(param, C3))                                                             \
  JSG_IF_NONEMPTY(C4, , op(param, C4))                                                             \
  JSG_IF_NONEMPTY(C5, , op(param, C5))                                                             \
  JSG_IF_NONEMPTY(C6, , op(param, C6))                                                             \
  JSG_IF_NONEMPTY(C7, , op(param, C7))                                                             \
  JSG_IF_NONEMPTY(C8, , op(param, C8))                                                             \
  JSG_IF_NONEMPTY(D1, , op(param, D1))                                                             \
  JSG_IF_NONEMPTY(D2, , op(param, D2))                                                             \
  JSG_IF_NONEMPTY(D3, , op(param, D3))                                                             \
  JSG_IF_NONEMPTY(D4, , op(param, D4))                                                             \
  JSG_IF_NONEMPTY(D5, , op(param, D5))                                                             \
  JSG_IF_NONEMPTY(D6, , op(param, D6))                                                             \
  JSG_IF_NONEMPTY(D7, , op(param, D7))                                                             \
  JSG_IF_NONEMPTY(D8, , op(param, D8))                                                             \
  JSG_IF_NONEMPTY(E1, , op(param, E1))                                                             \
  JSG_IF_NONEMPTY(E2, , op(param, E2))                                                             \
  JSG_IF_NONEMPTY(E3, , op(param, E3))                                                             \
  JSG_IF_NONEMPTY(E4, , op(param, E4))                                                             \
  JSG_IF_NONEMPTY(E5, , op(param, E5))                                                             \
  JSG_IF_NONEMPTY(E6, , op(param, E6))                                                             \
  JSG_IF_NONEMPTY(E7, , op(param, E7))                                                             \
  JSG_IF_NONEMPTY(E8, , op(param, E8))                                                             \
  JSG_IF_NONEMPTY(F1, , op(param, F1))                                                             \
  JSG_IF_NONEMPTY(F2, , op(param, F2))                                                             \
  JSG_IF_NONEMPTY(F3, , op(param, F3))                                                             \
  JSG_IF_NONEMPTY(F4, , op(param, F4))                                                             \
  JSG_IF_NONEMPTY(F5, , op(param, F5))                                                             \
  JSG_IF_NONEMPTY(F6, , op(param, F6))                                                             \
  JSG_IF_NONEMPTY(F7, , op(param, F7))                                                             \
  JSG_IF_NONEMPTY(F8, , op(param, F8))                                                             \
  JSG_IF_NONEMPTY(sentinel, error_JSG_FOR_EACH_only_supports_48_parameters)

#define JSG_FOR_EACH(op, param, ...)                                                               \
  JSG_FOR_EACH_(op, param, ##__VA_ARGS__, , , , , , , , , , , , , , , , , , , , , , , , , , , , ,  \
      , , , , , , , , , , , , , , , , , , , , )
// JSG_FOR_EACH(op, param, A, B, C, ...) expands to: op(param, A), op(param, B), op(param, C) ...
//
// Currently only supports up to 48 params.
