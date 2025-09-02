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

    console.log(received);

    // spans emitted by r2-test.js in execution order
    let expected = [
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'rpc.service': 'r2',
        'rpc.method': 'UploadPart',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_abortMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'AbortMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_delete',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'DeleteObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.keys': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_delete',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'DeleteObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.keys': 'basicKey, basicKey2',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'rangeOffLen',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'rangeSuff',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'throwOnInvalidEtag',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'throwOnInvalidEtag',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfStrongEtag',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfWildcard',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfStrongEtag',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfWildcard',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssecMultipart',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'rpc.service': 'r2',
        'rpc.method': 'UploadPart',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.r2.operation.name': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssecMultipart',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'rpc.service': 'r2',
        'rpc.method': 'UploadPart',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'rpc.service': 'r2',
        'rpc.method': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
    ];

    assert.deepStrictEqual(received, expected);
  },
};
