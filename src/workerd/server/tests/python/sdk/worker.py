# The tests in this file are primarily spread across Default.fetch() (in this module) and
# Default.fetch() (in server.py).
#
# The code in `Default.test()` (in this module) is used to actually perform the testing and its
# behaviour doesn't need to strictly be held consistent. In fact it uses the JS fetch, so it's not
# going to follow the SDK at all.

from contextlib import asynccontextmanager
from http import HTTPMethod, HTTPStatus

import js
import pyodide.http
from pyodide.ffi import JsProxy, to_js
from workers import Blob, File, FormData, Request, Response, WorkerEntrypoint, fetch


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


class Default(WorkerEntrypoint):
    # Each path in this handler is its own test. The URLs that are being fetched
    # here are defined in server.py.
    async def fetch(self, request):
        assert isinstance(request, Request)
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
                cf={
                    "cacheTtl": 5,
                    "cacheEverything": True,
                    "cacheKey": "someCustomKey",
                },
            )
            assert resp.status == 301
            return Response("success")
        elif request.url.endswith("/event_decorator"):
            # Verify that the `@event`` decorator has transformed the `request` parameter to the correct
            # type.
            #
            # Try to grab headers which should contain a duplicated header.
            headers = request.headers.get_all("X-Custom-Header")
            assert "some_value" in headers
            assert "some_other_value" in headers
            return Response("success")
        else:
            resp = await fetch("https://example.com/sub")
            return resp

    async def scheduled(self, ctrl, env, ctx):
        assert ctrl.scheduledTime == 1000
        assert ctrl.cron == "* * * * 30"

    async def test(self):
        js_env = self.env._env
        await can_support_scheduled_cron_trigger(js_env)
        await can_return_custom_fetch_response(js_env)
        await can_modify_response(js_env)
        await can_use_duplicate_headers(js_env)
        await can_use_fetch_opts(js_env)
        await gets_nice_error_on_jsism(js_env)
        await can_use_undefined_options_and_redirect(js_env)
        await can_use_inherited_response_methods(js_env)
        await errors_on_invalid_input_to_redirect(js_env)
        await can_use_response_json(js_env)
        await can_request_form_data(js_env)
        await form_data_unit_tests(js_env)
        await blob_unit_tests(js_env)
        await can_request_form_data_blob(js_env)
        await replace_body_unit_tests(js_env)
        await can_use_cf_fetch_opts(js_env)
        await request_unit_tests(js_env)
        await can_use_event_decorator(js_env)
        await response_unit_tests(js_env)
        await response_buffer_source_unit_tests(js_env)
        await can_fetch_python_request()


# TODO: Right now the `fetch` that's available on a binding is the JS fetch.
# We may wish to rewrite it to be the same as the `fetch` defined in
# `cloudflare.workers`. Doing so will require a feature flag.
#
# WARNING: Don't expect the below code itself to make use of the features in the SDK, it's mostly
# calling the JS fetch via the FFI.


async def can_return_custom_fetch_response(env):
    assert isinstance(env, JsProxy), (
        "Expecting the env for these tests not to be wrapped"
    )
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
    assert text == '{"obj": {"field": 123}}'


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


async def request_unit_tests(env):
    req = Request("https://test.com", method=HTTPMethod.POST)
    assert req.method == HTTPMethod.POST
    assert repr(req) == "Request(method='POST', url='https://test.com/')"

    # Verify that we can pass JS headers to Request
    js_headers = js.Headers.new()
    js_headers.set("foo", "bar")
    req_with_headers = Request("http://example.com", headers=js_headers)
    assert req_with_headers.headers["foo"] == "bar"

    # Verify that we can pass a dictionary as headers to Request
    req_with_headers = Request("http://example.com", headers={"aaaa": "test"})
    assert req_with_headers.headers["aaaa"] == "test"

    # Verify that BodyUserError is thrown correctly.
    req_used_twice = Request(
        "http://example.com", body='{"field": 42}', method=HTTPMethod.POST
    )
    data = await req_used_twice.json()
    assert data["field"] == 42
    try:
        req_used_twice.clone()
        raise ValueError("Expected to throw")  # noqa: TRY301
    except Exception as exc:
        assert exc.__class__.__name__ == "OSError"  # TODO: BodyUsedError when available

    # Verify that duplicate header keys are returned correctly.
    js_headers = js.Headers.new()
    js_headers.append("Accept-encoding", "deflate")
    js_headers.append("Accept-encoding", "gzip")
    req_with_dup_headers = Request("http://example.com", headers=js_headers)
    assert req_with_dup_headers.url == "http://example.com/"
    encoding = req_with_dup_headers.headers.get_all("Accept-encoding")
    assert "deflate" in encoding
    assert "gzip" in encoding

    # Verify that we can get a Blob.
    req_for_blob = Request("http://example.com", body="foobar", method="POST")
    blob = await req_for_blob.blob()
    assert (await blob.text()) == "foobar"

    # Verify that we can get a FormData back.
    js_form_data = js.FormData.new()
    js_form_data.append("foobar", 123)
    req_with_form_data = Request("http://example.com", body=js_form_data, method="POST")
    form_data = await req_with_form_data.form_data()
    assert form_data["foobar"] == "123"


async def can_use_event_decorator(env):
    js_headers = js.Headers.new()
    js_headers.append("X-Custom-Header", "some_value")
    js_headers.append("X-Custom-Header", "some_other_value")
    response = await env.SELF.fetch(
        "http://example.com/event_decorator", headers=js_headers
    )
    text = await response.text()
    assert text == "success"


async def response_unit_tests(env):
    response_json = Response.json([1, 2, 3])
    assert await response_json.text() == "[1, 2, 3]"
    assert (
        repr(response_json)
        == "Response(status=200, status_text='OK', content_type='application/json')"
    )

    response_json = Response.from_json([1, 2, 3])
    assert await response_json.text() == "[1, 2, 3]"

    response_json = Response("[1, 2, 3]")
    assert await response_json.json() == [1, 2, 3]

    response_json = Response.json("test")
    assert await response_json.text() == '"test"'

    response_json = Response.json(["hi", "foo", 42])
    assert await response_json.text() == '["hi", "foo", 42]'

    response_json = Response.json({"field": 42})
    assert await response_json.text() == '{"field": 42}'

    response_json = Response.json("test")
    assert response_json.headers.get("content-type") == "application/json"

    response_json = Response.json("test", headers={"x-other-header": "42"})
    assert response_json.headers.get("content-type") == "application/json"

    response_json = Response.json("test", headers={"Content-Type": "42"})
    assert response_json.headers.get("content-type") == "42"

    response_none = Response(None, status=204)
    assert response_none.status == 204
    assert response_none.body is None

    response_bytes = Response(b"test")
    assert response_bytes.status == 200
    assert await response_bytes.text() == "test"

    class Test:
        def __init__(self, x):
            self.x = x

    try:
        response_json = Response.json(Test(42))
        await response_json.text()
        raise ValueError("Should have raised")  # noqa: TRY301
    except Exception as err:
        assert str(err) == "Object of type Test is not JSON serializable"

    try:
        response_json = Response.json({1, 2, 3})
        await response_json.text()
        raise ValueError("Should have raised")  # noqa: TRY301
    except Exception as err:
        assert str(err) == "Object of type set is not JSON serializable"

    try:
        response_json = Response(Test(42))
        await response_json.text()
        raise ValueError("Should have raised")  # noqa: TRY301
    except Exception as err:
        assert str(err) == "Unsupported type in Response: Test"

    response_ws = Response(
        None, status=101, web_socket=js.WebSocket.new("ws://example.com/ignore")
    )
    # TODO: it doesn't seem possible to access webSocket even in JS
    assert response_ws.status == 101


async def response_buffer_source_unit_tests(env):
    buffer_source_cases = [
        # TODO: Float16Array is not supported in Pyodide <= 0.29 (pyodide/pyodide#6005)
        ("ArrayBuffer", js.Uint8Array.new(to_js([1, 2, 3, 4, 5, 6, 7, 8])).buffer),
        ("DataView", js.DataView.new(js.Uint8Array.new(to_js([9, 10, 11, 12])).buffer)),
        ("Uint8Array", js.Uint8Array.new(to_js([1, 2, 3, 4]))),
        ("Uint8ClampedArray", js.Uint8ClampedArray.new(to_js([1, 2, 3, 4]))),
        ("Int8Array", js.Int8Array.new(to_js([1, -1, 2, -2]))),
        ("Uint16Array", js.Uint16Array.new(to_js([1, 2, 3, 4]))),
        ("Int16Array", js.Int16Array.new(to_js([1, -2, 3, -4]))),
        ("Uint32Array", js.Uint32Array.new(to_js([1, 2, 3, 4]))),
        ("Int32Array", js.Int32Array.new(to_js([1, -2, 3, -4]))),
        ("Float32Array", js.Float32Array.new(to_js([1.5, -2.5, 3.25, -4.75]))),
        ("Float64Array", js.Float64Array.new(to_js([1.5, -2.5]))),
        # BigInt64 not supported in Pyodide <= 0.26
        # ("BigInt64Array", js.BigInt64Array.new(to_js([2**53 + 1, -(2**53 + 1), 2**54 + 2, -(2**54 + 2)]))),
        # ("BigUint64Array", js.BigUint64Array.new(to_js([2**53 + 1, 2**54 + 2, 2**55 + 3, 2**56 + 4]))),
        # Test partial views to verify they work correctly when not viewing the whole backing buffer
        (
            "Uint8Array.subarray",
            js.Uint8Array.new(to_js([0, 1, 2, 3, 4, 5])).subarray(1),
        ),
        ("Int8Array.subarray", js.Int8Array.new(to_js([0, 1, -1, 2, -2])).subarray(1)),
    ]

    for type_name, body in buffer_source_cases:
        expected_length = int(body.byteLength)
        try:
            response = Response(body)
        except TypeError as exc:
            raise AssertionError(
                f"Response rejected BufferSource type {type_name}"
            ) from exc

        buffer = await response.buffer()
        assert int(buffer.byteLength) == expected_length, (
            f"Response buffer length mismatch for {type_name}"
        )


async def can_fetch_python_request():
    def fetch_check(request, opts):
        assert isinstance(request, JsProxy)

    async with _mock_fetch(fetch_check):
        await fetch(Request("https://example.com/redirect"))


async def can_support_scheduled_cron_trigger(env):
    result = await env.SELF.scheduled(scheduledTime=1000, cron="* * * * 30")
    assert result.outcome == "ok"
