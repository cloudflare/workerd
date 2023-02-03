// Copyright (c) 2017-2022 Cloudflare, Inc.
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

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { compare } from 'node-internal:internal_buffer';

import {
  isAnyArrayBuffer,
  isArrayBufferView,
  isDate,
  isMap,
  isRegExp,
  isSet,
  isNativeError,
  isBoxedPrimitive,
  isNumberObject,
  isStringObject,
  isBooleanObject,
  isBigIntObject,
  isSymbolObject,
  isFloat32Array,
  isFloat64Array,
} from 'node-internal:internal_types';

import {
  ONLY_ENUMERABLE,
  SKIP_SYMBOLS,
  getOwnNonIndexProperties
} from 'node-internal:internal_utils';

const kStrict = true;

const kNoIterator = 0;
const kIsArray = 1;
const kIsSet = 2;
const kIsMap = 3;

function areSimilarRegExps(a: RegExp, b: RegExp) {
  return a.source === b.source &&
         a.flags === b.flags &&
         a.lastIndex === b.lastIndex;
}

type FloatArray = Float32Array | Float64Array;
type AnyArrayBuffer = ArrayBuffer | SharedArrayBuffer;

type Memos = {
  val1: Map<unknown, unknown>;
  val2: Map<unknown, unknown>;
  position: number;
};

function areSimilarFloatArrays(a: FloatArray, b: FloatArray) {
  if (a.byteLength !== b.byteLength) {
    return false;
  }
  for (let offset = 0; offset < a.byteLength; offset++) {
    if (a[offset] !== b[offset]) {
      return false;
    }
  }
  return true;
}

function areSimilarTypedArrays(a: ArrayBufferView, b: ArrayBufferView) {
  if (a.byteLength !== b.byteLength) {
    return false;
  }
  return compare(new Uint8Array(a.buffer, a.byteOffset, a.byteLength),
                 new Uint8Array(b.buffer, b.byteOffset, b.byteLength)) === 0;
}

function areEqualArrayBuffers(buf1: AnyArrayBuffer, buf2: AnyArrayBuffer) {
  return buf1.byteLength === buf2.byteLength &&
    compare(new Uint8Array(buf1), new Uint8Array(buf2)) === 0;
}

function areEqualBoxedPrimitives(val1: unknown, val2: unknown) {
  if (isNumberObject(val1)) {
    return isNumberObject(val2) &&
           Object.is(Number.prototype.valueOf.call(val1),
                     Number.prototype.valueOf.call(val2));
  }
  if (isStringObject(val1)) {
    return isStringObject(val2) &&
           String.prototype.valueOf.call(val1) === String.prototype.valueOf.call(val2);
  }
  if (isBooleanObject(val1)) {
    return isBooleanObject(val2) &&
           Boolean.prototype.valueOf.call(val1) === Boolean.prototype.valueOf.call(val2);
  }
  if (isBigIntObject(val1)) {
    return isBigIntObject(val2) &&
           BigInt.prototype.valueOf.call(val1) === BigInt.prototype.valueOf.call(val2);
  }
  if (isSymbolObject(val1)) {
    return isSymbolObject(val2) &&
          Symbol.prototype.valueOf.call(val1) === Symbol.prototype.valueOf.call(val2);
  }

  // Should be unreachable, here just as a backup.
  throw new Error(`Unknown boxed type ${val1}`);
}

function innerDeepEqual(val1: unknown, val2: unknown, strict: boolean, memos?: Memos) {
  // All identical values are equivalent, as determined by ===.
  if (val1 === val2) {
    if (val1 !== 0)
      return true;
    return strict ? Object.is(val1, val2) : true;
  }

  // Check more closely if val1 and val2 are equal.
  if (strict) {
    if (typeof val1 !== 'object') {
      return typeof val1 === 'number' && Number.isNaN(val1) &&
        Number.isNaN(val2);
    }
    if (typeof val2 !== 'object' || val1 === null || val2 === null) {
      return false;
    }
    if (Object.getPrototypeOf(val1) !== Object.getPrototypeOf(val2)) {
      return false;
    }
  } else {
    if (val1 === null || typeof val1 !== 'object') {
      if (val2 === null || typeof val2 !== 'object') {
        // todo: eslint-disable-next-line eqeqeq
        return val1 == val2 || (Number.isNaN(val1) && Number.isNaN(val2));
      }
      return false;
    }
    if (val2 === null || typeof val2 !== 'object') {
      return false;
    }
  }
  const val1Tag = Object.prototype.toString.call(val1);
  const val2Tag = Object.prototype.toString.call(val2);

  if (val1Tag !== val2Tag) {
    return false;
  }

  if (Array.isArray(val1)) {
    // Check for sparse arrays and general fast path
    if (!Array.isArray(val2) || val1.length !== val2.length) {
      return false;
    }
    const filter = strict ? ONLY_ENUMERABLE : ONLY_ENUMERABLE | SKIP_SYMBOLS;
    const keys1 = getOwnNonIndexProperties(val1, filter);
    const keys2 = getOwnNonIndexProperties(val2, filter);
    if (keys1.length !== keys2.length) {
      return false;
    }
    return keyCheck(val1, val2, strict, memos, kIsArray, keys1);
  } else if (val1Tag === '[object Object]') {
    return keyCheck(val1, val2, strict, memos, kNoIterator);
  } else if (isDate(val1)) {
    if (!isDate(val2) ||
        Date.prototype.getTime.call(val1) !== Date.prototype.getTime.call(val2)) {
      return false;
    }
  } else if (isRegExp(val1)) {
    if (!isRegExp(val2) || !areSimilarRegExps(val1 as RegExp, val2 as RegExp)) {
      return false;
    }
  } else if (isNativeError(val1) || val1 instanceof Error) {
    // Do not compare the stack as it might differ even though the error itself
    // is otherwise identical.
    if ((!isNativeError(val2) && !(val2 instanceof Error)) ||
        (val1 as Error).message !== (val2 as Error).message ||
        (val1 as Error).name !== (val2 as Error).name) {
      return false;
    }
  } else if (isArrayBufferView(val1)) {
    if (!isArrayBufferView(val2)) return false;
    if ((val1 as any)[Symbol.toStringTag] !== (val2 as any)[Symbol.toStringTag]) {
      return false;
    }
    if (!strict && (isFloat32Array(val1) || isFloat64Array(val1))) {
      if (!areSimilarFloatArrays(val1 as FloatArray, val2 as FloatArray)) {
        return false;
      }
    } else if (!areSimilarTypedArrays(val1 as ArrayBufferView, val2 as ArrayBufferView)) {
      return false;
    }
    // Buffer.compare returns true, so val1.length === val2.length. If they both
    // only contain numeric keys, we don't need to exam further than checking
    // the symbols.
    const filter = strict ? ONLY_ENUMERABLE : ONLY_ENUMERABLE | SKIP_SYMBOLS;
    const keys1 = getOwnNonIndexProperties(val1, filter);
    const keys2 = getOwnNonIndexProperties(val2, filter);
    if (keys1.length !== keys2.length) {
      return false;
    }
    return keyCheck(val1, val2, strict, memos, kNoIterator, keys1);
  } else if (isSet(val1)) {
    if (!isSet(val2) || (val1 as Set<unknown>).size !== (val2 as Set<unknown>).size) {
      return false;
    }
    return keyCheck(val1, val2, strict, memos, kIsSet);
  } else if (isMap(val1)) {
    if (!isMap(val2) || (val1 as Map<unknown,unknown>).size !==
                        (val2 as Map<unknown,unknown>).size) {
      return false;
    }
    return keyCheck(val1, val2, strict, memos, kIsMap);
  } else if (isAnyArrayBuffer(val1)) {
    if (!isAnyArrayBuffer(val2) ||
        !areEqualArrayBuffers(val1 as ArrayBuffer, val2 as ArrayBuffer)) {
      return false;
    }
  } else if (isBoxedPrimitive(val1)) {
    if (!areEqualBoxedPrimitives(val1, val2)) {
      return false;
    }
  } else if (Array.isArray(val2) ||
             isArrayBufferView(val2) ||
             isSet(val2) ||
             isMap(val2) ||
             isDate(val2) ||
             isRegExp(val2) ||
             isAnyArrayBuffer(val2) ||
             isBoxedPrimitive(val2) ||
             isNativeError(val2) ||
             val2 instanceof Error) {
    return false;
  }
  return keyCheck(val1, val2, strict, memos, kNoIterator);
}

function getEnumerables(val: Object, keys : (string | symbol)[]) {
  return keys.filter((k) => val.propertyIsEnumerable(k));
}

function keyCheck(val1: Object,
                  val2: Object,
                  strict: boolean,
                  memos?: Memos,
                  iterationType?: number,
                  aKeys? : (string | symbol)[]) {
  // For all remaining Object pairs, including Array, objects and Maps,
  // equivalence is determined by having:
  // a) The same number of owned enumerable properties
  // b) The same set of keys/indexes (although not necessarily the same order)
  // c) Equivalent values for every corresponding key/index
  // d) For Sets and Maps, equal contents
  // Note: this accounts for both named and indexed properties on Arrays.
  if (arguments.length === 5) {
    aKeys = Object.keys(val1 as Object);
    const bKeys = Object.keys(val2 as Object);

    // The pair must have the same number of owned properties.
    if (aKeys.length !== bKeys.length) {
      return false;
    }
  }

  // Cheap key test
  let i = 0;
  for (; i < aKeys!.length; i++) {
    if (!val2.propertyIsEnumerable(aKeys![i]!)) {
      return false;
    }
  }

  if (strict && arguments.length === 5) {
    const symbolKeysA = Object.getOwnPropertySymbols(val1);
    if (symbolKeysA.length !== 0) {
      let count = 0;
      for (i = 0; i < symbolKeysA.length; i++) {
        const key = symbolKeysA[i];
        if (val1.propertyIsEnumerable(key!)) {
          if (!val2.propertyIsEnumerable(key!)) {
            return false;
          }
          aKeys!.push(key!);
          count++;
        } else if (val2.propertyIsEnumerable(key!)) {
          return false;
        }
      }
      const symbolKeysB = Object.getOwnPropertySymbols(val2);
      if (symbolKeysA.length !== symbolKeysB.length &&
          getEnumerables(val2, symbolKeysB).length !== count) {
        return false;
      }
    } else {
      const symbolKeysB = Object.getOwnPropertySymbols(val2);
      if (symbolKeysB.length !== 0 &&
          getEnumerables(val2, symbolKeysB).length !== 0) {
        return false;
      }
    }
  }

  if (aKeys!.length === 0 &&
      (iterationType === kNoIterator ||
        (iterationType === kIsArray && (val1 as any[]).length === 0) ||
        (val1 as any).size === 0)) {
    return true;
  }

  // Use memos to handle cycles.
  if (memos === undefined) {
    memos = {
      val1: new Map(),
      val2: new Map(),
      position: 0
    };
  } else {
    // We prevent up to two map.has(x) calls by directly retrieving the value
    // and checking for undefined. The map can only contain numbers, so it is
    // safe to check for undefined only.
    const val2MemoA = memos.val1.get(val1);
    if (val2MemoA !== undefined) {
      const val2MemoB = memos.val2.get(val2);
      if (val2MemoB !== undefined) {
        return val2MemoA === val2MemoB;
      }
    }
    memos.position++;
  }

  memos.val1.set(val1, memos.position);
  memos.val2.set(val2, memos.position);

  const areEq = objEquiv(val1, val2, strict, aKeys!, memos, iterationType);

  memos.val1.delete(val1);
  memos.val2.delete(val2);

  return areEq;
}

function setHasEqualElement(set : Set<unknown>, val1: unknown, strict: boolean, memo: Memos) {
  // Go looking.
  for (const val2 of set) {
    if (innerDeepEqual(val1, val2, strict, memo)) {
      // Remove the matching element to make sure we do not check that again.
      set.delete(val2);
      return true;
    }
  }

  return false;
}

function findLooseMatchingPrimitives(prim : unknown) {
  switch (typeof prim) {
    case 'undefined':
      return null;
    case 'object': // Only pass in null as object!
      return undefined;
    case 'symbol':
      return false;
    case 'string':
      return !Number.isNaN(+prim);
      // Loose equal entries exist only if the string is possible to convert to
      // a regular number and not NaN.
    case 'number':
      return !Number.isNaN(prim);
  }
  return true;
}

function setMightHaveLoosePrim(a: Set<unknown>, b: Set<unknown>, prim: unknown) {
  const altValue = findLooseMatchingPrimitives(prim);
  if (altValue != null)
    return altValue;

  return b.has(altValue) && !a.has(altValue);
}

function mapMightHaveLoosePrim(a: Map<unknown, unknown>, b: Map<unknown, unknown>, prim: unknown,
                               item: unknown, memo: Memos) {
  const altValue = findLooseMatchingPrimitives(prim);
  if (altValue != null) {
    return altValue;
  }
  const curB = b.get(altValue);
  if ((curB === undefined && !b.has(altValue)) ||
      !innerDeepEqual(item, curB, false, memo)) {
    return false;
  }
  return !a.has(altValue) && innerDeepEqual(item, curB, false, memo);
}

function setEquiv(a: Set<unknown>, b: Set<unknown>, strict: boolean, memo: Memos) {
  // This is a lazily initiated Set of entries which have to be compared
  // pairwise.
  let set = null;
  for (const val of a) {
    // Note: Checking for the objects first improves the performance for object
    // heavy sets but it is a minor slow down for primitives. As they are fast
    // to check this improves the worst case scenario instead.
    if (typeof val === 'object' && val !== null) {
      if (set === null) {
        set = new Set();
      }
      // If the specified value doesn't exist in the second set it's a non-null
      // object (or non strict only: a not matching primitive) we'll need to go
      // hunting for something that's deep-(strict-)equal to it. To make this
      // O(n log n) complexity we have to copy these values in a new set first.
      set.add(val);
    } else if (!b.has(val)) {
      if (strict)
        return false;

      // Fast path to detect missing string, symbol, undefined and null values.
      if (!setMightHaveLoosePrim(a, b, val)) {
        return false;
      }

      if (set === null) {
        set = new Set();
      }
      set.add(val);
    }
  }

  if (set !== null) {
    for (const val of b) {
      // We have to check if a primitive value is already
      // matching and only if it's not, go hunting for it.
      if (typeof val === 'object' && val !== null) {
        if (!setHasEqualElement(set, val, strict, memo))
          return false;
      } else if (!strict &&
                 !a.has(val) &&
                 !setHasEqualElement(set, val, strict, memo)) {
        return false;
      }
    }
    return set.size === 0;
  }

  return true;
}

function mapHasEqualEntry(set: Set<unknown>,
                          map: Map<unknown, unknown>,
                          key1: unknown,
                          item1: unknown,
                          strict: boolean,
                          memo: Memos) {
  // To be able to handle cases like:
  //   Map([[{}, 'a'], [{}, 'b']]) vs Map([[{}, 'b'], [{}, 'a']])
  // ... we need to consider *all* matching keys, not just the first we find.
  for (const key2 of set) {
    if (innerDeepEqual(key1, key2, strict, memo) &&
      innerDeepEqual(item1, map.get(key2), strict, memo)) {
      set.delete(key2);
      return true;
    }
  }

  return false;
}

function mapEquiv(a: Map<unknown,unknown>, b: Map<unknown,unknown>,
                  strict: boolean, memo: Memos) {
  let set = null;

  for (const { 0: key, 1: item1 } of a) {
    if (typeof key === 'object' && key !== null) {
      if (set === null) {
        set = new Set();
      }
      set.add(key);
    } else {
      // By directly retrieving the value we prevent another b.has(key) check in
      // almost all possible cases.
      const item2 = b.get(key);
      if (((item2 === undefined && !b.has(key)) ||
          !innerDeepEqual(item1, item2, strict, memo))) {
        if (strict)
          return false;
        // Fast path to detect missing string, symbol, undefined and null
        // keys.
        if (!mapMightHaveLoosePrim(a, b, key, item1, memo))
          return false;
        if (set === null) {
          set = new Set();
        }
        set.add(key);
      }
    }
  }

  if (set !== null) {
    for (const { 0: key, 1: item } of b) {
      if (typeof key === 'object' && key !== null) {
        if (!mapHasEqualEntry(set, a, key, item, strict, memo))
          return false;
      } else if (!strict &&
                 (!a.has(key) ||
                   !innerDeepEqual(a.get(key), item, false, memo)) &&
                 !mapHasEqualEntry(set, a, key, item, false, memo)) {
        return false;
      }
    }
    return set.size === 0;
  }

  return true;
}

function objEquiv(a: Object, b: Object, strict: boolean, keys: (string|symbol)[],
                  memos: Memos, iterationType?: number) {
  // Sets and maps don't have their entries accessible via normal object
  // properties.
  let i = 0;

  if (iterationType === kIsSet) {
    if (!setEquiv(a as Set<unknown>, b as Set<unknown>, strict, memos)) {
      return false;
    }
  } else if (iterationType === kIsMap) {
    if (!mapEquiv(a as Map<unknown,unknown>, b as Map<unknown,unknown>, strict, memos)) {
      return false;
    }
  } else if (iterationType === kIsArray) {
    for (; i < (a as [any]).length; i++) {
      if (a.hasOwnProperty(i)) {
        if (!b.hasOwnProperty(i) ||
            !innerDeepEqual((a as [any])[i], (b as [any])[i], strict, memos)) {
          return false;
        }
      } else if (b.hasOwnProperty(i)) {
        return false;
      } else {
        // Array is sparse.
        const keysA = Object.keys(a);
        for (; i < keysA.length; i++) {
          const key = keysA[i];
          if (!b.hasOwnProperty(key!) ||
              !innerDeepEqual((a as any)[key!], (b as any)[key!], strict, memos)) {
            return false;
          }
        }
        if (keysA.length !== Object.keys(b).length) {
          return false;
        }
        return true;
      }
    }
  }

  // The pair must have equivalent values for every corresponding key.
  // Possibly expensive deep test:
  for (i = 0; i < keys.length; i++) {
    const key = keys[i];
    if (!innerDeepEqual((a as any)[key!], (b as any)[key!], strict, memos)) {
      return false;
    }
  }
  return true;
}

export function isDeepStrictEqual(val1: unknown, val2: unknown) {
  return innerDeepEqual(val1, val2, kStrict);
}
