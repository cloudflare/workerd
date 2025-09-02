// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
let invocationPromises = [];
let spans = new Map();

export default {
  tailStream(event, env, ctx) {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Accumulate the span info for easier testing
    return (event) => {
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          spans.set(event.spanId, { name: event.event.name });
          break;
        case 'attributes': {
          let span = spans.get(event.spanId);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(event.spanId, span);
          break;
        }
        case 'spanClose': {
          let span = spans.get(event.spanId);
          span['closed'] = true;
          spans.set(event.spanId, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};

export const test = {
  async test() {
    // Wait for all the tailStream executions to finish
    await Promise.allSettled(invocationPromises);

    // Recorded streaming tail worker events, in insertion order,
    let received = Array.from(spans.values());

    // spans emitted by r2-test.js in execution order
    let expected = [
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'rpc.service': 'r2',
        'rpc.method': 'UploadPart',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_abortMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'AbortMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_list',
        'rpc.service': 'r2',
        'rpc.method': 'ListObjects',
        closed: true,
      },
      {
        name: 'r2_delete',
        'rpc.service': 'r2',
        'rpc.method': 'DeleteObject',
        'cloudflare.r2.delete': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_delete',
        'rpc.service': 'r2',
        'rpc.method': 'DeleteObject',
        'cloudflare.r2.delete': 'basicKey, basicKey2',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'rpc.service': 'r2',
        'rpc.method': 'ListObjects',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'rpc.service': 'r2',
        'rpc.method': 'ListObjects',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'rpc.service': 'r2',
        'rpc.method': 'ListObjects',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'rpc.service': 'r2',
        'rpc.method': 'UploadPart',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'rpc.service': 'r2',
        'rpc.method': 'PutObject',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CreateMultipartUpload',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'rpc.service': 'r2',
        'rpc.method': 'UploadPart',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'onlyIfWildcard',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'onlyIfStrongEtag',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'rangeSuff',
        closed: true,
      },
      {
        name: 'r2_get',
        'rpc.service': 'r2',
        'rpc.method': 'GetObject',
        'cloudflare.r2.key': 'rangeOffLen',
        closed: true,
      },
    ];

    assert.deepStrictEqual(received, expected);
  },
};
