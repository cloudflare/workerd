from http import HTTPMethod

from cloudflare.workers import Response, fetch


async def on_fetch(request):
    if request.url.endswith("/modify"):
        resp = await fetch("https://example.com/sub")
        return Response(
            resp.body,
            status=201,
            headers={"Custom-Header-That-Should-Passthrough": "modified"},
        )
    elif request.url.endswith("/modify_tuple"):
        resp = await fetch("https://example.com/sub")
        return Response(
            resp.body,
            headers=[
                ("Custom-Header-That-Should-Passthrough", "modified"),
                ("Custom-Header-That-Should-Passthrough", "another"),
            ],
        )
    elif request.url.endswith("/fetch_opts"):
        resp = await fetch(
            "https://example.com/sub_headers",
            method=HTTPMethod.GET,
            body=None,
            headers={"Custom-Req-Header": 123},
        )
        return resp
    else:
        resp = await fetch("https://example.com/sub")
        return resp


# TODO: Right now the `fetch` that's available on a binding is the JS fetch.
# We may wish to rewrite it to be the same as the `fetch` defined in
# `cloudflare.workers`. Doing so will require a feature flag.


async def can_return_custom_fetch_response(env):
    response = await env.SELF.fetch(
        "http://example.com/",
    )
    text = await response.text()
    assert text == "Hi there!"


async def can_modify_response(env):
    response = await env.SELF.fetch(
        "http://example.com/modify",
    )
    text = await response.text()
    assert text == "Hi there!"
    assert response.status == 201
    assert response.headers.get("Custom-Header-That-Should-Passthrough") == "modified"


async def can_use_duplicate_headers(env):
    response = await env.SELF.fetch(
        "http://example.com/modify_tuple",
    )
    text = await response.text()
    assert text == "Hi there!"
    assert (
        response.headers.get("Custom-Header-That-Should-Passthrough")
        == "modified, another"
    )


async def can_use_fetch_opts(env):
    response = await env.SELF.fetch(
        "http://example.com/fetch_opts",
    )
    text = await response.text()
    assert text == "Hi there!"
    assert response.headers.get("Custom-Header-That-Should-Passthrough") == "true"


async def test(ctrl, env):
    await can_return_custom_fetch_response(env)
    await can_modify_response(env)
    await can_use_duplicate_headers(env)
    await can_use_fetch_opts(env)
