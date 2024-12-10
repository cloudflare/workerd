from contextlib import asynccontextmanager
from http import HTTPMethod, HTTPStatus

import js

import pyodide.http
from cloudflare.workers import Blob, File, FormData, Response, fetch
from pyodide.ffi import to_js


@asynccontextmanager
async def _mock_fetch(check):
    async def mocked_fetch(original_fetch, url, opts):
        check(url, opts)
        return await original_fetch(url, opts)

    original_fetch = pyodide.http._jsfetch
    pyodide.http._jsfetch = lambda url, opts: mocked_fetch(original_fetch, url, opts)
    try:
        yield
    finally:
        pyodide.http._jsfetch = original_fetch


# Each path in this handler is its own test. The URLs that are being fetched
# here are defined in server.py.
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
    elif request.url.endswith("/jsism"):
        # Headers should be specified via a keyword argument, but it's done
        # differently in JS. So we test what happens when someone does it
        # that way accidentally to make sure we give them a reasonable error
        # message.
        try:
            resp = await fetch(
                "https://example.com/sub",
                {"headers": {"Custom-Req-Header": 123}},
            )
        except TypeError as exc:
            if str(exc) == "fetch() takes 1 positional argument but 2 were given":
                return Response("success")

        return Response("fail")
    elif request.url.endswith("/undefined_opts"):
        # This tests two things:
        #   * `Response.redirect` static method
        #   * that other options can be passed into `fetch` (so that we can support
        #       new options without updating this code)

        # Mock pyodide.http._jsfetch to ensure `foobarbaz` gets passed in.
        def fetch_check(url, opts):
            assert opts.foobarbaz == 42

        async with _mock_fetch(fetch_check):
            resp = await fetch(
                "https://example.com/redirect", redirect="manual", foobarbaz=42
            )

        return resp
    elif request.url.endswith("/response_inherited"):
        expected = "test123"
        resp = Response(expected)
        text = await resp.text()
        if text == expected:
            return Response("success")
        else:
            return Response("invalid")
    elif request.url.endswith("/redirect_invalid_input"):
        try:
            return Response.redirect("", HTTPStatus.BAD_GATEWAY)
        except ValueError:
            return Response("success")
        return Response("invalid")
    elif request.url.endswith("/response_json"):
        return Response.json({"obj": {"field": 123}}, status=HTTPStatus.NOT_FOUND)
    elif request.url.endswith("/formdata"):
        # server.py creates a new FormData and verifies that it can be passed
        # to Response.
        resp = await fetch("https://example.com/formdata")
        data = await resp.formData()
        if data["field"] == "value":
            return Response("success")
        else:
            return Response("fail")
    elif request.url.endswith("/formdatablob"):
        # server.py creates a new FormData and verifies that it can be passed
        # to Response.
        resp = await fetch("https://example.com/formdatablob")
        data = await resp.formData()
        assert data["field"] == "value"
        assert (await data["blob.py"].text()) == "print(42)"
        assert data["blob.py"].content_type == "text/python"
        assert data["metadata"].name == "metadata.json"

        return Response("success")
    elif request.url.endswith("/cf_opts"):
        resp = await fetch(
            "http://example.com/redirect",
            redirect="manual",
            cf={"cacheTtl": 5, "cacheEverything": True, "cacheKey": "someCustomKey"},
        )
        assert resp.status == 301
        return Response("success")
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


async def gets_nice_error_on_jsism(env):
    response = await env.SELF.fetch(
        "http://example.com/jsism",
    )
    text = await response.text()
    assert text == "success"


async def can_use_undefined_options_and_redirect(env):
    response = await env.SELF.fetch(
        "http://example.com/undefined_opts", redirect="manual"
    )
    # The above path in this worker hits server's /redirect path,
    # which returns a 301 to /sub. The code uses a fetch option which
    # instructs it to not follow redirects, so we expect to get
    # a 301 here.
    assert response.status == 301


async def can_use_inherited_response_methods(env):
    response = await env.SELF.fetch(
        "http://example.com/response_inherited",
    )
    text = await response.text()
    assert text == "success"


async def errors_on_invalid_input_to_redirect(env):
    response = await env.SELF.fetch(
        "http://example.com/redirect_invalid_input",
    )
    text = await response.text()
    assert text == "success"


async def can_use_response_json(env):
    response = await env.SELF.fetch(
        "http://example.com/response_json",
    )
    text = await response.text()
    assert text == '{"obj":{"field":123}}'


async def can_request_form_data(env):
    response = await env.SELF.fetch(
        "http://example.com/formdata",
    )
    text = await response.text()
    assert text == "success"


async def form_data_unit_tests(env):
    # Verify that existing JS formdata is loaded correctly.
    js_data = js.FormData.new()
    js_data.append("foobar", 123)
    js_data.append("key", "lorem ipsum")
    js_data.append("key", "dolor sit amet")
    data = FormData(js_data)
    assert data["foobar"] == "123"
    assert data["key"] == "lorem ipsum"
    assert data.get_all("key") == ["lorem ipsum", "dolor sit amet"]
    assert "unknown key" not in data
    assert "key" in data

    # Verify that dictionary can instantiate form data.
    data = FormData({"key": "foobar"})
    assert data["key"] == "foobar"
    assert "key" in data

    # Test iterators
    data.append("key", "another")
    data.append("key2", "foobar2")
    data_items = set(data.items())
    assert ("key", "foobar") in data_items
    assert ("key", "another") in data_items
    assert ("key2", "foobar2") in data_items
    data_keys = set(data.keys())
    assert len(list(data.keys())) == 3
    assert "key" in data_keys
    assert "key2" in data_keys
    data_values = set(data.values())
    assert "foobar" in data_values
    assert "another" in data_values
    assert "foobar2" in data_values


async def blob_unit_tests(env):
    # Verify that we can create a FormData and add Blob in there.
    data = FormData()
    data["test"] = Blob(["some string"])
    test_contents = await data["test"].text()
    assert test_contents == "some string"
    data["js"] = js.Blob.new(to_js(["another string"]))
    # Even though we added a JS Blob, we should get a Python Blob.
    assert isinstance(data["js"], Blob)
    data.append("blah", js.Blob.new(to_js(["another string"])))
    # Iterating through the items should give us Python Blobs.
    for key, val in data.items():
        assert isinstance(key, str)
        assert isinstance(val, Blob)
    # Verify that content type can be set.
    content_type_blob = Blob(["test"], content_type="application/json")
    assert content_type_blob.js_object.type == "application/json"
    # Verify instance properties.
    assert content_type_blob.size == 4
    assert content_type_blob.content_type == "application/json"
    # Verify instance methods.
    assert (await content_type_blob.bytes()) == b"test"
    assert (await content_type_blob.slice(1, 3).text()) == "es"
    # Verify that Blob can be created with a memoryview.
    memory_data = b"foobar"
    memory_view_blob = Blob([memoryview(memory_data)])
    assert await memory_view_blob.text() == "foobar"
    # Verify that Blob constructor inherits type.
    inherited = Blob(content_type_blob)
    not_inherited = Blob(content_type_blob, content_type="other/type")
    assert inherited.content_type == "application/json"
    assert not_inherited.content_type == "other/type"
    not_inherited2 = Blob([content_type_blob])
    assert not_inherited2.content_type == ""
    # Verify that we can create Files.
    file_blob = File("my file", filename="test.txt", content_type="text/plain")
    assert file_blob.content_type == "text/plain"
    assert file_blob.name == "test.txt"


async def can_request_form_data_blob(env):
    response = await env.SELF.fetch(
        "http://example.com/formdatablob",
    )
    text = await response.text()
    assert text == "success"


async def replace_body_unit_tests(env):
    response = Response("test", status=201, status_text="Created")
    cloned = response.replace_body("other")
    assert cloned.status == 201
    assert cloned.status_text == "Created"
    t = await cloned.text()
    assert t == "other"


async def can_use_cf_fetch_opts(env):
    response = await env.SELF.fetch(
        "http://example.com/cf_opts",
    )
    text = await response.text()
    assert text == "success"


async def test(ctrl, env):
    await can_return_custom_fetch_response(env)
    await can_modify_response(env)
    await can_use_duplicate_headers(env)
    await can_use_fetch_opts(env)
    await gets_nice_error_on_jsism(env)
    await can_use_undefined_options_and_redirect(env)
    await can_use_inherited_response_methods(env)
    await errors_on_invalid_input_to_redirect(env)
    await can_use_response_json(env)
    await can_request_form_data(env)
    await form_data_unit_tests(env)
    await blob_unit_tests(env)
    await can_request_form_data_blob(env)
    await replace_body_unit_tests(env)
    await can_use_cf_fetch_opts(env)
