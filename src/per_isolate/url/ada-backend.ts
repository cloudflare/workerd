'use strict';

// The standard (url_standard) backend: a URL is its href and the offsets of
// its components within it, which utils.urlParse() and utils.urlUpdate()
// (api/url-bootstrap.h) write. The getters slice the href, as ada's
// url_aggregator getters do; a setter is one native call. A serialized URL
// is ASCII, so ada's byte offsets are string indices.
//
// The origin is ada's, computed on first read and cached until a setter that
// can change it runs: href, protocol, host, hostname or port. The others
// cannot. Username, password, search and hash are not part of an origin, and
// a blob: URL's origin comes from its opaque path, which the pathname setter
// leaves alone; a path that is not opaque starts with '/', so it never
// parses as the absolute URL a blob: origin needs. src/tests/url/
// origin-cache.js checks this after every setter.

import type {
  SearchParamsEntry,
  SearchParamsStore,
  UrlAttributes,
  UrlBackend,
} from './backend';
import type { InternalArrayHelpers } from './internal-array';
import type { UrlencodedHelpers } from './urlencoded';

const {
  ArrayPrototypeSort,
  ObjectFreeze,
  ObjectSetPrototypeOf,
  StringPrototypeCharCodeAt,
  StringPrototypeSlice,
  TypeError,
  TypedArrayCtorByName,
} = primordials;

const { urlCanParse, urlOrigin, urlParse, urlUpdate } = utils;

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
const { parseUrlencoded, serializeUrlencoded } =
  require('url/urlencoded') as UrlencodedHelpers;
const { internalArray, toUserArray } =
  require('url/internal-array') as InternalArrayHelpers;

const kOmitted = 0xffffffff;
const kAt = 0x40;
const kQuestionMark = 0x3f;

// What urlParse() and urlUpdate() write: jsg::Url::Components, then the
// scheme type. One array serves every call.
const kComponents = new TypedArrayCtorByName.Uint32Array(9);

// Internal: never reaches user code. Null-prototype, so that nothing is read
// from Object.prototype.
class AdaUrlRecord {
  declare href: string;
  declare protocolEnd: number;
  declare usernameEnd: number;
  declare hostStart: number;
  declare hostEnd: number;
  declare port: number;
  declare pathnameStart: number;
  declare searchStart: number;
  declare hashStart: number;
  declare origin: string | undefined;
  declare store: ListStore | undefined;

  // `href` is what urlParse() or urlUpdate() just returned.
  constructor(href: string) {
    this.href = href;
    this.protocolEnd = kComponents[0] as number;
    this.usernameEnd = kComponents[1] as number;
    this.hostStart = kComponents[2] as number;
    this.hostEnd = kComponents[3] as number;
    this.port = kComponents[4] as number;
    this.pathnameStart = kComponents[5] as number;
    this.searchStart = kComponents[6] as number;
    this.hashStart = kComponents[7] as number;
    this.origin = undefined;
    this.store = undefined;
  }
}

function nullPrototype(
  recordClass: abstract new (...args: never[]) => object
): void {
  const prototype = recordClass.prototype as { constructor?: unknown };
  ObjectSetPrototypeOf(prototype, null);
  delete prototype.constructor;
  ObjectFreeze(prototype);
}
nullPrototype(AdaUrlRecord);

function load(url: AdaUrlRecord, href: string): void {
  url.href = href;
  url.protocolEnd = kComponents[0] as number;
  url.usernameEnd = kComponents[1] as number;
  url.hostStart = kComponents[2] as number;
  url.hostEnd = kComponents[3] as number;
  url.port = kComponents[4] as number;
  url.pathnameStart = kComponents[5] as number;
  url.searchStart = kComponents[6] as number;
  url.hashStart = kComponents[7] as number;
}

// Where the host starts: after the '@' that ends any credentials.
function hostStart(url: AdaUrlRecord): number {
  const start = url.hostStart;
  return url.hostEnd > start &&
    StringPrototypeCharCodeAt(url.href, start) === kAt
    ? start + 1
    : start;
}

// Where the pathname ends: at the query, else the fragment, else the end.
function pathnameEnd(url: AdaUrlRecord): number {
  if (url.searchStart !== kOmitted) return url.searchStart;
  if (url.hashStart !== kOmitted) return url.hashStart;
  return url.href.length;
}

// The query's end: at the fragment, else the end.
function searchEnd(url: AdaUrlRecord): number {
  return url.hashStart !== kOmitted ? url.hashStart : url.href.length;
}

function getAttribute(url: AdaUrlRecord, attribute: number): string {
  const href = url.href;
  switch (attribute) {
    case HREF:
      return href;
    case PROTOCOL:
      return StringPrototypeSlice(href, 0, url.protocolEnd);
    case USERNAME:
      return url.protocolEnd + 2 < url.usernameEnd
        ? StringPrototypeSlice(href, url.protocolEnd + 2, url.usernameEnd)
        : '';
    case PASSWORD:
      return url.hostStart > url.usernameEnd
        ? StringPrototypeSlice(href, url.usernameEnd + 1, url.hostStart)
        : '';
    case HOST: {
      const start = hostStart(url);
      return start === url.hostEnd
        ? ''
        : StringPrototypeSlice(href, start, url.pathnameStart);
    }
    case HOSTNAME:
      return StringPrototypeSlice(href, hostStart(url), url.hostEnd);
    case PORT:
      return url.port === kOmitted
        ? ''
        : StringPrototypeSlice(href, url.hostEnd + 1, url.pathnameStart);
    case PATHNAME:
      return StringPrototypeSlice(href, url.pathnameStart, pathnameEnd(url));
    case SEARCH: {
      if (url.searchStart === kOmitted) return '';
      const end = searchEnd(url);
      return end - url.searchStart <= 1
        ? ''
        : StringPrototypeSlice(href, url.searchStart, end);
    }
    case HASH:
      return url.hashStart === kOmitted || href.length - url.hashStart <= 1
        ? ''
        : StringPrototypeSlice(href, url.hashStart);
  }
  throw new TypeError('Unknown URL attribute');
}

// The query, without its '?', as the list's source.
function queryOf(url: AdaUrlRecord): string {
  return url.searchStart === kOmitted
    ? ''
    : StringPrototypeSlice(url.href, url.searchStart + 1, searchEnd(url));
}

function compareNames(a: SearchParamsEntry, b: SearchParamsEntry): number {
  // String comparison is by UTF-16 code units, as sort() requires.
  return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0;
}

// A URLSearchParams list. A URL's store runs the URL's update steps after
// every mutation, whether or not it changed anything.
class ListStore implements SearchParamsStore {
  #list: SearchParamsEntry[];
  readonly #url: AdaUrlRecord | undefined;

  constructor(list: SearchParamsEntry[], url: AdaUrlRecord | undefined) {
    this.#list = list;
    this.#url = url;
  }

  // https://url.spec.whatwg.org/#concept-urlsearchparams-update
  #update(): void {
    const url = this.#url;
    if (url === undefined) return;
    // Setting the query to the serialization, or clearing it if that is
    // empty, cannot fail.
    load(
      url,
      urlUpdate(
        url.href,
        SEARCH,
        serializeUrlencoded(this.#list),
        kComponents
      ) as string
    );
  }

  // The URL's href or search changed: the list becomes its new query's.
  reload(): void {
    this.#list = parseUrlencoded(queryOf(this.#url as AdaUrlRecord));
  }

  size(): number {
    return this.#list.length;
  }

  append(name: string, value: string): void {
    this.#list[this.#list.length] = [name, value];
    this.#update();
  }

  delete(name: string, value: string | undefined): void {
    const list = this.#list;
    const kept = internalArray<SearchParamsEntry>();
    for (let i = 0; i < list.length; i++) {
      const entry = list[i] as SearchParamsEntry;
      if (entry[0] !== name || (value !== undefined && entry[1] !== value)) {
        kept[kept.length] = entry;
      }
    }
    this.#list = kept;
    this.#update();
  }

  get(name: string): string | null {
    const list = this.#list;
    for (let i = 0; i < list.length; i++) {
      const entry = list[i] as SearchParamsEntry;
      if (entry[0] === name) return entry[1];
    }
    return null;
  }

  getAll(name: string): string[] {
    const list = this.#list;
    const values = internalArray<string>();
    for (let i = 0; i < list.length; i++) {
      const entry = list[i] as SearchParamsEntry;
      if (entry[0] === name) values[values.length] = entry[1];
    }
    return toUserArray(values);
  }

  has(name: string, value: string | undefined): boolean {
    const list = this.#list;
    for (let i = 0; i < list.length; i++) {
      const entry = list[i] as SearchParamsEntry;
      if (entry[0] === name && (value === undefined || entry[1] === value)) {
        return true;
      }
    }
    return false;
  }

  set(name: string, value: string): void {
    const list = this.#list;
    const kept = internalArray<SearchParamsEntry>();
    let found = false;
    for (let i = 0; i < list.length; i++) {
      const entry = list[i] as SearchParamsEntry;
      if (entry[0] !== name) {
        kept[kept.length] = entry;
      } else if (!found) {
        found = true;
        entry[1] = value;
        kept[kept.length] = entry;
      }
    }
    if (!found) kept[kept.length] = [name, value];
    this.#list = kept;
    this.#update();
  }

  sort(): void {
    ArrayPrototypeSort(this.#list, compareNames);
    this.#update();
  }

  nameAt(index: number): string {
    return (this.#list[index] as SearchParamsEntry)[0];
  }

  valueAt(index: number): string {
    return (this.#list[index] as SearchParamsEntry)[1];
  }

  serialize(): string {
    return serializeUrlencoded(this.#list);
  }
}

const backend: UrlBackend<AdaUrlRecord> = {
  parse(url, base) {
    const href = urlParse(url, base, kComponents);
    if (href === undefined) throw new TypeError('Invalid URL string.');
    return new AdaUrlRecord(href);
  },

  tryParse(url, base) {
    const href = urlParse(url, base, kComponents);
    return href === undefined ? undefined : new AdaUrlRecord(href);
  },

  canParse(url, base) {
    return urlCanParse(url, base);
  },

  get: getAttribute,

  set(url, attribute, value) {
    const href = urlUpdate(url.href, attribute, value, kComponents);
    if (href === undefined) return false;
    load(url, href);
    if (
      attribute === HREF ||
      attribute === PROTOCOL ||
      attribute === HOST ||
      attribute === HOSTNAME ||
      attribute === PORT
    ) {
      url.origin = undefined;
    }
    if (
      (attribute === HREF || attribute === SEARCH) &&
      url.store !== undefined
    ) {
      url.store.reload();
    }
    return true;
  },

  origin(url) {
    let origin = url.origin;
    if (origin === undefined) {
      origin = urlOrigin(url.href);
      url.origin = origin;
    }
    return origin;
  },

  storeForUrl(url) {
    let store = url.store;
    if (store === undefined) {
      store = new ListStore(parseUrlencoded(queryOf(url)), url);
      url.store = store;
    }
    return store;
  },

  newStore(init) {
    if (typeof init !== 'string') return new ListStore(init, undefined);
    const query =
      StringPrototypeCharCodeAt(init, 0) === kQuestionMark
        ? StringPrototypeSlice(init, 1)
        : init;
    return new ListStore(parseUrlencoded(query), undefined);
  },
};

module.exports = backend;
