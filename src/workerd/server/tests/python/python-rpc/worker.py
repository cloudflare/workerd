# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

import collections.abc
import json
from asyncio import Future, sleep
from datetime import datetime
from http import HTTPMethod
from unittest import TestCase

import js
from pyodide.ffi import JsException, JsProxy, to_js
from workers import Blob, Request, Response, WorkerEntrypoint, handler

assertRaises = TestCase().assertRaises
assertRaisesRegex = TestCase().assertRaisesRegex

testFuture = Future()


class PythonRpcTester(WorkerEntrypoint):
    def __init__(self, ctx, env):
        super().__init__(ctx, env)
        # Verify that the superclass constructor initialises the env/ctx fields.
        assert self.env is not None
        assert self.ctx is not None

        assert not isinstance(env, JsProxy)
        self.env = env

    async def no_args(self):
        return "hello from python"

    async def one_arg(self, x):
        return f"{x}"

    async def identity(self, x):
        assert not isinstance(x, JsProxy)
        return x

    async def handle_response(self, response):
        # Verify that we receive a Python object here...
        assert isinstance(response, Response)
        return response

    async def handle_request(self, req):
        assert isinstance(req, Request)
        return req

    async def check_env(self):
        # Verify that the `env` supplied to the entrypoint class is wrapped.
        curr_date = datetime.now()
        py_date = await self.env.PythonRpc.identity(curr_date)
        assert isinstance(py_date, datetime)
        # JavaScript Date objects only have millisecond precision
        assert abs((py_date - curr_date).microseconds) < 1e6

        return True

    async def sleep_then_set_result(self):
        await sleep(0.1)
        testFuture.set_result(100)

    async def test_wait_until_coroutine_lifetime(self):
        self.ctx.waitUntil(self.sleep_then_set_result())


class CustomType:
    def __init__(self, x):
        self.x = 42


def assert_equal(a, b, avoid_type_check):
    # Convert to JSON so that the contents of the values can be easily compared.
    received = json.dumps(
        json.loads(js.JSON.stringify(a) if isinstance(a, JsProxy) else json.dumps(a))
    )
    expected = json.dumps(
        json.loads(js.JSON.stringify(b) if isinstance(b, JsProxy) else json.dumps(b))
    )
    if received != expected:
        raise ValueError(
            f"Assert failed, args contents are not equal. received='{received}' expected='{expected}'"
        )

    if type(a) is not type(b) and not avoid_type_check:
        raise ValueError(
            f"Assert failed, types don't match. received={type(a)} expected={type(b)}"
        )


@handler
async def test(ctrl, env, ctx):
    # https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Structured_clone_algorithm#supported_types
    #
    # Workers RPC doesn't support all of the above, but does support all the JS Types and
    # some of the Web API types.
    #
    # ReadableStream and WritableStream are also supported.
    #
    # We verify that we can send and receive as much of these as possible, including between
    # Python<->Python, JS<->Python and vice versa. We also ensure that the types we receive are
    # native Python types, rather than `JsProxy`s.

    # Simple tests.
    assert await env.PythonRpc.no_args() == "hello from python"
    assert await env.JsRpc.noArgs() == "hello from js"
    assert await env.PythonRpc.one_arg(42) == "42"
    assert await env.JsRpc.oneArg(42) == "42"
    arr = await env.PythonRpc.identity([1, 2, 3])
    assert isinstance(arr, collections.abc.Sequence) and not isinstance(arr, str)

    # Verify that text bindings can be accessed.
    assert env.FOO == "text binding"

    # Python Types
    for val in ["test", [1, 2, 3], {"key": 42}, 42, 1.2345, False, True, None]:
        received = await env.PythonRpc.identity(val)
        assert not isinstance(received, JsProxy), (
            "Expected the returned value from RPC to be a Python type."
        )
        assert_equal(received, val, False)
        received = await env.JsRpc.identity(val)
        assert not isinstance(received, JsProxy), (
            "Expected the returned value from RPC to be a Python type."
        )
        assert_equal(received, val, False)

    curr_date = datetime.now()
    py_date = await env.PythonRpc.identity(curr_date)
    assert isinstance(py_date, datetime)
    # JavaScript Date objects only have millisecond precision
    assert abs((py_date - curr_date).microseconds) < 1e6

    py_date_list = await env.PythonRpc.identity([datetime.now(), datetime.now()])
    for d in py_date_list:
        assert isinstance(d, datetime)

    py_set = await env.PythonRpc.identity({1, 2, 3})
    assert isinstance(py_set, set)
    assert 2 in py_set
    assert 42 not in py_set

    py_binary = await env.PythonRpc.identity(b"binary")
    assert isinstance(py_binary, memoryview)
    py_binary = await env.PythonRpc.identity(memoryview(b"abcefg"))
    assert isinstance(py_binary, memoryview)

    # JS types
    for val in [
        to_js([1, 2, 3, 4]),
        js.Number.new("1234"),
    ]:
        received = await env.PythonRpc.identity(val)
        assert not isinstance(received, JsProxy), (
            "Expected the returned value from RPC to be a Python type."
        )
        assert_equal(received, val.to_py(), True)
        received = await env.JsRpc.identity(val)
        assert not isinstance(received, JsProxy), (
            "Expected the returned value from RPC to be a Python type."
        )
        assert_equal(received, val.to_py(), True)

    for val in [
        js.ArrayBuffer.new(8),
        js.DataView.new(js.ArrayBuffer.new(16)),
        js.Int16Array.of("10", "24"),
    ]:
        js_binary = await env.PythonRpc.identity(val)
        assert isinstance(js_binary, memoryview)
        js_binary = await env.JsRpc.identity(val)
        assert isinstance(js_binary, memoryview)

    js_map = await env.PythonRpc.identity(
        js.Map.new(
            [
                [1, "one"],
                [2, "two"],
                [3, "three"],
            ]
        ),
    )
    assert isinstance(js_map, dict)
    assert js_map[1] == "one"

    js_set = await env.PythonRpc.identity(
        js.Set.new([1, 2, 3, 4]),
    )
    assert isinstance(js_set, set)
    assert 1 in js_set
    assert 42 not in js_set

    js_undefined = await env.PythonRpc.identity(js.undefined)
    assert js_undefined is None

    js_date = await env.PythonRpc.identity(js.Date.new())
    assert isinstance(js_date, datetime)

    js_date_list = await env.PythonRpc.identity([js.Date.new(), js.Date.new()])
    for d in js_date_list:
        assert isinstance(d, datetime)

    js_exception = await env.PythonRpc.identity(js.Error.new("message"))
    assert isinstance(js_exception, Exception)

    js_obj = await env.PythonRpc.identity(
        to_js({"foo": 42}, dict_converter=js.Object.fromEntries)
    )
    assert isinstance(js_obj, dict)
    assert js_obj["foo"] == 42

    # Web/API Types
    # - Response
    py_response = await env.PythonRpc.handle_response(Response("this is a response"))
    assert isinstance(py_response, Response)
    assert await py_response.text() == "this is a response"
    js_response = await env.JsRpc.handleResponse(Response("this is a response"))
    assert await js_response.text() == "this is a response"
    assert isinstance(js_response, Response)

    # - Request
    py_response = await env.PythonRpc.handle_request(
        Request("https://test.com", method=HTTPMethod.POST)
    )
    assert isinstance(py_response, Request)
    assert py_response.method == "POST"
    js_response = await env.JsRpc.handleRequest(
        Request("https://test.com", method=HTTPMethod.POST)
    )
    assert js_response.method == "POST"
    assert isinstance(js_response, Request)

    # - Verify that a JS type can be sent.
    py_response2 = await env.PythonRpc.handle_response(js.Response.new("a JS response"))
    assert await py_response2.text() == "a JS response"

    # Verify that sending unsupported types fails.
    data_clone_regex = "^DataCloneError"
    with assertRaisesRegex(JsException, data_clone_regex):
        await env.PythonRpc.one_arg(CustomType(42))
    with assertRaisesRegex(JsException, data_clone_regex):
        await env.PythonRpc.one_arg(Blob("print(42)", content_type="text/python"))
    with assertRaisesRegex(JsException, data_clone_regex):
        await env.PythonRpc.identity(complex(1.23))
    with assertRaises(TypeError):
        await env.PythonRpc.identity((1, 2, 3))
    with assertRaisesRegex(JsException, data_clone_regex):
        await env.PythonRpc.identity(range(0, 30, 5))
    with assertRaises(TypeError):
        await env.PythonRpc.identity(bytearray.fromhex("2Ef0 F1f2  "))
    with assertRaises(TypeError):
        await env.PythonRpc.identity([(1, 2, 3)])
    # TODO: Support RegExp.
    with assertRaises(TypeError):
        await env.PythonRpc.identity(js.RegExp.new("ab+c", "i"))
    with assertRaises(TypeError):
        await env.PythonRpc.identity(lambda x: x + x)
    with assertRaises(TypeError):

        def my_func():
            pass

        await env.PythonRpc.identity(my_func)
    with assertRaises(TypeError):
        await env.PythonRpc.identity({"test": (1, 2, 3)})

    # Verify that the `env` in the DO is correctly wrapped.
    assert await env.PythonRpc.check_env()

    # Check that the coroutine returned by sleep_then_set_result() lasts long enough.
    # sleep_then_set_result() resolves testFuture after sleeping for 100ms.
    await env.PythonRpc.test_wait_until_coroutine_lifetime()
    assert not testFuture.done()
    await sleep(0.2)
    assert testFuture.result() == 100
