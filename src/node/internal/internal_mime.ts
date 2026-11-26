// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// node:util MIMEType and MIMEParams. Parsing, serializing and validating are native
// (node-internal:util, over workerd's MimeType in src/workerd/util/mimetype.h), so that a MIME type
// parses here as it does for Blob, Body and fetch. This file keeps the arguments, the parameters
// and the classes. Several details differ from Node.js and are kept for compatibility:
// - MIMEParams.get() and has() match the name exactly, while set() and delete() lowercase it.
// - MIMEParams.set() does not replace the value of an existing parameter.
// - Deleting a parameter moves the last parameter into its place.
// - A MIMEParams created with `new MIMEParams()` ignores every operation.
// - Lone surrogates in an argument become three U+FFFD characters each, the result of converting
//   them to UTF-8 and back.
//
// Besides the native calls, which take strings and objects with a null prototype, the code uses
// only language syntax: string indexing, `length`, operators, and objects with a null prototype.
// It calls no built-in methods and reads no globals, so user code that patches built-ins after
// this module loads can't change its results.

import { default as utilImpl } from 'node-internal:util';

const TypeErrorCtor = TypeError;

// The errors below match the ones the runtime throws for its built-in types.
function illegalInvocation(): TypeError {
  return new TypeErrorCtor(
    'Illegal invocation: function called with incorrect `this` reference. ' +
      'See https://developers.cloudflare.com/workers/observability/errors/' +
      '#illegal-invocation-errors for details.'
  );
}

function missingArgument(
  type: string,
  method: string,
  index: number
): TypeError {
  if (method === 'constructor') {
    return new TypeErrorCtor(
      `Failed to construct '${type}': constructor parameter ${index} is not of type 'string'.`
    );
  }
  return new TypeErrorCtor(
    `Failed to execute '${method}' on '${type}': parameter ${index} is not of type 'string'.`
  );
}

// Converts a value the way the runtime converts a string argument: ToString (which throws for a
// Symbol, unlike String()), with lone surrogates replaced as converting to UTF-8 and back does.
function toRuntimeString(value: unknown): string {
  return replaceLoneSurrogates(`${value}`);
}

// Converts argument `index` (0-based); a missing argument is an error.
function stringArg(
  args: unknown[],
  index: number,
  type: string,
  method: string
): string {
  if (index >= args.length) throw missingArgument(type, method, index + 1);
  return toRuntimeString(args[index]);
}

const lowercaseOf: Record<string, string | undefined> = {
  __proto__: null,
  A: 'a',
  B: 'b',
  C: 'c',
  D: 'd',
  E: 'e',
  F: 'f',
  G: 'g',
  H: 'h',
  I: 'i',
  J: 'j',
  K: 'k',
  L: 'l',
  M: 'm',
  N: 'n',
  O: 'o',
  P: 'p',
  Q: 'q',
  R: 'r',
  S: 's',
  T: 't',
  U: 'u',
  V: 'v',
  W: 'w',
  X: 'x',
  Y: 'y',
  Z: 'z',
} as unknown as Record<string, string | undefined>;

function asciiLowercase(str: string): string {
  let result = '';
  for (let i = 0; i < str.length; i++) {
    const c = str[i] as string;
    result += lowercaseOf[c] ?? c;
  }
  return result;
}

function isHighSurrogate(c: string | undefined): boolean {
  return c !== undefined && c >= '\ud800' && c <= '\udbff';
}

function isLowSurrogate(c: string | undefined): boolean {
  return c !== undefined && c >= '\udc00' && c <= '\udfff';
}

// Replaces each lone surrogate with three U+FFFD: converting it to UTF-8 gives three bytes that are
// not valid UTF-8, each of which becomes U+FFFD when converted back.
function replaceLoneSurrogates(str: string): string {
  let result = '';
  for (let i = 0; i < str.length; i++) {
    const c = str[i] as string;
    if (isHighSurrogate(c) && isLowSurrogate(str[i + 1])) {
      result += c + (str[i + 1] as string);
      i++;
    } else if (isHighSurrogate(c) || isLowSurrogate(c)) {
      result += '\ufffd\ufffd\ufffd';
    } else {
      result += c;
    }
  }
  return result;
}

// A list held in an object with no prototype, so that writing past the end can't reach a setter
// defined on Array.prototype.
interface List<T> {
  items: Record<number, T>;
  length: number;
}

function newList<T>(): List<T> {
  return {
    items: { __proto__: null } as unknown as Record<number, T>,
    length: 0,
  };
}

// The list of `length` items produced by `item`.
function listOf<T>(length: number, item: (i: number) => T): List<T> {
  const result = newList<T>();
  for (let i = 0; i < length; i++) result.items[i] = item(i);
  result.length = length;
  return result;
}

// A map where erasing an entry moves the last entry into its place, like kj::HashMap. This decides
// the order of the parameters after one is deleted.
class ParamTable {
  // The entries as node-internal:util takes them: the key of entry `i` at `2 * i`, its value next.
  #items = { __proto__: null } as unknown as Record<number, string>;
  #size = 0;
  #index = { __proto__: null } as unknown as Record<string, number | undefined>;

  get size(): number {
    return this.#size;
  }

  get(key: string): string | undefined {
    const i = this.#index[key];
    return i === undefined ? undefined : this.#items[2 * i + 1];
  }

  // Adds the entry unless the key is already present.
  add(key: string, value: string): void {
    if (this.#index[key] !== undefined) return;
    const i = this.#size;
    this.#index[key] = i;
    this.#items[2 * i] = key;
    this.#items[2 * i + 1] = value;
    this.#size = i + 1;
  }

  // Removes the entry's properties instead of leaving them behind, so that a long-lived table
  // that sets and deletes many distinct keys doesn't keep growing.
  delete(key: string): void {
    const i = this.#index[key];
    if (i === undefined) return;
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this.#index[key];
    const last = this.#size - 1;
    if (i < last) {
      const lastKey = this.#items[2 * last] as string;
      this.#items[2 * i] = lastKey;
      this.#items[2 * i + 1] = this.#items[2 * last + 1] as string;
      this.#index[lastKey] = i;
    }
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this.#items[2 * last];
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this.#items[2 * last + 1];
    this.#size = last;
  }

  // Copies of the entries, for iterators.
  keys(): List<string> {
    return listOf(this.#size, (i) => this.#items[2 * i] as string);
  }

  values(): List<string> {
    return listOf(this.#size, (i) => this.#items[2 * i + 1] as string);
  }

  entries(): List<string[]> {
    return listOf(this.#size, (i) => [
      this.#items[2 * i] as string,
      this.#items[2 * i + 1] as string,
    ]);
  }

  serialize(): string {
    return utilImpl.serializeMimeParams(this.#items, this.#size);
  }

  serializeWith(type: string, subtype: string): string {
    return utilImpl.serializeMimeType(type, subtype, this.#items, this.#size);
  }
}

interface ParsedMimeType {
  type: string;
  subtype: string;
  params: ParamTable;
}

// Adds a parameter if its name is a token and its value is made of quoted-string token code
// points. Returns whether the parameter was valid.
function addParam(params: ParamTable, name: string, value: string): boolean {
  if (!utilImpl.isMimeToken(name) || !utilImpl.isMimeParamValue(value)) {
    return false;
  }
  params.add(asciiLowercase(name), value);
  return true;
}

function parseMimeType(str: string): ParsedMimeType | null {
  const parts = utilImpl.parseMimeType(str);
  if (parts === undefined) return null;
  const params = new ParamTable();
  for (let i = 2; i < parts.length; i += 2) {
    params.add(parts[i] as string, parts[i + 1] as string);
  }
  return {
    type: parts[0] as string,
    subtype: parts[1] as string,
    params,
  };
}

const kInternalClass = Symbol.for('cloudflare:internal-class');

// Marks a prototype the way the runtime marks its built-in types: tools look for the
// cloudflare:internal-class symbol, and util.inspect() formats instances the same way (the accessor
// values, and the entries for MIMEParams).
function markAsRuntimeClass(proto: object): void {
  Object.defineProperty(proto, kInternalClass, {
    value: kInternalClass,
    writable: false,
    enumerable: false,
    configurable: false,
  });
  Object.defineProperty(proto, utilImpl.kResourceTypeInspect, {
    value: Object.freeze({ __proto__: null }),
    writable: false,
    enumerable: false,
    configurable: true,
  });
}

// Built-in types get a Symbol.toStringTag under the set_tostring_tag flag.
function setToStringTag(proto: object, name: string): void {
  if (Cloudflare.compatibilityFlags['set_tostring_tag']) {
    Object.defineProperty(proto, Symbol.toStringTag, {
      value: name,
      writable: true,
      configurable: true,
      enumerable: false,
    });
  }
}

// Without enable_fast_jsg_struct, the runtime leaves the value out of the final iterator result.
const finalResultHasValue =
  !!Cloudflare.compatibilityFlags['enable_fast_jsg_struct'];

const kCreate = Symbol('kCreate');

// Each iterator class extends Iterator directly, so its prototype holds next() and inherits the
// iterator helpers.
function makeIteratorClass<T>(
  name: string
): new (token: symbol, values: List<T>) => IteratorObject<T, undefined> {
  const IteratorClass = class extends Iterator<T> {
    #values: List<T>;
    #index = 0;

    constructor(token: symbol, values: List<T>) {
      if (token !== kCreate) throw new TypeErrorCtor('Illegal constructor');
      super();
      this.#values = values;
    }

    override next(): IteratorResult<T, undefined> {
      if (!(#values in this)) throw illegalInvocation();
      if (this.#index >= this.#values.length) {
        return finalResultHasValue
          ? { done: true, value: undefined }
          : ({ done: true } as IteratorReturnResult<undefined>);
      }
      return { done: false, value: this.#values.items[this.#index++] as T };
    }

    override [Symbol.iterator](): this {
      return this;
    }
  };
  Object.defineProperty(IteratorClass, 'name', { value: name });
  markAsRuntimeClass(IteratorClass.prototype);
  setToStringTag(IteratorClass.prototype, name);
  return IteratorClass;
}

const EntryIterator = makeIteratorClass<string[]>('EntryIterator');
const KeyIterator = makeIteratorClass<string>('KeyIterator');
const ValueIterator = makeIteratorClass<string>('ValueIterator');
type EntryIterator = InstanceType<typeof EntryIterator>;
type KeyIterator = InstanceType<typeof KeyIterator>;
type ValueIterator = InstanceType<typeof ValueIterator>;

export class MIMEParams {
  // The MIMEType these parameters belong to. A MIMEParams created by user code has none, and
  // ignores every operation.
  #mime: ParsedMimeType | null;

  constructor(...args: unknown[]) {
    this.#mime = args[0] === kCreate ? (args[1] as ParsedMimeType) : null;
  }

  static #check(params: unknown): asserts params is MIMEParams {
    if (typeof params !== 'object' || params === null || !(#mime in params)) {
      throw illegalInvocation();
    }
  }

  delete(...args: unknown[]): void {
    MIMEParams.#check(this);
    const name = stringArg(args, 0, 'MIMEParams', 'delete');
    this.#mime?.params.delete(asciiLowercase(name));
  }

  get(...args: unknown[]): string | null {
    MIMEParams.#check(this);
    const name = stringArg(args, 0, 'MIMEParams', 'get');
    return this.#mime?.params.get(name) ?? null;
  }

  has(...args: unknown[]): boolean {
    MIMEParams.#check(this);
    const name = stringArg(args, 0, 'MIMEParams', 'has');
    return this.#mime?.params.get(name) !== undefined;
  }

  set(...args: unknown[]): void {
    MIMEParams.#check(this);
    const name = stringArg(args, 0, 'MIMEParams', 'set');
    const value = stringArg(args, 1, 'MIMEParams', 'set');
    if (this.#mime === null) return;
    if (!addParam(this.#mime.params, name, value)) {
      throw new TypeErrorCtor('Not a valid MIME parameter');
    }
  }

  entries(): EntryIterator {
    MIMEParams.#check(this);
    return new EntryIterator(
      kCreate,
      this.#mime?.params.entries() ?? newList()
    );
  }

  keys(): KeyIterator {
    MIMEParams.#check(this);
    return new KeyIterator(kCreate, this.#mime?.params.keys() ?? newList());
  }

  values(): ValueIterator {
    MIMEParams.#check(this);
    return new ValueIterator(kCreate, this.#mime?.params.values() ?? newList());
  }

  toString(): string {
    MIMEParams.#check(this);
    return this.#mime === null ? '' : this.#mime.params.serialize();
  }

  toJSON(): string {
    MIMEParams.#check(this);
    return this.#mime === null ? '' : this.#mime.params.serialize();
  }

  // Not MIMEParams.prototype.entries, so that replacing entries() doesn't change iteration.
  [Symbol.iterator](): EntryIterator {
    MIMEParams.#check(this);
    return new EntryIterator(
      kCreate,
      this.#mime?.params.entries() ?? newList()
    );
  }

  static {
    markAsRuntimeClass(this.prototype);
    setToStringTag(this.prototype, 'MIMEParams');
  }
}

export class MIMEType {
  #mime: ParsedMimeType;
  #params: MIMEParams;

  constructor(...args: unknown[]) {
    const input = stringArg(args, 0, 'MIMEType', 'constructor');
    const mime = parseMimeType(input);
    if (mime === null) {
      throw new TypeErrorCtor(`Not a valid MIME type: ${input}`);
    }
    this.#mime = mime;
    this.#params = new MIMEParams(kCreate, mime);
  }

  static #check(mime: unknown): asserts mime is MIMEType {
    if (typeof mime !== 'object' || mime === null || !(#mime in mime)) {
      throw illegalInvocation();
    }
  }

  get type(): string {
    MIMEType.#check(this);
    return this.#mime.type;
  }

  set type(value: unknown) {
    MIMEType.#check(this);
    const type = toRuntimeString(value);
    if (!utilImpl.isMimeToken(type)) {
      throw new TypeErrorCtor('Not a valid MIME type');
    }
    this.#mime.type = asciiLowercase(type);
  }

  get subtype(): string {
    MIMEType.#check(this);
    return this.#mime.subtype;
  }

  set subtype(value: unknown) {
    MIMEType.#check(this);
    const subtype = toRuntimeString(value);
    if (!utilImpl.isMimeToken(subtype)) {
      throw new TypeErrorCtor('Not a valid MIME subtype');
    }
    this.#mime.subtype = asciiLowercase(subtype);
  }

  get essence(): string {
    MIMEType.#check(this);
    return `${this.#mime.type}/${this.#mime.subtype}`;
  }

  get params(): MIMEParams {
    MIMEType.#check(this);
    return this.#params;
  }

  toString(): string {
    MIMEType.#check(this);
    return this.#mime.params.serializeWith(this.#mime.type, this.#mime.subtype);
  }

  toJSON(): string {
    MIMEType.#check(this);
    return this.#mime.params.serializeWith(this.#mime.type, this.#mime.subtype);
  }

  static {
    markAsRuntimeClass(this.prototype);
    setToStringTag(this.prototype, 'MIMEType');
  }
}
