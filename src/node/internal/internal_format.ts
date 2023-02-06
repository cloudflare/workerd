// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
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

export function format(...args: [string, unknown[]?]) {
  return formatInternal(...args);
}

function formatBigInt(bigint: BigInt, numericSeparator?: boolean) {
  const string = String(bigint);
  if (!numericSeparator) {
    return `${string}n`;
  }
  return `${addNumericSeparator(string)}n`;
}

function addNumericSeparator(integerString: string) {
  let result = '';
  let i = integerString.length;
  const start = integerString.startsWith('-') ? 1 : 0;
  for (; i >= start + 4; i -= 3) {
    result = `_${integerString.slice(i - 3, i)}${result}`;
  }
  return i === integerString.length ?
    integerString :
    `${integerString.slice(0, i)}${result}`;
}

function addNumericSeparatorEnd(integerString: string) {
  let result = '';
  let i = 0;
  for (; i < integerString.length - 3; i += 3) {
    result += `${integerString.slice(i, i + 3)}_`;
  }
  return i === 0 ?
    integerString :
    `${result}${integerString.slice(i)}`;
}

function formatNumber(number: number, numericSeparator?: boolean) {
  if (!numericSeparator) {
    // Format -0 as '-0'. Checking `number === -0` won't distinguish 0 from -0.
    if (Object.is(number, -0)) {
      return '-0';
    }
    return `${number}`;
  }
  const integer = Math.trunc(number);
  const string = String(integer);
  if (integer === number) {
    if (!Number.isFinite(number) || string.includes('e')) {
      return string;
    }
    return `${addNumericSeparator(string)}`;
  }
  if (Number.isNaN(number)) {
    return string;
  }
  return `${addNumericSeparator(string)}.${addNumericSeparatorEnd(String(number).slice(string.length + 1))}`;
}

function formatStringOrSymbol(val: unknown) {
  if (typeof val === 'symbol') {
    return (val as symbol).description;
  }
  return `${val}`;
}

const firstErrorLine = (error: any) => error?.message?.split('\n', 1)[0];
let CIRCULAR_ERROR_MESSAGE: string;
function tryStringify(arg: unknown) {
  try {
    return JSON.stringify(arg);
  } catch (err) {
    // Populate the circular error message lazily
    if (!CIRCULAR_ERROR_MESSAGE) {
      try {
        const a = {};
        (a as any).a = a;
        JSON.stringify(a);
      } catch (circularError) {
        CIRCULAR_ERROR_MESSAGE = firstErrorLine(circularError);
      }
    }
    if ((err as Error)?.name === 'TypeError' &&
        firstErrorLine(err) === CIRCULAR_ERROR_MESSAGE) {
      return '[Circular]';
    }
    throw err;
  }
}

function formatInternal(...args: [string, unknown[]?]) {
  const first = args[0];
  let a = 0;
  let str = '';
  let join = '';

  if (typeof first === 'string') {
    if (args.length === 1) {
      return first;
    }
    let tempStr;
    let lastPos = 0;

    for (let i = 0; i < first.length - 1; i++) {
      if (first.charCodeAt(i) == 37) { // %
        const nextChar = first.charCodeAt(++i);
        if (a + 1 !== args.length) {
          switch (nextChar) {
            case 115: { // 's'
              const tempArg = args[++a];
              if (typeof tempArg === 'number') {
                tempStr = formatNumber(tempArg);
              } else if (typeof tempArg === 'bigint') {
                tempStr = formatBigInt(tempArg);
              } else {
                tempStr = formatStringOrSymbol(tempArg);
              }
              break;
            }
            case 106: // 'j'
              tempStr = tryStringify(args[++a]);
              break;
            case 100: { // 'd'
              const tempNum = args[++a];
              if (typeof tempNum === 'bigint') {
                tempStr = formatBigInt(tempNum);
              } else if (typeof tempNum === 'symbol') {
                tempStr = 'NaN';
              } else {
                tempStr = formatNumber(Number(tempNum));
              }
              break;
            }
            case 79: // 'O'
              tempStr = `${args[++a]}`;
              break;
            case 111: // 'o'
              tempStr = formatStringOrSymbol(args[++a]);
              break;
            case 105: { // 'i'
              const tempInteger = args[++a];
              if (typeof tempInteger === 'bigint') {
                tempStr = formatBigInt(tempInteger);
              } else if (typeof tempInteger === 'symbol') {
                tempStr = 'NaN';
              } else {
                tempStr = formatNumber(Number.parseInt(`${tempInteger}`));
              }
              break;
            }
            case 102: { // 'f'
              const tempFloat = args[++a];
              if (typeof tempFloat === 'symbol') {
                tempStr = 'NaN';
              } else {
                tempStr = formatNumber(Number.parseFloat(`${tempFloat}`));
              }
              break;
            }
            case 99: // 'c'
              a += 1;
              tempStr = '';
              break;
            case 37: // '%'
              str += first.slice(lastPos, i);
              lastPos = i + 1;
              continue;
            default: // Any other character is not a correct placeholder
              continue;
          }
          if (lastPos !== i - 1) {
            str += first.slice(lastPos, i - 1);
          }
          str += tempStr;
          lastPos = i + 1;
        } else if (nextChar === 37) {
          str += first.slice(lastPos, i);
          lastPos = i + 1;
        }
      }
    }
    if (lastPos !== 0) {
      a++;
      join = ' ';
      if (lastPos < first.length) {
        str += first.slice(lastPos);
      }
    }
  }

  while (a < args.length) {
    const value = args[a];
    str += `${join}${formatStringOrSymbol(value)}`
    join = ' ';
    a++;
  }
  return str;
}

