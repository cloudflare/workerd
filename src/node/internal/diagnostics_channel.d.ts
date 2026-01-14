// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

export * from 'node:diagnostics_channel'
export type TransformCallback = (value: unknown) => unknown
export type MessageCallback = (message: unknown, name: string | symbol) => void
