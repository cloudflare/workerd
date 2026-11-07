'use strict';

// URL and URLSearchParams, under the TS_URL autogate. They replace both C++
// implementations: api/url-standard.h (url_standard) and api/url.h
// (url_original). The parser and every attribute's semantics stay those of
// the backend the url_standard flag selects (url/backend.ts); everything
// else follows WHATWG URL and WebIDL for both backends, honoring the dated
// flags urlsearchparams_delete_has_value_arg,
// workers_api_getters_setters_on_prototype and set_tostring_tag. Where that
// departs from the C++ is listed in src/tests/url/AGENTS.md, whose suite
// runs against both implementations.
//
// Instances are marked uncloneable, so serializing one throws
// DataCloneError, as it does for the C++ classes.

import type {
  SearchParamsEntry,
  SearchParamsStore,
  UrlAttribute,
  UrlAttributes,
  UrlBackend,
} from './backend';
import type { InternalArrayHelpers } from './internal-array';
import type { RuntimeClassHelpers } from '../runtime-class';
import type { SerializationHelpers } from '../serialization';

const {
  BlobPrototypeSlice,
  Error,
  FunctionPrototypeCall,
  IteratorPrototype,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectGetOwnPropertyDescriptor,
  ObjectSetPrototypeOf,
  ReflectOwnKeys,
  SafeMap,
  StringPrototypeToWellFormed,
  SymbolIterator,
  SymbolToStringTag,
  TypeError,
} = primordials;

const {
  HREF,
  PROTOCOL,
  USERNAME,
  PASSWORD,
  HOST,
  HOSTNAME,
  PORT,
  PATHNAME,
  SEARCH,
  HASH,
} = require('url/backend') as UrlAttributes;
const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;
const { internalArray } = require('url/internal-array') as InternalArrayHelpers;

// Only the selected backend is loaded.
const backend = require(
  compatFlags['url_standard'] === true ? 'url/ada-backend' : 'url/kj-backend'
) as UrlBackend<unknown>;

const kDeleteHasValueArg =
  compatFlags['urlsearchparams_delete_has_value_arg'] === true;
// The legacy C++ URL's attributes are own properties of each instance unless
// workers_api_getters_setters_on_prototype; the standard one's never are.
const kInstanceProperties =
  compatFlags['url_standard'] !== true &&
  compatFlags['workers_api_getters_setters_on_prototype'] !== true;

const { getPrivateSymbol } = utils;

// V8-private, so user code cannot read or forge them: the C++ bridge
// (api/url-bridge.h) recognizes an instance by its brand without running
// JavaScript.
const kUrlBrand = getPrivateSymbol('workerd:URL');
const kSearchParamsBrand = getPrivateSymbol('workerd:URLSearchParams');

const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kInvalidHref =
  "Failed to set the 'href' property on 'URL': the provided value is not " +
  "of type 'valid URL string'.";

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

function requireArguments(count: number, required: number, what: string): void {
  if (count < required) {
    throw new TypeError(
      `${what}: ${required} argument${required === 1 ? '' : 's'} required, ` +
        `but only ${count} present.`
    );
  }
}

const kConstructSearchParams = "Failed to construct 'URLSearchParams'";

// Iterates `iterable` with `method`, its @@iterator, as WebIDL creates a
// sequence: without closing the iterator if a conversion throws.
function forEachOfIterable(
  iterable: object,
  method: unknown,
  each: (value: unknown) => void
): void {
  const iterator = FunctionPrototypeCall(
    method as (this: object) => unknown,
    iterable
  );
  if (!isObject(iterator)) {
    throw new TypeError(
      `${kConstructSearchParams}: the iterator is not an object.`
    );
  }
  const next = (iterator as { next: unknown }).next;
  for (;;) {
    const result = FunctionPrototypeCall(
      next as (this: object) => unknown,
      iterator
    );
    if (!isObject(result)) {
      throw new TypeError(
        `${kConstructSearchParams}: the iterator result is not an object.`
      );
    }
    if ((result as IteratorResult<unknown>).done) return;
    each((result as IteratorResult<unknown>).value);
  }
}

function getIteratorMethod(value: object): unknown {
  const method = (value as { [SymbolIterator]?: unknown })[SymbolIterator];
  if (method === undefined || method === null) return undefined;
  if (typeof method !== 'function') {
    throw new TypeError(
      `${kConstructSearchParams}: the value's Symbol.iterator is not a function.`
    );
  }
  return method;
}

// sequence<sequence<USVString>>, then each inner sequence must be a pair.
function sequenceInit(init: object, method: unknown): SearchParamsEntry[] {
  const sequences = internalArray<string[]>();
  forEachOfIterable(init, method, (element) => {
    const notASequence = `${kConstructSearchParams}: the value cannot be converted to a sequence.`;
    if (!isObject(element)) throw new TypeError(notASequence);
    const inner = getIteratorMethod(element);
    if (inner === undefined) throw new TypeError(notASequence);
    const strings = internalArray<string>();
    forEachOfIterable(element, inner, (item) => {
      strings[strings.length] = toUSVString(item);
    });
    sequences[sequences.length] = strings;
  });
  const entries = internalArray<SearchParamsEntry>();
  for (let i = 0; i < sequences.length; i++) {
    const pair = sequences[i] as string[];
    if (pair.length !== 2) {
      throw new TypeError(
        `${kConstructSearchParams}: each sequence must be a name-value pair.`
      );
    }
    entries[entries.length] = [pair[0] as string, pair[1] as string];
  }
  return entries;
}

// record<USVString, USVString>: the own enumerable properties, in
// [[OwnPropertyKeys]] order. Two keys that convert to the same USVString are
// one entry, at the first one's position, with the last one's value.
function recordInit(init: object): SearchParamsEntry[] {
  const entries = internalArray<SearchParamsEntry>();
  const positions = new SafeMap() as Map<string, number>;
  const keys = ReflectOwnKeys(init);
  for (let i = 0; i < keys.length; i++) {
    const key = keys[i] as PropertyKey;
    const descriptor = ObjectGetOwnPropertyDescriptor(init, key);
    if (descriptor === undefined || descriptor.enumerable !== true) continue;
    const name = toUSVString(key);
    const value = toUSVString((init as Record<PropertyKey, unknown>)[key]);
    const position = positions.get(name);
    if (position === undefined) {
      positions.set(name, entries.length);
      entries[entries.length] = [name, value];
    } else {
      (entries[position] as SearchParamsEntry)[1] = value;
    }
  }
  return entries;
}

// The constructor's (sequence<sequence<USVString>> or record<USVString,
// USVString> or USVString) init = "".
function storeFromInit(init: unknown): SearchParamsStore {
  if (init === undefined) return backend.newStore('');
  if (isObject(init)) {
    const method = getIteratorMethod(init);
    return backend.newStore(
      method === undefined ? recordInit(init) : sequenceInit(init, method)
    );
  }
  return backend.newStore(toUSVString(init));
}

// ---------------------------------------------------------------------------
// URL

// Passed as the first argument by URL.parse() and URL.prototype.searchParams,
// which pass the state the constructor adopts, rather than parses, as the
// second. User code cannot reach it.
const kAdopt: symbol = Symbol('adopt');

let urlState: (url: unknown) => unknown;

class URL {
  #state: unknown;
  #searchParams: URLSearchParams | undefined;

  constructor(url: unknown, base?: unknown) {
    let state;
    if (url === kAdopt) {
      state = base;
    } else {
      requireArguments(arguments.length, 1, "Failed to construct 'URL'");
      const urlString = toUSVString(url);
      state = backend.parse(urlString, toOptionalUSVString(base));
    }
    this.#state = state;
    this.#searchParams = undefined;
    (this as unknown as Record<symbol, unknown>)[kUrlBrand] = true;
    markUncloneable(this);
    if (kInstanceProperties) {
      ObjectDefineProperties(this, kInstanceDescriptors);
    }
  }

  static {
    urlState = (url: unknown): unknown => {
      if (!isObject(url) || !(#state in url)) {
        throw new TypeError(kIllegalInvocation);
      }
      return url.#state;
    };
  }

  get href(): string {
    return backend.get(urlState(this), HREF);
  }
  set href(value: string) {
    const state = urlState(this);
    if (!backend.set(state, HREF, toUSVString(value))) {
      throw new TypeError(kInvalidHref);
    }
  }

  get origin(): string {
    return backend.origin(urlState(this));
  }

  get protocol(): string {
    return getAttribute(this, PROTOCOL);
  }
  set protocol(value: string) {
    setAttribute(this, PROTOCOL, value);
  }

  get username(): string {
    return getAttribute(this, USERNAME);
  }
  set username(value: string) {
    setAttribute(this, USERNAME, value);
  }

  get password(): string {
    return getAttribute(this, PASSWORD);
  }
  set password(value: string) {
    setAttribute(this, PASSWORD, value);
  }

  get host(): string {
    return getAttribute(this, HOST);
  }
  set host(value: string) {
    setAttribute(this, HOST, value);
  }

  get hostname(): string {
    return getAttribute(this, HOSTNAME);
  }
  set hostname(value: string) {
    setAttribute(this, HOSTNAME, value);
  }

  get port(): string {
    return getAttribute(this, PORT);
  }
  set port(value: string) {
    setAttribute(this, PORT, value);
  }

  get pathname(): string {
    return getAttribute(this, PATHNAME);
  }
  set pathname(value: string) {
    setAttribute(this, PATHNAME, value);
  }

  get search(): string {
    return getAttribute(this, SEARCH);
  }
  set search(value: string) {
    setAttribute(this, SEARCH, value);
  }

  get searchParams(): URLSearchParams {
    const state = urlState(this);
    let searchParams = this.#searchParams;
    if (searchParams === undefined) {
      searchParams = new URLSearchParams(kAdopt, backend.storeForUrl(state));
      this.#searchParams = searchParams;
    }
    return searchParams;
  }

  get hash(): string {
    return getAttribute(this, HASH);
  }
  set hash(value: string) {
    setAttribute(this, HASH, value);
  }

  toJSON(): string {
    return backend.get(urlState(this), HREF);
  }

  toString(): string {
    return backend.get(urlState(this), HREF);
  }

  static canParse(url: unknown, base?: unknown): boolean {
    requireArguments(
      arguments.length,
      1,
      "Failed to execute 'canParse' on 'URL'"
    );
    const urlString = toUSVString(url);
    return backend.canParse(urlString, toOptionalUSVString(base));
  }

  static parse(url: unknown, base?: unknown): URL | null {
    requireArguments(arguments.length, 1, "Failed to execute 'parse' on 'URL'");
    const urlString = toUSVString(url);
    const state = backend.tryParse(urlString, toOptionalUSVString(base));
    if (state === undefined) return null;
    return new URL(kAdopt, state);
  }

  static createObjectURL(object: unknown): string {
    const what = "Failed to execute 'createObjectURL' on 'URL'";
    requireArguments(arguments.length, 1, what);
    try {
      BlobPrototypeSlice(object as Blob, 0, 0);
    } catch {
      throw new TypeError(`${what}: parameter 1 is not of type 'Blob'.`);
    }
    throw new Error('URL.createObjectURL() is not implemented');
  }

  static revokeObjectURL(url: unknown): void {
    requireArguments(
      arguments.length,
      1,
      "Failed to execute 'revokeObjectURL' on 'URL'"
    );
    toUSVString(url);
    throw new Error('URL.revokeObjectURL() is not implemented');
  }
}

function getAttribute(url: URL, attribute: UrlAttribute): string {
  return backend.get(urlState(url), attribute);
}

function setAttribute(url: URL, attribute: UrlAttribute, value: unknown): void {
  const state = urlState(url);
  backend.set(state, attribute, toUSVString(value));
}

// ---------------------------------------------------------------------------
// URLSearchParams

let searchParamsStore: (params: unknown) => SearchParamsStore;

const kAppend = "Failed to execute 'append' on 'URLSearchParams'";
const kDelete = "Failed to execute 'delete' on 'URLSearchParams'";
const kGet = "Failed to execute 'get' on 'URLSearchParams'";
const kGetAll = "Failed to execute 'getAll' on 'URLSearchParams'";
const kHas = "Failed to execute 'has' on 'URLSearchParams'";
const kSet = "Failed to execute 'set' on 'URLSearchParams'";
const kForEach = "Failed to execute 'forEach' on 'URLSearchParams'";

class URLSearchParams {
  #store: SearchParamsStore;

  constructor(init?: unknown, store?: unknown) {
    this.#store =
      init === kAdopt ? (store as SearchParamsStore) : storeFromInit(init);
    (this as unknown as Record<symbol, unknown>)[kSearchParamsBrand] = true;
    markUncloneable(this);
  }

  static {
    searchParamsStore = (params: unknown): SearchParamsStore => {
      if (!isObject(params) || !(#store in params)) {
        throw new TypeError(kIllegalInvocation);
      }
      return params.#store;
    };
  }

  get size(): number {
    return searchParamsStore(this).size();
  }

  append(name: unknown, value: unknown): void {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 2, kAppend);
    const nameString = toUSVString(name);
    store.append(nameString, toUSVString(value));
  }

  delete(name: unknown, value?: unknown): void {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 1, kDelete);
    const nameString = toUSVString(name);
    store.delete(
      nameString,
      kDeleteHasValueArg ? toOptionalUSVString(value) : undefined
    );
  }

  get(name: unknown): string | null {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 1, kGet);
    return store.get(toUSVString(name));
  }

  getAll(name: unknown): string[] {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 1, kGetAll);
    return store.getAll(toUSVString(name));
  }

  has(name: unknown, value?: unknown): boolean {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 1, kHas);
    const nameString = toUSVString(name);
    return store.has(
      nameString,
      kDeleteHasValueArg ? toOptionalUSVString(value) : undefined
    );
  }

  set(name: unknown, value: unknown): void {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 2, kSet);
    const nameString = toUSVString(name);
    store.set(nameString, toUSVString(value));
  }

  sort(): void {
    searchParamsStore(this).sort();
  }

  entries(): URLSearchParamsIterator {
    return new URLSearchParamsIterator(searchParamsStore(this), kEntries);
  }

  keys(): URLSearchParamsIterator {
    return new URLSearchParamsIterator(searchParamsStore(this), kKeys);
  }

  values(): URLSearchParamsIterator {
    return new URLSearchParamsIterator(searchParamsStore(this), kValues);
  }

  forEach(callback: unknown, thisArg?: unknown): void {
    const store = searchParamsStore(this);
    requireArguments(arguments.length, 1, kForEach);
    if (typeof callback !== 'function') {
      throw new TypeError(
        `${kForEach}: parameter 1 is not of type 'Function'.`
      );
    }
    // Live: entries added by the callback are visited.
    for (let i = 0; i < store.size(); i++) {
      FunctionPrototypeCall(
        callback as (
          this: unknown,
          value: string,
          name: string,
          params: URLSearchParams
        ) => void,
        thisArg,
        store.valueAt(i),
        store.nameAt(i),
        this
      );
    }
  }

  toString(): string {
    return searchParamsStore(this).serialize();
  }
}

// ---------------------------------------------------------------------------
// The iterators: WebIDL default iterator objects, sharing one prototype
// that inherits %IteratorPrototype%. Each reads its list live, by index.

const kEntries = 0;
const kKeys = 1;
const kValues = 2;

class URLSearchParamsIterator {
  readonly #store: SearchParamsStore;
  readonly #kind: number;
  #index: number;

  constructor(store: SearchParamsStore, kind: number) {
    this.#store = store;
    this.#kind = kind;
    this.#index = 0;
    markUncloneable(this);
  }

  next(): IteratorResult<string | [string, string], undefined> {
    if (!isObject(this) || !(#store in this)) {
      throw new TypeError(kIllegalInvocation);
    }
    const store = this.#store;
    const index = this.#index;
    if (index >= store.size()) return { value: undefined, done: true };
    this.#index = index + 1;
    switch (this.#kind) {
      case kKeys:
        return { value: store.nameAt(index), done: false };
      case kValues:
        return { value: store.valueAt(index), done: false };
      default:
        return {
          value: [store.nameAt(index), store.valueAt(index)],
          done: false,
        };
    }
  }
}

// ---------------------------------------------------------------------------
// The rest gives the classes their WebIDL shape, and JSG's marks.

// WebIDL operations and attributes are enumerable; class members are not.
function makeEnumerable(object: object, names: readonly PropertyKey[]): void {
  for (let i = 0; i < names.length; i++) {
    ObjectDefineProperty(
      object,
      names[i] as PropertyKey,
      {
        __proto__: null,
        enumerable: true,
      } as PropertyDescriptor
    );
  }
}

// WebIDL makes the tag non-writable; JSG's is writable.
function markAsWebIdlClass(proto: object, name: string): void {
  markAsRuntimeClass(proto, name);
  if (compatFlags['set_tostring_tag'] === true) {
    ObjectDefineProperty(proto, SymbolToStringTag, {
      __proto__: null,
      writable: false,
    } as PropertyDescriptor);
  }
}

const kUrlAttributes = [
  'href',
  'origin',
  'protocol',
  'username',
  'password',
  'host',
  'hostname',
  'port',
  'pathname',
  'search',
  'searchParams',
  'hash',
] as const;

makeEnumerable(URL.prototype, [...kUrlAttributes, 'toJSON', 'toString']);
makeEnumerable(URL, [
  'canParse',
  'parse',
  'createObjectURL',
  'revokeObjectURL',
]);
setNameAndLength(URL, 'URL', 1);
setNameAndLength(URL.canParse, 'canParse', 1);
setNameAndLength(URL.parse, 'parse', 1);
markAsWebIdlClass(URL.prototype, 'URL');

// The legacy instance layout: the prototype's accessors, again as own
// properties of each instance, in the order JSG defines them (the reverse
// of the WebIDL order). JSG's are native data properties, which JavaScript
// cannot make.
const kInstanceDescriptors = {
  __proto__: null,
} as unknown as PropertyDescriptorMap;
for (let i = kUrlAttributes.length - 1; i >= 0; i--) {
  const name = kUrlAttributes[i] as string;
  const descriptor = ObjectGetOwnPropertyDescriptor(
    URL.prototype,
    name
  ) as PropertyDescriptor;
  kInstanceDescriptors[name] = {
    __proto__: null,
    get: descriptor.get,
    set: descriptor.set,
    enumerable: true,
    configurable: true,
  } as PropertyDescriptor;
}

makeEnumerable(URLSearchParams.prototype, [
  'size',
  'append',
  'delete',
  'get',
  'getAll',
  'has',
  'set',
  'sort',
  'entries',
  'keys',
  'values',
  'forEach',
  'toString',
]);
setNameAndLength(URLSearchParams, 'URLSearchParams', 0);
setNameAndLength(URLSearchParams.prototype.delete, 'delete', 1);
setNameAndLength(URLSearchParams.prototype.has, 'has', 1);
setNameAndLength(URLSearchParams.prototype.forEach, 'forEach', 1);
ObjectDefineProperty(URLSearchParams.prototype, SymbolIterator, {
  __proto__: null,
  value: URLSearchParams.prototype.entries,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);
markAsWebIdlClass(URLSearchParams.prototype, 'URLSearchParams');

const kIteratorProto = URLSearchParamsIterator.prototype;
ObjectSetPrototypeOf(kIteratorProto, IteratorPrototype);
delete (kIteratorProto as { constructor?: unknown }).constructor;
makeEnumerable(kIteratorProto, ['next']);
if (compatFlags['set_tostring_tag'] === true) {
  ObjectDefineProperty(kIteratorProto, SymbolToStringTag, {
    __proto__: null,
    value: 'URLSearchParams Iterator',
    writable: false,
    enumerable: false,
    configurable: true,
  } as PropertyDescriptor);
}

// For the C++ bridge, through url/cpp_exports: each reads the
// implementation's own state, whatever the instance's methods have become.
// The bridge passes only branded instances.
const cppExports = {
  urlHref(url: URL): string {
    return backend.get(urlState(url), HREF);
  },
  searchParamsSerialize(params: URLSearchParams): string {
    return searchParamsStore(params).serialize();
  },
};

export type UrlExports = {
  URL: typeof URL;
  URLSearchParams: typeof URLSearchParams;
  cppExports: typeof cppExports;
};

module.exports = {
  URL,
  URLSearchParams,
  cppExports,
};
