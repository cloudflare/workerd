# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from js import Float32Array


async def test(context, env):
    IDX = env.vectorSearch

    res_array = [0] * 5
    results = await IDX.query(
        Float32Array.new(res_array),
        topK=3,
        returnValues=True,
        returnMetadata=True,
    )
    assert results.count > 0
    assert results.matches[0].id == "b0daca4a-ffd8-4865-926b-e24800af2a2d"
    assert (
        results.matches[1].metadata.text
        == "Peter Piper picked a peck of pickled peppers"
    )
