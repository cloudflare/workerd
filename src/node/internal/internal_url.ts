// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  ERR_INVALID_FILE_URL_HOST,
  ERR_INVALID_FILE_URL_PATH,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INVALID_URL_SCHEME,
} from 'node-internal:internal_errors';
import { default as urlUtil } from 'node-internal:url';
import { CHAR_LOWERCASE_A, CHAR_LOWERCASE_Z } from 'node-internal:constants';
import {
  win32 as pathWin32,
  posix as pathPosix,
} from 'node-internal:internal_path';

const FORWARD_SLASH = /\//g;

// The following characters are percent-encoded when converting from file path
// to URL:
// - %: The percent character is the only character not encoded by the
//        `pathname` setter.
// - \: Backslash is encoded on non-windows platforms since it's a valid
//      character but the `pathname` setters replaces it by a forward slash.
// - LF: The newline character is stripped out by the `pathname` setter.
//       (See whatwg/url#419)
// - CR: The carriage return character is also stripped out by the `pathname`
//       setter.
// - TAB: The tab character is also stripped out by the `pathname` setter.
const percentRegEx = /%/g;
const backslashRegEx = /\\/g;
const newlineRegEx = /\n/g;
const carriageReturnRegEx = /\r/g;
const tabRegEx = /\t/g;
const questionRegex = /\?/g;
const hashRegex = /#/g;

function encodePathChars(
  filepath: string,
  options?: { windows?: boolean | undefined }
): string {
  const windows = options?.windows;
  if (filepath.indexOf('%') !== -1)
    filepath = filepath.replace(percentRegEx, '%25');
  // In posix, backslash is a valid character in paths:
  if (!windows && filepath.indexOf('\\') !== -1)
    filepath = filepath.replace(backslashRegEx, '%5C');
  if (filepath.indexOf('\n') !== -1)
    filepath = filepath.replace(newlineRegEx, '%0A');
  if (filepath.indexOf('\r') !== -1)
    filepath = filepath.replace(carriageReturnRegEx, '%0D');
  if (filepath.indexOf('\t') !== -1)
    filepath = filepath.replace(tabRegEx, '%09');
  return filepath;
}

/**
 * Checks if a value has the shape of a WHATWG URL object.
 *
 * Using a symbol or instanceof would not be able to recognize URL objects
 * coming from other implementations (e.g. in Electron), so instead we are
 * checking some well known properties for a lack of a better test.
 *
 * We use `href` and `protocol` as they are the only properties that are
 * easy to retrieve and calculate due to the lazy nature of the getters.
 *
 * We check for `auth` and `path` attribute to distinguish legacy url instance with
 * WHATWG URL instance.
 */
/* eslint-disable */
export function isURL(self?: any): self is URL {
  return Boolean(
    self?.href &&
      self.protocol &&
      self.auth === undefined &&
      self.path === undefined
  );
}
/* eslint-enable */

export function getPathFromURLPosix(url: URL): string {
  if (url.hostname !== '') {
    // Note: Difference between Node.js and Workerd.
    // Node.js uses `process.platform` whereas workerd hard codes it to linux.
    // This is done to avoid confusion regarding non-linux support and conformance.
    throw new ERR_INVALID_FILE_URL_HOST('linux');
  }
  const pathname = url.pathname;
  for (let n = 0; n < pathname.length; n++) {
    if (pathname[n] === '%') {
      // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
      const third = pathname.codePointAt(n + 2)! | 0x20;
      if (pathname[n + 1] === '2' && third === 102) {
        throw new ERR_INVALID_FILE_URL_PATH(
          'must not include encoded / characters'
        );
      }
    }
  }
  return decodeURIComponent(pathname);
}

export function getPathFromURLWin32(url: URL): string {
  const hostname = url.hostname;
  let pathname = url.pathname;
  for (let n = 0; n < pathname.length; n++) {
    if (pathname[n] === '%') {
      // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
      const third = pathname.codePointAt(n + 2)! | 0x20;
      if (
        (pathname[n + 1] === '2' && third === 102) || // 2f 2F /
        (pathname[n + 1] === '5' && third === 99)
      ) {
        // 5c 5C \
        throw new ERR_INVALID_FILE_URL_PATH(
          'must not include encoded \\ or / characters'
        );
      }
    }
  }
  pathname = pathname.replace(FORWARD_SLASH, '\\');
  pathname = decodeURIComponent(pathname);
  if (hostname !== '') {
    // If hostname is set, then we have a UNC path
    // Pass the hostname through domainToUnicode just in case
    // it is an IDN using punycode encoding. We do not need to worry
    // about percent encoding because the URL parser will have
    // already taken care of that for us. Note that this only
    // causes IDNs with an appropriate `xn--` prefix to be decoded.
    return `\\\\${urlUtil.domainToUnicode(hostname)}${pathname}`;
  }
  // Otherwise, it's a local path that requires a drive letter
  // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
  const letter = pathname.codePointAt(1)! | 0x20;
  const sep = pathname.charAt(2);
  if (
    letter < CHAR_LOWERCASE_A ||
    letter > CHAR_LOWERCASE_Z || // a..z A..Z
    sep !== ':'
  ) {
    throw new ERR_INVALID_FILE_URL_PATH('must be absolute');
  }
  return pathname.slice(1);
}

export function fileURLToPath(
  input: string | URL,
  options?: { windows?: boolean }
): string {
  const windows = options?.windows;
  let path: URL;
  if (typeof input === 'string') {
    path = new URL(input);
  } else if (!isURL(input)) {
    throw new ERR_INVALID_ARG_TYPE('path', ['string', 'URL'], input);
  } else {
    path = input;
  }
  if (path.protocol !== 'file:') {
    throw new ERR_INVALID_URL_SCHEME('file');
  }
  return windows ? getPathFromURLWin32(path) : getPathFromURLPosix(path);
}

export function pathToFileURL(
  filepath: string,
  options?: { windows?: boolean }
): URL {
  const windows = options?.windows;
  // IMPORTANT: Difference between Node.js and workerd.
  // The following check does not exist in Node.js due to primordial usage.
  if (typeof filepath !== 'string') {
    throw new ERR_INVALID_ARG_TYPE('filepath', 'string', filepath);
  }
  if (windows && filepath.startsWith('\\\\')) {
    const outURL = new URL('file://');
    // UNC path format: \\server\share\resource
    // Handle extended UNC path and standard UNC path
    // "\\?\UNC\" path prefix should be ignored.
    // Ref: https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
    const isExtendedUNC = filepath.startsWith('\\\\?\\UNC\\');
    const prefixLength = isExtendedUNC ? 8 : 2;
    const hostnameEndIndex = filepath.indexOf('\\', prefixLength);
    if (hostnameEndIndex === -1) {
      throw new ERR_INVALID_ARG_VALUE(
        'path',
        filepath,
        'Missing UNC resource path'
      );
    }
    if (hostnameEndIndex === 2) {
      throw new ERR_INVALID_ARG_VALUE('path', filepath, 'Empty UNC servername');
    }
    const hostname = filepath.slice(prefixLength, hostnameEndIndex);
    outURL.hostname = urlUtil.domainToASCII(hostname);
    outURL.pathname = encodePathChars(
      filepath.slice(hostnameEndIndex).replace(backslashRegEx, '/'),
      { windows }
    );
    return outURL;
  }
  let resolved = windows
    ? pathWin32.resolve(filepath)
    : pathPosix.resolve(filepath);

  // Call encodePathChars first to avoid encoding % again for ? and #.
  resolved = encodePathChars(resolved, { windows });

  // Question and hash character should be included in pathname.
  // Therefore, encoding is required to eliminate parsing them in different states.
  // This is done as an optimization to not creating a URL instance and
  // later triggering pathname setter, which impacts performance
  if (resolved.indexOf('?') !== -1)
    resolved = resolved.replace(questionRegex, '%3F');
  if (resolved.indexOf('#') !== -1)
    resolved = resolved.replace(hashRegex, '%23');
  return new URL(`file://${resolved}`);
}

export function toPathIfFileURL(fileURLOrPath: URL | string): string {
  if (!isURL(fileURLOrPath)) return fileURLOrPath;
  return fileURLToPath(fileURLOrPath);
}
