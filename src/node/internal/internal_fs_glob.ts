// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as cffs } from 'cloudflare-internal:filesystem';
import type { DirEntryHandle } from 'cloudflare-internal:filesystem';
import { normalizePath } from 'node-internal:internal_fs_utils';

// UV_DIRENT_DIR constant from internal_fs_constants
const UV_DIRENT_DIR = 2;

// Maximum recursion depth to prevent stack overflow on deeply nested VFS
const MAX_WALK_DEPTH = 256;

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
export function expandBraces(pattern: string): string[] {
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

        const alternatives = splitTopLevel(body, ',');

        if (alternatives.length === 1) {
          return [pattern];
        }

        const results: string[] = [];
        for (const alt of alternatives) {
          for (const expanded of expandBraces(prefix + alt + suffix)) {
            results.push(expanded);
          }
        }
        return results;
      }
    }
  }

  return [pattern];
}

// ============================================================================
// Pattern Normalization
// ============================================================================

export function normalizePattern(pattern: string): string {
  // Strip leading ./
  if (pattern.startsWith('./')) {
    pattern = pattern.slice(2);
  }
  // Collapse multiple slashes to single
  pattern = pattern.replace(/\/\/+/g, '/');
  // Strip trailing slash
  if (pattern.endsWith('/') && pattern.length > 1) {
    pattern = pattern.slice(0, -1);
  }
  return pattern;
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

function segmentToRegexStr(segment: string): string {
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
        const altRegexes = alts.map((a) => segmentToRegexStr(a));
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
            regex += '(?!(?:' + group + ')$)[^/]*';
            break;
        }

        i = closeIdx + 1;
        continue;
      }
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
// Full-Path Regex (used for exclude pattern matching)
// ============================================================================

// Converts a full glob pattern (with /) to a single RegExp for exclude matching.
export function globToRegex(pattern: string): RegExp {
  const normalized = normalizePattern(pattern);
  const segments = normalized.split('/').filter((s) => s !== '');
  const parts: string[] = [];

  for (const seg of segments) {
    if (seg === '**') {
      // ** matches zero or more path segments
      // We handle this by inserting a special marker
      parts.push('**');
    } else if (seg === '.') {
      // skip
    } else if (seg === '..') {
      parts.pop();
    } else {
      parts.push(segmentToRegexStr(seg));
    }
  }

  // Now build regex from parts, handling ** markers
  let regex = '';
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i] ?? '';
    const prevPart = i > 0 ? (parts[i - 1] ?? '') : '';
    if (part === '**') {
      if (parts.length === 1) {
        // ** alone: match everything
        regex = '.*';
      } else if (i === 0) {
        // ** at start: match zero or more leading segments
        regex += '(?:.*\\/)?';
      } else if (i === parts.length - 1) {
        // ** at end: match zero or more trailing segments
        regex += '(?:\\/.*)?';
      } else {
        // ** in middle: match zero or more middle segments
        regex += '(?:\\/[^/]+)*(?:\\/)?';
      }
    } else {
      if (i > 0 && prevPart !== '**') {
        regex += '\\/';
      }
      regex += part;
    }
  }

  return new RegExp('^' + regex + '$');
}

// ============================================================================
// Exclude Compiler
// ============================================================================

// Compiles an array of glob patterns into an exclude function.
// Only accepts string arrays — user-provided functions are handled
// separately in globSync to preserve their (string | Dirent) signature.
export function compileExcludePatterns(
  exclude: readonly string[]
): (path: string) => boolean {
  const regexes: RegExp[] = [];
  for (const pat of exclude) {
    for (const expanded of expandBraces(pat)) {
      regexes.push(globToRegex(expanded));
    }
  }

  return (path: string): boolean => {
    for (const re of regexes) {
      if (re.test(path)) return true;
    }
    return false;
  };
}

// ============================================================================
// Directory Entry Cache
// ============================================================================

type EntryCache = Map<string, DirEntryHandle[]>;

function getDirectoryEntries(
  absPath: string,
  cache: EntryCache
): DirEntryHandle[] {
  const cached = cache.get(absPath);
  if (cached !== undefined) return cached;

  try {
    const entries = cffs.readdir(normalizePath(absPath), { recursive: false });
    cache.set(absPath, entries);
    return entries;
  } catch {
    const empty: DirEntryHandle[] = [];
    cache.set(absPath, empty);
    return empty;
  }
}

function isDirectory(entry: DirEntryHandle): boolean {
  return entry.type === UV_DIRENT_DIR;
}

// ============================================================================
// Pattern-Driven Directory Walk
// ============================================================================

export interface GlobResult {
  relativePath: string;
  handle: DirEntryHandle | null;
}

// Collapses consecutive '**' segments to prevent exponential blowup.
export function collapseGlobstars(segments: string[]): string[] {
  const result: string[] = [];
  for (const seg of segments) {
    if (seg === '**' && result[result.length - 1] === '**') continue;
    result.push(seg);
  }
  return result;
}

// Precompiles segment regexes for all non-special segments.
export function precompileSegmentRegexes(
  segments: string[]
): (RegExp | null)[] {
  return segments.map((seg) => {
    if (seg === '**' || seg === '.' || seg === '..') return null;
    return segmentToRegex(seg);
  });
}

export function walkGlob(
  cwd: string,
  segments: string[],
  segIdx: number,
  currentAbsPath: string,
  relativePath: string,
  results: Map<string, GlobResult>,
  cache: EntryCache,
  segmentRegexes: (RegExp | null)[],
  visitedGlobstar?: Set<string>,
  depth: number = 0
): void {
  // Guard against excessive recursion depth
  if (depth >= MAX_WALK_DEPTH) return;

  // All segments consumed: this path is a match
  if (segIdx >= segments.length) {
    if (!results.has(relativePath)) {
      // Resolve handle for the matched path by reading the parent directory
      const lastSlash = currentAbsPath.lastIndexOf('/');
      const parentDir =
        lastSlash > 0 ? currentAbsPath.slice(0, lastSlash) : currentAbsPath;
      const basename = relativePath.split('/').pop() ?? '';
      const entries = getDirectoryEntries(parentDir, cache);
      const handle = entries.find((e) => e.name === basename) ?? null;
      results.set(relativePath, { relativePath, handle });
    }
    return;
  }

  const seg = segments[segIdx] ?? '';

  // Handle '.' — stay in current directory
  if (seg === '.') {
    walkGlob(
      cwd,
      segments,
      segIdx + 1,
      currentAbsPath,
      relativePath,
      results,
      cache,
      segmentRegexes,
      visitedGlobstar,
      depth
    );
    return;
  }

  // Handle '..' — go up one directory (but don't escape cwd)
  if (seg === '..') {
    if (currentAbsPath === cwd || !currentAbsPath.startsWith(cwd + '/')) {
      return;
    }

    const lastSlash = currentAbsPath.lastIndexOf('/');
    const newAbs = lastSlash > 0 ? currentAbsPath.slice(0, lastSlash) : '/';

    const relParts = relativePath.split('/').filter(Boolean);
    relParts.pop();
    const newRel = relParts.join('/');

    walkGlob(
      cwd,
      segments,
      segIdx + 1,
      newAbs,
      newRel,
      results,
      cache,
      segmentRegexes,
      visitedGlobstar,
      depth
    );
    return;
  }

  // Handle '**' — match zero or more directory levels
  if (seg === '**') {
    const gsKey = `${currentAbsPath}:${String(segIdx)}`;
    if (visitedGlobstar === undefined) {
      visitedGlobstar = new Set();
    }
    if (visitedGlobstar.has(gsKey)) return;
    visitedGlobstar.add(gsKey);

    // Zero levels: advance to next segment at current path
    walkGlob(
      cwd,
      segments,
      segIdx + 1,
      currentAbsPath,
      relativePath,
      results,
      cache,
      segmentRegexes,
      visitedGlobstar,
      depth
    );

    // One or more levels: enumerate children
    const entries = getDirectoryEntries(currentAbsPath, cache);
    for (const entry of entries) {
      const childAbs = currentAbsPath + '/' + entry.name;
      const childRel = relativePath
        ? relativePath + '/' + entry.name
        : entry.name;

      // Try matching next segment against this child
      walkGlob(
        cwd,
        segments,
        segIdx + 1,
        childAbs,
        childRel,
        results,
        cache,
        segmentRegexes,
        visitedGlobstar,
        depth + 1
      );

      // If directory, recurse ** deeper
      if (isDirectory(entry)) {
        walkGlob(
          cwd,
          segments,
          segIdx,
          childAbs,
          childRel,
          results,
          cache,
          segmentRegexes,
          visitedGlobstar,
          depth + 1
        );
      }
    }
    return;
  }

  // Regular segment: match against precompiled regex
  const segRegex = segmentRegexes[segIdx] ?? segmentToRegex(seg);
  const entries = getDirectoryEntries(currentAbsPath, cache);

  for (const entry of entries) {
    if (segRegex.test(entry.name)) {
      const childAbs = currentAbsPath + '/' + entry.name;
      const childRel = relativePath
        ? relativePath + '/' + entry.name
        : entry.name;

      if (segIdx + 1 >= segments.length) {
        // This is the last segment — record match
        if (!results.has(childRel)) {
          results.set(childRel, { relativePath: childRel, handle: entry });
        }
      } else {
        // More segments to match — recurse
        walkGlob(
          cwd,
          segments,
          segIdx + 1,
          childAbs,
          childRel,
          results,
          cache,
          segmentRegexes,
          visitedGlobstar,
          depth + 1
        );
      }
    }
  }
}
