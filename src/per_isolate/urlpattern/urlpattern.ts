'use strict';

// URLPattern, under the TS_URL_PATTERN autogate. It replaces both C++
// implementations: api/urlpattern-standard.h (urlpattern_standard) and
// api/urlpattern.h (urlpattern_original). The pattern parser, and so every
// pattern and the components an init input processes to, stay those of the
// parser the urlpattern_standard flag selects, which utils compiles with
// (api/urlpattern-bootstrap.h). Everything else, matching included, is
// here, and follows the URLPattern standard and WebIDL for both parsers,
// honoring set_tostring_tag. Where that departs from the C++ is listed in
// src/tests/urlpattern/AGENTS.md, whose suite runs against both
// implementations.
//
// Instances are marked uncloneable, so serializing one throws
// DataCloneError, as it does for the C++ classes.

import type { UrlAttributes, UrlBackend } from '../url/backend';
import type { RuntimeClassHelpers } from '../runtime-class';
import type { SerializationHelpers } from '../serialization';

const {
  ObjectDefineProperty,
  ObjectFreeze,
  ObjectSetPrototypeOf,
  RegExpPrototypeExec,
  StringPrototypeSlice,
  StringPrototypeToWellFormed,
  SymbolToStringTag,
  TypeError,
} = primordials;

const { PROTOCOL, USERNAME, PASSWORD, HOSTNAME, PORT, PATHNAME, SEARCH, HASH } =
  require('url/backend') as UrlAttributes;
// String inputs are parsed as URLs with ada, whichever parser compiled the
// pattern, as both C++ implementations do.
const adaUrl = require('url/ada-backend') as UrlBackend<unknown>;
const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;

const kStandard = compatFlags['urlpattern_standard'] === true;

// The parser's members of utils: they compile a pattern, and process an
// init input into the components matching compares.
const compilePattern = kStandard
  ? utils.urlPatternCompile
  : utils.legacyUrlPatternCompile;
// The legacy parser's throws its TypeError where the standard one's
// returns undefined.
const processInit = kStandard
  ? utils.urlPatternProcessInit
  : utils.legacyUrlPatternProcessInit;

const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kExecInitWithBase = kStandard
  ? 'Failed to exec URLPattern'
  : 'A baseURL is not allowed when input is an object.';
const kTestInitWithBase = kStandard
  ? 'Failed to test URLPattern'
  : 'A baseURL is not allowed when input is an object.';

// The component types, as UrlPatternBootstrap::ComponentType numbers them:
// ada's fast paths. The others, REGEXP (0), match by their RegExp, which
// only they have.
const FULL_WILDCARD = 1;
const EXACT_MATCH = 2;
const EMPTY = 3;

// The components, in the order utils lists them, which is also the init
// list's order (UrlPatternInitList); BASE_URL follows them in an init.
const kComponentCount = 8;
const I_PROTOCOL = 0;
const I_USERNAME = 1;
const I_PASSWORD = 2;
const I_HOSTNAME = 3;
const I_PORT = 4;
const I_PATHNAME = 5;
const I_SEARCH = 6;
const I_HASH = 7;
const I_BASE_URL = 8;

// URLPatternInit's members in WebIDL's order, lexicographic, as indices
// into an init list.
const kInitMemberNames = [
  'baseURL',
  'hash',
  'hostname',
  'password',
  'pathname',
  'port',
  'protocol',
  'search',
  'username',
] as const;
const kInitMemberIndices = [
  I_BASE_URL,
  I_HASH,
  I_HOSTNAME,
  I_PASSWORD,
  I_PATHNAME,
  I_PORT,
  I_PROTOCOL,
  I_SEARCH,
  I_USERNAME,
] as const;

// The results, as the URLPattern standard's dictionaries.
type ComponentResult = {
  groups: Record<string, string | undefined>;
  input: string;
};
type InitDictionary = Partial<
  Record<(typeof kInitMemberNames)[number], string>
>;
type PatternResult = {
  hash: ComponentResult;
  hostname: ComponentResult;
  inputs: (string | InitDictionary)[];
  password: ComponentResult;
  pathname: ComponentResult;
  port: ComponentResult;
  protocol: ComponentResult;
  search: ComponentResult;
  username: ComponentResult;
};

// ---------------------------------------------------------------------------
// WebIDL conversions

function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

// ToString, which throws for a Symbol, then lone surrogates to U+FFFD.
function toUSVString(value: unknown): string {
  return StringPrototypeToWellFormed(`${value as string}`);
}

function toOptionalUSVString(value: unknown): string | undefined {
  return value === undefined ? undefined : toUSVString(value);
}

// A new init list, every member absent. Its elements are its own, so a
// store to one never runs an index setter user code put on
// Array.prototype, and it needs no null prototype (url/internal-array).
function emptyInitList(): UrlPatternInitList {
  // prettier-ignore
  return [
    undefined, undefined, undefined, undefined, undefined, undefined,
    undefined, undefined, undefined,
  ];
}

// URLPatternInit, as an init list. `init` is undefined, null or an object.
function toInit(init: unknown): UrlPatternInitList {
  const list = emptyInitList();
  if (init === undefined || init === null) return list;
  for (let i = 0; i < kInitMemberNames.length; i++) {
    const value = (init as Record<string, unknown>)[
      kInitMemberNames[i] as string
    ];
    if (value !== undefined) {
      list[kInitMemberIndices[i] as number] = toUSVString(value);
    }
  }
  return list;
}

// (USVString or URLPatternInit): an object, null or undefined is an init.
function toInput(value: unknown): string | UrlPatternInitList {
  if (value === undefined || value === null || isObject(value)) {
    return toInit(value);
  }
  return toUSVString(value);
}

// URLPatternOptions' ignoreCase.
function toIgnoreCase(options: unknown, position: number): boolean {
  if (options === undefined || options === null) return false;
  if (!isObject(options)) {
    throw new TypeError(
      `Failed to construct 'URLPattern': parameter ${position} is not ` +
        "of type 'URLPatternOptions'."
    );
  }
  return !!(options as { ignoreCase?: unknown }).ignoreCase;
}

// ---------------------------------------------------------------------------
// Compiled patterns

// Internal: never reaches user code. Null-prototype, so that nothing is read
// from Object.prototype.
function nullPrototype(
  recordClass: abstract new (...args: never[]) => object
): void {
  const prototype = recordClass.prototype as { constructor?: unknown };
  ObjectSetPrototypeOf(prototype, null);
  delete prototype.constructor;
  ObjectFreeze(prototype);
}

class Component {
  declare pattern: string;
  declare regexp: RegExp | undefined;
  declare names: string[];
  declare type: number;
  declare exact: string;

  constructor(compiled: unknown[], index: number) {
    const at = 1 + index * 5;
    this.pattern = compiled[at] as string;
    this.regexp = compiled[at + 1] as RegExp | undefined;
    this.names = compiled[at + 2] as string[];
    this.type = compiled[at + 3] as number;
    this.exact = compiled[at + 4] as string;
  }
}
nullPrototype(Component);

class Pattern {
  declare components: Component[];
  declare hasRegExpGroups: boolean;

  constructor(compiled: unknown[]) {
    this.components = [
      new Component(compiled, I_PROTOCOL),
      new Component(compiled, I_USERNAME),
      new Component(compiled, I_PASSWORD),
      new Component(compiled, I_HOSTNAME),
      new Component(compiled, I_PORT),
      new Component(compiled, I_PATHNAME),
      new Component(compiled, I_SEARCH),
      new Component(compiled, I_HASH),
    ];
    this.hasRegExpGroups = compiled[0] as boolean;
  }
}
nullPrototype(Pattern);

// ---------------------------------------------------------------------------
// Matching

// The value of a string input's component `index`, as matching compares it:
// the protocol without its ':', the search and hash without their '?' and
// '#'.
function urlComponent(url: unknown, index: number): string {
  switch (index) {
    case I_PROTOCOL: {
      const protocol = adaUrl.get(url, PROTOCOL);
      return StringPrototypeSlice(protocol, 0, protocol.length - 1);
    }
    case I_USERNAME:
      return adaUrl.get(url, USERNAME);
    case I_PASSWORD:
      return adaUrl.get(url, PASSWORD);
    case I_HOSTNAME:
      return adaUrl.get(url, HOSTNAME);
    case I_PORT:
      return adaUrl.get(url, PORT);
    case I_PATHNAME:
      return adaUrl.get(url, PATHNAME);
    case I_SEARCH:
      return withoutFirst(adaUrl.get(url, SEARCH));
    default:
      return withoutFirst(adaUrl.get(url, HASH));
  }
}

// The search or hash getter's value without its '?' or '#', if it has one.
function withoutFirst(value: string): string {
  return value.length === 0 ? value : StringPrototypeSlice(value, 1);
}

// The legacy parser makes a group that did not participate ''.
const kUnmatchedGroup = kStandard ? undefined : '';

function groupValue(capture: string | undefined): string | undefined {
  return capture === undefined ? kUnmatchedGroup : capture;
}

// The groups object for a match's captures, `captures[i + 1]` for name i.
// Object literals define their members, as CreateDataProperty does, so an
// Object.prototype setter of the same name does not run; a computed-key
// literal is far cheaper than ObjectDefineProperty, which only patterns of
// more groups than these use.
function groupsOf(
  names: string[],
  captures: ArrayLike<string | undefined>
): Record<string, string | undefined> {
  switch (names.length) {
    case 0:
      return {};
    case 1:
      return { [names[0] as string]: groupValue(captures[1]) };
    case 2:
      return {
        [names[0] as string]: groupValue(captures[1]),
        [names[1] as string]: groupValue(captures[2]),
      };
    case 3:
      return {
        [names[0] as string]: groupValue(captures[1]),
        [names[1] as string]: groupValue(captures[2]),
        [names[2] as string]: groupValue(captures[3]),
      };
    default: {
      const groups = {};
      for (let i = 0; i < names.length; i++) {
        defineData(groups, names[i] as string, groupValue(captures[i + 1]));
      }
      return groups;
    }
  }
}

// The component's result for `value`, or null if it does not match.
function matchComponent(
  component: Component,
  value: string
): ComponentResult | null {
  let groups: Record<string, string | undefined>;
  switch (component.type) {
    case FULL_WILDCARD:
      // Its one group, if it has a name, captures the whole value.
      groups =
        component.names.length === 0
          ? {}
          : { [component.names[0] as string]: value };
      break;
    case EXACT_MATCH:
      if (value !== component.exact) return null;
      groups = {};
      break;
    case EMPTY:
      if (value !== '') return null;
      groups = {};
      break;
    default: {
      const match = RegExpPrototypeExec(component.regexp as RegExp, value);
      if (match === null) return null;
      groups = groupsOf(component.names, match);
      break;
    }
  }
  return { groups, input: value };
}

function testComponent(component: Component, value: string): boolean {
  switch (component.type) {
    case FULL_WILDCARD:
      return true;
    case EXACT_MATCH:
      return value === component.exact;
    case EMPTY:
      return value === '';
    default:
      return RegExpPrototypeExec(component.regexp as RegExp, value) !== null;
  }
}

// What matching compares for an input: the parsed URL of a string input,
// or the processed components of an init input; kNoMatch for an input that
// matches no pattern.
const kNoMatch = Symbol('no match');

function subjectOf(
  input: string | UrlPatternInitList,
  base: string | undefined,
  initWithBase: string
): unknown {
  if (typeof input === 'string') {
    const url = adaUrl.tryParse(input, base);
    return url === undefined ? kNoMatch : url;
  }
  if (base !== undefined) throw new TypeError(initWithBase);
  const components = processInit(input);
  return components === undefined ? kNoMatch : components;
}

// Component `index` of a subject: a string input's parsed URL, or an init
// input's processed components.
function subjectComponent(
  subject: unknown,
  isString: boolean,
  index: number
): string {
  return isString
    ? urlComponent(subject, index)
    : ((subject as string[])[index] as string);
}

// The descriptor a group, or an init copy's member, is defined with, as
// CreateDataProperty defines it: an Object.prototype setter of the same
// name does not run. Reused, its value set just before each use; its
// prototype is a frozen null-prototype object, so that it is not in
// dictionary mode and reads nothing from Object.prototype.
const kDataProperty = {
  __proto__: ObjectFreeze({ __proto__: null }),
  value: undefined as unknown,
  writable: true,
  enumerable: true,
  configurable: true,
} as PropertyDescriptor;

function defineData(object: object, key: string, value: unknown): void {
  kDataProperty.value = value;
  ObjectDefineProperty(object, key, kDataProperty);
  kDataProperty.value = undefined;
}

// The URLPatternInit an init list converts back to: its members present,
// in WebIDL's order.
function initDictionary(init: UrlPatternInitList): InitDictionary {
  const dictionary = {};
  for (let i = 0; i < kInitMemberNames.length; i++) {
    const value = init[kInitMemberIndices[i] as number];
    if (value !== undefined) {
      defineData(dictionary, kInitMemberNames[i] as string, value);
    }
  }
  return dictionary;
}

function exec(
  pattern: Pattern,
  input: string | UrlPatternInitList,
  base: string | undefined
): PatternResult | null {
  const subject = subjectOf(input, base, kExecInitWithBase);
  if (subject === kNoMatch) return null;
  const isString = typeof input === 'string';
  const components = pattern.components;
  // Pre-sized, so that its elements are its own (see emptyInitList()).
  // prettier-ignore
  const results: (ComponentResult | undefined)[] = [
    undefined, undefined, undefined, undefined, undefined, undefined,
    undefined, undefined,
  ];
  for (let i = 0; i < kComponentCount; i++) {
    const result = matchComponent(
      components[i] as Component,
      subjectComponent(subject, isString, i)
    );
    if (result === null) return null;
    results[i] = result;
  }
  let inputs: (string | InitDictionary)[];
  if (typeof input !== 'string') {
    inputs = [initDictionary(input)];
  } else if (base === undefined) {
    inputs = [input];
  } else {
    inputs = [input, base];
  }
  // Members in WebIDL's order, lexicographic.
  return {
    hash: results[I_HASH] as ComponentResult,
    hostname: results[I_HOSTNAME] as ComponentResult,
    inputs,
    password: results[I_PASSWORD] as ComponentResult,
    pathname: results[I_PATHNAME] as ComponentResult,
    port: results[I_PORT] as ComponentResult,
    protocol: results[I_PROTOCOL] as ComponentResult,
    search: results[I_SEARCH] as ComponentResult,
    username: results[I_USERNAME] as ComponentResult,
  };
}

function test(
  pattern: Pattern,
  input: string | UrlPatternInitList,
  base: string | undefined
): boolean {
  const subject = subjectOf(input, base, kTestInitWithBase);
  if (subject === kNoMatch) return false;
  const isString = typeof input === 'string';
  const components = pattern.components;
  for (let i = 0; i < kComponentCount; i++) {
    const component = components[i] as Component;
    // A wildcard matches whatever the component is, so it is not read.
    if (component.type === FULL_WILDCARD) continue;
    const value = subjectComponent(subject, isString, i);
    if (!testComponent(component, value)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// URLPattern

let patternOf: (pattern: unknown) => Pattern;

class URLPattern {
  #pattern: Pattern;

  constructor(input?: unknown, baseURL?: unknown, options?: unknown) {
    // WebIDL's overload resolution: with three arguments, or a second that
    // is not an object, null or undefined, the second is the baseURL; else
    // it is the options.
    const count = arguments.length;
    const hasBase =
      count >= 3 ||
      (count === 2 &&
        baseURL !== undefined &&
        baseURL !== null &&
        !isObject(baseURL));
    const patternInput = toInput(input);
    let base: string | undefined;
    let ignoreCase: boolean;
    if (hasBase) {
      base = toUSVString(baseURL);
      ignoreCase = toIgnoreCase(options, 3);
    } else {
      base = undefined;
      ignoreCase = toIgnoreCase(baseURL, 2);
    }
    this.#pattern = new Pattern(compilePattern(patternInput, base, ignoreCase));
    markUncloneable(this);
  }

  static {
    patternOf = (pattern: unknown): Pattern => {
      if (!isObject(pattern) || !(#pattern in pattern)) {
        throw new TypeError(kIllegalInvocation);
      }
      return pattern.#pattern;
    };
  }

  get protocol(): string {
    return componentPattern(this, I_PROTOCOL);
  }

  get username(): string {
    return componentPattern(this, I_USERNAME);
  }

  get password(): string {
    return componentPattern(this, I_PASSWORD);
  }

  get hostname(): string {
    return componentPattern(this, I_HOSTNAME);
  }

  get port(): string {
    return componentPattern(this, I_PORT);
  }

  get pathname(): string {
    return componentPattern(this, I_PATHNAME);
  }

  get search(): string {
    return componentPattern(this, I_SEARCH);
  }

  get hash(): string {
    return componentPattern(this, I_HASH);
  }

  get hasRegExpGroups(): boolean {
    return patternOf(this).hasRegExpGroups;
  }

  test(input?: unknown, baseURL?: unknown): boolean {
    const pattern = patternOf(this);
    const patternInput = toInput(input);
    return test(pattern, patternInput, toOptionalUSVString(baseURL));
  }

  exec(input?: unknown, baseURL?: unknown): PatternResult | null {
    const pattern = patternOf(this);
    const patternInput = toInput(input);
    return exec(pattern, patternInput, toOptionalUSVString(baseURL));
  }
}

function componentPattern(pattern: URLPattern, index: number): string {
  return (patternOf(pattern).components[index] as Component).pattern;
}

// ---------------------------------------------------------------------------
// The rest gives the class its WebIDL shape, and JSG's marks.

const proto = URLPattern.prototype;

// WebIDL operations and attributes are enumerable; class members are not.
// The class defines the attributes, then the operations, as WebIDL does.
// The legacy parser has no hasRegExpGroups.
const kMembers = [
  'protocol',
  'username',
  'password',
  'hostname',
  'port',
  'pathname',
  'search',
  'hash',
  'hasRegExpGroups',
  'test',
  'exec',
] as const;
for (let i = 0; i < kMembers.length; i++) {
  ObjectDefineProperty(
    proto,
    kMembers[i] as string,
    {
      __proto__: null,
      enumerable: true,
    } as PropertyDescriptor
  );
}
if (!kStandard) {
  delete (proto as { hasRegExpGroups?: unknown }).hasRegExpGroups;
}

setNameAndLength(URLPattern, 'URLPattern', 0);
setNameAndLength(proto.test, 'test', 0);
setNameAndLength(proto.exec, 'exec', 0);

markAsRuntimeClass(proto, 'URLPattern');
// WebIDL makes the tag non-writable; JSG's is writable.
if (compatFlags['set_tostring_tag'] === true) {
  ObjectDefineProperty(proto, SymbolToStringTag, {
    __proto__: null,
    writable: false,
  } as PropertyDescriptor);
}

export type UrlPatternExports = {
  URLPattern: typeof URLPattern;
};

module.exports = {
  URLPattern,
};
