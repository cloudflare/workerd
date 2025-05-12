# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from asyncio import sleep
from urllib.parse import urlparse

from js import Date
from workers import DurableObject, Response

import pyodide


class DurableObjectExample(DurableObject):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0
        self.storage = state.storage
        self.alarm_triggered = False

    async def on_fetch(self, request):
        curr = await self.storage.getAlarm()
        if curr is None:
            self.storage.setAlarm(Date.now() + 100)

        url = urlparse(request.url)
        if url.path == "/counter":
            self.counter += 1
            return Response(f"hello from python {self.counter}")
        elif url.path == "/alarm":
            return Response(str(self.alarm_triggered))
        else:
            return Response("404")

    async def on_alarm(self, alarm_info):
        self.alarm_triggered = True

    async def no_args_method(self):
        return "value from python"

    async def args_method(self, arg):
        return "value from python " + arg

    def jspi_method(self, arg):
        from pyodide.ffi import run_sync

        run_sync(sleep(0.01))
        return arg + 1


async def test(ctrl, env, ctx):
    id = env.ns.idFromName("A")
    obj = env.ns.get(id)

    first_resp = await obj.fetch("http://foo.com/counter")
    first_resp_data = await first_resp.text()
    assert first_resp_data == "hello from python 1"

    second_resp = await obj.fetch("http://foo.com/counter")
    second_resp_data = await second_resp.text()
    assert second_resp_data == "hello from python 2"

    no_arg_resp = await obj.no_args_method()
    assert no_arg_resp == "value from python"

    arg_resp = await obj.args_method("test")
    assert arg_resp == "value from python test"

    if pyodide.__version__ != "0.26.0a2":
        res = await obj.jspi_method(9)
        assert res == 10

    # Wait for alarm to get triggered.
    while True:
        await sleep(0.2)
        resp = await obj.fetch("http://foo.com/alarm")

        alarm_triggered = await resp.text() == "True"
        if alarm_triggered:
            break
