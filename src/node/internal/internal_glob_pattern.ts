// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Glob pattern parsing shared by fs.glob() and path.matchesGlob(), plus the
// matcher behind path.matchesGlob(). It imports nothing, so node:path can use
// it without loading node:fs.

// ============================================================================
// Brace Expansion
// ============================================================================

// Splits a string by top-level occurrences of a separator character,
// respecting nested braces/parens and backslash escapes.
function splitTopLevel(str: string, sep: string): string[] {
  const parts: string[] = [];
  let depth = 0;
  let current = '';

  for (let i = 0; i < str.length; i++) {
    const c = str.charAt(i);
    if (c === '\\' && i + 1 < str.length) {
      current += c + str.charAt(i + 1);
      i++;
    } else if (c === '{' || c === '(') {
      depth++;
      current += c;
    } else if (c === '}' || c === ')') {
      depth--;
      current += c;
    } else if (c === sep && depth === 0) {
      parts.push(current);
      current = '';
    } else {
      current += c;
    }
  }

  parts.push(current);
  return parts;
}

// Expands brace expressions in a glob pattern into multiple patterns.
//
// With minimatch set, expandBraces() and segmentToRegexStr() also accept the
// syntax minimatch, and so path.matchesGlob(), does beyond fs.glob(): {a..b}
// sequences, a [ with no closing ] as a literal, and !(...) negating the rest
// of the segment too.
export function expandBraces(pattern: string, minimatch = false): string[] {
  let depth = 0;
  let braceStart = -1;

  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '\\' && i + 1 < pattern.length) {
      i++;
      continue;
    }
    if (c === '{') {
      if (depth === 0) braceStart = i;
      depth++;
    } else if (c === '}') {
      depth--;
      if (depth === 0 && braceStart !== -1) {
        const prefix = pattern.slice(0, braceStart);
        const body = pattern.slice(braceStart + 1, i);
        const suffix = pattern.slice(i + 1);

        let alternatives = splitTopLevel(body, ',');

        if (alternatives.length === 1) {
          if (!minimatch) return [pattern];
          const sequence = expandSequence(body);
          if (sequence === undefined) {
            braceStart = -1;
            continue;
          }
          alternatives = sequence;
        }

        const results: string[] = [];
        for (const alt of alternatives) {
          for (const expanded of expandBraces(
            prefix + alt + suffix,
            minimatch
          )) {
            results.push(expanded);
            // Like brace-expansion, stop at kMaxExpansions patterns.
            if (minimatch && results.length >= kMaxExpansions) return results;
          }
        }
        return results;
      }
    }
  }

  return [pattern];
}

const kNumericSequence = /^(-?\d+)\.\.(-?\d+)(?:\.\.(-?\d+))?$/;
const kAlphaSequence = /^([a-zA-Z])\.\.([a-zA-Z])(?:\.\.(-?\d+))?$/;
const kMaxExpansions = 100_000;

// Expands {1..3}, {01..10}, {a..e} and {1..9..2} like brace-expansion does.
function expandSequence(body: string): string[] | undefined {
  const numeric = kNumericSequence.exec(body);
  const match = numeric ?? kAlphaSequence.exec(body);
  if (match === null) return undefined;
  const [, start = '', end = '', step] = match;
  const x = numeric ? Number(start) : start.charCodeAt(0);
  const y = numeric ? Number(end) : end.charCodeAt(0);
  const incr = Math.max(Math.abs(Number(step ?? 1)), 1) * (y < x ? -1 : 1);
  const width =
    numeric && (/^-?0\d/.test(start) || /^-?0\d/.test(end))
      ? Math.max(start.length, end.length)
      : 0;
  const out: string[] = [];
  for (
    let i = x;
    (incr > 0 ? i <= y : i >= y) && out.length < kMaxExpansions;
    i += incr
  ) {
    if (!numeric) {
      const c = String.fromCharCode(i);
      out.push(c === '\\' ? '' : c);
    } else if (i < 0) {
      out.push('-' + String(-i).padStart(width - 1, '0'));
    } else {
      out.push(String(i).padStart(width, '0'));
    }
  }
  return out;
}

// ============================================================================
// Segment-Level Regex (with extglob support)
// ============================================================================

// Converts a single path segment pattern to a RegExp.
// Supports: *, ?, [...], [!...], @(...), *(...), +(...), ?(...), !(...)
export function segmentToRegex(segment: string): RegExp {
  const regex = segmentToRegexStr(segment);
  return new RegExp('^' + regex + '$');
}

export function segmentToRegexStr(segment: string, minimatch = false): string {
  let regex = '';
  let i = 0;
  let inCharClass = false;

  while (i < segment.length) {
    const c = segment[i] ?? '';

    // Handle escape sequences
    if (c === '\\' && i + 1 < segment.length) {
      regex += '\\' + escapeRegexChar(segment[i + 1] ?? '');
      i += 2;
      continue;
    }

    // Inside character classes, most chars are literal
    if (inCharClass) {
      if (c === ']') {
        regex += ']';
        inCharClass = false;
      } else {
        regex += c;
      }
      i++;
      continue;
    }

    // Check for extglob: @(...), *(...), +(...), ?(...), !(...)
    if (
      (c === '@' || c === '*' || c === '+' || c === '?' || c === '!') &&
      i + 1 < segment.length &&
      segment[i + 1] === '('
    ) {
      const closeIdx = findMatchingParen(segment, i + 1);
      if (closeIdx !== -1) {
        const inner = segment.slice(i + 2, closeIdx);
        // Convert pipe-separated alternatives, each may contain glob chars
        const alts = splitTopLevel(inner, '|');
        const altRegexes = alts.map((a) => segmentToRegexStr(a, minimatch));
        const group = altRegexes.join('|');

        switch (c) {
          case '@': // exactly one
            regex += '(?:' + group + ')';
            break;
          case '*': // zero or more
            regex += '(?:' + group + ')*';
            break;
          case '+': // one or more
            regex += '(?:' + group + ')+';
            break;
          case '?': // zero or one
            regex += '(?:' + group + ')?';
            break;
          case '!': // none of (negative lookahead)
            if (minimatch) {
              const rest = segmentToRegexStr(
                segment.slice(closeIdx + 1),
                minimatch
              );
              regex += '(?!(?:' + group + ')' + rest + '$)[^/]*?';
            } else {
              regex += '(?!(?:' + group + ')$)[^/]*';
            }
            break;
        }

        i = closeIdx + 1;
        continue;
      }
    }

    // minimatch reads a ] right after [ or [! as a literal, and a [ with no
    // closing ] as a literal.
    if (c === '[' && minimatch) {
      let start = i + 1;
      const negate = segment[start] === '!' || segment[start] === '^';
      if (negate) start++;
      const close = segment.indexOf(']', start + 1);
      if (close === -1) {
        regex += '\\[';
        i++;
      } else {
        // minimatch drops reversed ranges like z-a, and a class left
        // empty matches nothing.
        const body = segment
          .slice(start, close)
          .replace(/(.)-(.)/g, (range, from: string, to: string) =>
            from > to ? '' : range
          )
          .replace(/[[\]^\\]/g, '\\$&');
        if (body === '') regex += '(?!)';
        else regex += (negate ? '[^' : '[') + body + ']';
        i = close + 1;
      }
      continue;
    }

    switch (c) {
      case '[':
        inCharClass = true;
        regex += '[';
        if (i + 1 < segment.length && segment[i + 1] === '!') {
          regex += '^';
          i++;
        }
        break;

      case '*':
        regex += '[^/]*';
        break;

      case '?':
        regex += '[^/]';
        break;

      case '.':
      case '+':
      case '^':
      case '$':
      case '|':
      case '(':
      case ')':
      case '{':
      case '}':
        regex += '\\' + c;
        break;

      default:
        regex += c;
        break;
    }
    i++;
  }

  return regex;
}

function findMatchingParen(str: string, openIdx: number): number {
  let depth = 0;
  for (let i = openIdx; i < str.length; i++) {
    if (str[i] === '\\' && i + 1 < str.length) {
      i++;
      continue;
    }
    if (str[i] === '(') depth++;
    else if (str[i] === ')') {
      depth--;
      if (depth === 0) return i;
    }
  }
  return -1;
}

function escapeRegexChar(c: string): string {
  if ('.+*?^$|()[]{}\\'.includes(c)) {
    return '\\' + c;
  }
  return c;
}

// ============================================================================
// path.matchesGlob()
// ============================================================================

// Matches the way minimatch does with the options Node.js's matchGlobPattern()
// passes: windowsPathsNoEscape, nonegate, nocomment and optimizationLevel 2.
// nocase is false because Workers report process.platform as 'linux'.
//
// Unlike minimatch, it doesn't read POSIX classes like [[:alpha:]], keeps
// empty extglob alternatives like @(a|), and doesn't rewrite **/.. into two
// patterns.

const kGlobStar = Symbol('globstar');
type GlobPart = string | RegExp | typeof kGlobStar;

const kGlobMagic = /[*?[]|[@+!]\(/;
const kDrive = /^[a-z]:$/i;
// A part that is just * must match at least one character.
const kStar = /^(?!\.)[^/]+$/;

// Splits on runs of /, then drops . and empty parts and resolves <part>/..,
// as minimatch does to both the path and the pattern. Only in a path does a
// Windows drive stop .. from resolving.
function splitPath(path: string, windows: boolean, isPath: boolean): string[] {
  const parts = path.split(/\/+/);
  if (windows && /^\/\/[^/]+/.test(path)) parts.unshift('');
  let changed: boolean;
  do {
    changed = false;
    for (let i = 1; i < parts.length - 1; i++) {
      const p = parts[i];
      if (i === 1 && p === '' && parts[0] === '') continue;
      if (p === '.' || p === '') {
        parts.splice(i--, 1);
        changed = true;
      }
    }
    if (
      parts[0] === '.' &&
      parts.length === 2 &&
      (parts[1] === '.' || parts[1] === '')
    ) {
      parts.pop();
      changed = true;
    }
    for (let i = parts.indexOf('..', 1); i !== -1;) {
      const p = parts[i - 1];
      if (
        p &&
        p !== '.' &&
        p !== '..' &&
        p !== '**' &&
        !(windows && isPath && kDrive.test(p))
      ) {
        // minimatch keeps a . when this turns a pattern into a leading **.
        const keepDot = !isPath && i === 1 && parts[2] === '**';
        parts.splice(i - 1, 2, ...(keepDot ? ['.'] : []));
        i -= 2;
        changed = true;
      }
      i = parts.indexOf('..', i + 1);
    }
  } while (changed);
  return parts.length === 0 ? [''] : parts;
}

function compilePart(part: string): GlobPart {
  if (part === '**') return kGlobStar;
  if (part === '*') return kStar;
  if (!kGlobMagic.test(part)) return part;
  // Magic never matches a leading dot unless the part starts with one, and
  // never matches . or .. at all.
  const dot = part.startsWith('.') ? '(?!\\.\\.?$)' : '(?!\\.)';
  return new RegExp('^' + dot + segmentToRegexStr(part, true) + '$');
}

function matchParts(
  file: string[],
  fi: number,
  pattern: GlobPart[],
  pi: number
): boolean {
  for (; pi < pattern.length; fi++, pi++) {
    const p = pattern[pi];
    if (p === kGlobStar) {
      // A trailing ** needs at least one part; neither form crosses a dot part.
      if (pi === pattern.length - 1) {
        if (fi >= file.length) return false;
        for (; fi < file.length; fi++) {
          if (file[fi]?.startsWith('.') === true) return false;
        }
        return true;
      }
      for (let k = fi; k <= file.length; k++) {
        if (matchParts(file, k, pattern, pi + 1)) return true;
        if (file[k]?.startsWith('.') === true) return false;
      }
      return false;
    }
    const f = file[fi];
    if (f === undefined || p === undefined) return false;
    if (typeof p === 'string' ? p !== f : !p.test(f)) return false;
  }
  return fi === file.length || (fi === file.length - 1 && file[fi] === '');
}

export class GlobMatcher {
  readonly #windows: boolean;
  readonly #empty: boolean;
  readonly #set: GlobPart[][];

  constructor(pattern: string, windows: boolean) {
    pattern = pattern.replaceAll('\\', '/');
    this.#windows = windows;
    this.#empty = pattern === '';
    // Like brace-expansion, drop the empty patterns that {a,} expands to.
    const patterns = new Set(expandBraces(pattern, true));
    patterns.delete('');
    this.#set = [...patterns].map((p) =>
      splitPath(p, windows, false)
        .filter((part, i, parts) => part !== '**' || parts[i - 1] !== '**')
        .map(compilePart)
    );
  }

  match(path: string): boolean {
    if (this.#empty) return path === '';
    if (this.#windows) path = path.replaceAll('\\', '/');
    const file = splitPath(path, this.#windows, true);
    // A Windows drive letter at the start of the path, before .. resolves,
    // matches in either case.
    const drive =
      this.#windows && /^[a-z]:(?:\/|$)/i.test(path) ? path.slice(0, 2) : '';
    return this.#set.some((pattern) => {
      const p = pattern[0];
      if (
        drive &&
        typeof p === 'string' &&
        p.toLowerCase() === drive.toLowerCase()
      ) {
        pattern = [drive, ...pattern.slice(1)];
      }
      return matchParts(file, 0, pattern, 0);
    });
  }
}
