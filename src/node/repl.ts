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

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { builtinModules as _builtinModules } from 'node-internal:internal_module';
import { Interface } from 'node-internal:internal_readline_promises';
import type { InspectOptions } from 'node:util';
import type { Context } from 'node:vm';
import type ReplType from 'node:repl';
import type { Completer, AsyncCompleter } from 'readline';

export function writer(): ReplType.REPLWriter & { options: InspectOptions } {
  throw new ERR_METHOD_NOT_IMPLEMENTED('writer');
}

export function start(): ReplType.REPLServer {
  throw new ERR_METHOD_NOT_IMPLEMENTED('start');
}

export class Recoverable extends SyntaxError implements ReplType.Recoverable {
  err: Error;
  constructor(err: Error) {
    super();
    this.err = err;
  }
}

export class REPLServer extends Interface implements ReplType.REPLServer {
  context: Context;
  inputStream: NodeJS.ReadableStream;
  input: NodeJS.ReadableStream;
  outputStream: NodeJS.WritableStream;
  output: NodeJS.WritableStream;
  commands: NodeJS.ReadOnlyDict<ReplType.REPLCommand>;
  editorMode: boolean;
  underscoreAssigned: boolean;
  underscoreErrAssigned: boolean;
  last: unknown;
  lastError: unknown;
  eval: ReplType.REPLEval;
  useColors: boolean;
  useGlobal: boolean;
  ignoreUndefined: boolean;
  writer: ReplType.REPLWriter;
  completer: Completer | AsyncCompleter;
  replMode: typeof ReplType.REPL_MODE_SLOPPY | typeof ReplType.REPL_MODE_STRICT;

  constructor() {
    super();
    throw new ERR_METHOD_NOT_IMPLEMENTED('REPLServer');
  }

  setupHistory(_historyConfig?: unknown, _callback?: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('setupHistory');
  }

  defineCommand(
    _keyword: string,
    _cmd: ReplType.REPLCommandAction | ReplType.REPLCommand
  ): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('defineCommand');
  }

  displayPrompt(_preserveCursor?: boolean): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('displayPrompt');
  }

  clearBufferedCommand(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('clearBufferedCommand');
  }
}

export const REPL_MODE_SLOPPY: unique symbol = Symbol('repl-sloppy');

export const REPL_MODE_STRICT: unique symbol = Symbol('repl-strict');

export const builtinModules: string[] = _builtinModules.filter(
  (m) => m[0] !== '_'
);

export const _builtinLibs = builtinModules;

export default {
  writer,
  start,
  Recoverable,
  REPLServer,
  builtinModules,
  _builtinLibs,
  REPL_MODE_SLOPPY,
  REPL_MODE_STRICT,
};
