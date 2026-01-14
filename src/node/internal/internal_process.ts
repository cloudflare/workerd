// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Our implementation of process.nextTick is just queueMicrotask. The timing
// of when the queue is drained is different, as is the error handling so this
// is only an approximation of process.nextTick semantics. Hopefully it's good
// enough because we really do not want to implement Node.js' idea of a nextTick
// queue.

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  type NodeError,
} from 'node-internal:internal_errors'
import type legacyProcessType from 'node-internal:legacy_process'
import {
  type EmitWarningOptions,
  type ErrorWithDetail,
  default as processImpl,
} from 'node-internal:process'
import type publicProcessType from 'node-internal:public_process'
import { validateObject } from 'node-internal:validators'

export const platform = processImpl.platform

// eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
export function nextTick(cb: Function, ...args: unknown[]): void {
  queueMicrotask(() => {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-call
    cb(...args)
  })
}

// Decide if a value can round-trip to JSON without losing any information.
function isJsonSerializable(
  value: unknown,
  seen: Set<unknown> = new Set(),
): boolean {
  switch (typeof value) {
    case 'boolean':
    case 'number':
    case 'string':
      return true

    case 'object': {
      if (value === null) {
        return true
      }

      if (seen.has(value)) {
        // Don't allow cycles or aliases. (Non-cyclic aliases technically could be OK, but a
        // round trip to JSON would lose the fact that they are aliases.)
        return false
      }
      seen.add(value)

      // TODO(revisit): While any object that implements the toJSON function is
      // generally expected to be JSON serializable, when working with jsrpc
      // targets, the `toJSON` property ends up being Proxied and appears to be
      // a legit property on some object types when it really shouldn't be, causing
      // issues with certain types of bindings. Fun!
      // Commenting this out instead of removing it because it would be great if
      // we could find a way to support this reliably.
      //
      // if (typeof value.toJSON === 'function') {
      //   // This type is explicitly designed to be JSON-serialized so we'll accept it.
      //   return true;
      // }

      // We only consider objects to be serializable if they are plain objects or plain arrays.
      // Technically, JSON can serialize any subclass of Object (as well as objects with null
      // prototypes), but the round trip would lose information about the original type. Hence,
      // we assume that any env var containing such things is not intended to be appear as JSON
      // in process.env. For example, we wouldn't want a KV namespace to show up in process.env as
      // "{}" -- this would be weird.
      switch (Object.getPrototypeOf(value)) {
        case Object.prototype:
          // Note that Object.values() only returns string-keyed values, not symbol-keyed.
          return Object.values(value).every((prop) =>
            isJsonSerializable(prop, seen),
          )
        case Array.prototype:
          return (value as Array<unknown>).every((elem: unknown) =>
            isJsonSerializable(elem, seen),
          )
        default:
          return false
      }
    }

    default:
      return false
  }
}

function getInitialEnv(): Record<string, string> {
  const env: Record<string, string> = {}
  for (const [key, value] of Object.entries(processImpl.getEnvObject())) {
    // Workers environment variables can have a variety of types, but process.env vars are
    // strictly strings. We want to convert our workers env into process.env, but allowing
    // process.env to contain non-strings would probably break Node apps.
    //
    // As a compromise, we say:
    // - Workers env vars that are plain strings are unchanged in process.env.
    // - Workers env vars that can be represented as JSON will be JSON-stringified in process.env.
    // - Anything else will be omitted.
    //
    // Note that you might argue that, at the config layer, it's possible to differentiate between
    // plain strings and JSON values that evaluated to strings. Wouldn't it be nice if we could
    // check which way the binding was originally configured in order to decide whether to
    // represent it plain or as JSON here. However, there is no way to tell just by looking at
    // the `env` object inside a Worker whether a particular var was originally configured as
    // plain text, or as JSON that evaluated to a string. Either way, you just get a string. And
    // indeed, the Workers Runtime itself does not necessarily know this. In many cases it does
    // know, but in general the abstraction the Runtime intends to provide is that `env` is just
    // a JavaScript object, and how exactly the contents were originally represented is not
    // intended to be conveyed. This is important because, for example, we could extend dynamic
    // dispatch bindings in the future such that the caller can specify `env` directly, and in
    // that case the caller would simply specify a JS object, without JSON or any other
    // serialization involved. In this case, there would be no way to know if a string var was
    // "supposed to be" raw text vs. JSON.
    //
    // So, we have to do the best we can given just what we know -- the JavaScript object that is
    // `env`.
    //
    // As a consolation, this is consistent with how variables are defined in wrangler.toml: you
    // do not explicitly specify whether a variable is text or JSON. If you define a variable with
    // a simple string value, it gets configured as a text var. If you specify an object, then it's
    // configured as JSON.

    if (typeof value === 'string') {
      env[key] = value
    } else if (isJsonSerializable(value)) {
      env[key] = JSON.stringify(value)
    }
  }
  return env
}

export const env = new Proxy(getInitialEnv(), {
  // Per Node.js rules. process.env values must be coerced to strings.
  // When defined using defineProperty, the property descriptor must be writable,
  // configurable, and enumerable using just a falsy check. Getters and setters
  // are not permitted.
  set(obj: object, prop: PropertyKey, value: unknown): boolean {
    if (typeof prop === 'symbol' || typeof value === 'symbol')
      throw new TypeError(`Cannot convert a symbol value to a string`)
    return Reflect.set(obj, prop, `${value}`)
  },
  defineProperty(
    obj: object,
    prop: PropertyKey,
    descriptor: PropertyDescriptor,
  ): boolean {
    validateObject(descriptor, 'descriptor')
    if (Reflect.has(descriptor, 'get') || Reflect.has(descriptor, 'set')) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor',
        descriptor,
        'process.env value must not have getter/setter',
      )
    }
    if (!descriptor.configurable) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.configurable',
        descriptor,
        'process.env value must be configurable',
      )
    }
    if (!descriptor.enumerable) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.enumerable',
        descriptor,
        'process.env value must be enumerable',
      )
    }
    if (!descriptor.writable) {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.writable',
        descriptor,
        'process.env value must be writable',
      )
    }
    if (Reflect.has(descriptor, 'value')) {
      if (typeof prop === 'symbol' || typeof descriptor.value === 'symbol')
        throw new TypeError(`Cannot convert a symbol value to a string`)
      Reflect.set(descriptor, 'value', `${descriptor.value}`)
    } else {
      throw new ERR_INVALID_ARG_VALUE(
        'descriptor.value',
        descriptor,
        'process.env value must be specified explicitly',
      )
    }
    if (typeof prop === 'symbol')
      throw new TypeError(`Cannot convert a symbol value to a string`)
    return Reflect.defineProperty(obj, prop, descriptor)
  },
})

// The following features does not include deprecated or experimental flags mentioned in
// https://nodejs.org/docs/latest/api/process.html
export const features = Object.freeze({
  // A boolean value that is true if the current Node.js build is caching builtin modules.
  cached_builtins: true,
  // A boolean value that is true if the current Node.js build is a debug build.
  debug: false,
  // A boolean value that is true if the current Node.js build includes the inspector.
  inspector: false,
  // A boolean value that is true if the current Node.js build supports loading ECMAScript modules using require().
  // TODO(soon): Update this when we support ESM modules through require().
  require_module: false,
  // A boolean value that is true if the current Node.js build includes support for TLS.
  tls: true,
})

export function emitWarning(
  warning: string | Error,
  ctor?: ErrorConstructor,
): void
export function emitWarning(
  warning: string | Error,
  type?: string,
  ctor?: ErrorConstructor,
): void
export function emitWarning(
  warning: string | Error,
  type?: string,
  code?: string,
  ctor?: ErrorConstructor,
): void
export function emitWarning(
  warning: string | Error,
  options?: ErrorConstructor | string | EmitWarningOptions,
  codeOrCtor?: ErrorConstructor | string,
  maybeCtor?: ErrorConstructor,
): void {
  let err: Error
  let name = 'Warning'
  let detail: string | undefined
  let code: string | undefined
  let ctor: ErrorConstructor | undefined

  // Handle different overloads
  if (typeof options === 'object' && !Array.isArray(options)) {
    // emitWarning(warning, options)
    if (options.type) name = options.type
    if (options.code) code = options.code
    if (options.detail) detail = options.detail
    ctor = options.ctor
  } else if (typeof options === 'string') {
    // emitWarning(warning, type, ...)
    name = options
    if (typeof codeOrCtor === 'string') {
      // emitWarning(warning, type, code, ctor)
      code = codeOrCtor
      if (typeof maybeCtor === 'function') {
        ctor = maybeCtor
      } else if ((maybeCtor as unknown) !== undefined) {
        throw new ERR_INVALID_ARG_TYPE('ctor', 'function', maybeCtor)
      }
    } else if (typeof codeOrCtor === 'function') {
      // emitWarning(warning, type, ctor)
      ctor = codeOrCtor
    } else if ((codeOrCtor as unknown) !== undefined) {
      throw new ERR_INVALID_ARG_TYPE('ctor', 'function', codeOrCtor)
    }
  } else if (typeof options === 'function') {
    // emitWarning(warning, ctor)
    ctor = options
  } else if (options !== undefined) {
    throw new ERR_INVALID_ARG_TYPE('options', 'object', options)
  }

  // Convert string warning to Error
  if (typeof warning === 'string') {
    // Use the provided constructor if available, otherwise use Error
    const ErrorConstructor = ctor || Error
    err = new ErrorConstructor(warning)
    err.name = name
  } else if (warning instanceof Error) {
    err = warning
    // Override name if provided
    if (name && name !== 'Warning') {
      err.name = name
    }
  } else {
    throw new ERR_INVALID_ARG_TYPE('warning', 'string or Error', warning)
  }

  // Add code if provided
  if (code) {
    ;(err as NodeError).code = code
  }

  // Add detail if provided
  if (detail && typeof detail === 'string') {
    ;(err as ErrorWithDetail).detail = detail
  }

  // Capture stack trace using the provided constructor or emitWarning itself
  // This excludes the constructor (and frames above it) from the stack trace
  Error.captureStackTrace(err, ctor || emitWarning)

  // Emit the warning event on the process object
  // Use nextTick to ensure the warning is emitted asynchronously
  queueMicrotask(() => {
    ;(_process as typeof publicProcessType).emit('warning', err)
  })
}

// Events has a cycle with process, so to resolve that we lazily bind
// this _process for events usage only. All other internal importers should
// import 'node:process' directly rather, as _eventsProcess is only guaranteed
// to be available when that has been imported.
export let _process: typeof legacyProcessType | typeof publicProcessType
export function _setEventsProcess(
  process: typeof legacyProcessType | typeof publicProcessType,
): void {
  _process = process
}
