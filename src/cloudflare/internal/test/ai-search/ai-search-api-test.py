# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from js import Object

from pyodide.ffi import to_js


async def test(context, env):
    # Test list()
    # Convert JS array to Python list to avoid Pyodide borrowed proxy lifecycle issues
    instances = list(await env.ai.aiSearch.list())
    assert len(instances) == 1
    assert instances[0].id == "my-ai-search"

    # Get instance reference once to avoid Pyodide proxy lifecycle issues
    instance = env.ai.aiSearch.get("my-ai-search")

    # Test search()
    # Use to_js() with dict_converter to properly convert nested Python dicts/lists
    # to JS objects, avoiding Pyodide borrowed proxy lifecycle issues
    search_resp = await instance.search(
        to_js(
            {
                "messages": [{"role": "user", "content": "How many woodchucks?"}],
                "ai_search_options": {"retrieval": {"max_num_results": 10}},
            },
            dict_converter=Object.fromEntries,
        )
    )
    assert search_resp.search_query == "How many woodchucks?"
    assert len(search_resp.chunks) == 1
    assert search_resp.chunks[0].text == "Two woodchucks per passenger."

    # Test chatCompletions()
    chat_resp = await instance.chatCompletions(
        to_js(
            {
                "messages": [{"role": "user", "content": "How many woodchucks?"}],
                "model": "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
                "stream": False,
            },
            dict_converter=Object.fromEntries,
        )
    )
    assert chat_resp.response == "Based on the documents, two woodchucks are allowed."
