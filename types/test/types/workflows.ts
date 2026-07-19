// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const workflow: Workflow;

async function testDeleteBatch() {
  const result = await workflow.deleteBatch(['one', 'two']);
  expectType<WorkflowBatchDeleteResult>(result);
  expectType<string>(result.deleted[0].id);
  expectType<number>(result.errors[0].index);
}

void testDeleteBatch;
