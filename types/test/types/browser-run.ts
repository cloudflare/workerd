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

// Snapshot accepts every published format and the options each format uses.
expectType<Promise<Response>>(
  browser.quickAction('snapshot', {
    url: 'https://example.com/',
    formats: ['pdf', 'links', 'scrape', 'json', 'accessibilityTree'],
    elements: [{ selector: 'p' }],
    pdfOptions: { format: 'a4', printBackground: true },
    visibleLinksOnly: true,
    excludeExternalLinks: false,
    interestingOnly: true,
    root: 'main',
    prompt: 'Return the page title.',
    custom_ai: [
      { model: 'workers-ai/@cf/meta/llama-3.3-70b-instruct-fp8-fast' },
    ],
  })
);

// @ts-expect-error `crawl` is internal and is not a published snapshot format.
const internalSnapshotFormat: BrowserRunSnapshotFormat = 'crawl';
void internalSnapshotFormat;

// A snapshot result carries one field per requested format.
declare const snapshotBody: BrowserRunSnapshotSuccessResponse;
expectType<string | undefined>(snapshotBody.result.pdf);
expectType<string[] | undefined>(snapshotBody.result.links);
expectType<BrowserRunScrapeSuccessResponse['result'] | undefined>(
  snapshotBody.result.scrape
);
expectType<Record<string, unknown> | unknown[] | null | undefined>(
  snapshotBody.result.json
);
