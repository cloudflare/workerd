// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A port of src/workerd/api/node/buffer-string-search.h, used by the
// TypeScript implementation of `node-internal:buffer`. The structure, names,
// and search strategies follow the C++ header. `Char` is either a byte
// (Uint8Array) or a UTF-16 code unit (Uint16Array).

// Typed-array element reads below are always in bounds, as they are in the C++.
/* eslint-disable @typescript-eslint/no-non-null-assertion */

type CharArray = Uint8Array | Uint16Array;

class Vector {
  #start: CharArray;
  #length: number;
  #isForward: boolean;

  constructor(data: CharArray, length: number, isForward: boolean) {
    this.#start = data;
    this.#length = length;
    this.#isForward = isForward;
  }

  // Returns the start of the memory range.
  // For vector v this is NOT necessarily &v[0], see forward().
  start(): CharArray {
    return this.#start;
  }

  // Returns the length of the vector, in characters.
  length(): number {
    return this.#length;
  }

  // Returns true if the Vector is front-to-back, false if back-to-front.
  // In the latter case, v[0] corresponds to the *end* of the memory range.
  forward(): boolean {
    return this.#isForward;
  }

  at(index: number): number {
    return this.#start[this.#isForward ? index : this.#length - index - 1]!;
  }
}

//---------------------------------------------------------------------
// String Search object.
//---------------------------------------------------------------------

// Cap on the maximal shift in the Boyer-Moore implementation. By setting a
// limit, we can fix the size of tables. For a needle longer than this limit,
// search will not be optimal, since we only build tables for a suffix
// of the string, but it is a safe approximation.
const kBMMaxShift = 250;

// Reduce alphabet to this size.
// One of the tables used by Boyer-Moore and Boyer-Moore-Horspool has size
// proportional to the input alphabet. We reduce the alphabet size by
// equating input characters modulo a smaller alphabet size. This gives
// a potentially less efficient searching, but is a safe approximation.
// For needles using only characters in the same Unicode 256-code point page,
// there is no search speed degradation.
const kLatin1AlphabetSize = 256;
const kUC16AlphabetSize = 256;

// Bad-char shift table stored in the state. It's length is the alphabet size.
// For patterns below this length, the skip length of Boyer-Moore is too short
// to compensate for the algorithmic overhead compared to simple brute force.
const kBMMinPatternLength = 8;

const kBoyerMooreHorspool = 0;
const kBoyerMoore = 1;
const kInitial = 2;
const kLinear = 3;
const kSingleChar = 4;

type SearchStrategy =
  | typeof kBoyerMooreHorspool
  | typeof kBoyerMoore
  | typeof kInitial
  | typeof kLinear
  | typeof kSingleChar;

// Finds the first occurrence of pattern[0] in the string `subject`, starting
// at `index`. Does not check that the whole pattern matches.
//
// The C++ searches raw memory with memchr / memrchr (for two-byte
// characters, searching for one of the two bytes and then verifying the
// character). The typed-array indexOf / lastIndexOf used here compare whole
// characters directly, which finds the same position.
function findFirstCharacter(
  pattern: Vector,
  subject: Vector,
  index: number
): number {
  const patternFirstChar = pattern.at(0);
  const subjLen = subject.length();
  const maxN = subjLen - pattern.length() + 1;
  const start = subject.start();

  if (subject.forward()) {
    // memchr(subject.start() + index, pattern_first_char, max_n - index)
    const rawPos = start.indexOf(patternFirstChar, index);
    if (rawPos === -1 || rawPos >= maxN) {
      return subjLen;
    }
    return rawPos;
  }

  // MemrchrFill(subject.start() + pattern.length() - 1, pattern_first_char,
  //             max_n - index)
  const regionStart = pattern.length() - 1;
  const regionEnd = regionStart + (maxN - index);
  const rawPos = start.lastIndexOf(patternFirstChar, regionEnd - 1);
  if (rawPos === -1 || rawPos < regionStart) {
    return subjLen;
  }
  return subjLen - rawPos - 1;
}

class StringSearch {
  // Store for the BoyerMoore(Horspool) bad char shift table.
  #badCharShiftTable = new Int32Array(kUC16AlphabetSize);
  // Store for the BoyerMoore good suffix shift table.
  #goodSuffixShiftTable = new Int32Array(kBMMaxShift + 1);
  // Table used temporarily while building the BoyerMoore good suffix
  // shift table.
  #suffixTable = new Int32Array(kBMMaxShift + 1);

  // The pattern to search for.
  #pattern: Vector;
  #strategy: SearchStrategy;
  #isTwoByte: boolean;
  // Cache value of Max(0, pattern_length() - kBMMaxShift)
  #start = 0;

  constructor(pattern: Vector, isTwoByte: boolean) {
    this.#pattern = pattern;
    this.#isTwoByte = isTwoByte;
    if (pattern.length() >= kBMMaxShift) {
      this.#start = pattern.length() - kBMMaxShift;
    }

    const patternLength = this.#pattern.length();
    if (patternLength < kBMMinPatternLength) {
      if (patternLength === 1) {
        this.#strategy = kSingleChar;
        return;
      }
      this.#strategy = kLinear;
      return;
    }
    this.#strategy = kInitial;
  }

  search(subject: Vector, index: number): number {
    switch (this.#strategy) {
      case kBoyerMooreHorspool:
        return this.#boyerMooreHorspoolSearch(subject, index);
      case kBoyerMoore:
        return this.#boyerMooreSearch(subject, index);
      case kInitial:
        return this.#initialSearch(subject, index);
      case kLinear:
        return this.#linearSearch(subject, index);
      case kSingleChar:
        return this.#singleCharSearch(subject, index);
    }
  }

  #alphabetSize(): number {
    if (!this.#isTwoByte) {
      // Latin1 needle.
      return kLatin1AlphabetSize;
    } else {
      // UC16 needle.
      return kUC16AlphabetSize;
    }
  }

  #charOccurrence(badCharOccurrence: Int32Array, charCode: number): number {
    if (!this.#isTwoByte) {
      return badCharOccurrence[charCode]!;
    }
    // Both pattern and subject are UC16. Reduce character to equivalence class.
    const equivClass = charCode % kUC16AlphabetSize;
    return badCharOccurrence[equivClass]!;
  }

  //---------------------------------------------------------------------
  // Single Character Pattern Search Strategy
  //---------------------------------------------------------------------

  #singleCharSearch(subject: Vector, index: number): number {
    return findFirstCharacter(this.#pattern, subject, index);
  }

  //---------------------------------------------------------------------
  // Linear Search Strategy
  //---------------------------------------------------------------------

  // Simple linear search for short patterns. Never bails out.
  #linearSearch(subject: Vector, index: number): number {
    const pattern = this.#pattern;
    const n = subject.length() - pattern.length();
    for (let i = index; i <= n; i++) {
      i = findFirstCharacter(pattern, subject, i);
      if (i === subject.length()) return subject.length();

      let matches = true;
      for (let j = 1; j < pattern.length(); j++) {
        if (pattern.at(j) !== subject.at(i + j)) {
          matches = false;
          break;
        }
      }
      if (matches) {
        return i;
      }
    }
    return subject.length();
  }

  //---------------------------------------------------------------------
  // Boyer-Moore string search
  //---------------------------------------------------------------------

  #boyerMooreSearch(subject: Vector, startIndex: number): number {
    const pattern = this.#pattern;
    const subjectLength = subject.length();
    const patternLength = pattern.length();
    // Only preprocess at most kBMMaxShift last characters of pattern.
    const start = this.#start;

    const badCharOccurrence = this.#badCharShiftTable;
    // The C++ biases a pointer into good_suffix_shift_table_ by -start_.
    const goodSuffixShift = this.#goodSuffixShiftTable;
    const goodSuffixShiftBias = -this.#start;

    const lastChar = pattern.at(patternLength - 1);
    let index = startIndex;
    // Continue search from i.
    while (index <= subjectLength - patternLength) {
      let j = patternLength - 1;
      let c: number;
      while (lastChar !== (c = subject.at(index + j))) {
        const shift = j - this.#charOccurrence(badCharOccurrence, c);
        index += shift;
        if (index > subjectLength - patternLength) {
          return subject.length();
        }
      }
      while (pattern.at(j) === (c = subject.at(index + j))) {
        if (j === 0) {
          return index;
        }
        j--;
      }
      if (j < start) {
        // we have matched more than our tables allow us to be smart about.
        // Fall back on BMH shift.
        index +=
          patternLength - 1 - this.#charOccurrence(badCharOccurrence, lastChar);
      } else {
        const gsShift = goodSuffixShift[j + 1 + goodSuffixShiftBias]!;
        const bcOcc = this.#charOccurrence(badCharOccurrence, c);
        let shift = j - bcOcc;
        if (gsShift > shift) {
          shift = gsShift;
        }
        index += shift;
      }
    }

    return subject.length();
  }

  #populateBoyerMooreTable(): void {
    const pattern = this.#pattern;
    const patternLength = pattern.length();
    // Only look at the last kBMMaxShift characters of pattern (from start_
    // to pattern_length).
    const start = this.#start;
    const length = patternLength - start;

    // Biased tables so that we can use pattern indices as table indices,
    // even if we only cover the part of the pattern from offset start.
    const shiftTable = this.#goodSuffixShiftTable;
    const suffixTable = this.#suffixTable;
    const bias = -this.#start;

    // Initialize table.
    for (let i = start; i < patternLength; i++) {
      shiftTable[i + bias] = length;
    }
    shiftTable[patternLength + bias] = 1;
    suffixTable[patternLength + bias] = patternLength + 1;

    if (patternLength <= start) {
      return;
    }

    // Find suffixes.
    const lastChar = pattern.at(patternLength - 1);
    let suffix = patternLength + 1;
    {
      let i = patternLength;
      while (i > start) {
        const c = pattern.at(i - 1);
        while (suffix <= patternLength && c !== pattern.at(suffix - 1)) {
          if (shiftTable[suffix + bias] === length) {
            shiftTable[suffix + bias] = suffix - i;
          }
          suffix = suffixTable[suffix + bias]!;
        }
        suffixTable[--i + bias] = --suffix;
        if (suffix === patternLength) {
          // No suffix to extend, so we check against last_char only.
          while (i > start && pattern.at(i - 1) !== lastChar) {
            if (shiftTable[patternLength + bias] === length) {
              shiftTable[patternLength + bias] = patternLength - i;
            }
            suffixTable[--i + bias] = patternLength;
          }
          if (i > start) {
            suffixTable[--i + bias] = --suffix;
          }
        }
      }
    }
    // Build shift table using suffixes.
    if (suffix < patternLength) {
      for (let i = start; i <= patternLength; i++) {
        if (shiftTable[i + bias] === length) {
          shiftTable[i + bias] = suffix - start;
        }
        if (i === suffix) {
          suffix = suffixTable[suffix + bias]!;
        }
      }
    }
  }

  //---------------------------------------------------------------------
  // Boyer-Moore-Horspool string search.
  //---------------------------------------------------------------------

  #boyerMooreHorspoolSearch(subject: Vector, startIndex: number): number {
    const pattern = this.#pattern;
    const subjectLength = subject.length();
    const patternLength = pattern.length();
    const charOccurrences = this.#badCharShiftTable;
    let badness = -patternLength;

    // How bad we are doing without a good-suffix table.
    const lastChar = pattern.at(patternLength - 1);
    const lastCharShift =
      patternLength - 1 - this.#charOccurrence(charOccurrences, lastChar);

    // Perform search
    let index = startIndex; // No matches found prior to this index.
    while (index <= subjectLength - patternLength) {
      let j = patternLength - 1;
      let subjectChar: number;
      while (lastChar !== (subjectChar = subject.at(index + j))) {
        const bcOcc = this.#charOccurrence(charOccurrences, subjectChar);
        const shift = j - bcOcc;
        index += shift;
        badness += 1 - shift; // at most zero, so badness cannot increase.
        if (index > subjectLength - patternLength) {
          return subjectLength;
        }
      }
      j--;
      while (pattern.at(j) === subject.at(index + j)) {
        if (j === 0) {
          return index;
        }
        j--;
      }
      index += lastCharShift;
      // Badness increases by the number of characters we have
      // checked, and decreases by the number of characters we
      // can skip by shifting. It's a measure of how we are doing
      // compared to reading each character exactly once.
      badness += patternLength - j - lastCharShift;
      if (badness > 0) {
        this.#populateBoyerMooreTable();
        this.#strategy = kBoyerMoore;
        return this.#boyerMooreSearch(subject, index);
      }
    }
    return subject.length();
  }

  #populateBoyerMooreHorspoolTable(): void {
    const pattern = this.#pattern;
    const patternLength = pattern.length();

    const badCharOccurrence = this.#badCharShiftTable;

    // Only preprocess at most kBMMaxShift last characters of pattern.
    const start = this.#start;
    // Run forwards to populate bad_char_table, so that *last* instance
    // of character equivalence class is the one registered.
    // Notice: Doesn't include the last character.
    const tableSize = this.#alphabetSize();
    if (start === 0) {
      // All patterns less than kBMMaxShift in length.
      badCharOccurrence.fill(-1, 0, tableSize);
    } else {
      for (let i = 0; i < tableSize; i++) {
        badCharOccurrence[i] = start - 1;
      }
    }
    for (let i = start; i < patternLength - 1; i++) {
      const c = pattern.at(i);
      const bucket = !this.#isTwoByte ? c : c % this.#alphabetSize();
      badCharOccurrence[bucket] = i;
    }
  }

  //---------------------------------------------------------------------
  // Linear string search with bailout to BMH.
  //---------------------------------------------------------------------

  // Simple linear search for short patterns, which bails out if the string
  // isn't found very early in the subject. Upgrades to BoyerMooreHorspool.
  #initialSearch(subject: Vector, index: number): number {
    const pattern = this.#pattern;
    const patternLength = pattern.length();
    // Badness is a count of how much work we have done.  When we have
    // done enough work we decide it's probably worth switching to a better
    // algorithm.
    let badness = -10 - patternLength * 4;

    // We know our pattern is at least 2 characters, we cache the first so
    // the common case of the first character not matching is faster.
    for (let i = index, n = subject.length() - patternLength; i <= n; i++) {
      badness++;
      if (badness <= 0) {
        i = findFirstCharacter(pattern, subject, i);
        if (i === subject.length()) return subject.length();
        let j = 1;
        do {
          if (pattern.at(j) !== subject.at(i + j)) {
            break;
          }
          j++;
        } while (j < patternLength);
        if (j === patternLength) {
          return i;
        }
        badness += j;
      } else {
        this.#populateBoyerMooreHorspoolTable();
        this.#strategy = kBoyerMooreHorspool;
        return this.#boyerMooreHorspoolSearch(subject, i);
      }
    }
    return subject.length();
  }
}

// Returns the position of `needle` in `haystack`, or `haystackLength` if it
// is not found. `haystack` and `needle` must be the same kind of array.
export function searchString(
  haystack: CharArray,
  haystackLength: number,
  needle: CharArray,
  needleLength: number,
  startIndex: number,
  isForward: boolean
): number {
  if (haystackLength < needleLength) return haystackLength;
  // To do a reverse search (lastIndexOf instead of indexOf) without redundant
  // code, create two vectors that are reversed views into the input strings.
  // For example, v_needle[0] would return the *last* character of the needle.
  // So we're searching for the first instance of rev(needle) in rev(haystack)
  const vNeedle = new Vector(needle, needleLength, isForward);
  const vHaystack = new Vector(haystack, haystackLength, isForward);
  const diff = haystackLength - needleLength;
  let relativeStartIndex: number;
  if (isForward) {
    relativeStartIndex = startIndex;
  } else if (diff < startIndex) {
    relativeStartIndex = 0;
  } else {
    relativeStartIndex = diff - startIndex;
  }
  const search = new StringSearch(vNeedle, haystack instanceof Uint16Array);
  const pos = search.search(vHaystack, relativeStartIndex);
  if (pos === haystackLength) {
    // not found
    return pos;
  }
  return isForward ? pos : haystackLength - needleLength - pos;
}
