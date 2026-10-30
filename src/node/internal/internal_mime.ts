// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// node:util MIMEType and MIMEParams. The parser follows the WHATWG MIME Sniffing standard the way
// workerd's C++ MimeType (src/workerd/util/mimetype.c++) does, so that both agree on every input.
// Several details differ from Node.js and are kept for compatibility:
// - MIMEParams.get() and has() match the name exactly, while set() and delete() lowercase it.
// - MIMEParams.set() does not replace the value of an existing parameter.
// - Deleting a parameter moves the last parameter into its place.
// - A MIMEParams created with `new MIMEParams()` ignores every operation.
// - Lone surrogates in an argument become three U+FFFD characters each, the result of converting
//   them to UTF-8 and back.
//
// Parsing, serialization and the parameter operations use only language syntax: string indexing,
// `length`, operators, and objects with a null prototype. They call no built-in methods and read no
// globals, so user code that patches built-ins after this module loads can't change their results.

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

function isWhitespace(c: string | undefined): boolean {
  return c === '\t' || c === '\n' || c === '\r' || c === ' ';
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

// HTTP token code points besides ASCII alphanumerics. The backslash is accepted to match the C++
// parser.
const tokenSymbols: Record<string, true | undefined> = {
  __proto__: null,
  '!': true,
  '#': true,
  $: true,
  '%': true,
  '&': true,
  "'": true,
  '*': true,
  '+': true,
  '\\': true,
  '-': true,
  '.': true,
  '^': true,
  _: true,
  '`': true,
  '|': true,
  '~': true,
} as unknown as Record<string, true | undefined>;

function isTokenChar(c: string): boolean {
  return (
    (c >= 'a' && c <= 'z') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= '0' && c <= '9') ||
    tokenSymbols[c] === true
  );
}

function isToken(str: string): boolean {
  if (str.length === 0) return false;
  for (let i = 0; i < str.length; i++) {
    if (!isTokenChar(str[i] as string)) return false;
  }
  return true;
}

// HTTP quoted-string token code points: tab, 0x20 to 0x7E, and everything outside ASCII.
function isQuotedStringToken(str: string): boolean {
  for (let i = 0; i < str.length; i++) {
    const c = str[i] as string;
    if (c !== '\t' && (c < ' ' || c === '\x7f')) return false;
  }
  return true;
}

function asciiLowercase(str: string): string {
  let result = '';
  for (let i = 0; i < str.length; i++) {
    const c = str[i] as string;
    result += lowercaseOf[c] ?? c;
  }
  return result;
}

// str.slice(start, end), without calling String.prototype.slice.
function substring(str: string, start: number, end: number): string {
  let result = '';
  for (let i = start; i < end; i++) result += str[i] as string;
  return result;
}

// The index of the first `c` in str at or after `start`, or -1.
function find(str: string, c: string, start: number): number {
  for (let i = start; i < str.length; i++) {
    if (str[i] === c) return i;
  }
  return -1;
}

// The end of str[start, end) with trailing whitespace removed.
function trimTrailingWhitespace(
  str: string,
  start: number,
  end: number
): number {
  while (end > start && isWhitespace(str[end - 1])) end--;
  return end;
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

// A map where erasing an entry moves the last entry into its place, like kj::HashMap. This decides
// the order of the parameters after one is deleted.
class ParamTable {
  #keys = newList<string>();
  #values = newList<string>();
  #index = { __proto__: null } as unknown as Record<string, number | undefined>;

  get size(): number {
    return this.#keys.length;
  }

  get(key: string): string | undefined {
    const i = this.#index[key];
    return i === undefined ? undefined : this.#values.items[i];
  }

  // Adds the entry unless the key is already present.
  add(key: string, value: string): void {
    if (this.#index[key] !== undefined) return;
    const i = this.#keys.length;
    this.#index[key] = i;
    this.#keys.items[i] = key;
    this.#values.items[i] = value;
    this.#keys.length = this.#values.length = i + 1;
  }

  // Removes the entry's properties instead of leaving them behind, so that a long-lived table
  // that sets and deletes many distinct keys doesn't keep growing.
  delete(key: string): void {
    const i = this.#index[key];
    if (i === undefined) return;
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this.#index[key];
    const last = this.#keys.length - 1;
    if (i < last) {
      const lastKey = this.#keys.items[last] as string;
      this.#keys.items[i] = lastKey;
      this.#values.items[i] = this.#values.items[last] as string;
      this.#index[lastKey] = i;
    }
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this.#keys.items[last];
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this.#values.items[last];
    this.#keys.length = this.#values.length = last;
  }

  // Copies of the parameters, for iterators and serialization.
  keys(): List<string> {
    return copyList(this.#keys, (i) => this.#keys.items[i] as string);
  }

  values(): List<string> {
    return copyList(this.#values, (i) => this.#values.items[i] as string);
  }

  entries(): List<string[]> {
    return copyList(this.#keys, (i) => [
      this.#keys.items[i] as string,
      this.#values.items[i] as string,
    ]);
  }
}

function copyList<T>(source: List<unknown>, item: (i: number) => T): List<T> {
  const result = newList<T>();
  for (let i = 0; i < source.length; i++) result.items[i] = item(i);
  result.length = source.length;
  return result;
}

interface ParsedMimeType {
  type: string;
  subtype: string;
  params: ParamTable;
}

// Adds a parameter if its name is a token and its value is made of quoted-string token code
// points. Returns whether the parameter was valid.
function addParam(params: ParamTable, name: string, value: string): boolean {
  if (!isToken(name) || !isQuotedStringToken(value)) return false;
  params.add(asciiLowercase(name), value);
  return true;
}

function parseMimeType(str: string): ParsedMimeType | null {
  const end = str.length;
  let pos = 0;
  while (pos < end && isWhitespace(str[pos])) pos++;
  if (pos === end) return null;

  const slash = find(str, '/', pos);
  if (slash === -1) return null;
  const type = substring(str, pos, slash);
  if (!isToken(type)) return null;
  pos = slash + 1;
  if (pos === end) return null;

  let semicolon = find(str, ';', pos);
  if (semicolon === -1) semicolon = end;
  const subtype = substring(
    str,
    pos,
    trimTrailingWhitespace(str, pos, semicolon)
  );
  if (!isToken(subtype)) return null;
  pos = semicolon + 1;

  const params = new ParamTable();
  while (pos < end) {
    while (pos < end && isWhitespace(str[pos])) pos++;
    if (pos === end) break;

    let delimiter = pos;
    while (
      delimiter < end &&
      str[delimiter] !== ';' &&
      str[delimiter] !== '='
    ) {
      delimiter++;
    }
    // Text with no ';' or '=' up to the end is ignored.
    if (delimiter === end) break;
    // A parameter without '=' is ignored.
    if (str[delimiter] === ';') {
      pos = delimiter + 1;
      continue;
    }

    const name = substring(str, pos, delimiter);
    pos = delimiter + 1;
    if (!isToken(name)) {
      // Skip to the next parameter.
      const next = find(str, ';', pos);
      if (next === -1) break;
      pos = next + 1;
      continue;
    }
    if (pos === end) break;

    let value: string;
    if (str[pos] === '"') {
      // Collect an HTTP quoted string, unescaping backslashes. A backslash at the very end of the
      // input is kept as is.
      value = '';
      pos++;
      while (pos < end) {
        const c = str[pos] as string;
        pos++;
        if (c === '"') break;
        if (c === '\\') {
          if (pos === end) {
            value += '\\';
            break;
          }
          value += str[pos] as string;
          pos++;
        } else {
          value += c;
        }
      }
      // Anything after the closing quote, up to the next ';', is ignored.
      addParam(params, name, value);
    } else {
      let next = find(str, ';', pos);
      if (next === -1) next = end;
      value = substring(str, pos, trimTrailingWhitespace(str, pos, next));
      if (value.length > 0) addParam(params, name, value);
      pos = next;
    }
    const next = find(str, ';', pos);
    if (next === -1) break;
    pos = next + 1;
  }

  return {
    type: asciiLowercase(type),
    subtype: asciiLowercase(subtype),
    params,
  };
}

function serializeParams(params: ParamTable): string {
  const names = params.keys();
  const values = params.values();
  let result = '';
  for (let i = 0; i < names.length; i++) {
    const name = names.items[i] as string;
    const value = values.items[i] as string;
    if (i > 0) result += ';';
    // A space before the boundary parameter matches the HTML form submission serialization.
    if (name === 'boundary') result += ' ';
    result += name + '=';
    if (value.length === 0) {
      result += '""';
    } else if (isToken(value)) {
      result += value;
    } else {
      result += '"';
      for (let j = 0; j < value.length; j++) {
        const c = value[j] as string;
        result += c === '"' || c === '\\' ? '\\' + c : c;
      }
      result += '"';
    }
  }
  return result;
}

function serializeMimeType(mime: ParsedMimeType): string {
  const result = `${mime.type}/${mime.subtype}`;
  return mime.params.size > 0
    ? `${result};${serializeParams(mime.params)}`
    : result;
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
    return this.#mime === null ? '' : serializeParams(this.#mime.params);
  }

  toJSON(): string {
    MIMEParams.#check(this);
    return this.#mime === null ? '' : serializeParams(this.#mime.params);
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
    if (!isToken(type)) throw new TypeErrorCtor('Not a valid MIME type');
    this.#mime.type = asciiLowercase(type);
  }

  get subtype(): string {
    MIMEType.#check(this);
    return this.#mime.subtype;
  }

  set subtype(value: unknown) {
    MIMEType.#check(this);
    const subtype = toRuntimeString(value);
    if (!isToken(subtype)) throw new TypeErrorCtor('Not a valid MIME subtype');
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
    return serializeMimeType(this.#mime);
  }

  toJSON(): string {
    MIMEType.#check(this);
    return serializeMimeType(this.#mime);
  }

  static {
    markAsRuntimeClass(this.prototype);
    setToStringTag(this.prototype, 'MIMEType');
  }
}
