// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

const kUrl = 'https://example.com/events';

const source = new EventSource(kUrl);
expectType<EventTarget>(source);
expectType<string>(source.url);
expectType<boolean>(source.withCredentials);
expectType<number>(source.readyState);
// @ts-expect-error readyState is read-only.
source.readyState = EventSource.CLOSED;
source.onopen = (event: Event) => event.type;
source.onmessage = (event: MessageEvent) => event.data;
source.onerror = null;
source.close();

expectType<number>(EventSource.CONNECTING);
expectType<number>(EventSource.OPEN);
expectType<number>(EventSource.CLOSED);

declare const fetcher: Fetcher;
new EventSource(kUrl, { fetcher });
new EventSource(kUrl, { withCredentials: false });
// @ts-expect-error a fetcher is a Fetcher.
new EventSource(kUrl, { fetcher: {} });
// @ts-expect-error the URL is required.
new EventSource();

declare const stream: ReadableStream;
expectType<EventSource>(EventSource.from(stream));

expectType<typeof EventSource>(self.EventSource);
