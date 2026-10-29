// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import tracing, { type Span as RuntimeSpan } from 'cloudflare-internal:tracing';
import tracingContext from 'cloudflare-internal:tracing-context';
type SpanValue = Parameters<RuntimeSpan['setAttribute']>[1];

interface Context {
  getValue(key: symbol): unknown;
  setValue(key: symbol, value: unknown): Context;
  deleteValue(key: symbol): Context;
}

interface SpanContext {
  traceId: string;
  spanId: string;
  traceFlags: number;
  isRemote?: boolean;
  traceState?: TraceState;
}

interface SpanLike {
  spanContext(): SpanContext;
}

interface SpanOptions {
  root?: boolean;
  attributes?: Record<string, unknown>;
  kind?: number;
  links?: unknown[];
  startTime?: unknown;
}

const SPAN = Symbol.for('OpenTelemetry Context Key SPAN');
const BAGGAGE = Symbol.for('OpenTelemetry Baggage Key');
const SUPPRESS = Symbol.for('OpenTelemetry SDK Context Key SUPPRESS_TRACING');
const INVALID: SpanContext = {
  traceId: '00000000000000000000000000000000',
  spanId: '0000000000000000',
  traceFlags: 0,
};

function timestamp(
  value: unknown
): { seconds: number; nanoseconds: number } | undefined {
  return attempt(() => normalizeTimestamp(value), undefined);
}

function isTimeInput(value: unknown): boolean {
  return (
    typeof value === 'number' ||
    value instanceof Date ||
    (Array.isArray(value) &&
      value.length === 2 &&
      value.every((item) => typeof item === 'number'))
  );
}

function normalizeTimestamp(
  value: unknown
): { seconds: number; nanoseconds: number } | undefined {
  const date = value instanceof Date;
  if (date) value = (value as Date).getTime();
  let seconds: number;
  let nanoseconds: number;
  if (typeof value === 'number' && Number.isFinite(value)) {
    const millis =
      !date && value < performance.timeOrigin / 2
        ? performance.timeOrigin + value
        : value;
    seconds = Math.floor(millis / 1000);
    nanoseconds = Math.round((millis - seconds * 1000) * 1e6);
  } else if (
    Array.isArray(value) &&
    value.length === 2 &&
    value.every((item) => typeof item === 'number' && Number.isFinite(item))
  ) {
    seconds = Math.floor(value[0] as number);
    nanoseconds = Math.round(
      ((value[0] as number) - seconds) * 1e9 + (value[1] as number)
    );
  } else return undefined;
  if (!Number.isSafeInteger(nanoseconds)) return undefined;
  seconds += Math.floor(nanoseconds / 1e9);
  nanoseconds = ((nanoseconds % 1e9) + 1e9) % 1e9;
  if (
    !Number.isSafeInteger(seconds) ||
    seconds < 0 ||
    seconds > 9223372036 ||
    (seconds === 9223372036 && nanoseconds >= 854775808)
  )
    return undefined;
  return { seconds, nanoseconds };
}

function eventAttributes(input: unknown): Record<string, SpanValue> {
  const result = Object.create(null) as Record<string, SpanValue>;
  if (input === null || typeof input !== 'object') return result;
  const scalar = (value: unknown): boolean =>
    typeof value === 'string' ||
    typeof value === 'number' ||
    typeof value === 'boolean';
  for (const [key, value] of Object.entries(input)) {
    if (Object.keys(result).length === 128) break;
    if (!key) continue;
    if (scalar(value)) result[key] = value as SpanValue;
    else if (Array.isArray(value)) {
      const first = value.find((item) => item != null) as unknown;
      if (
        value.every(
          (item) =>
            item == null || (scalar(item) && typeof item === typeof first)
        )
      )
        result[key] = value as SpanValue;
    }
  }
  return result;
}

function attempt<T>(fn: () => T, fallback: T): T {
  try {
    return fn();
  } catch {
    return fallback;
  }
}

class BaseContext implements Context {
  readonly #values: Map<symbol, unknown>;
  constructor(values?: Map<symbol, unknown>) {
    this.#values = new Map(values);
  }
  getValue(key: symbol): unknown {
    return this.#values.get(key);
  }
  setValue(key: symbol, value: unknown): Context {
    const result = new BaseContext(this.#values);
    result.#values.set(key, value);
    return result;
  }
  deleteValue(key: symbol): Context {
    const result = new BaseContext(this.#values);
    result.#values.delete(key);
    return result;
  }
}

const ROOT = new BaseContext();
function contextOrRoot(value: unknown): Context {
  return value !== null && typeof value === 'object'
    ? (value as Context)
    : ROOT;
}
function getSpan(ctx: Context): SpanLike | undefined {
  return attempt(
    () => (ctx.getValue(SPAN) as SpanLike | null | undefined) ?? undefined,
    undefined
  );
}
function valid(ctx: SpanContext | undefined): ctx is SpanContext {
  return attempt(
    () =>
      ctx !== undefined &&
      /^[0-9a-f]{32}$/i.test(ctx.traceId) &&
      ctx.traceId !== INVALID.traceId &&
      /^[0-9a-f]{16}$/i.test(ctx.spanId) &&
      ctx.spanId !== INVALID.spanId &&
      Number.isInteger(ctx.traceFlags) &&
      ctx.traceFlags >= 0 &&
      ctx.traceFlags <= 255,
    false
  );
}

function fromRuntime(
  raw: ReturnType<RuntimeSpan['spanContext']> | undefined
): SpanContext | undefined {
  if (raw === undefined) return undefined;
  const { traceState, ...ids } = raw;
  return traceState === undefined
    ? ids
    : { ...ids, traceState: new TraceState(traceState) };
}

class Span implements SpanLike {
  readonly inner: RuntimeSpan | undefined;
  readonly suppressNative: boolean;
  readonly #context: SpanContext;
  #ended = false;
  #status = 0;
  #eventCount = 0;
  #linkCount = 0;

  constructor(
    inner?: RuntimeSpan,
    parent?: SpanContext,
    identity?: SpanContext,
    suppressNative = false
  ) {
    this.inner = inner;
    this.suppressNative = suppressNative;
    this.#context =
      identity ??
      attempt(() => fromRuntime(inner?.spanContext()) ?? { ...INVALID }, {
        ...INVALID,
      });
    if (parent?.traceState !== undefined)
      this.#context.traceState = parent.traceState;
  }
  spanContext(): SpanContext {
    return this.#context;
  }
  isRecording(): boolean {
    return !this.#ended && attempt(() => this.inner?.isTraced ?? false, false);
  }
  setAttribute(key: string, value: unknown): this {
    if (this.#ended || typeof key !== 'string' || key.length === 0) return this;
    attempt(() => {
      const scalar = (v: unknown): boolean =>
        typeof v === 'string' ||
        typeof v === 'boolean' ||
        typeof v === 'number';
      if (scalar(value)) this.inner?.setAttribute(key, value as SpanValue);
      else if (Array.isArray(value)) {
        const first = value.find(
          (v) => v !== null && v !== undefined
        ) as unknown;
        if (
          value.every(
            (v) => v == null || (scalar(v) && typeof v === typeof first)
          )
        ) {
          this.inner?.setAttribute(key, value as SpanValue);
        }
      }
    }, undefined);
    return this;
  }
  setAttributes(attributes: Record<string, unknown>): this {
    attempt(() => {
      for (const [key, value] of Object.entries(attributes))
        this.setAttribute(key, value);
    }, undefined);
    return this;
  }
  addEvent(name: string, attributesOrTime?: unknown, time?: unknown): this {
    if (
      this.#ended ||
      this.inner === undefined ||
      typeof name !== 'string' ||
      this.#eventCount >= 128
    )
      return this;
    attempt(() => {
      const secondIsTime = isTimeInput(attributesOrTime);
      tracingContext.addEvent(
        this.inner as RuntimeSpan,
        name,
        eventAttributes(secondIsTime ? undefined : attributesOrTime),
        timestamp(secondIsTime && !isTimeInput(time) ? attributesOrTime : time)
      );
      ++this.#eventCount;
    }, undefined);
    return this;
  }
  addLink(link: unknown): this {
    return this.addLinks([link]);
  }
  addLinks(links: unknown[]): this {
    if (this.#ended || this.inner === undefined || !Array.isArray(links))
      return this;
    attempt(() => {
      const output: Parameters<typeof tracingContext.addLinks>[1] = [];
      for (const link of links) {
        if (this.#linkCount + output.length >= 128) break;
        attempt(() => {
          if (link === null || typeof link !== 'object') return;
          const value = link as { context?: SpanContext; attributes?: unknown };
          const identity = value.context;
          if (!valid(identity)) return;
          const traceState = identity.traceState?.serialize();
          output.push({
            context: {
              traceId: identity.traceId.toLowerCase(),
              spanId: identity.spanId.toLowerCase(),
              traceFlags: identity.traceFlags,
            },
            attributes: eventAttributes(value.attributes),
            isRemote: identity.isRemote === true,
            ...(typeof traceState === 'string' ? { traceState } : {}),
          });
        }, undefined);
      }
      if (output.length !== 0)
        tracingContext.addLinks(this.inner as RuntimeSpan, output);
      this.#linkCount += output.length;
    }, undefined);
    return this;
  }
  setStatus(status: { code: number; message?: string }): this {
    attempt(() => {
      if (
        this.#ended ||
        this.#status === 1 ||
        (status.code !== 1 && status.code !== 2)
      )
        return;
      this.#status = status.code;
      this.inner?.setStatus({
        code: status.code === 1 ? 'ok' : 'error',
        ...(typeof status.message === 'string'
          ? { message: status.message }
          : {}),
      });
    }, undefined);
    return this;
  }
  updateName(name: string): this {
    if (!this.#ended && typeof name === 'string')
      attempt(() => {
        this.inner?.updateName(name);
      }, undefined);
    return this;
  }
  end(endTime?: unknown): void {
    if (this.#ended) return;
    this.#ended = true;
    attempt(() => {
      if (this.inner !== undefined)
        tracingContext.end(this.inner, timestamp(endTime));
    }, undefined);
  }
  recordException(exception: unknown, time?: unknown): void {
    if (!this.#ended && this.#eventCount < 128)
      attempt(() => {
        if (this.inner !== undefined)
          tracingContext.recordException(
            this.inner,
            exception,
            timestamp(time)
          );
        ++this.#eventCount;
      }, undefined);
  }
}

const wrappers = new WeakMap<RuntimeSpan, Span>();
function nonRecording(parent: SpanContext, inner?: RuntimeSpan): Span {
  let spanId: string;
  do {
    spanId = Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) =>
      b.toString(16).padStart(2, '0')
    ).join('');
  } while (spanId === INVALID.spanId);
  return new Span(inner, parent, {
    traceId: parent.traceId.toLowerCase(),
    spanId,
    traceFlags: 0,
  });
}

function wrap(span: RuntimeSpan, parent?: SpanContext): Span {
  let result = wrappers.get(span);
  if (result === undefined) {
    // A native no-op nested inside a propagated context must not erase that context.
    result =
      valid(parent) &&
      !valid(attempt(() => fromRuntime(span.spanContext()), undefined))
        ? new Span(span, parent, parent)
        : new Span(span);
    wrappers.set(span, result);
  }
  return result;
}
function native(span: SpanLike | undefined): RuntimeSpan | undefined {
  return span instanceof Span
    ? span.inner
    : span instanceof tracing.Span
      ? span
      : undefined;
}

interface ActiveState {
  context: Context;
  native: RuntimeSpan | undefined;
}
class ContextManager {
  readonly #state = tracingContext.createAsyncContextVariable();
  readonly #contexts = new WeakMap<RuntimeSpan, Context>();
  readonly #nested = new WeakMap<Context, WeakMap<RuntimeSpan, Context>>();
  #enabled = true;

  active(): Context {
    if (!this.#enabled) return ROOT;
    return attempt(() => {
      const state = this.#state.get() as ActiveState | undefined;
      const span = tracing.getActiveSpan();
      if (state !== undefined && state.native === span) return state.context;
      if (span === undefined) return state?.context ?? ROOT;
      // The runtime's identity-less invocation placeholder is not an OTel span.
      if (
        state === undefined &&
        !valid(attempt(() => fromRuntime(span.spanContext()), undefined))
      )
        return ROOT;
      let contexts = this.#contexts;
      if (state !== undefined) {
        let nested = this.#nested.get(state.context);
        if (nested === undefined) {
          nested = new WeakMap();
          this.#nested.set(state.context, nested);
        }
        contexts = nested;
      }
      let ctx = contexts.get(span);
      if (ctx === undefined) {
        ctx = (state?.context ?? ROOT).setValue(
          SPAN,
          wrap(span, getSpan(state?.context ?? ROOT)?.spanContext())
        );
        contexts.set(span, ctx);
      }
      return ctx;
    }, ROOT);
  }
  with<A extends unknown[], R>(
    ctx: Context,
    fn: (...args: A) => R,
    thisArg?: unknown,
    ...args: A
  ): R {
    ctx = contextOrRoot(ctx);
    const invoke = (): R => fn.call(thisArg, ...args);
    if (!this.#enabled) return invoke();
    let span = getSpan(ctx);
    if (span instanceof tracing.Span) {
      span = wrap(span);
      ctx = attempt(() => ctx.setValue(SPAN, span), ctx);
    }
    const inner = attempt(() => {
      const candidate = native(span);
      // IoOwn-backed spans from another invocation cannot be dereferenced here. Their cached
      // serializable identity can still parent a span in this invocation.
      if (candidate !== undefined) candidate.spanContext();
      return candidate;
    }, undefined);
    const spanContext = attempt(() => span?.spanContext(), undefined);
    const traceState = spanContext?.traceState;
    const run = (): R =>
      this.#state.run(
        { context: ctx, native: inner } satisfies ActiveState,
        invoke
      );
    if (attempt(() => ctx.getValue(SUPPRESS), false)) {
      return tracingContext.withActiveSpanContext(undefined, run, true);
    }
    if (
      span instanceof Span &&
      span.suppressNative &&
      tracingContext.getSpanCreationMode() === 'untraced'
    ) {
      return tracingContext.withActiveSpanContext(undefined, run, true);
    }
    if (inner !== undefined) return tracing.withActiveSpan(inner, run);
    return tracingContext.withActiveSpanContext(
      valid(spanContext)
        ? {
            traceId: spanContext.traceId.toLowerCase(),
            spanId: spanContext.spanId.toLowerCase(),
            traceFlags: spanContext.traceFlags,
            ...(traceState === undefined
              ? {}
              : {
                  traceState: attempt(() => traceState.serialize(), ''),
                }),
          }
        : undefined,
      run
    );
  }
  bind<T>(ctx: Context, target: T): T {
    if (typeof target !== 'function') return target;
    const withContext = this.with.bind(this);
    const bound = function (this: unknown, ...args: unknown[]): unknown {
      return withContext(
        ctx,
        target as (...args: unknown[]) => unknown,
        this,
        ...args
      );
    };
    Object.defineProperty(bound, 'length', { value: target.length });
    return bound as T;
  }
  enable(): this {
    this.#enabled = true;
    return this;
  }
  disable(): this {
    this.#enabled = false;
    return this;
  }
}
const context = new ContextManager();

class Tracer {
  readonly scope: { name: string; version?: string; schemaUrl?: string };
  constructor(scope: { name: string; version?: string; schemaUrl?: string }) {
    this.scope = scope;
  }

  startSpan(
    name: string,
    options: SpanOptions = {},
    parent: Context = context.active()
  ): Span {
    return attempt(() => {
      if (parent.getValue(SUPPRESS)) return new Span();
      let parentSpan = getSpan(parent);
      let detached =
        options.root === true || (parentSpan === undefined && parent !== ROOT);
      const rootMode = tracingContext.getRootCreationMode();
      if (detached && rootMode === 'attached') {
        detached = false;
        parentSpan = undefined;
        parent = ROOT;
      }
      const parentSpanContext = attempt(
        () => parentSpan?.spanContext(),
        undefined
      );
      // Without a tracer, native activation cannot change the span creation policy.
      if (!tracingContext.hasTracer()) {
        if (
          detached ||
          (parentSpan instanceof Span && parentSpan.suppressNative)
        )
          return new Span(undefined, undefined, undefined, true);
        return valid(parentSpanContext)
          ? new Span(undefined, parentSpanContext, parentSpanContext)
          : new Span();
      }
      // Decide under the explicit parent, not an unrelated surrounding context. Native
      // activation restores same-trace invocation flags before selecting the recording mode.
      const create = (): Span => {
        const mode = detached ? rootMode : tracingContext.getSpanCreationMode();
        if (mode === 'untraced') {
          if (
            detached ||
            (parentSpan instanceof Span && parentSpan.suppressNative)
          )
            return new Span(undefined, undefined, undefined, true);
          return valid(parentSpanContext)
            ? new Span(undefined, parentSpanContext, parentSpanContext)
            : new Span();
        }
        if (mode === 'unsampled') {
          if (detached) {
            const traceId = tracingContext.createRootTraceId();
            const span = nonRecording({
              traceId,
              spanId: INVALID.spanId,
              traceFlags: 0,
            });
            span.spanContext().isRemote = false;
            return span;
          }
          // An explicitly unsampled owning invocation wins even over a foreign sampled parent.
          // Explicit unsampled parents likewise never override the platform into recording.
          const identity = valid(parentSpanContext)
            ? parentSpanContext
            : attempt(
                () => fromRuntime(tracingContext.getInvocationSpanContext()),
                undefined
              );
          return valid(identity) ? nonRecording(identity) : new Span();
        }
        const startTime = timestamp(options.startTime);
        const inner = tracingContext.startSpan(
          typeof name === 'string' ? name : '',
          {
            kind:
              typeof options.kind === 'number' &&
              Number.isInteger(options.kind) &&
              options.kind >= 0 &&
              options.kind <= 4
                ? options.kind
                : 0,
            scope: this.scope,
            ...(detached ? { root: true } : {}),
            ...(startTime === undefined ? {} : { startTime }),
          }
        );
        const span =
          !detached && !inner.isTraced && valid(parentSpanContext)
            ? new Span(undefined, parentSpanContext, parentSpanContext)
            : new Span(inner, detached ? undefined : parentSpanContext);
        wrappers.set(inner, span);
        if (detached) span.spanContext().isRemote = false;
        if (options.attributes !== undefined)
          span.setAttributes(options.attributes);
        if (options.links !== undefined) span.addLinks(options.links);
        return span;
      };
      // A root uses the owning invocation, never an unrelated explicit/active remote context.
      return detached || rootMode === 'unsampled'
        ? create()
        : context.with(parent, create);
    }, new Span());
  }
  startActiveSpan<R>(name: string, fn: (span: Span) => R): R;
  startActiveSpan<R>(
    name: string,
    options: SpanOptions,
    fn: (span: Span) => R
  ): R;
  startActiveSpan<R>(
    name: string,
    options: SpanOptions,
    parent: Context,
    fn: (span: Span) => R
  ): R;
  startActiveSpan(name: string, ...args: unknown[]): unknown {
    const fn = args[args.length - 1];
    if (typeof fn !== 'function') return undefined;
    const options = (args.length > 1 ? args[0] : {}) as SpanOptions;
    const parent = (
      args.length > 2 ? (args[1] ?? context.active()) : context.active()
    ) as Context;
    const span = this.startSpan(name, options, parent);
    const ctx = attempt(
      () => parent.setValue(SPAN, span),
      ROOT.setValue(SPAN, span)
    );
    return context.with(ctx, fn as (span: Span) => unknown, undefined, span);
  }
}

const tracers = new Map<string, Tracer>();
const trace = {
  getTracer(
    name: string,
    version?: string,
    options?: { schemaUrl?: string }
  ): Tracer {
    const key = attempt(
      () => JSON.stringify([name, version, options?.schemaUrl]),
      ''
    );
    let tracer = tracers.get(key);
    if (tracer === undefined) {
      tracer = new Tracer({
        name: typeof name === 'string' ? name : '',
        ...(typeof version === 'string' ? { version } : {}),
        ...(typeof options?.schemaUrl === 'string'
          ? { schemaUrl: options.schemaUrl }
          : {}),
      });
      tracers.set(key, tracer);
    }
    return tracer;
  },
};

function validTraceStateEntry(key: string, value: string): boolean {
  return (
    /^(?:[a-z][_0-9a-z*/-]{0,255}|[a-z0-9][_0-9a-z*/-]{0,240}@[a-z][_0-9a-z*/-]{0,13})$/.test(
      key
    ) &&
    /^[\x20-\x7e]{0,255}[\x21-\x7e]$/.test(value) &&
    !/[,=]/.test(value)
  );
}

class TraceState {
  readonly #entries: Map<string, string>;
  constructor(text = '') {
    this.#entries = new Map();
    if (text.length > 512) return;
    let length = 0;
    for (const part of text.split(',')) {
      const [key, value, ...rest] = part.trim().split('=');
      if (
        key !== undefined &&
        value !== undefined &&
        rest.length === 0 &&
        validTraceStateEntry(key, value) &&
        length + part.trim().length + (this.#entries.size ? 1 : 0) <= 512
      ) {
        length += part.trim().length + (this.#entries.size ? 1 : 0);
        this.#entries.set(key, value);
        if (this.#entries.size === 32) break;
      }
    }
  }
  get(key: string): string | undefined {
    return this.#entries.get(key);
  }
  set(key: string, value: string): TraceState {
    return attempt(() => {
      if (
        !validTraceStateEntry(key, value) ||
        (this.#entries.size === 32 && !this.#entries.has(key))
      )
        return this;
      const text = `${key}=${value},${this.unset(key).serialize()}`;
      return text.length <= 512 ? new TraceState(text) : this;
    }, this);
  }
  unset(key: string): TraceState {
    return new TraceState(
      [...this.#entries]
        .filter(([k]) => k !== key)
        .map(([k, v]) => `${k}=${v}`)
        .join(',')
    );
  }
  serialize(): string {
    return [...this.#entries].map(([k, v]) => `${k}=${v}`).join(',');
  }
}

interface BaggageEntry {
  value: string;
  metadata?: { toString(): string };
}
class Baggage {
  readonly #entries: Map<string, BaggageEntry>;
  constructor(entries: [string, BaggageEntry][] = []) {
    this.#entries = new Map(entries);
  }
  getEntry(key: string): BaggageEntry | undefined {
    const entry = this.#entries.get(key);
    return entry === undefined ? undefined : { ...entry };
  }
  getAllEntries(): [string, BaggageEntry][] {
    return [...this.#entries].map(([k, v]) => [k, { ...v }]);
  }
  setEntry(key: string, entry: BaggageEntry): Baggage {
    return new Baggage([...this.#entries, [key, entry]]);
  }
  removeEntry(key: string): Baggage {
    return this.removeEntries(key);
  }
  removeEntries(...keys: string[]): Baggage {
    return new Baggage([...this.#entries].filter(([k]) => !keys.includes(k)));
  }
  clear(): Baggage {
    return new Baggage();
  }
}

interface Getter {
  get(carrier: unknown, key: string): unknown;
}
interface Setter {
  set(carrier: unknown, key: string, value: string): void;
}
const propagation = {
  fields(): string[] {
    return ['traceparent', 'tracestate', 'baggage'];
  },
  inject(ctx: Context, carrier: unknown, setter: Setter): void {
    attempt(() => {
      if (ctx.getValue(SUPPRESS)) return;
      const sc = getSpan(ctx)?.spanContext();
      if (valid(sc)) {
        setter.set(
          carrier,
          'traceparent',
          `00-${sc.traceId.toLowerCase()}-${sc.spanId.toLowerCase()}-${(sc.traceFlags & 0x03).toString(16).padStart(2, '0')}`
        );
        const state = sc.traceState?.serialize();
        if (state) setter.set(carrier, 'tracestate', state);
      }
      const baggage = ctx.getValue(BAGGAGE) as Baggage | undefined;
      if (baggage !== undefined) {
        const entries = baggage
          .getAllEntries()
          .filter(([key, entry]) => key.length > 0 && entry.value.length > 0)
          .map(
            ([key, entry]) =>
              `${encodeURIComponent(key)}=${encodeURIComponent(entry.value)}${entry.metadata ? `;${entry.metadata.toString()}` : ''}`
          )
          .filter((entry) => entry.length <= 4096)
          .slice(0, 180);
        const value = entries.reduce((result, entry) => {
          const next = result ? `${result},${entry}` : entry;
          return next.length <= 8192 ? next : result;
        }, '');
        if (value) setter.set(carrier, 'baggage', value);
      }
    }, undefined);
  },
  extract(ctx: Context, carrier: unknown, getter: Getter): Context {
    return attempt(() => {
      const header = (key: string): string =>
        attempt(() => {
          const value = getter.get(carrier, key);
          return typeof value === 'string'
            ? value
            : Array.isArray(value)
              ? key === 'traceparent'
                ? (value[0] as string)
                : value.join(',')
              : '';
        }, '');
      const tp = header('traceparent').trim();
      const match =
        /^([0-9a-f]{2})-([0-9a-f]{32})-([0-9a-f]{16})-([0-9a-f]{2})(-.*)?$/.exec(
          tp
        );
      if (
        match !== null &&
        match[1] !== 'ff' &&
        (match[1] !== '00' || match[5] === undefined)
      ) {
        const sc: SpanContext = {
          traceId: match[2] ?? INVALID.traceId,
          spanId: match[3] ?? INVALID.spanId,
          traceFlags: parseInt(match[4] ?? '0', 16),
          isRemote: true,
        };
        if (valid(sc)) {
          const state = header('tracestate');
          if (state) sc.traceState = new TraceState(state);
          ctx = ctx.setValue(SPAN, new Span(undefined, undefined, sc));
        }
      }
      const entries: [string, BaggageEntry][] = [];
      let size = 0;
      for (const part of header('baggage').split(',')) {
        if (part.length > 4096) continue;
        if (
          entries.length === 180 ||
          size + part.length + (entries.length ? 1 : 0) > 8192
        )
          break;
        attempt(() => {
          const [pair = '', ...metadata] = part.split(';');
          const index = pair.indexOf('=');
          if (index < 1) return;
          const key = decodeURIComponent(pair.slice(0, index).trim());
          const value = decodeURIComponent(pair.slice(index + 1).trim());
          if (!key || !value) return;
          const text = metadata.map((m) => m.trim()).join(';');
          size += part.length + (entries.length ? 1 : 0);
          entries.push([
            key,
            {
              value,
              ...(text ? { metadata: { toString: (): string => text } } : {}),
            },
          ]);
        }, undefined);
      }
      if (entries.length) ctx = ctx.setValue(BAGGAGE, new Baggage(entries));
      return ctx;
    }, ctx);
  },
};

// Leave the object extensible: API copies register diag independently at this version.
export default { version: '1.10.0', trace, context, propagation };
