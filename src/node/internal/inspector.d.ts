// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for the C++ implementation of `node-internal:inspector`.

export type MessageCallback = (message: string) => void;

// A thin in-process pipe to the isolate's own V8 inspector. CDP messages are exchanged as opaque
// JSON strings; all envelope logic lives in the `node:inspector` TypeScript layer.
export class Connection {
  constructor(callback: MessageCallback);
  dispatch(message: string): void;
  disconnect(): void;
}

declare const moduleDefault: {
  Connection: typeof Connection;
};

export default moduleDefault;
