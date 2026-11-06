// URLPattern, as the URLPattern standard declares it, for both parsers.
// The legacy parser (the urlpattern_original compatibility flag, the
// default before 2025-05-01) has no hasRegExpGroups.
/**
 * The **`URLPattern`** interface of the URL Pattern API matches URLs or parts of URLs against a pattern. The pattern can contain capturing groups that extract parts of the matched URL.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern)
 */
declare class URLPattern {
  constructor(
    input?: string | URLPatternInit,
    baseURL?: string | URLPatternOptions,
    patternOptions?: URLPatternOptions,
  );
  /**
   * The **`protocol`** read-only property of the URLPattern interface is a string containing the pattern used to match the protocol part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/protocol)
   */
  get protocol(): string;
  /**
   * The **`username`** read-only property of the URLPattern interface is a string containing the pattern used to match the username part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/username)
   */
  get username(): string;
  /**
   * The **`password`** read-only property of the URLPattern interface is a string containing the pattern used to match the password part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/password)
   */
  get password(): string;
  /**
   * The **`hostname`** read-only property of the URLPattern interface is a string containing the pattern used to match the hostname part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/hostname)
   */
  get hostname(): string;
  /**
   * The **`port`** read-only property of the URLPattern interface is a string containing the pattern used to match the port part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/port)
   */
  get port(): string;
  /**
   * The **`pathname`** read-only property of the URLPattern interface is a string containing the pattern used to match the pathname part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/pathname)
   */
  get pathname(): string;
  /**
   * The **`search`** read-only property of the URLPattern interface is a string containing the pattern used to match the search part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/search)
   */
  get search(): string;
  /**
   * The **`hash`** read-only property of the URLPattern interface is a string containing the pattern used to match the fragment part of a URL.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/hash)
   */
  get hash(): string;
  /**
   * The **`hasRegExpGroups`** read-only property of the URLPattern interface is a boolean indicating whether or not any of the URLPattern components contain regular expression capturing groups.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/hasRegExpGroups)
   */
  get hasRegExpGroups(): boolean;
  /**
   * The **`test()`** method of the URLPattern interface takes a URL string or object of URL parts, and returns a boolean indicating if the given input matches the current pattern.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/test)
   */
  test(input?: string | URLPatternInit, baseURL?: string): boolean;
  /**
   * The **`exec()`** method of the URLPattern interface takes a URL or object of URL parts, and returns either an object containing the results of matching the URL to the pattern, or null if the URL does not match the pattern.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/URLPattern/exec)
   */
  exec(
    input?: string | URLPatternInit,
    baseURL?: string,
  ): URLPatternResult | null;
}
interface URLPatternInit {
  protocol?: string;
  username?: string;
  password?: string;
  hostname?: string;
  port?: string;
  pathname?: string;
  search?: string;
  hash?: string;
  baseURL?: string;
}
interface URLPatternComponentResult {
  input: string;
  groups: Record<string, string>;
}
interface URLPatternResult {
  inputs: (string | URLPatternInit)[];
  protocol: URLPatternComponentResult;
  username: URLPatternComponentResult;
  password: URLPatternComponentResult;
  hostname: URLPatternComponentResult;
  port: URLPatternComponentResult;
  pathname: URLPatternComponentResult;
  search: URLPatternComponentResult;
  hash: URLPatternComponentResult;
}
interface URLPatternOptions {
  ignoreCase?: boolean;
}
