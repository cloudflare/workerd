# Copyright (c) 2024 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0


async def test(context, env):
    resp = await env.ai.toMarkdown().supported()
    assert len(resp) == 2
    assert resp[0].extension == ".md"
    assert resp[1].extension == ".png"
