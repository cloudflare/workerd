// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright © web-platform-tests contributors. BSD license
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

import crypto from 'node:crypto'
import {
  getHostInfo,
  type HostInfo,
  type PromiseTestFn,
  type TestFn,
  type UnknownFunc,
} from './common'

declare global {
  var GLOBAL: { isWindow(): boolean; isWorker(): boolean }

  function done(): undefined
  function subsetTestByKey<T>(
    _key: string,
    testType: (
      testCallback: TestFn | PromiseTestFn | string,
      testMessage?: string,
    ) => T,
    testCallback: TestFn | PromiseTestFn | string,
    testMessage?: string,
  ): T
  function subsetTest(
    testType: TestRunnerFn,
    testCallback: TestFn | PromiseTestFn,
    testMessage: string,
  ): void
  // Used by idlharness.js to determine if a subtest should be run
  function shouldRunSubTest(name?: string): boolean

  function step_timeout(
    func: UnknownFunc,
    timeout: number,
    ...rest: unknown[]
  ): ReturnType<typeof setTimeout>

  function get_host_info(): HostInfo
  function token(): string
  function setup(func: UnknownFunc | Record<string, unknown>): void
  function add_completion_callback(func: UnknownFunc): void
  // Provided by /common/gc.js
  function garbageCollect(): Promise<void>
  function format_value(val: unknown): string
  function createBuffer(
    type: 'ArrayBuffer' | 'SharedArrayBuffer',
    length: number,
    opts: { maxByteLength?: number } | undefined,
  ): ArrayBuffer | SharedArrayBuffer
  // Used by idlharness.js to fetch IDL files
  function fetch_spec(spec: string): Promise<{ spec: string; idl: string }>
}

type TestRunnerFn = (callback: TestFn | PromiseTestFn, message: string) => void

globalThis.get_host_info = (): HostInfo => {
  return getHostInfo()
}

globalThis.GLOBAL = {
  isWindow(): boolean {
    return false
  },
  isWorker(): boolean {
    return false
  },
}

globalThis.done = (): undefined => undefined

globalThis.subsetTestByKey = <T>(
  _key: string,
  testType: (testCallback: TestFn | PromiseTestFn, testMessage: string) => T,
  testCallback: TestFn | PromiseTestFn | string,
  testMessage?: string,
): T => {
  // This function is designed to allow selecting only certain tests when
  // running in a browser, by changing the query string. We'll always run
  // all the tests.
  return testType(testCallback as TestFn | PromiseTestFn, testMessage as string)
}

// Used by idlharness.js to determine if a subtest should be run.
// We always run all subtests.
globalThis.shouldRunSubTest = (): boolean => true

globalThis.subsetTest = (testType, testCallback, testMessage): void => {
  // This function is designed to allow selecting only certain tests when
  // running in a browser, by changing the query string. We'll always run
  // all the tests.

  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression -- We are emulating WPT's existing interface which always passes through the returned value
  return testType(testCallback, testMessage)
}

/**
 * Global version of :js:func:`Test.step_timeout` for use in single page tests.
 *
 * @param func - Function to run after the timeout
 * @param timeout - Time in ms to wait before running the
 * test step. The actual wait time is ``timeout`` x
 * ``timeout_multiplier``.
 */
globalThis.step_timeout = (
  func: UnknownFunc,
  timeout: number,
  ...rest: unknown[]
): ReturnType<typeof setTimeout> => {
  return setTimeout(() => func(...rest), timeout)
}

globalThis.token = (): string => {
  return crypto.randomUUID()
}

globalThis.setup = (func): void => {
  if (typeof func === 'object') {
    return
  }

  func()
}

globalThis.add_completion_callback = (func: UnknownFunc): void => {
  globalThis.state.completionCallbacks.push(func)
}

globalThis.format_value = (val): string => {
  return JSON.stringify(val, null, 2)
}

globalThis.createBuffer = (
  type,
  length,
  _opts,
): ArrayBuffer | SharedArrayBuffer => {
  switch (type) {
    case 'ArrayBuffer':
      return new ArrayBuffer(length)
    case 'SharedArrayBuffer':
      return new SharedArrayBuffer(length)
    default:
      throw new TypeError(`Unsupported buffer type: ${type}`)
  }
}

/**
 * fetch_spec is used by idlharness.js to fetch IDL files.
 * This implementation reads from the embedded bindings instead of using fetch().
 */
globalThis.fetch_spec = (
  spec: string,
): Promise<{ spec: string; idl: string }> => {
  const path = `/interfaces/${spec}.idl`
  const idl = globalThis.state.env[path]
  if (typeof idl !== 'string') {
    return Promise.reject(
      new Error(`Error fetching ${path}: IDL file not found in bindings`),
    )
  }
  return Promise.resolve({ spec, idl })
}
