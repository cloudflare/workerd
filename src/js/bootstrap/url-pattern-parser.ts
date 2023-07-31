// The parse has been translated from the chromium c++ implementation at:
//
//  https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/modules/url_pattern/url_pattern_parser.h;l=36;drc=f66c35e3c41629675130001dbce0dcfba870160b
//

import {lexer, LexToken, stringToRegexp, ParseOptions, Options} from './path-to-regex-modified';
import {URLPatternInit} from './url-pattern.interfaces';
import {DEFAULT_OPTIONS, protocolEncodeCallback, isSpecialScheme} from './url-utils';

enum State {
  INIT,
  PROTOCOL,
  AUTHORITY,
  USERNAME,
  PASSWORD,
  HOSTNAME,
  PORT,
  PATHNAME,
  SEARCH,
  HASH,
  DONE,
}

// A helper class to parse the first string passed to the URLPattern
// constructor.  In general the parser works by using the path-to-regexp
// lexer to first split up the input into pattern tokens.  It can
// then look through the tokens to find non-special characters that match
// the different URL component separators.  Each component is then split
// off and stored in a `URLPatternInit` object that can be accessed via
// the `Parser.result` getter.  The intent is that this init object should
// then be processed as if it was passed into the constructor itself.
export class Parser {
  // The input string to the parser.
  #input: string;

  // The list of `LexToken`s produced by the path-to-regexp `lexer()` function
  // when passed `input` with lenient mode enabled.
  #tokenList: LexToken[] = [];

  // As we parse the input string we populate a `URLPatternInit` dictionary
  // with each component pattern.  This is then the final result of the parse.
  #internalResult: URLPatternInit = {};

  // The index of the current `LexToken` being considered.
  #tokenIndex: number = 0;

  // The value to add to `tokenIndex` on each turn through the parse loop.
  // While typically this is `1`, it is also set to `0` at times for things
  // like state transitions, etc.  It is automatically reset back to `1` at
  // the top of the parse loop.
  #tokenIncrement: number = 1;

  // The index of the first `LexToken` to include in the component string.
  #componentStart: number = 0;

  // The current parse state.  This should only be changed via `changeState()`
  // or `rewindAndSetState()`.
  #state: State = State.INIT;

  // The current nest depth of `{ }` pattern groupings.
  #groupDepth: number = 0;

  // The current nesting depth of `[ ]` in hostname patterns.
  #hostnameIPv6BracketDepth: number = 0;

  // True if we should apply parse rules as if this is a "standard" URL.  If
  // false then this is treated as a "not a base URL".
  #shouldTreatAsStandardURL: boolean = false;

  public constructor(input: string) {
    this.#input = input;
  }

  // Return the parse result.  The result is only available after the
  // `parse()` method completes.
  public get result(): URLPatternInit {
    return this.#internalResult;
  }

  // Attempt to parse the input string used to construct the Parser object.
  // This method may only be called once.  Any errors will be thrown as an
  // exception.  Retrieve the parse result by accessing the `Parser.result`
  // property getter.
  public parse(): void {
    this.#tokenList = lexer(this.#input, /*lenient=*/true);

    for (; this.#tokenIndex < this.#tokenList.length;
         this.#tokenIndex += this.#tokenIncrement) {
      // Reset back to our default tokenIncrement value.
      this.#tokenIncrement = 1;

      // All states must respect the end of the token list.  The path-to-regexp
      // lexer guarantees that the last token will have the type `END`.
      if (this.#tokenList[this.#tokenIndex].type === 'END') {
        // If we failed to find a protocol terminator then we are still in
        // relative mode.  We now need to determine the first component of the
        // relative URL.
        if (this.#state === State.INIT) {
          // Reset back to the start of the input string.
          this.#rewind();

          // If the string begins with `?` then its a relative search component.
          // If it starts with `#` then its a relative hash component.  Otherwise
          // its a relative pathname.
          if (this.#isHashPrefix()) {
            this.#changeState(State.HASH, /*skip=*/1);
          } else if (this.#isSearchPrefix()) {
            this.#changeState(State.SEARCH, /*skip=*/1);
            this.#internalResult.hash = '';
          } else {
            this.#changeState(State.PATHNAME, /*skip=*/0);
            this.#internalResult.search = '';
            this.#internalResult.hash = '';
          }
          continue;
        }
        //
        // If we failed to find an `@`, then there is no username and password.
        // We should rewind and process the data as a hostname.
        else if (this.#state === State.AUTHORITY) {
          this.#rewindAndSetState(State.HOSTNAME);
          continue;
        }

        this.#changeState(State.DONE, /*skip=*/0);
        break;
      }

      // In addition, all states must handle pattern groups.  We do not permit
      // a component to end in the middle of a pattern group.  Therefore we skip
      // past any tokens that are within `{` and `}`.  Note, the tokenizer
      // handles group `(` and `)` and `:foo` groups for us automatically, so
      // we don't need special code for them here.
      if (this.#groupDepth > 0) {
        if (this.#isGroupClose()) {
          this.#groupDepth -= 1;
        } else {
          continue;
        }
      }

      if (this.#isGroupOpen()) {
        this.#groupDepth += 1;
        continue;
      }

      switch (this.#state) {
        case State.INIT:
          if (this.#isProtocolSuffix()) {
            // We are in absolute mode and we know values will not be inherited
            // from a base URL.  Therefore initialize the rest of the components
            // to the empty string.
            this.#internalResult.username = '';
            this.#internalResult.password = '';
            this.#internalResult.hostname = '';
            this.#internalResult.port = '';
            this.#internalResult.pathname = '';
            this.#internalResult.search = '';
            this.#internalResult.hash = '';

            // Update the state to expect the start of an absolute URL.
            this.#rewindAndSetState(State.PROTOCOL);
          }
          break;

        case State.PROTOCOL:
          // If we find the end of the protocol component...
          if (this.#isProtocolSuffix()) {
            // First we eagerly compile the protocol pattern and use it to
            // compute if this entire URLPattern should be treated as a
            // "standard" URL.  If any of the special schemes, like `https`,
            // match the protocol pattern then we treat it as standard.
            this.#computeShouldTreatAsStandardURL();

            // By default we treat this as a "cannot-be-a-base-URL" or what chrome
            // calls a "path" URL.  In this case we go straight to the pathname
            // component.  The hostname and port are left with their default
            // empty string values.
            let nextState: State = State.PATHNAME;
            let skip: number = 1;

            if (this.#shouldTreatAsStandardURL) {
              this.#internalResult.pathname = '/';
            }

            // If there are authority slashes, like `https://`, then
            // we must transition to the authority section of the URLPattern.
            if (this.#nextIsAuthoritySlashes()) {
              nextState = State.AUTHORITY;
              skip = 3;
            }

            // If there are no authority slashes, but the protocol is special
            // then we still go to the authority section as this is a "standard"
            // URL.  This differs from the above case since we don't need to skip
            // the extra slashes.
            else if (this.#shouldTreatAsStandardURL) {
              nextState = State.AUTHORITY;
            }

            this.#changeState(nextState, skip);
          }
          break;

        case State.AUTHORITY:
          // Before going to the hostname state we must see if there is an
          // identity of the form:
          //
          //  <username>:<password>@<hostname>
          //
          // We check for this by looking for the `@` character.  The username
          // and password are themselves each optional, so the `:` may not be
          // present.  If we see the `@` we just go to the username state
          // and let it proceed until it hits either the password separator
          // or the `@` terminator.
          if (this.#isIdentityTerminator()) {
            this.#rewindAndSetState(State.USERNAME);
          }

          // Stop searching for the `@` character if we see the beginning
          // of the pathname, search, or hash components.
          else if (this.#isPathnameStart() || this.#isSearchPrefix() ||
                   this.#isHashPrefix()) {
            this.#rewindAndSetState(State.HOSTNAME);
          }
          break;

        case State.USERNAME:
          // If we find a `:` then transition to the password component state.
          if (this.#isPasswordPrefix()) {
            this.#changeState(State.PASSWORD, /*skip=*/1);
          }

          // If we find a `@` then transition to the hostname component state.
          else if (this.#isIdentityTerminator()) {
            this.#changeState(State.HOSTNAME, /*skip=*/1);
          }
          break;

        case State.PASSWORD:
          // If we find a `@` then transition to the hostname component state.
          if (this.#isIdentityTerminator()) {
            this.#changeState(State.HOSTNAME, /*skip=*/1);
          }
          break;

        case State.HOSTNAME:
          // Track whether we are inside ipv6 address brackets.
          if (this.#isIPv6Open()) {
            this.#hostnameIPv6BracketDepth += 1;
          } else if (this.#isIPv6Close()) {
            this.#hostnameIPv6BracketDepth -= 1;
          }

          // If we find a `:` then we transition to the port component state.
          // However, we ignore `:` when parsing an ipv6 address.
          if (this.#isPortPrefix() && !this.#hostnameIPv6BracketDepth) {
            this.#changeState(State.PORT, /*skip=*/1);
          }

          // If we find a `/` then we transition to the pathname component state.
          else if (this.#isPathnameStart()) {
            this.#changeState(State.PATHNAME, /*skip=*/0);
          }

          // If we find a `?` then we transition to the search component state.
          else if (this.#isSearchPrefix()) {
            this.#changeState(State.SEARCH, /*skip=*/1);
          }

          // If we find a `#` then we transition to the hash component state.
          else if (this.#isHashPrefix()) {
            this.#changeState(State.HASH, /*skip=*/1);
          }
          break;

        case State.PORT:
          // If we find a `/` then we transition to the pathname component state.
          if (this.#isPathnameStart()) {
            this.#changeState(State.PATHNAME, /*skip=*/0);
          }

          // If we find a `?` then we transition to the search component state.
          else if (this.#isSearchPrefix()) {
            this.#changeState(State.SEARCH, /*skip=*/1);
          }

          // If we find a `#` then we transition to the hash component state.
          else if (this.#isHashPrefix()) {
            this.#changeState(State.HASH, /*skip=*/1);
          }
          break;

        case State.PATHNAME:
          // If we find a `?` then we transition to the search component state.
          if (this.#isSearchPrefix()) {
            this.#changeState(State.SEARCH, /*skip=*/1);
          }

          // If we find a `#` then we transition to the hash component state.
          else if (this.#isHashPrefix()) {
            this.#changeState(State.HASH, /*skip=*/1);
          }
          break;

        case State.SEARCH:
          // If we find a `#` then we transition to the hash component state.
          if (this.#isHashPrefix()) {
            this.#changeState(State.HASH, /*skip=*/1);
          }
          break;

        case State.HASH:
          // Nothing to do here as we are just looking for the end.
          break;

        case State.DONE:
          // This should not be reached.
          break;
      }
    }
  }

  #changeState(newState: State, skip: number): void {
    switch (this.#state) {
      case State.INIT:
        // No component to set when transitioning from this state.
        break;
      case State.PROTOCOL:
        this.#internalResult.protocol = this.#makeComponentString();
        break;
      case State.AUTHORITY:
        // No component to set when transitioning from this state.
        break;
      case State.USERNAME:
        this.#internalResult.username = this.#makeComponentString();
        break;
      case State.PASSWORD:
        this.#internalResult.password = this.#makeComponentString();
        break;
      case State.HOSTNAME:
        this.#internalResult.hostname = this.#makeComponentString();
        break;
      case State.PORT:
        this.#internalResult.port = this.#makeComponentString();
        break;
      case State.PATHNAME:
        this.#internalResult.pathname = this.#makeComponentString();
        break;
      case State.SEARCH:
        this.#internalResult.search = this.#makeComponentString();
        break;
      case State.HASH:
        this.#internalResult.hash = this.#makeComponentString();
        break;
      case State.DONE:
        // No component to set when transitioning from this state.
        break;
    }

    this.#changeStateWithoutSettingComponent(newState, skip);
  }

  #changeStateWithoutSettingComponent(newState: State, skip: number): void {
    this.#state = newState;

    // Now update `componentStart` to point to the new component.  The `skip`
    // argument tells us how many tokens to ignore to get to the next start.
    this.#componentStart = this.#tokenIndex + skip;

    // Next, move the `tokenIndex` so that the top of the loop will begin
    // parsing the new component.
    this.#tokenIndex += skip;
    this.#tokenIncrement = 0;
  }

  #rewind(): void {
    this.#tokenIndex = this.#componentStart;
    this.#tokenIncrement = 0;
  }

  #rewindAndSetState(newState: State): void {
    this.#rewind();
    this.#state = newState;
  }

  #safeToken(index: number): LexToken {
    if (index < 0) {
      index = this.#tokenList.length - index;
    }

    if (index < this.#tokenList.length) {
      return this.#tokenList[index];
    }
    return this.#tokenList[this.#tokenList.length - 1];
  }

  #isNonSpecialPatternChar(index: number, value: string): boolean {
    const token: LexToken = this.#safeToken(index);
    return token.value === value &&
      (token.type === 'CHAR' ||
       token.type === 'ESCAPED_CHAR' ||
       token.type === 'INVALID_CHAR');
  }

  #isProtocolSuffix(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, ':');
  }

  #nextIsAuthoritySlashes(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex + 1, '/') &&
           this.#isNonSpecialPatternChar(this.#tokenIndex + 2, '/');
  }

  #isIdentityTerminator(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, '@');
  }

  #isPasswordPrefix(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, ':');
  }

  #isPortPrefix(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, ':');
  }

  #isPathnameStart(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, '/');
  }

  #isSearchPrefix(): boolean {
    if (this.#isNonSpecialPatternChar(this.#tokenIndex, '?')) {
      return true;
    }

    if (this.#tokenList[this.#tokenIndex].value !== '?') {
      return false;
    }

    // We have a `?` tokenized as a modifier.  We only want to treat this as
    // the search prefix if it would not normally be valid in a path-to-regexp
    // string.  A modifier must follow a matching group.  Therefore we inspect
    // the preceding token to if the `?` is immediately following a group
    // construct.
    //
    // So if the string is:
    //
    //  https://exmaple.com/foo?bar
    //
    // Then we return true because the previous token is a `o` with type `CHAR`.
    // For the string:
    //
    //  https://example.com/:name?bar
    //
    // Then we return false because the previous token is `:name` with type
    // `NAME`.  If the developer intended this to be a search prefix then they
    // would need to escape the quest mark like `:name\\?bar`.
    //
    // Note, if `tokenIndex` is zero the index will wrap around and
    // `safeToken()` will return the `END` token.  This will correctly return
    // true from this method as a pattern cannot normally begin with an
    // unescaped `?`.
    const previousToken: LexToken = this.#safeToken(this.#tokenIndex - 1);
    return previousToken.type !== 'NAME' &&
           previousToken.type !== 'REGEX' &&
           previousToken.type !== 'CLOSE' &&
           previousToken.type !== 'ASTERISK';
  }

  #isHashPrefix(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, '#');
  }

  #isGroupOpen(): boolean {
    return this.#tokenList[this.#tokenIndex].type == 'OPEN';
  }

  #isGroupClose(): boolean {
    return this.#tokenList[this.#tokenIndex].type == 'CLOSE';
  }

  #isIPv6Open(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, '[');
  }

  #isIPv6Close(): boolean {
    return this.#isNonSpecialPatternChar(this.#tokenIndex, ']');
  }

  #makeComponentString(): string {
    const token: LexToken = this.#tokenList[this.#tokenIndex];
    const componentCharStart = this.#safeToken(this.#componentStart).index;
    return this.#input.substring(componentCharStart, token.index);
  }

  #computeShouldTreatAsStandardURL(): void {
    const options: Options & ParseOptions = {};
    Object.assign(options, DEFAULT_OPTIONS);
    options.encodePart = protocolEncodeCallback;
    const regexp = stringToRegexp(this.#makeComponentString(), /*keys=*/undefined, options);
    this.#shouldTreatAsStandardURL = isSpecialScheme(regexp);
  }
}
