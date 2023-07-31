/**
 * Tokenizer results.
 */
export interface LexToken {
  type:
  | "OPEN"
  | "CLOSE"
  | "REGEX"
  | "NAME"
  | "CHAR"
  | "ESCAPED_CHAR"
  | "OTHER_MODIFIER"
  | "ASTERISK"
  | "END"
  | "INVALID_CHAR";
  index: number;
  value: string;
}

export enum Modifier {
  // The `*` modifier.
  kZeroOrMore = 0,
  // The `?` modifier.
  kOptional = 1,
  // The `+` modifier.
  kOneOrMore = 2,
  // No modifier.
  kNone = 3,
};

export enum PartType {
  // A part that matches any character to the end of the input string.
  kFullWildcard = 0,
  // A part that matches any character to the next segment separator.
  kSegmentWildcard = 1,
  // A part with a custom regular expression.
  kRegex = 2,
  // A fixed, non-variable part of the pattern.  Consists of kChar and
  // kEscapedChar Tokens.
  kFixed = 3,
}

export class Part {
  type: PartType = PartType.kFixed;
  name: string = '';
  prefix: string = '';
  value: string = '';
  suffix: string = '';
  modifier: Modifier = Modifier.kNone;

  constructor(type: PartType, name: string, prefix: string, value: string, suffix: string, modifier: Modifier) {
    this.type = type;
    this.name = name;
    this.prefix = prefix;
    this.value = value;
    this.suffix = suffix;
    this.modifier = modifier;
  }

  hasCustomName() {
    return this.name !== "" && typeof this.name !== "number";
  }
}

// Note, the `//u` suffix triggers this typescript linting bug:
//
//  https://github.com/buzinas/tslint-eslint-rules/issues/289
//
// This requires disabling the no-empty-character-class lint rule.
const regexIdentifierStart = /[$_\p{ID_Start}]/u;
const regexIdentifierPart = /[$_\u200C\u200D\p{ID_Continue}]/u;

const kFullWildcardRegex = ".*";

function isASCII(str: string, extended: boolean) {
  return (extended ? /^[\x00-\xFF]*$/ : /^[\x00-\x7F]*$/).test(str);
}

/**
 * Tokenize input string.
 */
export function lexer(str: string, lenient: boolean = false): LexToken[] {
  const tokens: LexToken[] = [];
  let i = 0;

  while (i < str.length) {
    const char = str[i];

    const ErrorOrInvalid = function (msg: string) {
      if (!lenient) throw new TypeError(msg);
      tokens.push({ type: "INVALID_CHAR", index: i, value: str[i++] });
    };

    if (char === "*") {
      tokens.push({ type: "ASTERISK", index: i, value: str[i++] });
      continue;
    }

    if (char === "+" || char === "?") {
      tokens.push({ type: "OTHER_MODIFIER", index: i, value: str[i++] });
      continue;
    }

    if (char === "\\") {
      tokens.push({ type: "ESCAPED_CHAR", index: i++, value: str[i++] });
      continue;
    }

    if (char === "{") {
      tokens.push({ type: "OPEN", index: i, value: str[i++] });
      continue;
    }

    if (char === "}") {
      tokens.push({ type: "CLOSE", index: i, value: str[i++] });
      continue;
    }

    if (char === ":") {
      let name = "";
      let j = i + 1;

      while (j < str.length) {
        const code = str.substr(j, 1);

        if (
          (j === i + 1 && regexIdentifierStart.test(code)) ||
          (j !== i + 1 && regexIdentifierPart.test(code))
        ) {
          name += str[j++];
          continue;
        }

        break;
      }

      if (!name) {
        ErrorOrInvalid(`Missing parameter name at ${i}`);
        continue;
      }

      tokens.push({ type: "NAME", index: i, value: name });
      i = j;
      continue;
    }

    if (char === "(") {
      let count = 1;
      let pattern = "";
      let j = i + 1;
      let error = false;

      if (str[j] === "?") {
        ErrorOrInvalid(`Pattern cannot start with "?" at ${j}`);
        continue;
      }

      while (j < str.length) {
        if (!isASCII(str[j], false)) {
          ErrorOrInvalid(`Invalid character '${str[j]}' at ${j}.`);
          error = true;
          break;
        }

        if (str[j] === "\\") {
          pattern += str[j++] + str[j++];
          continue;
        }

        if (str[j] === ")") {
          count--;
          if (count === 0) {
            j++;
            break;
          }
        } else if (str[j] === "(") {
          count++;
          if (str[j + 1] !== "?") {
            ErrorOrInvalid(`Capturing groups are not allowed at ${j}`);
            error = true;
            break;
          }
        }

        pattern += str[j++];
      }

      if (error) {
        continue;
      }

      if (count) {
        ErrorOrInvalid(`Unbalanced pattern at ${i}`);
        continue;
      }
      if (!pattern) {
        ErrorOrInvalid(`Missing pattern at ${i}`);
        continue;
      }

      tokens.push({ type: "REGEX", index: i, value: pattern });
      i = j;
      continue;
    }

    tokens.push({ type: "CHAR", index: i, value: str[i++] });
  }

  tokens.push({ type: "END", index: i, value: "" });

  return tokens;
}

/**
 * Callback type that is invoked for every plain text part of the pattern.
 * This is intended to be used to apply URL canonicalization to the pattern
 * itself.  This is different from the encode callback used to encode group
 * values passed to compile, match, etc.
 */
type EncodePartCallback = (value: string) => string;

export interface ParseOptions {
  /**
   * Set the default delimiter for repeat parameters. (default: `'/'`)
   */
  delimiter?: string;
  /**
   * List of characters to automatically consider prefixes when parsing.
   */
  prefixes?: string;

  /**
   * Encoding callback to apply to each plaintext part of the pattern.
   */
  encodePart?: EncodePartCallback;
}

/**
 * Parse a string for the raw tokens.
 */
export function parse(str: string, options: ParseOptions = {}): Part[] {
  const tokens = lexer(str);

  options.delimiter ??=  "/#?";
  options.prefixes ??= "./";

  const segmentWildcardRegex = `[^${escapeString(options.delimiter)}]+?`;
  const result: Part[] = [];
  let key = 0;
  let i = 0;
  let path = "";
  let nameSet = new Set();

  const tryConsume = (type: LexToken["type"]): string | undefined => {
    if (i < tokens.length && tokens[i].type === type) return tokens[i++].value;
  };

  const tryConsumeModifier = (): string | undefined => {
    return tryConsume("OTHER_MODIFIER") ?? tryConsume("ASTERISK");
  };

  const mustConsume = (type: LexToken["type"]): string => {
    const value = tryConsume(type);
    if (value !== undefined) return value;
    const { type: nextType, index } = tokens[i];
    throw new TypeError(`Unexpected ${nextType} at ${index}, expected ${type}`);
  };

  const consumeText = (): string => {
    let result = "";
    let value: string | undefined;
    // tslint:disable-next-line
    while ((value = tryConsume("CHAR") ?? tryConsume("ESCAPED_CHAR"))) {
      result += value;
    }
    return result;
  };

  const DefaultEncodePart = (value: string): string => {
    return value;
  };
  const encodePart = options.encodePart || DefaultEncodePart;

  let pendingFixedValue: string = '';
  const appendToPendingFixedValue = (value: string) => {
    pendingFixedValue += value;
  }

  const maybeAddPartFromPendingFixedValue = () => {
    if (!pendingFixedValue.length) {
      return;
    }

    result.push(new Part(PartType.kFixed, "", "", encodePart(pendingFixedValue), "", Modifier.kNone));
    pendingFixedValue = '';
  }

  const addPart = (prefix: string, nameToken: string, regexOrWildcardToken: string, suffix: string, modifierToken: string) => {
    let modifier = Modifier.kNone;
    switch (modifierToken) {
      case '?':
        modifier = Modifier.kOptional;
        break;
      case '*':
        modifier = Modifier.kZeroOrMore;
        break;
      case '+':
        modifier = Modifier.kOneOrMore;
        break;
    }

    // If this is a `{ ... }` grouping containing only fixed text, then
    // just add it to our pending value for now.  We want to collect as
    // much fixed text as possible in the buffer before commiting it to
    // a fixed part.
    if (!nameToken && !regexOrWildcardToken && modifier === Modifier.kNone) {
      appendToPendingFixedValue(prefix);
      return;
    }

    // We are about to add some kind of matching group Part to the list.
    // Before doing that make sure to flush any pending fixed test to a
    // kFixed Part.
    maybeAddPartFromPendingFixedValue();

    // If there is no name, regex, or wildcard tokens then this is just a fixed
    // string grouping; e.g. "{foo}?".  The fixed string ends up in the prefix
    // value since it consumed the entire text of the grouping.  If the prefix
    // value is empty then its an empty "{}" group and we return without adding
    // any Part.
    if (!nameToken && !regexOrWildcardToken) {
      if (!prefix) {
        return;
      }

      result.push(new Part(PartType.kFixed, "", "", encodePart(prefix), "", modifier));
      return;
    }

    // Determine the regex value.  If there is a |kRegex| Token, then this is
    // explicitly set by that Token.  If there is a wildcard token, then this
    // is set to the |kFullWildcardRegex| constant.  Otherwise a kName Token by
    // itself gets an implicit regex value that matches through to the end of
    // the segment. This is represented by the |regexOrWildcardToken| value.
    let regexValue;
    if (!regexOrWildcardToken) {
      regexValue = segmentWildcardRegex;
    } else if (regexOrWildcardToken === '*') {
      regexValue = kFullWildcardRegex;
    } else {
      regexValue = regexOrWildcardToken;
    }

    // Next determine the type of the Part.  This depends on the regex value
    // since we give certain values special treatment with their own type.
    // A |segmentWildcardRegex| is mapped to the kSegmentWildcard type.  A
    // |kFullWildcardRegex| is mapped to the kFullWildcard type.  Otherwise
    // the Part gets the kRegex type.
    let type = PartType.kRegex;
    if (regexValue === segmentWildcardRegex) {
      type = PartType.kSegmentWildcard;
      regexValue = "";
    } else if (regexValue === kFullWildcardRegex) {
      type = PartType.kFullWildcard;
      regexValue = "";
    }

    // Every kRegex, kSegmentWildcard, and kFullWildcard Part must have a
    // group name.  If there was a kName Token, then use the explicitly
    // set name.  Otherwise we generate a numeric based key for the name.
    let name;
    if (nameToken) {
      name = nameToken;
    } else if (regexOrWildcardToken) {
      name = key++;
    }

    if (nameSet.has(name)) {
      throw new TypeError(`Duplicate name '${name}'.`);
    }
    nameSet.add(name);

    result.push(new Part(type, name, encodePart(prefix), regexValue, encodePart(suffix), modifier));
  }

  while (i < tokens.length) {
    // Look for the sequence: <prefix char><name><regex><modifier>
    // There could be from zero to all through of these tokens.  For
    // example:
    //  * "/:foo(bar)?" - all four tokens
    //  * "/" - just a char token
    //  * ":foo" - just a name token
    //  * "(bar)" - just a regex token
    //  * "/:foo" - char and name tokens
    //  * "/(bar)" - char and regex tokens
    //  * "/:foo?" - char, name, and modifier tokens
    //  * "/(bar)?" - char, regex, and modifier tokens
    const charToken = tryConsume("CHAR");
    const nameToken = tryConsume("NAME");
    let regexOrWildcardToken = tryConsume("REGEX");

    // If there is no name or regex token, then we may have a wildcard `*`
    // token in place of an unnamed regex token.  Each wildcard will be
    // treated as being equivalent to a "(.*)" regex token.  For example:
    //  * "/*" - equivalent to "/(.*)"
    //  * "/*?" - equivalent to "/(.*)?"
    if (!nameToken && !regexOrWildcardToken) {
      regexOrWildcardToken = tryConsume("ASTERISK");
    }

    // If there is a name, regex, or wildcard token then we need to add a
    // Pattern Part immediately.
    if (nameToken || regexOrWildcardToken) {
      // Determine if the char token is a valid prefix.  Only characters in the
      // configured prefix_list are automatically treated as prefixes.  A
      // kEscapedChar Token is never treated as a prefix.
      let prefix = charToken ?? "";
      if (options.prefixes.indexOf(prefix) === -1) {
        // This is not a prefix character.  Add it to the buffered characters
        // to be added as a kFixed Part later.
        appendToPendingFixedValue(prefix);
        prefix = "";
      }

      // If we have any buffered characters in a pending fixed value, then
      // convert them into a kFixed Part now.
      maybeAddPartFromPendingFixedValue();

      // kName and kRegex tokens can optionally be followed by a modifier.
      let modifierToken = tryConsumeModifier();

      // Add the Part for the name and regex/wildcard tokens.
      addPart(prefix, nameToken, regexOrWildcardToken, "", modifierToken);

      continue;
    }

    // There was neither a kRegex or kName token, so consider if we just have a
    // fixed string part.  A fixed string can consist of kChar or kEscapedChar
    // tokens.  These just get added to the buffered pending fixed value for
    // now. It will get converted to a kFixed Part later.
    const value = charToken ?? tryConsume("ESCAPED_CHAR");
    if (value) {
      appendToPendingFixedValue(value);
      continue;
    }

    // There was not a char or escaped char token, so we no we are at the end
    // of any fixed string.  Do not yet convert the pending fixed value into
    // a kFixedPart, though.  Its possible there will be further fixed text in
    // a `{ ... }` group, etc.
    // Look for the sequence:
    //
    //  <open><char prefix><name><regex><char suffix><close><modifier>
    //
    // The open and close are required, but the other tokens are optional.
    // For example:
    //  * "{a:foo(.*)b}?" - all tokens present
    //  * "{:foo}?" - just name and modifier tokens
    //  * "{(.*)}?" - just regex and modifier tokens
    //  * "{ab}?" - just char and modifier tokens
    const openToken = tryConsume("OPEN");
    if (openToken) {
      const prefix = consumeText();
      const nameToken = tryConsume("NAME");
      let regexOrWildcardToken = tryConsume("REGEX");

      // If there is no name or regex token, then we may have a wildcard `*`
      // token in place of an unnamed regex token.  Each wildcard will be
      // treated as being equivalent to a "(.*)" regex token.  For example,
      // "{a*b}" is equivalent to "{a(.*)b}".
      if (!nameToken && !regexOrWildcardToken) {
        regexOrWildcardToken = tryConsume("ASTERISK");
      }

      const suffix = consumeText();

      mustConsume("CLOSE");

      const modifierToken = tryConsumeModifier();

      addPart(prefix, nameToken, regexOrWildcardToken, suffix, modifierToken);
      continue;
    }

    // We are about to end the pattern string, so flush any pending text to
    // a kFixed Part.
    maybeAddPartFromPendingFixedValue();

    // We didn't find any tokens allowed by the syntax, so we should be
    // at the end of the token list.  If there is a syntax error, this
    // is where it will typically be caught.
    mustConsume("END");
  }

  return result;
}

/**
 * Escape a regular expression string.
 */
function escapeString(str: string) {
  return str.replace(/([.+*?^${}()[\]|/\\])/g, "\\$1");
}

/**
 * Get the flags for a regexp from the options.
 */
function flags(options?: { ignoreCase?: boolean }) {
  return options && options.ignoreCase ? "ui" : "u";
}

/**
 * Create a path regexp from string input.
 */
export function stringToRegexp(
  path: string,
  names?: string[],
  options?: Options & ParseOptions
) {
  return partsToRegexp(parse(path, options), names, options);
}

export interface Options {
  /**
   * When `true` the regexp will be case insensitive. (default: `false`)
   */
  ignoreCase?: boolean;
  /**
   * When `true` the regexp won't allow an optional trailing delimiter to match. (default: `false`)
   */
  strict?: boolean;
  /**
   * When `true` the regexp will match to the end of the string. (default: `true`)
   */
  end?: boolean;
  /**
   * When `true` the regexp will match from the beginning of the string. (default: `true`)
   */
  start?: boolean;
  /**
   * Sets the final character for non-ending optimistic matches. (default: `/`)
   */
  delimiter?: string;
  /**
   * List of characters that can also be "end" characters.
   */
  endsWith?: string;
  /**
   * Encode path tokens for use in the `RegExp`.
   */
  encode?: (value: string) => string;
}

export function modifierToString(modifier: Modifier) {
  switch (modifier) {
    case Modifier.kZeroOrMore:
      return '*';
    case Modifier.kOptional:
      return '?';
    case Modifier.kOneOrMore:
      return '+';
    case Modifier.kNone:
      return '';
  }
}

/**
 * Expose a function for taking tokens and returning a RegExp.
 */
export function partsToRegexp(
  parts: Part[],
  names?: string[],
  options: Options = {}
) {
  options.delimiter ??=  "/#?";
  options.prefixes ??= "./";
  options.sensitive ??= false;
  options.strict ??= false;
  options.end ??= true;
  options.start ??= true;
  options.endsWith = '';

  let result = options.start ? "^" : "";

  // Iterate over the parts and create our regexp string.
  for (const part of parts) {
    // Handle kFixed Parts.  If there is a modifier we must wrap the escaped
    // value in a non-capturing group.  Otherwise we just append the escaped
    // value.  For example:
    //
    //  <escaped-fixed-value>
    //
    // Or:
    //
    //  (?:<escaped-fixed-value>)<modifier>
    //
    if (part.type === PartType.kFixed) {
      if (part.modifier === Modifier.kNone) {
        result += escapeString(part.value);
      } else {
        result += `(?:${escapeString(part.value)})${modifierToString(part.modifier)}`;
      }
      continue;
    }

    // All remaining Part types must have a name.  Append it to the output
    // names if provided.
    if (names) names.push(part.name);

    const segmentWildcardRegex = `[^${escapeString(options.delimiter)}]+?`;

    // Compute the Part regex value.  For kSegmentWildcard and kFullWildcard
    // types we must convert the type enum back to the defined regex value.
    let regexValue = part.value;
    if (part.type === PartType.kSegmentWildcard)
      regexValue = segmentWildcardRegex;
    else if (part.type === PartType.kFullWildcard)
      regexValue = kFullWildcardRegex;

    // Handle the case where there is no prefix or suffix value.  This varies a
    // bit depending on the modifier.
    //
    // If there is no modifier or an optional modifier, then we simply wrap the
    // regex value in a capturing group:
    //
    //  (<regex-value>)<modifier>
    //
    // If there is a modifier, then we need to use a non-capturing group for the
    // regex value and an outer capturing group that includes the modifier as
    // well.  Like:
    //
    //  ((?:<regex-value>)<modifier>)
    if (!part.prefix.length && !part.suffix.length) {
      if (part.modifier === Modifier.kNone ||
          part.modifier === Modifier.kOptional) {
        result += `(${regexValue})${modifierToString(part.modifier)}`;
      } else {
        result += `((?:${regexValue})${modifierToString(part.modifier)})`;
      }
      continue;
    }

    // Handle non-repeating regex Parts with a prefix and/or suffix.  The
    // capturing group again only contains the regex value.  This inner group
    // is compined with the prefix and/or suffix in an outer non-capturing
    // group.  Finally the modifier is applied to the entire outer group.
    // For example:
    //
    //  (?:<prefix>(<regex-value>)<suffix>)<modifier>
    //
    if (part.modifier === Modifier.kNone ||
      part.modifier === Modifier.kOptional) {
      result += `(?:${escapeString(part.prefix)}(${regexValue})${escapeString(part.suffix)})`;
      result += modifierToString(part.modifier);
      continue;
    }

    // Repeating Parts are dramatically more complicated.  We want to exclude
    // the initial prefix and the final suffix, but include them between any
    // repeated elements.  To achieve this we provide a separate initial
    // part that excludes the prefix.  Then the part is duplicated with the
    // prefix/suffix values included in an optional repeating element.  If
    // zero values are permitted then a final optional modifier may be added.
    // For example:
    //
    //  (?:<prefix>((?:<regex-value>)(?:<suffix><prefix>(?:<regex-value>))*)<suffix>)?
    //
    result += `(?:${escapeString(part.prefix)}`;
    result += `((?:${regexValue})(?:`;
    result += escapeString(part.suffix);
    result += escapeString(part.prefix);
    result += `(?:${regexValue}))*)${escapeString(part.suffix)})`;
    if (part.modifier === Modifier.kZeroOrMore) {
      result += "?";
    }
  }

  const endsWith = `[${escapeString(options.endsWith)}]|$`;
  const delimiter = `[${escapeString(options.delimiter)}]`;

  // Should we anchor the pattern to the end of the input string?
  if (options.end) {
    // In non-strict mode an optional delimiter character is always
    // permitted at the end of the string.  For example, if the pattern
    // is "/foo/bar" then it would match "/foo/bar/".
    //
    //  [<delimiter chars>]?
    //
    if (!options.strict) {
      result += `${delimiter}?`;
    }

    // The options ends_with value contains a list of characters that
    // may also signal the end of the pattern match.
    if (!options.endsWith.length) {
      // Simply anchor to the end of the input string.
      result += "$";
    } else {
      // Anchor to either a ends_with character or the end of the input
      // string.  This uses a lookahead assertion.
      //
      //  (?=[<ends_with chars>]|$)
      //
      result += `(?=${endsWith})`;
    }
    return new RegExp(result, flags(options));
  }

  // We are not anchored to the end of the input string.
  // Again, if not in strict mode we permit an optional trailing delimiter
  // character before anchoring to any ends_with characters with a lookahead
  // assertion.
  //
  //  (?:[<delimiter chars>](?=[<ends_with chars>]|$))?
  if (!options.strict) {
    result += `(?:${delimiter}(?=${endsWith}))?`;
  }

  // Further, if the pattern does not end with a trailing delimiter character
  // we also anchor to a delimiter character in our lookahead assertion.  So
  // a pattern "/foo/bar" would match "/foo/bar/baz", but not "/foo/barbaz".
  //
  //  (?=[<delimiter chars>]|[<ends_with chars>]|$)
  let isEndDelimited = false;
  if (parts.length) {
    const lastPart = parts[parts.length - 1];
    if (lastPart.type === PartType.kFixed && lastPart.modifier === Modifier.kNone) {
      isEndDelimited = options.delimiter.indexOf(lastPart) > -1;
    }
  }

  if (!isEndDelimited) {
    result += `(?=${delimiter}|${endsWith})`;
  }

  return new RegExp(result, flags(options));
}
