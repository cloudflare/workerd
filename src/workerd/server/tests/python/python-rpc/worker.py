# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from http import HTTPMethod

import js
from workers import Blob, Request, Response, WorkerEntrypoint

from pyodide.ffi import JsProxy, to_js


class PythonRpcTester(WorkerEntrypoint):
    async def no_args(self):
        return "hello from python"

    async def one_arg(self, x):
        return f"{x}"

    async def identity(self, x):
        return x

    # https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Structured_clone_algorithm#supported_types
    #
    # Workers RPC doesn't support all of the above, but does support all the JS Types and
    # some of the Web API types.
    #
    # ReadableStream and WritableStream are also supported.
    #
    # We verify that we can send and receive all of these, including between Python<->Python,
    # JS<->Python and vice versa.

    async def handle_response(self, response):
        # Verify that we receive a Python object here...
        assert isinstance(response, Response)
        return response

    async def handle_request(self, req):
        assert isinstance(req, Request)
        return req


class CustomType:
    def __init__(self, x):
        self.x = 42


async def assertRaises(func):
    try:
        await func()
        raise ValueError("assertRaises failed")  # noqa: TRY301
    except Exception as e:
        assert "DataCloneError" in str(e)


async def test(ctrl, env, ctx):
    # Simple tests.
    assert await env.PythonRpc.no_args() == "hello from python"
    assert await env.JsRpc.noArgs() == "hello from js"
    assert await env.PythonRpc.one_arg(42) == "42"
    assert await env.JsRpc.oneArg(42) == "42"

    # JS Types
    for val in [
        # [1,2,3,4], TODO
        js.ArrayBuffer.new(8),  # TODO: Check contents
        False,
        True,
        js.DataView.new(js.ArrayBuffer.new(16)),
        js.Date.new(),
        js.Error.new("message"),
        js.Map.new(
            [
                [1, "one"],
                [2, "two"],
                [3, "three"],
            ]
        ),
        js.Number.new("1234"),
        "some string",
        None,
        js.undefined,
        js.RegExp.new("ab+c", "i"),
        js.Set.new([1, 2, 3, 4]),
        js.Int16Array.of("10", "24"),
    ]:
        received = js.JSON.stringify(await env.PythonRpc.identity(val))
        expected = js.JSON.stringify(val)
        assert received == expected
        received = js.JSON.stringify(await env.JsRpc.identity(val))
        expected = js.JSON.stringify(val)
        assert received == expected

    # Web/API Types
    # - Response
    py_response = await env.PythonRpc.handle_response(Response("this is a response"))
    assert isinstance(
        py_response, JsProxy
    )  # TODO: Can we auto convert this to the Python types somehow?
    assert await py_response.text() == "this is a response"
    js_response = await env.JsRpc.handleResponse(Response("this is a response"))
    assert await js_response.text() == "this is a response"

    # - Request
    py_response = await env.PythonRpc.handle_request(
        Request("https://test.com", method=HTTPMethod.POST)
    )
    assert isinstance(py_response, JsProxy)
    assert py_response.method == "POST"
    js_response = await env.JsRpc.handleRequest(
        Request("https://test.com", method=HTTPMethod.POST)
    )
    assert js_response.method == "POST"

    # - Verify that a JS type can be sent.
    py_response2 = await env.PythonRpc.handle_response(js.Response.new("a JS response"))
    assert await py_response2.text() == "a JS response"

    # Functions/Lambdas

    # Verify that sending unsupported types fails.
    await assertRaises(lambda: env.PythonRpc.one_arg(CustomType(42)))
    # TODO: Should the below work?
    await assertRaises(
        lambda: env.PythonRpc.one_arg(Blob("print(42)", content_type="text/python"))
    )
    await assertRaises(
        lambda: env.PythonRpc.identity(
            to_js({"foo": 42}, dict_converter=js.Object.fromEntries)
        )
    )
