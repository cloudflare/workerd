// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// Each `blobs` and `indexes` element is unwrapped from any ArrayBuffer or
// ArrayBufferView, or a string; `null` leaves that position empty.
type DataPointElement = ArrayBuffer | ArrayBufferView | string | null;
expectTypeOf<AnalyticsEngineDataPoint['blobs']>().toEqualTypeOf<
  DataPointElement[] | undefined
>();
expectTypeOf<AnalyticsEngineDataPoint['indexes']>().toEqualTypeOf<
  DataPointElement[] | undefined
>();

declare const dataset: AnalyticsEngineDataset;
dataset.writeDataPoint({
  blobs: [
    new Uint8Array([1, 2, 3]),
    'x',
    new DataView(new ArrayBuffer(4)),
    null,
  ],
  doubles: [25],
  indexes: [new Uint8Array(32)],
});
