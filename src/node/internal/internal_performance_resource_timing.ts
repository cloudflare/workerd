// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  PerformanceEntry,
  kSkipThrow,
} from 'node-internal:internal_performance_entry';
import { ERR_ILLEGAL_CONSTRUCTOR } from 'node-internal:internal_errors';
import { validateThisInternalField } from 'node-internal:validators';
import { kEnumerableProperty } from 'node-internal:internal_utils';
import type {
  PerformanceResourceTiming as _PerformanceResourceTiming,
  EntryType,
} from 'node:perf_hooks';

const kCacheMode = Symbol('kCacheMode');
const kRequestedUrl = Symbol('kRequestedUrl');
const kTimingInfo = Symbol('kTimingInfo');
const kInitiatorType = Symbol('kInitiatorType');
const kDeliveryType = Symbol('kDeliveryType');
const kResponseStatus = Symbol('kResponseStatus');

export class PerformanceResourceTiming extends PerformanceEntry {
  // @ts-expect-error TS2411 Fix this soon.
  [kTimingInfo]: {
    startTime: number;
    endTime: number;
    finalServiceWorkerStartTime: number;
    redirectStartTime: number;
    redirectEndTime: number;
    postRedirectStartTime: number;
    finalConnectionTimingInfo: Record<string, unknown> | undefined;
    finalNetworkRequestStartTime: number;
    finalNetworkResponseStartTime: number;
    encodedBodySize: number;
    decodedBodySize: number;
  } = {
    startTime: 0,
    endTime: 0,
    finalServiceWorkerStartTime: 0,
    redirectStartTime: 0,
    redirectEndTime: 0,
    postRedirectStartTime: 0,
    finalConnectionTimingInfo: undefined,
    finalNetworkRequestStartTime: 0,
    finalNetworkResponseStartTime: 0,
    encodedBodySize: 0,
    decodedBodySize: 0,
  };
  [kRequestedUrl]: string = '';
  [kInitiatorType]: EntryType = 'function';
  [kDeliveryType]: string = '';
  [kResponseStatus]: number = 0;

  constructor(
    skipThrowSymbol: symbol | undefined = undefined,
    name: string | undefined = undefined,
    type: EntryType | undefined = undefined
  ) {
    if (skipThrowSymbol !== kSkipThrow) {
      throw new ERR_ILLEGAL_CONSTRUCTOR();
    }

    super(skipThrowSymbol, name, type);
  }

  override get name(): string {
    validateThisInternalField(this, kRequestedUrl, 'PerformanceResourceTiming');
    return this[kRequestedUrl];
  }

  override get startTime(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].startTime;
  }

  override get duration(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].endTime - this[kTimingInfo].startTime;
  }

  get initiatorType(): EntryType {
    validateThisInternalField(
      this,
      kInitiatorType,
      'PerformanceResourceTiming'
    );
    return this[kInitiatorType];
  }

  get workerStart(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalServiceWorkerStartTime;
  }

  get redirectStart(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].redirectStartTime;
  }

  get redirectEnd(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].redirectEndTime;
  }

  get fetchStart(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].postRedirectStartTime;
  }

  get domainLookupStart(): unknown {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalConnectionTimingInfo?.domainLookupStartTime;
  }

  get domainLookupEnd(): unknown {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalConnectionTimingInfo?.domainLookupEndTime;
  }

  get connectStart(): unknown {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalConnectionTimingInfo?.connectionStartTime;
  }

  get connectEnd(): unknown {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalConnectionTimingInfo?.connectionEndTime;
  }

  get secureConnectionStart(): unknown {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalConnectionTimingInfo
      ?.secureConnectionStartTime;
  }

  get nextHopProtocol(): unknown {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalConnectionTimingInfo?.ALPNNegotiatedProtocol;
  }

  get requestStart(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalNetworkRequestStartTime;
  }

  get responseStart(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].finalNetworkResponseStartTime;
  }

  get responseEnd(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].endTime;
  }

  get encodedBodySize(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].encodedBodySize;
  }

  get decodedBodySize(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kTimingInfo].decodedBodySize;
  }

  get transferSize(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    if (this[kCacheMode] === 'local') return 0;
    if (this[kCacheMode] === 'validated') return 300;

    return this[kTimingInfo].encodedBodySize + 300;
  }

  get deliveryType(): string {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kDeliveryType];
  }

  get responseStatus(): number {
    validateThisInternalField(this, kTimingInfo, 'PerformanceResourceTiming');
    return this[kResponseStatus];
  }

  override toJSON(): Record<string, unknown> {
    validateThisInternalField(
      this,
      kInitiatorType,
      'PerformanceResourceTiming'
    );
    return {
      name: this.name,
      entryType: this.entryType,
      startTime: this.startTime,
      duration: this.duration,
      initiatorType: this[kInitiatorType],
      nextHopProtocol: this.nextHopProtocol,
      workerStart: this.workerStart,
      redirectStart: this.redirectStart,
      redirectEnd: this.redirectEnd,
      fetchStart: this.fetchStart,
      domainLookupStart: this.domainLookupStart,
      domainLookupEnd: this.domainLookupEnd,
      connectStart: this.connectStart,
      connectEnd: this.connectEnd,
      secureConnectionStart: this.secureConnectionStart,
      requestStart: this.requestStart,
      responseStart: this.responseStart,
      responseEnd: this.responseEnd,
      transferSize: this.transferSize,
      encodedBodySize: this.encodedBodySize,
      decodedBodySize: this.decodedBodySize,
      deliveryType: this.deliveryType,
      responseStatus: this.responseStatus,
    };
  }
}

Object.defineProperties(PerformanceResourceTiming.prototype, {
  initiatorType: kEnumerableProperty,
  nextHopProtocol: kEnumerableProperty,
  workerStart: kEnumerableProperty,
  redirectStart: kEnumerableProperty,
  redirectEnd: kEnumerableProperty,
  fetchStart: kEnumerableProperty,
  domainLookupStart: kEnumerableProperty,
  domainLookupEnd: kEnumerableProperty,
  connectStart: kEnumerableProperty,
  connectEnd: kEnumerableProperty,
  secureConnectionStart: kEnumerableProperty,
  requestStart: kEnumerableProperty,
  responseStart: kEnumerableProperty,
  responseEnd: kEnumerableProperty,
  transferSize: kEnumerableProperty,
  encodedBodySize: kEnumerableProperty,
  decodedBodySize: kEnumerableProperty,
  deliveryType: kEnumerableProperty,
  responseStatus: kEnumerableProperty,
  toJSON: kEnumerableProperty,
  [Symbol.toStringTag]: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    configurable: true,
    value: 'PerformanceResourceTiming',
  },
});
