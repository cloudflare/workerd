// @ts-nocheck
// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { PipelineTransformationEntrypoint } from 'cloudflare:pipelines';

// this is how "Pipeline" would be implemented by the user
const customTransform = class MyEntrypoint extends PipelineTransformationEntrypoint {
  /**
   * @param {any} batch
   * @override
   */
  async run(records, _) {
    for (const record of records) {
      record.dispatcher = 'was here!';
      await new Promise((resolve) => setTimeout(resolve, 1));
      record.wait = 'happened!';
    }

    return records;
  }
};

const lines = [];
const jimmy = `${JSON.stringify({ name: 'jimmy', age: '42', bio: 'hello\nworld\n\n' })}\n`;
const jonny = `${JSON.stringify({ name: 'jonny', age: '9' })}\n`;
const joey = `${JSON.stringify({ name: 'joey', age: '108' })}\n\n`;

for (let i = 0; i < 1000; i++) {
  lines.push(jimmy, jonny, joey);
}

function newBatch() {
  return {
    id: 'test',
    shard: '0',
    ts: Date.now(),
    format: 'json_stream',
    data: new ReadableStream({
      start(controller) {
        const encoder = new TextEncoder();
        for (const line of lines) {
          controller.enqueue(encoder.encode(line));
        }
        controller.close();
      },
    }),
  };
}

// bazel test //src/cloudflare/internal/test/pipeline-transform:transform --test_output=errors --sandbox_debug
export const tests = {
  async test(ctr, env, ctx) {
    {
      // should fail dispatcher test call when PipelineTransform class not extended
      const transformer = new PipelineTransformationEntrypoint(ctx, env);
      await assert.rejects(transformer._ping(), (err) => {
        assert.strictEqual(
          err.message,
          'the run method must be overridden by the PipelineTransformationEntrypoint subclass'
        );
        return true;
      });
    }

    {
      // should correctly handle dispatcher test call
      const transform = new customTransform(ctx, env);
      await assert.doesNotReject(transform._ping());
    }

    {
      // should return mutated batch
      const transformer = new customTransform(ctx, env);
      const batch = newBatch();

      const result = await transformer._run(batch, {
        id: 'abc',
        name: 'mypipeline',
      });
      assert.equal(true, result.data instanceof ReadableStream);

      const reader = result.data
        .pipeThrough(new TextDecoderStream())
        .getReader();

      let data = '';
      while (true) {
        const { done, value } = await reader.read();
        if (done) {
          break;
        } else {
          data += value;
        }
      }
      reader.releaseLock();

      assert.notEqual(data.length, 0);

      const objects = [];
      const resultLines = data.split('\n');
      for (const line of resultLines) {
        if (line.trim().length > 0) {
          // Guard against empty lines
          objects.push(JSON.parse(line));
        }
      }
      assert.equal(objects.length, 3000);

      let index = 0;
      for (const obj of objects) {
        assert.equal(obj.dispatcher, 'was here!');
        delete obj.dispatcher;
        assert.equal(obj.wait, 'happened!');
        delete obj.wait;

        assert.equal(`${JSON.stringify(obj)}`, lines[index].trim());
        index++;
      }
    }
  },
};
