'use strict';

// The contract between the URL facade (url/url.ts) and its two backends:
// url/ada-backend.ts, over ada through utils, and url/kj-backend.ts, over
// the legacy kj::Url parser through LegacyUrlHandle. The facade picks one
// for the context from the url_standard flag, does every WebIDL conversion
// itself, and reaches the backend only through these members, so every
// argument here is already a USVString.
//
// The module's exports are the attribute numbers.

// The URL attributes, numbered as UrlBootstrap::Attribute
// (api/url-bootstrap.h) numbers them. Origin and searchParams have no
// setters and so no number there.
const kAttributes = {
  HREF: 0,
  PROTOCOL: 1,
  USERNAME: 2,
  PASSWORD: 3,
  HOST: 4,
  HOSTNAME: 5,
  PORT: 6,
  PATHNAME: 7,
  SEARCH: 8,
  HASH: 9,
} as const;
type UrlAttribute = (typeof kAttributes)[keyof typeof kAttributes];
type UrlAttributes = typeof kAttributes;

// A name-value pair of a URLSearchParams list.
type SearchParamsEntry = [name: string, value: string];

// The list behind a URLSearchParams. A URL's store is the URL's query: the
// store's mutators update the URL, and the URL's href and search setters
// replace the store's list.
interface SearchParamsStore {
  size(): number;
  append(name: string, value: string): void;
  // Removes the entries named `name`, and only those with `value` if it is
  // not undefined.
  delete(name: string, value: string | undefined): void;
  get(name: string): string | null;
  getAll(name: string): string[];
  has(name: string, value: string | undefined): boolean;
  set(name: string, value: string): void;
  sort(): void;
  // index < size().
  nameAt(index: number): string;
  valueAt(index: number): string;
  // The application/x-www-form-urlencoded serialization.
  serialize(): string;
}

interface UrlBackend<State> {
  // Throws the backend's TypeError for input that does not parse.
  parse(url: string, base: string | undefined): State;
  tryParse(url: string, base: string | undefined): State | undefined;
  canParse(url: string, base: string | undefined): boolean;

  // Getting the legacy backend's href can throw.
  get(state: State, attribute: UrlAttribute): string;
  // False only for an href that does not parse: the other setters ignore a
  // value they reject, leaving the URL unchanged.
  set(state: State, attribute: UrlAttribute, value: string): boolean;
  origin(state: State): string;

  // The URL's own store, the same one each time.
  storeForUrl(state: State): SearchParamsStore;
  // A store of its own: a list of entries, or a string to parse as
  // application/x-www-form-urlencoded after removing one leading '?'.
  newStore(init: SearchParamsEntry[] | string): SearchParamsStore;
}

export type {
  SearchParamsEntry,
  SearchParamsStore,
  UrlAttribute,
  UrlAttributes,
  UrlBackend,
};

module.exports = kAttributes;
