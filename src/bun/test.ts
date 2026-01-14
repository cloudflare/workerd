// Copyright (c) 2024 Jeju Network
// Bun Test Compatibility Layer for Workerd
// Licensed under the Apache 2.0 license

/**
 * bun:test Module
 *
 * Test runner APIs for Bun compatibility.
 * In workerd, these are stubs that throw - tests should be run in actual Bun.
 */

export type TestFunction = () => void | Promise<void>
export type DescribeFunction = () => void

export interface TestOptions {
  timeout?: number
  retry?: number
  skip?: boolean
  todo?: boolean
  only?: boolean
}

export interface Expect<T> {
  toBe(expected: T): void
  toEqual(expected: T): void
  toStrictEqual(expected: T): void
  toBeTruthy(): void
  toBeFalsy(): void
  toBeNull(): void
  toBeUndefined(): void
  toBeDefined(): void
  toBeNaN(): void
  toBeGreaterThan(expected: number): void
  toBeGreaterThanOrEqual(expected: number): void
  toBeLessThan(expected: number): void
  toBeLessThanOrEqual(expected: number): void
  toBeCloseTo(expected: number, precision?: number): void
  toContain(expected: unknown): void
  toHaveLength(expected: number): void
  toHaveProperty(key: string, value?: unknown): void
  toThrow(expected?: string | RegExp | Error): void
  toMatch(expected: string | RegExp): void
  toMatchObject(expected: object): void
  toMatchSnapshot(name?: string): void
  toMatchInlineSnapshot(snapshot?: string): void
  not: Expect<T>
  resolves: Expect<T>
  rejects: Expect<T>
}

// =============================================================================
// Test Runner Stubs
// =============================================================================

function notAvailable(name: string): never {
  throw new Error(
    `bun:test ${name}() is not available in workerd. Run tests with 'bun test' instead.`,
  )
}

export function describe(_name: string, _fn: DescribeFunction): void {
  notAvailable('describe')
}

export function it(
  _name: string,
  _fn: TestFunction,
  _options?: TestOptions,
): void {
  notAvailable('it')
}

export function test(
  _name: string,
  _fn: TestFunction,
  _options?: TestOptions,
): void {
  notAvailable('test')
}

export function expect<T>(_value: T): Expect<T> {
  notAvailable('expect')
}

export function beforeAll(_fn: TestFunction): void {
  notAvailable('beforeAll')
}

export function afterAll(_fn: TestFunction): void {
  notAvailable('afterAll')
}

export function beforeEach(_fn: TestFunction): void {
  notAvailable('beforeEach')
}

export function afterEach(_fn: TestFunction): void {
  notAvailable('afterEach')
}

export function mock<T extends (...args: unknown[]) => unknown>(_fn?: T): T {
  notAvailable('mock')
}

export function spyOn<T extends object, K extends keyof T>(
  _object: T,
  _method: K,
): T[K] {
  notAvailable('spyOn')
}

export function setSystemTime(_time: Date | number): void {
  notAvailable('setSystemTime')
}

// =============================================================================
// Default Export
// =============================================================================

export default {
  describe,
  it,
  test,
  expect,
  beforeAll,
  afterAll,
  beforeEach,
  afterEach,
  mock,
  spyOn,
  setSystemTime,
}
