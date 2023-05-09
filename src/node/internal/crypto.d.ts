// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
    Buffer,
} from 'node-internal:internal_buffer';

// random
export function getRandomInt(min: number, max: number): number;
export function checkPrimeSync(candidate: ArrayBufferView, num_checks: number): boolean;
export function randomPrime(size: number, safe: boolean, add?: ArrayBufferView|undefined, rem?: ArrayBufferView|undefined): ArrayBuffer;

// pbkdf2
export type ArrayLike = ArrayBuffer|string|Buffer|ArrayBufferView;
export function getPbkdf(password: ArrayLike, salt: ArrayLike, iterations: number, keylen: number, digest: string): ArrayBuffer;
