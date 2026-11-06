'use strict';

// The values the runtime passes to a Worker's handlers: the TestController
// given to test() and the AlarmInvocationInfo given to a Durable Object's
// alarm(). C++ creates them through createTestController() and
// createAlarmInvocationInfo(), which global-scope.c++ calls when it invokes
// the handler.
//
// Both look as JSG resource types do: a constructor that always throws
// "Illegal constructor", with or without `new`, a writable `prototype` unless
// immutable_api_prototypes, and the marks JSG puts on every resource type's
// prototype. The constructors are plain functions rather than classes because
// a class can be neither called without `new` nor given a writable
// `prototype`.
//
// DIFFERENCES FROM JSG RESOURCE TYPES
//
// - structuredClone() copies an instance as a plain object, as it does any
//   object without internal fields; it throws a DataCloneError for a JSG
//   object. JS RPC refuses both, since it serializes no class instance.
// - Function.prototype.toString() of a constructor gives its source rather
//   than `function Name() { [native code] }`.

const {
  ObjectCreate,
  ObjectDefineProperties,
  ObjectFreeze,
  SymbolFor,
  SymbolToStringTag,
  TypeError,
} = primordials;

const { getApiSymbol } = utils;

const kInternalClass = SymbolFor('cloudflare:internal-class');
const kResourceTypeInspect = getApiSymbol('kResourceTypeInspect');
const kNoInspectProperties = ObjectFreeze({ __proto__: null });

// What JSG puts on every resource type: under set_tostring_tag the tag, then
// the marker tools use to recognize runtime classes and util.inspect()'s
// formatting hook, on the prototype; and, under immutable_api_prototypes (or
// python_workers, which implies it), a read-only `prototype`.
function makeRuntimeClass(ctor: Function, name: string): void {
  const proto = ctor.prototype as object;
  if (compatFlags['set_tostring_tag'] === true) {
    ObjectDefineProperties(proto, {
      __proto__: null,
      [SymbolToStringTag]: {
        __proto__: null,
        value: name,
        writable: true,
        enumerable: false,
        configurable: true,
      },
    });
  }
  ObjectDefineProperties(proto, {
    __proto__: null,
    [kInternalClass]: {
      __proto__: null,
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    },
    [kResourceTypeInspect]: {
      __proto__: null,
      value: kNoInspectProperties,
      writable: false,
      enumerable: false,
      configurable: true,
    },
  });
  if (
    compatFlags['immutable_api_prototypes'] === true ||
    compatFlags['python_workers'] === true
  ) {
    ObjectDefineProperties(ctor, {
      __proto__: null,
      prototype: { __proto__: null, writable: false },
    });
  }
}

function TestController(): never {
  throw new TypeError('Illegal constructor');
}
makeRuntimeClass(TestController, 'TestController');

function AlarmInvocationInfo(): never {
  throw new TypeError('Illegal constructor');
}
makeRuntimeClass(AlarmInvocationInfo, 'AlarmInvocationInfo');

// A JSG readonly instance property: an own data property, enumerable and
// configurable but not writable.
function readonlyValue(value: unknown): PropertyDescriptor {
  return {
    __proto__: null,
    value,
    writable: false,
    enumerable: true,
    configurable: true,
  } as PropertyDescriptor;
}

function createTestController(): object {
  return ObjectCreate(TestController.prototype) as object;
}

// `scheduledTime` is in milliseconds since the Unix epoch. The properties are
// defined in the order JSG gave the instance template's.
function createAlarmInvocationInfo(
  scheduledTime: number,
  retryCount: number
): object {
  return ObjectCreate(AlarmInvocationInfo.prototype, {
    scheduledTime: readonlyValue(scheduledTime),
    retryCount: readonlyValue(retryCount),
    isRetry: readonlyValue(retryCount > 0),
  }) as object;
}

export type HandlersExports = {
  createTestController: typeof createTestController;
  createAlarmInvocationInfo: typeof createAlarmInvocationInfo;
};

module.exports = {
  createTestController,
  createAlarmInvocationInfo,
};
