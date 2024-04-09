/* This project is licensed under the MIT license.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE. */

// Derived from DefinitelyTyped https://github.com/DefinitelyTyped/DefinitelyTyped/blob/master/types/node/stream/promises.d.ts

/* eslint-disable @typescript-eslint/no-explicit-any */

import {
  FinishedOptions,
  PipelineDestination,
  PipelineOptions,
  PipelinePromise,
  PipelineSource,
  PipelineTransform,
} from "node:stream";
import { NodeJS } from "node-internal:streams_transform";

export function finished(
  stream: NodeJS.ReadableStream | NodeJS.WritableStream | NodeJS.ReadWriteStream,
  options?: FinishedOptions,
): Promise<void>;
export function pipeline<A extends PipelineSource<any>, B extends PipelineDestination<A, any>>(
  source: A,
  destination: B,
  options?: PipelineOptions,
): PipelinePromise<B>;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  B extends PipelineDestination<T1, any>,
>(
  source: A,
  transform1: T1,
  destination: B,
  options?: PipelineOptions,
): PipelinePromise<B>;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  T2 extends PipelineTransform<T1, any>,
  B extends PipelineDestination<T2, any>,
>(
  source: A,
  transform1: T1,
  transform2: T2,
  destination: B,
  options?: PipelineOptions,
): PipelinePromise<B>;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  T2 extends PipelineTransform<T1, any>,
  T3 extends PipelineTransform<T2, any>,
  B extends PipelineDestination<T3, any>,
>(
  source: A,
  transform1: T1,
  transform2: T2,
  transform3: T3,
  destination: B,
  options?: PipelineOptions,
): PipelinePromise<B>;
export function pipeline<
  A extends PipelineSource<any>,
  T1 extends PipelineTransform<A, any>,
  T2 extends PipelineTransform<T1, any>,
  T3 extends PipelineTransform<T2, any>,
  T4 extends PipelineTransform<T3, any>,
  B extends PipelineDestination<T4, any>,
>(
  source: A,
  transform1: T1,
  transform2: T2,
  transform3: T3,
  transform4: T4,
  destination: B,
  options?: PipelineOptions,
): PipelinePromise<B>;
export function pipeline(
  streams: ReadonlyArray<NodeJS.ReadableStream | NodeJS.WritableStream | NodeJS.ReadWriteStream>,
  options?: PipelineOptions,
): Promise<void>;
export function pipeline(
  stream1: NodeJS.ReadableStream,
  stream2: NodeJS.ReadWriteStream | NodeJS.WritableStream,
  ...streams: Array<NodeJS.ReadWriteStream | NodeJS.WritableStream | PipelineOptions>
): Promise<void>;
