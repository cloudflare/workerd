// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { CHAR_DOT, CHAR_FORWARD_SLASH } from 'node-internal:constants';

import { validateObject, validateString } from 'node-internal:validators';

function isPosixPathSeparator(code: number) {
  return code === CHAR_FORWARD_SLASH;
}

// Resolves . and .. elements in a path with directory names
function normalizeString(
  path: string,
  allowAboveRoot: boolean,
  separator: string,
  isPathSeparator: (code: number) => boolean
) {
  let res = '';
  let lastSegmentLength = 0;
  let lastSlash = -1;
  let dots = 0;
  let code = 0;
  for (let i = 0; i <= path.length; ++i) {
    if (i < path.length) code = path.charCodeAt(i);
    else if (isPathSeparator(code)) break;
    else code = CHAR_FORWARD_SLASH;

    if (isPathSeparator(code)) {
      if (lastSlash === i - 1 || dots === 1) {
        // NOOP
      } else if (dots === 2) {
        if (
          res.length < 2 ||
          lastSegmentLength !== 2 ||
          res.charCodeAt(res.length - 1) !== CHAR_DOT ||
          res.charCodeAt(res.length - 2) !== CHAR_DOT
        ) {
          if (res.length > 2) {
            const lastSlashIndex = res.lastIndexOf(separator);
            if (lastSlashIndex === -1) {
              res = '';
              lastSegmentLength = 0;
            } else {
              res = res.slice(0, lastSlashIndex);
              lastSegmentLength = res.length - 1 - res.lastIndexOf(separator);
            }
            lastSlash = i;
            dots = 0;
            continue;
          } else if (res.length !== 0) {
            res = '';
            lastSegmentLength = 0;
            lastSlash = i;
            dots = 0;
            continue;
          }
        }
        if (allowAboveRoot) {
          res += res.length > 0 ? `${separator}..` : '..';
          lastSegmentLength = 2;
        }
      } else {
        if (res.length > 0)
          res += `${separator}${path.slice(lastSlash + 1, i)}`;
        else res = path.slice(lastSlash + 1, i);
        lastSegmentLength = i - lastSlash - 1;
      }
      lastSlash = i;
      dots = 0;
    } else if (code === CHAR_DOT && dots !== -1) {
      ++dots;
    } else {
      dots = -1;
    }
  }
  return res;
}

function formatExt(ext: string) {
  return ext ? `${ext[0] === '.' ? '' : '.'}${ext}` : '';
}

/**
 * @param {string} sep
 * @param {{
 *  dir?: string;
 *  root?: string;
 *  base?: string;
 *  name?: string;
 *  ext?: string;
 *  }} pathObject
 * @returns {string}
 */

type PathObject = {
  dir?: string;
  root?: string;
  base?: string;
  name?: string;
  ext?: string;
};

function _format(sep: string, pathObject: PathObject) {
  validateObject(pathObject, 'pathObject', {});
  const dir = pathObject.dir || pathObject.root;
  const base =
    pathObject.base || `${pathObject.name || ''}${formatExt(pathObject.ext!)}`;
  if (!dir) {
    return base;
  }
  return dir === pathObject.root ? `${dir}${base}` : `${dir}${sep}${base}`;
}

// We currently do not implement the path.win32 subset.
const win32 = {
  resolve(..._: [string[], string]) {
    throw new Error('path.win32.resolve() is not implemented.');
  },

  normalize(_: string) {
    throw new Error('path.win32.normalize() is not implemented.');
  },

  isAbsolute(_: string) {
    throw new Error('path.win32.isAbsolute() is not implemented.');
  },

  join(..._: string[]) {
    throw new Error('path.win32.join() is not implemented.');
  },

  relative(_0: string, _1: string) {
    throw new Error('path.win32.relative() is not implemented.');
  },

  toNamespacedPath(_: string) {
    throw new Error('path.win32.toNamedspacedPath() is not implemented.');
  },

  dirname(_: string) {
    throw new Error('path.win32.dirname() is not implemented.');
  },

  basename(_0: string, _1?: string) {
    throw new Error('path.win32.basename() is not implemented.');
  },

  extname(_: string) {
    throw new Error('path.win32.extname() is not implemented.');
  },

  format(_: PathObject) {
    throw new Error('path.win32.format() is not implemented.');
  },

  parse(_: string) {
    throw new Error('path.win32.parse() is not implemented.');
  },

  matchesGlob(_path: string, _pattern: string): boolean {
    throw new Error('path.win32.matchesGlob() is not implemented.');
  },

  sep: '\\',
  delimiter: ';',
  win32: null as Object | null,
  posix: null as Object | null,
};

const posix = {
  /**
   * path.resolve([from ...], to)
   * @param {...string} args
   * @returns {string}
   */
  resolve(...args: string[]) {
    let resolvedPath = '';
    let resolvedAbsolute = false;

    for (let i = args.length - 1; i >= -1 && !resolvedAbsolute; i--) {
      const path = i >= 0 ? args[i] : '/';

      validateString(path, 'path');

      // Skip empty entries
      if (path!.length === 0) {
        continue;
      }

      resolvedPath = `${path}/${resolvedPath}`;
      resolvedAbsolute = path!.charCodeAt(0) === CHAR_FORWARD_SLASH;
    }

    // At this point the path should be resolved to a full absolute path, but
    // handle relative paths to be safe (might happen when process.cwd() fails)

    // Normalize the path
    resolvedPath = normalizeString(
      resolvedPath,
      !resolvedAbsolute,
      '/',
      isPosixPathSeparator
    );

    if (resolvedAbsolute) {
      return `/${resolvedPath}`;
    }
    return resolvedPath.length > 0 ? resolvedPath : '.';
  },

  /**
   * @param {string} path
   * @returns {string}
   */
  normalize(path: string) {
    validateString(path, 'path');

    if (path.length === 0) return '.';

    const isAbsolute = path.charCodeAt(0) === CHAR_FORWARD_SLASH;
    const trailingSeparator =
      path.charCodeAt(path.length - 1) === CHAR_FORWARD_SLASH;

    // Normalize the path
    path = normalizeString(path, !isAbsolute, '/', isPosixPathSeparator);

    if (path.length === 0) {
      if (isAbsolute) return '/';
      return trailingSeparator ? './' : '.';
    }
    if (trailingSeparator) path += '/';

    return isAbsolute ? `/${path}` : path;
  },

  /**
   * @param {string} path
   * @returns {boolean}
   */
  isAbsolute(path: string) {
    validateString(path, 'path');
    return path.length > 0 && path.charCodeAt(0) === CHAR_FORWARD_SLASH;
  },

  /**
   * @param {...string} args
   * @returns {string}
   */
  join(...args: string[]) {
    if (args.length === 0) return '.';
    let joined;
    for (let i = 0; i < args.length; ++i) {
      const arg = args[i];
      validateString(arg, 'path');
      if (arg!.length > 0) {
        if (joined === undefined) joined = arg;
        else joined += `/${arg}`;
      }
    }
    if (joined === undefined) return '.';
    return posix.normalize(joined);
  },

  /**
   * @param {string} from
   * @param {string} to
   * @returns {string}
   */
  relative(from: string, to: string) {
    validateString(from, 'from');
    validateString(to, 'to');

    if (from === to) return '';

    // Trim leading forward slashes.
    from = posix.resolve(from);
    to = posix.resolve(to);

    if (from === to) return '';

    const fromStart = 1;
    const fromEnd = from.length;
    const fromLen = fromEnd - fromStart;
    const toStart = 1;
    const toLen = to.length - toStart;

    // Compare paths to find the longest common path from root
    const length = fromLen < toLen ? fromLen : toLen;
    let lastCommonSep = -1;
    let i = 0;
    for (; i < length; i++) {
      const fromCode = from.charCodeAt(fromStart + i);
      if (fromCode !== to.charCodeAt(toStart + i)) break;
      else if (fromCode === CHAR_FORWARD_SLASH) lastCommonSep = i;
    }
    if (i === length) {
      if (toLen > length) {
        if (to.charCodeAt(toStart + i) === CHAR_FORWARD_SLASH) {
          // We get here if `from` is the exact base path for `to`.
          // For example: from='/foo/bar'; to='/foo/bar/baz'
          return to.slice(toStart + i + 1);
        }
        if (i === 0) {
          // We get here if `from` is the root
          // For example: from='/'; to='/foo'
          return to.slice(toStart + i);
        }
      } else if (fromLen > length) {
        if (from.charCodeAt(fromStart + i) === CHAR_FORWARD_SLASH) {
          // We get here if `to` is the exact base path for `from`.
          // For example: from='/foo/bar/baz'; to='/foo/bar'
          lastCommonSep = i;
        } else if (i === 0) {
          // We get here if `to` is the root.
          // For example: from='/foo/bar'; to='/'
          lastCommonSep = 0;
        }
      }
    }

    let out = '';
    // Generate the relative path based on the path difference between `to`
    // and `from`.
    for (i = fromStart + lastCommonSep + 1; i <= fromEnd; ++i) {
      if (i === fromEnd || from.charCodeAt(i) === CHAR_FORWARD_SLASH) {
        out += out.length === 0 ? '..' : '/..';
      }
    }

    // Lastly, append the rest of the destination (`to`) path that comes after
    // the common path parts.
    return `${out}${to.slice(toStart + lastCommonSep)}`;
  },

  /**
   * @param {string} path
   * @returns {string}
   */
  toNamespacedPath(path: string) {
    // Non-op on posix systems
    return path;
  },

  /**
   * @param {string} path
   * @returns {string}
   */
  dirname(path: string) {
    validateString(path, 'path');
    if (path.length === 0) return '.';
    const hasRoot = path.charCodeAt(0) === CHAR_FORWARD_SLASH;
    let end = -1;
    let matchedSlash = true;
    for (let i = path.length - 1; i >= 1; --i) {
      if (path.charCodeAt(i) === CHAR_FORWARD_SLASH) {
        if (!matchedSlash) {
          end = i;
          break;
        }
      } else {
        // We saw the first non-path separator
        matchedSlash = false;
      }
    }

    if (end === -1) return hasRoot ? '/' : '.';
    if (hasRoot && end === 1) return '//';
    return path.slice(0, end);
  },

  /**
   * @param {string} path
   * @param {string} [suffix]
   * @returns {string}
   */
  basename(path: string, suffix?: string) {
    if (suffix !== undefined) validateString(suffix, 'ext');
    validateString(path, 'path');

    let start = 0;
    let end = -1;
    let matchedSlash = true;

    if (
      suffix !== undefined &&
      suffix.length > 0 &&
      suffix.length <= path.length
    ) {
      if (suffix === path) return '';
      let extIdx = suffix.length - 1;
      let firstNonSlashEnd = -1;
      for (let i = path.length - 1; i >= 0; --i) {
        const code = path.charCodeAt(i);
        if (code === CHAR_FORWARD_SLASH) {
          // If we reached a path separator that was not part of a set of path
          // separators at the end of the string, stop now
          if (!matchedSlash) {
            start = i + 1;
            break;
          }
        } else {
          if (firstNonSlashEnd === -1) {
            // We saw the first non-path separator, remember this index in case
            // we need it if the extension ends up not matching
            matchedSlash = false;
            firstNonSlashEnd = i + 1;
          }
          if (extIdx >= 0) {
            // Try to match the explicit extension
            if (code === suffix.charCodeAt(extIdx)) {
              if (--extIdx === -1) {
                // We matched the extension, so mark this as the end of our path
                // component
                end = i;
              }
            } else {
              // Extension does not match, so our result is the entire path
              // component
              extIdx = -1;
              end = firstNonSlashEnd;
            }
          }
        }
      }

      if (start === end) end = firstNonSlashEnd;
      else if (end === -1) end = path.length;
      return path.slice(start, end);
    }
    for (let i = path.length - 1; i >= 0; --i) {
      if (path.charCodeAt(i) === CHAR_FORWARD_SLASH) {
        // If we reached a path separator that was not part of a set of path
        // separators at the end of the string, stop now
        if (!matchedSlash) {
          start = i + 1;
          break;
        }
      } else if (end === -1) {
        // We saw the first non-path separator, mark this as the end of our
        // path component
        matchedSlash = false;
        end = i + 1;
      }
    }

    if (end === -1) return '';
    return path.slice(start, end);
  },

  /**
   * @param {string} path
   * @returns {string}
   */
  extname(path: string) {
    validateString(path, 'path');
    let startDot = -1;
    let startPart = 0;
    let end = -1;
    let matchedSlash = true;
    // Track the state of characters (if any) we see before our first dot and
    // after any path separator we find
    let preDotState = 0;
    for (let i = path.length - 1; i >= 0; --i) {
      const code = path.charCodeAt(i);
      if (code === CHAR_FORWARD_SLASH) {
        // If we reached a path separator that was not part of a set of path
        // separators at the end of the string, stop now
        if (!matchedSlash) {
          startPart = i + 1;
          break;
        }
        continue;
      }
      if (end === -1) {
        // We saw the first non-path separator, mark this as the end of our
        // extension
        matchedSlash = false;
        end = i + 1;
      }
      if (code === CHAR_DOT) {
        // If this is our first dot, mark it as the start of our extension
        if (startDot === -1) startDot = i;
        else if (preDotState !== 1) preDotState = 1;
      } else if (startDot !== -1) {
        // We saw a non-dot and non-path separator before our dot, so we should
        // have a good chance at having a non-empty extension
        preDotState = -1;
      }
    }

    if (
      startDot === -1 ||
      end === -1 ||
      // We saw a non-dot character immediately before the dot
      preDotState === 0 ||
      // The (right-most) trimmed path component is exactly '..'
      (preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)
    ) {
      return '';
    }
    return path.slice(startDot, end);
  },

  format: _format.bind(null, '/'),

  /**
   * @param {string} path
   * @returns {{
   *   dir: string;
   *   root: string;
   *   base: string;
   *   name: string;
   *   ext: string;
   *   }}
   */
  parse(path: string): PathObject {
    validateString(path, 'path');

    const ret = { root: '', dir: '', base: '', ext: '', name: '' };
    if (path.length === 0) return ret;
    const isAbsolute = path.charCodeAt(0) === CHAR_FORWARD_SLASH;
    let start;
    if (isAbsolute) {
      ret.root = '/';
      start = 1;
    } else {
      start = 0;
    }
    let startDot = -1;
    let startPart = 0;
    let end = -1;
    let matchedSlash = true;
    let i = path.length - 1;

    // Track the state of characters (if any) we see before our first dot and
    // after any path separator we find
    let preDotState = 0;

    // Get non-dir info
    for (; i >= start; --i) {
      const code = path.charCodeAt(i);
      if (code === CHAR_FORWARD_SLASH) {
        // If we reached a path separator that was not part of a set of path
        // separators at the end of the string, stop now
        if (!matchedSlash) {
          startPart = i + 1;
          break;
        }
        continue;
      }
      if (end === -1) {
        // We saw the first non-path separator, mark this as the end of our
        // extension
        matchedSlash = false;
        end = i + 1;
      }
      if (code === CHAR_DOT) {
        // If this is our first dot, mark it as the start of our extension
        if (startDot === -1) startDot = i;
        else if (preDotState !== 1) preDotState = 1;
      } else if (startDot !== -1) {
        // We saw a non-dot and non-path separator before our dot, so we should
        // have a good chance at having a non-empty extension
        preDotState = -1;
      }
    }

    if (end !== -1) {
      const start = startPart === 0 && isAbsolute ? 1 : startPart;
      if (
        startDot === -1 ||
        // We saw a non-dot character immediately before the dot
        preDotState === 0 ||
        // The (right-most) trimmed path component is exactly '..'
        (preDotState === 1 &&
          startDot === end - 1 &&
          startDot === startPart + 1)
      ) {
        ret.base = ret.name = path.slice(start, end);
      } else {
        ret.name = path.slice(start, startDot);
        ret.base = path.slice(start, end);
        ret.ext = path.slice(startDot, end);
      }
    }

    if (startPart > 0) ret.dir = path.slice(0, startPart - 1);
    else if (isAbsolute) ret.dir = '/';

    return ret;
  },

  matchesGlob(_path: string, _pattern: string): boolean {
    throw new Error('path.posix.matchesGlob() is not implemented.');
  },

  sep: '/',
  delimiter: ':',
  win32: null as Object | null,
  posix: null as Object | null,
};

posix.win32 = win32.win32 = win32;
posix.posix = win32.posix = posix;

export default posix;
export { posix, win32 };
