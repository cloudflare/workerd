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
      // span ids are simple counters for tests, but invocation ID allows us to differentiate them
      let spanKey = event.invocationId + event.spanContext.spanId;
      switch (event.event.type) {
        case 'spanOpen':
          spans.set(event.invocationId + event.event.spanId, {
            name: event.event.name,
          });
          break;
        case 'attributes': {
          let span = spans.get(spanKey);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(spanKey, span);
          break;
        }
        case 'spanClose': {
          let span = spans.get(spanKey);
          span['closed'] = true;
          spans.set(spanKey, span);
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
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_abortMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_delete',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'DeleteObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.keys': 'basicKey',
        closed: true,
      },
      {
        name: 'r2_delete',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'DeleteObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.keys': 'basicKey, basicKey2',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'rangeOffLen',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'rangeSuff',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'throwOnInvalidEtag',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'throwOnInvalidEtag',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfStrongEtag',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfWildcard',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfStrongEtag',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'onlyIfWildcard',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'httpMetadata',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_list',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'ListObjects',
        'cloudflare.r2.bucket': 'r2-test',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'customMetadata',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classDefault',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classStandard',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'classInfrequentAccess',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssecMultipart',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_put',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'PutObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'GetObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_get',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'HeadObject',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssec',
        closed: true,
      },
      {
        name: 'r2_createMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CreateMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.request.key': 'ssecMultipart',
        closed: true,
      },
      {
        name: 'r2_uploadPart',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
      {
        name: 'r2_completeMultipartUpload',
        'cloudflare.binding.type': 'r2',
        'cloudflare.binding.name': 'BUCKET',
        'cloudflare.r2.operation': 'CompleteMultipartUpload',
        'cloudflare.r2.bucket': 'r2-test',
        'cloudflare.r2.upload_id': 'multipartId',
        closed: true,
      },
    ];

    assert.deepStrictEqual(received, expected);
  },
};
