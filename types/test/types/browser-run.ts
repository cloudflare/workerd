// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const browser: BrowserRun;
const code = 'async ({ page }) => page.title()';

// A new session.
expectType<Promise<Response>>(browser.script(code));
expectType<Promise<Response>>(
  browser.script(code, {
    library: 'playwright',
    timeout: 30000,
    keepAlive: 120000,
    lab: true,
    recording: true,
  })
);
expectType<Promise<Response>>(browser.script(code, { browser: 'kitesurf' }));

// An existing session.
expectType<Promise<Response>>(browser.script(code, { sessionId: 'session' }));
expectType<Promise<Response>>(
  browser.script(code, {
    sessionId: 'session',
    targetId: 'target',
    library: 'puppeteer',
    timeout: 30000,
  })
);

// @ts-expect-error Launch options cannot be combined with `sessionId`.
browser.script(code, { sessionId: 'session', keepAlive: 120000 });
// @ts-expect-error `targetId` needs `sessionId`.
browser.script(code, { targetId: 'target' });
// @ts-expect-error `browser` only accepts `kitesurf`.
browser.script(code, { browser: 'chrome' });
// @ts-expect-error `scriptInSession()` was replaced by `script()` with `sessionId`.
browser.scriptInSession('session', code);
