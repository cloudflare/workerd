# Copyright (c) 2024 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0


async def test(context, env):
    resp = await env.ai.gateway("my-gateway").getLog("my-log-123")
    assert resp.cached is False
    assert resp.model == "string"
