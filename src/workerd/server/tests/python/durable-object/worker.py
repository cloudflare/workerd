# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from asyncio import sleep
from urllib.parse import urlparse

from js import Date
from workers import DurableObject, FetchResponse, Request, Response, WorkerEntrypoint

import pyodide


class MixinTest:
    def test_mixin(self):
        return 1234


class DurableObjectExample(DurableObject, MixinTest):
    def __init__(self, state, env):
        super().__init__(state, env)
        assert self.env is not None
        assert self.ctx is not None

        self.state = state
        self.counter = 0
        self.storage = state.storage
        self.alarm_triggered = False

    async def fetch(self, request):
        assert isinstance(request, Request)

        curr = await self.storage.getAlarm()
        # TODO(EW-9548): It seems that running this code when taking a snapshot results in the alarm state
        # being preserved in the snapshot. But this code doesn't run before snapshot generation
        # occurs, so this shouldn't happen. So if you disable that logic (search for
        # "quitting without running test"), the alarm will never trigger and this test fails.
        if not curr:
            self.storage.setAlarm(Date.now() + 100)

        url = urlparse(request.url)
        if url.path == "/counter":
            self.counter += 1
            return Response(f"hello from python {self.counter}")
        elif url.path == "/alarm":
            return Response(str(self.alarm_triggered))
        else:
            return Response("404")

    async def alarm(self, alarm_info):
        self.alarm_triggered = True

    async def no_args_method(self):
        return "value from python"

    async def args_method(self, arg):
        return "value from python " + arg

    def mutate_dict(self, my_dict):
        my_dict["foo"] = 42

    async def test_self_call(self):
        test_dict = dict()
        test_dict["test"] = 1
        self.mutate_dict(test_dict)
        assert test_dict["test"] == 1
        assert test_dict["foo"] == 42
        return True

    def jspi_method(self, arg):
        from pyodide.ffi import run_sync

        run_sync(sleep(0.01))
        return arg + 1


class Default(WorkerEntrypoint):
    async def test(self, ctrl):
        id = self.env.ns.idFromName("A")
        obj = self.env.ns.get(id)

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

        assert await obj.test_self_call()

        if pyodide.__version__ != "0.26.0a2":
            res = await obj.jspi_method(9)
            assert res == 10

        # Verify that a mixin method can be called via RPC.
        assert await obj.test_mixin() == 1234

        # Verify that DO fetch is wrapped.
        third_resp = await obj.fetch("http://foo.com/counter")
        assert isinstance(third_resp, FetchResponse)
        third_resp_data = await third_resp.text()
        assert third_resp_data == "hello from python 3"

        # Wait for alarm to get triggered.
        for _ in range(20):
            await sleep(0.2)
            resp = await obj.fetch("http://foo.com/alarm")

            alarm_triggered = await resp.text() == "True"
            if alarm_triggered:
                break
        else:
            raise AssertionError("Alarm never triggered")
