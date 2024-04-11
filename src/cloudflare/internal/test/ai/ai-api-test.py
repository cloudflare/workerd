# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

async def test(context, env):
  resp = await env.ai.run('testModel', {"prompt": 'test'})
  assert resp.response == "model response"

  # Test logs is empty
  assert len(env.ai.getLogs()) == 0

  # Test request id is present
  assert env.ai.lastRequestId == '3a1983d7-1ddd-453a-ab75-c4358c91b582'
