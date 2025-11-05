import { throws, ok, strictEqual, deepStrictEqual } from 'node:assert';
import { validateHeaderName, validateHeaderValue, METHODS } from 'node:http';
import httpCommon from 'node:_http_common';
import { inspect } from 'node:util';
import http from 'node:http';

export const checkPortsSetCorrectly = {
  test(ctrl, env, ctx) {
    const keys = [
      'SIDECAR_HOSTNAME',
      'PONG_SERVER_PORT',
      'ASD_SERVER_PORT',
      'TIMEOUT_SERVER_PORT',
      'HELLO_WORLD_SERVER_PORT',
      'HEADER_VALIDATION_SERVER_PORT',
    ];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-header-validators.js
export const testHttpHeaderValidators = {
  async test() {
    // Expected static methods
    isFunc(validateHeaderName, 'validateHeaderName');
    isFunc(validateHeaderValue, 'validateHeaderValue');

    // - when used with valid header names - should not throw
    ['user-agent', 'USER-AGENT', 'User-Agent', 'x-forwarded-for'].forEach(
      (name) => {
        validateHeaderName(name);
      }
    );

    // - when used with invalid header names:
    ['איקס-פורוורד-פור', 'x-forwarded-fםr'].forEach((name) => {
      throws(() => validateHeaderName(name), {
        code: 'ERR_INVALID_HTTP_TOKEN',
      });
    });

    // - when used with valid header values - should not throw
    [
      ['x-valid', 1],
      ['x-valid', '1'],
      ['x-valid', 'string'],
    ].forEach(([name, value]) => {
      validateHeaderValue(name, value);
    });

    // - when used with invalid header values:
    [
      // [header, value, expectedCode]
      ['x-undefined', undefined, 'ERR_HTTP_INVALID_HEADER_VALUE'],
      ['x-bad-char', 'לא תקין', 'ERR_INVALID_CHAR'],
    ].forEach(([name, value, code]) => {
      throws(() => validateHeaderValue(name, value), { code });
    });

    // Misc.
    function isFunc(v, ttl) {
      ok(v.constructor === Function, `${ttl} is expected to be a function`);
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-invalidheaderfield2.js
export const testInvalidHeaderField2 = {
  async test() {
    // Good header field names
    [
      'TCN',
      'ETag',
      'date',
      'alt-svc',
      'Content-Type',
      '0',
      'Set-Cookie2',
      'Set_Cookie',
      'foo`bar^',
      'foo|bar',
      '~foobar',
      'FooBar!',
      '#Foo',
      '$et-Cookie',
      '%%Test%%',
      'Test&123',
      "It's_fun",
      '2*3',
      '4+2',
      '3.14159265359',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkIsHttpToken(str),
        true,
        `_checkIsHttpToken(${inspect(str)}) unexpectedly failed`
      );
    });
    // Bad header field names
    [
      ':',
      '@@',
      '中文呢', // unicode
      '((((())))',
      ':alternate-protocol',
      'alternate-protocol:',
      'foo\nbar',
      'foo\rbar',
      'foo\r\nbar',
      'foo\x00bar',
      '\x7FMe!',
      '{Start',
      '(Start',
      '[Start',
      'End}',
      'End)',
      'End]',
      '"Quote"',
      'This,That',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkIsHttpToken(str),
        false,
        `_checkIsHttpToken(${inspect(str)}) unexpectedly succeeded`
      );
    });

    // Good header field values
    [
      'foo bar',
      'foo\tbar',
      '0123456789ABCdef',
      '!@#$%^&*()-_=+\\;\':"[]{}<>,./?|~`',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkInvalidHeaderChar(str),
        false,
        `_checkInvalidHeaderChar(${inspect(str)}) unexpectedly failed`
      );
    });

    // Bad header field values
    [
      'foo\rbar',
      'foo\nbar',
      'foo\r\nbar',
      '中文呢', // unicode
      '\x7FMe!',
      'Testing 123\x00',
      'foo\vbar',
      'Ding!\x07',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkInvalidHeaderChar(str),
        true,
        `_checkInvalidHeaderChar(${inspect(str)}) unexpectedly succeeded`
      );
    });
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-methods.js
export const testHttpMethods = {
  async test() {
    const methods = [
      'ACL',
      'BIND',
      'CHECKOUT',
      'CONNECT',
      'COPY',
      'DELETE',
      'GET',
      'HEAD',
      'LINK',
      'LOCK',
      'M-SEARCH',
      'MERGE',
      'MKACTIVITY',
      'MKCALENDAR',
      'MKCOL',
      'MOVE',
      'NOTIFY',
      'OPTIONS',
      'PATCH',
      'POST',
      'PROPFIND',
      'PROPPATCH',
      'PURGE',
      'PUT',
      'QUERY',
      'REBIND',
      'REPORT',
      'SEARCH',
      'SOURCE',
      'SUBSCRIBE',
      'TRACE',
      'UNBIND',
      'UNLINK',
      'UNLOCK',
      'UNSUBSCRIBE',
    ];

    deepStrictEqual(METHODS, methods.toSorted());
    deepStrictEqual(httpCommon.methods, methods.toSorted());
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-common.js
export const testHttpCommon = {
  async test() {
    const checkIsHttpToken = httpCommon._checkIsHttpToken;
    const checkInvalidHeaderChar = httpCommon._checkInvalidHeaderChar;

    // checkIsHttpToken
    ok(checkIsHttpToken('t'));
    ok(checkIsHttpToken('tt'));
    ok(checkIsHttpToken('ttt'));
    ok(checkIsHttpToken('tttt'));
    ok(checkIsHttpToken('ttttt'));
    ok(checkIsHttpToken('content-type'));
    ok(checkIsHttpToken('etag'));

    strictEqual(checkIsHttpToken(''), false);
    strictEqual(checkIsHttpToken(' '), false);
    strictEqual(checkIsHttpToken('あ'), false);
    strictEqual(checkIsHttpToken('あa'), false);
    strictEqual(checkIsHttpToken('aaaaあaaaa'), false);

    // checkInvalidHeaderChar
    ok(checkInvalidHeaderChar('あ'));
    ok(checkInvalidHeaderChar('aaaaあaaaa'));

    strictEqual(checkInvalidHeaderChar(''), false);
    strictEqual(checkInvalidHeaderChar(1), false);
    strictEqual(checkInvalidHeaderChar(' '), false);
    strictEqual(checkInvalidHeaderChar(false), false);
    strictEqual(checkInvalidHeaderChar('t'), false);
    strictEqual(checkInvalidHeaderChar('tt'), false);
    strictEqual(checkInvalidHeaderChar('ttt'), false);
    strictEqual(checkInvalidHeaderChar('tttt'), false);
    strictEqual(checkInvalidHeaderChar('ttttt'), false);
  },
};

// Test is taken from test/parallel/test-http-request-invalid-method-error.js
export const testHttpRequestInvalidMethodError = {
  async test() {
    throws(() => http.request({ method: '\0' }), {
      code: 'ERR_INVALID_HTTP_TOKEN',
      name: 'TypeError',
      message: 'Method must be a valid HTTP token ["\u0000"]',
    });
  },
};

// Test is taken from test/parallel/test-http-content-length.js
export const testHttpContentLength = {
  async test(_ctrl, env) {
    const expectedHeadersEndWithData = {
      connection: 'keep-alive',
      'content-length': String('hello world'.length),
    };

    const expectedHeadersEndNoData = {
      connection: 'keep-alive',
      'content-length': '0',
    };

    const { promise, resolve } = Promise.withResolvers();
    let req;

    req = http.request({
      hostname: env.SIDECAR_HOSTNAME,
      port: env.HELLO_WORLD_SERVER_PORT,
      method: 'POST',
      path: '/end-with-data',
    });
    req.removeHeader('Date');
    req.end('hello world');
    req.on('response', function (res) {
      deepStrictEqual(res.headers, {
        ...expectedHeadersEndWithData,
        'keep-alive': 'timeout=1',
      });
      res.resume();
    });

    req = http.request({
      hostname: env.SIDECAR_HOSTNAME,
      port: env.HELLO_WORLD_SERVER_PORT,
      method: 'POST',
      path: '/empty',
    });
    req.removeHeader('Date');
    req.end();
    req.on('response', function (res) {
      deepStrictEqual(res.headers, {
        ...expectedHeadersEndNoData,
        'keep-alive': 'timeout=1',
      });
      res.resume();
      resolve();
    });
    await promise;
  },
};

// Test is taken from test/parallel/test-http-contentLength0.js
export const testHttpContentLength0 = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const request = http.request(
      {
        hostname: env.SIDECAR_HOSTNAME,
        port: env.HELLO_WORLD_SERVER_PORT,
        method: 'POST',
        path: '/content-length0',
      },
      (response) => {
        response.on('error', reject);
        response.resume();
        response.on('end', resolve);
      }
    );
    request.on('error', reject);
    request.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-dont-set-default-headers-with-set-header.js
export const testHttpDontSetDefaultHeadersWithSetHeader = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const req = http.request({
      method: 'POST',
      hostname: env.SIDECAR_HOSTNAME,
      port: env.HEADER_VALIDATION_SERVER_PORT,
      setDefaultHeaders: false,
      path: '/test-1',
    });

    req.setHeader('test', 'value');
    req.setHeader(
      'HOST',
      `${env.SIDECAR_HOSTNAME}:${env.HEADER_VALIDATION_SERVER_PORT}`
    );
    req.setHeader('foo', ['bar', 'baz']);
    req.setHeader('connection', 'close');
    req.on('response', resolve);
    req.on('error', reject);
    strictEqual(req.headersSent, false);
    req.end();
    await promise;
    strictEqual(req.headersSent, true);
  },
};

// Test is taken from test/parallel/test-http-dont-set-default-headers-with-setHost.js
export const testHttpDontSetDefaultHeadersWithSetHost = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    http
      .request({
        method: 'POST',
        hostname: env.SIDECAR_HOSTNAME,
        port: env.HEADER_VALIDATION_SERVER_PORT,
        setDefaultHeaders: false,
        setHost: true,
        path: '/test-2',
      })
      .on('error', reject)
      .on('response', resolve)
      .end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-end-twice.js
export const testHttpRequestEndTwice = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const req = http
      .get(
        {
          hostname: env.SIDECAR_HOSTNAME,
          port: env.HEADER_VALIDATION_SERVER_PORT,
        },
        function (res) {
          res.on('error', reject).on('end', function () {
            strictEqual(req.end(), req);
            resolve();
          });
          res.resume();
        }
      )
      .on('error', reject);
    await promise;
  },
};

// Test is taken from test/parallel/test-http-set-timeout.js
export const testHttpSetTimeout = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const request = http.get({
      hostname: env.SIDECAR_HOSTNAME,
      port: env.TIMEOUT_SERVER_PORT,
      path: '/',
    });
    request.setTimeout(100);
    request.on('error', reject);
    request.on('timeout', resolve);
    request.end();
    await promise;
  },
};

export const httpRedirectsAreNotFollowed = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const req = http.request(
      {
        port: 80,
        method: 'GET',
        protocol: 'http:',
        hostname: 'cloudflare.com',
        path: '/',
      },
      (res) => {
        strictEqual(res.statusCode, 301);
        resolve();
      }
    );
    req.end();
    await promise;
  },
};

export const testExports = {
  async test() {
    strictEqual(typeof http.WebSocket, 'function');
    strictEqual(typeof http.CloseEvent, 'function');
    strictEqual(typeof http.MessageEvent, 'function');
    strictEqual(typeof http._connectionListener, 'function');
    strictEqual(typeof http.setMaxIdleHTTPParsers, 'function');
  },
};

// The following tests does not make sense for workerd
//
// - [ ] test/parallel/test-http-parser-bad-ref.js
// - [ ] test/parallel/test-http-parser-finish-error.js
// - [ ] test/parallel/test-http-parser-free.js
// - [ ] test/parallel/test-http-parser-freed-before-upgrade.js
// - [ ] test/parallel/test-http-parser-lazy-loaded.js
// - [ ] test/parallel/test-http-parser-memory-retention.js
// - [ ] test/parallel/test-http-parser-multiple-execute.js
// - [ ] test/parallel/test-http-parser-timeout-reset.js
// - [ ] test/parallel/test-http-parser.js
