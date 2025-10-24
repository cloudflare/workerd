import { WorkflowEntrypoint } from 'cloudflare:workers';
import assert from "node:assert";

export class MyWorkflow extends WorkflowEntrypoint {
  async run(_event) {
    console.log("I ran!")
    return 'return val';
  }
}

export let shouldGetValueFromEntrypoint = {
    async test(ctrl, env, tx) {
        let result = await env.MY_WORKFLOW.runWorkflow({
            workflowName: "my-workflow",
            instanceId: "mock-instance-id",
            timestamp: Date.now(),
            payload: "payload"
        });

        assert.strictEqual(result.returnValue, "return val");
    }
};
