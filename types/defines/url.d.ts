// URL and URLSearchParams, as WHATWG URL declares them, at every compatibility
// date. Without the urlsearchparams_delete_has_value_arg compatibility flag
// (the default from 2023-07-01), delete() and has() ignore `value`.
/**
 * The **`URL`** interface is used to parse, construct, normalize, and encode URLs. It works by providing properties which allow you to easily read and modify the components of a URL.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL)
 */
declare class URL {
  constructor(url: string | URL, base?: string | URL);
  /**
   * The **`href`** property of the URL interface is a string containing the whole URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/href)
   */
  get href(): string;
  /**
   * The **`href`** property of the URL interface is a string containing the whole URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/href)
   */
  set href(value: string);
  /**
   * The **`origin`** read-only property of the URL interface returns a string containing the Unicode serialization of the origin of the represented URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/origin)
   */
  get origin(): string;
  /**
   * The **`protocol`** property of the URL interface is a string containing the protocol or scheme of the URL, including the final ":".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/protocol)
   */
  get protocol(): string;
  /**
   * The **`protocol`** property of the URL interface is a string containing the protocol or scheme of the URL, including the final ":".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/protocol)
   */
  set protocol(value: string);
  /**
   * The **`username`** property of the URL interface is a string containing the username component of the URL. If the URL does not have a username, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/username)
   */
  get username(): string;
  /**
   * The **`username`** property of the URL interface is a string containing the username component of the URL. If the URL does not have a username, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/username)
   */
  set username(value: string);
  /**
   * The **`password`** property of the URL interface is a string containing the password component of the URL. If the URL does not have a password, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/password)
   */
  get password(): string;
  /**
   * The **`password`** property of the URL interface is a string containing the password component of the URL. If the URL does not have a password, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/password)
   */
  set password(value: string);
  /**
   * The **`host`** property of the URL interface is a string containing the host, which is the hostname, and then, if the port of the URL is nonempty, a ":", followed by the port of the URL. If the URL does not have a hostname, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/host)
   */
  get host(): string;
  /**
   * The **`host`** property of the URL interface is a string containing the host, which is the hostname, and then, if the port of the URL is nonempty, a ":", followed by the port of the URL. If the URL does not have a hostname, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/host)
   */
  set host(value: string);
  /**
   * The **`hostname`** property of the URL interface is a string containing either the domain name or IP address of the URL. If the URL does not have a hostname, this property contains an empty string, "". IPv4 and IPv6 addresses are normalized, such as stripping leading zeros, and domain names are converted to IDN.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/hostname)
   */
  get hostname(): string;
  /**
   * The **`hostname`** property of the URL interface is a string containing either the domain name or IP address of the URL. If the URL does not have a hostname, this property contains an empty string, "". IPv4 and IPv6 addresses are normalized, such as stripping leading zeros, and domain names are converted to IDN.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/hostname)
   */
  set hostname(value: string);
  /**
   * The **`port`** property of the URL interface is a string containing the port number of the URL. If the port is the default for the protocol (80 for ws: and http:, 443 for wss: and https:, and 21 for ftp:), this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/port)
   */
  get port(): string;
  /**
   * The **`port`** property of the URL interface is a string containing the port number of the URL. If the port is the default for the protocol (80 for ws: and http:, 443 for wss: and https:, and 21 for ftp:), this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/port)
   */
  set port(value: string);
  /**
   * The **`pathname`** property of the URL interface represents a location in a hierarchical structure. It is a string constructed from a list of path segments, each of which is prefixed by a / character.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/pathname)
   */
  get pathname(): string;
  /**
   * The **`pathname`** property of the URL interface represents a location in a hierarchical structure. It is a string constructed from a list of path segments, each of which is prefixed by a / character.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/pathname)
   */
  set pathname(value: string);
  /**
   * The **`search`** property of the URL interface is a search string, also called a query string, that is a string containing a "?" followed by the parameters of the URL. If the URL does not have a search query, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/search)
   */
  get search(): string;
  /**
   * The **`search`** property of the URL interface is a search string, also called a query string, that is a string containing a "?" followed by the parameters of the URL. If the URL does not have a search query, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/search)
   */
  set search(value: string);
  /**
   * The **`searchParams`** read-only property of the URL interface returns a URLSearchParams object allowing access to the GET decoded query arguments contained in the URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/searchParams)
   */
  get searchParams(): URLSearchParams;
  /**
   * The **`hash`** property of the URL interface is a string containing a "#" followed by the fragment identifier of the URL. If the URL does not have a fragment identifier, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/hash)
   */
  get hash(): string;
  /**
   * The **`hash`** property of the URL interface is a string containing a "#" followed by the fragment identifier of the URL. If the URL does not have a fragment identifier, this property contains an empty string, "".
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/hash)
   */
  set hash(value: string);
  /**
   * The **`toJSON()`** method of the URL interface returns a string containing a serialized version of the URL, although in practice it seems to have the same effect as URL.toString().
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/toJSON)
   */
  toJSON(): string;
  toString(): string;
  /**
   * The **`URL.canParse()`** static method of the URL interface returns a boolean indicating whether or not an absolute URL, or a relative URL combined with a base URL, are parsable and valid.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/canParse_static)
   */
  static canParse(url: string, base?: string): boolean;
  /**
   * The **`URL.parse()`** static method of the URL interface returns a newly created URL object representing the URL defined by the parameters.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/parse_static)
   */
  static parse(url: string, base?: string): URL | null;
  /**
   * The **`createObjectURL()`** static method of the URL interface creates a string containing a blob URL pointing to the object given in the parameter.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/createObjectURL_static)
   */
  static createObjectURL(object: File | Blob): string;
  /**
   * The **`revokeObjectURL()`** static method of the URL interface releases an existing object URL which was previously created by calling URL.createObjectURL().
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URL/revokeObjectURL_static)
   */
  static revokeObjectURL(object_url: string): void;
}
/**
 * The **`URLSearchParams`** interface defines utility methods to work with the query string of a URL.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams)
 */
declare class URLSearchParams {
  constructor(
    init?: Iterable<Iterable<string>> | Record<string, string> | string,
  );
  /**
   * The **`size`** read-only property of the URLSearchParams interface indicates the total number of search parameter entries.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/size)
   */
  get size(): number;
  /**
   * The **`append()`** method of the URLSearchParams interface appends a specified key/value pair as a new search parameter.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/append)
   */
  append(name: string, value: string): void;
  /**
   * The **`delete()`** method of the URLSearchParams interface deletes specified parameters and their associated value(s) from the list of all search parameters.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/delete)
   */
  delete(name: string, value?: string): void;
  /**
   * The **`get()`** method of the URLSearchParams interface returns the first value associated to the given search parameter.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/get)
   */
  get(name: string): string | null;
  /**
   * The **`getAll()`** method of the URLSearchParams interface returns all the values associated with a given search parameter as an array.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/getAll)
   */
  getAll(name: string): string[];
  /**
   * The **`has()`** method of the URLSearchParams interface returns a boolean value that indicates whether the specified parameter is in the search parameters.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/has)
   */
  has(name: string, value?: string): boolean;
  /**
   * The **`set()`** method of the URLSearchParams interface sets the value associated with a given search parameter to the given value. If there were several matching values, this method deletes the others. If the search parameter doesn't exist, this method creates it.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/set)
   */
  set(name: string, value: string): void;
  /**
   * The **`URLSearchParams.sort()`** method sorts all key/value pairs contained in this object in place and returns undefined. Key/value pairs are sorted by the values of the UTF-16 code units of the keys. This method uses a stable sorting algorithm (i.e., the relative order between key/value pairs with equal keys will be preserved).
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLSearchParams/sort)
   */
  sort(): void;
  entries(): IterableIterator<[key: string, value: string]>;
  keys(): IterableIterator<string>;
  values(): IterableIterator<string>;
  forEach<This = unknown>(
    callback: (
      this: This,
      value: string,
      key: string,
      parent: URLSearchParams,
    ) => void,
    thisArg?: This,
  ): void;
  toString(): string;
  [Symbol.iterator](): IterableIterator<[key: string, value: string]>;
}
