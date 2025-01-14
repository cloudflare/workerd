# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0


async def test(context, env):
    resp = await env.browser.fetch("http://workers-binding.brapi/run")
    data = await resp.json()
    assert data.success is True
