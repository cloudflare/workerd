# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from asyncio import sleep

from js import Date
from workers import DurableObject, Request, Response, handler


class DurableObjectExample(DurableObject):
    def __init__(self, state, env):
        super().__init__(state, env)

        self.alarm_triggered = False
        self.storage = state.storage

    async def fetch(self, request):
        assert isinstance(request, Request)

        curr = await self.storage.getAlarm()
        if curr is None:
            self.storage.setAlarm(Date.now() + 100)

        return Response("alarm " + str(self.alarm_triggered))

    async def alarm(self, alarm_info):
        self.alarm_triggered = True


@handler
async def test(ctrl, env, ctx):
    id = env.ns.idFromName("A")
    obj = env.ns.get(id)

    first_resp = await obj.fetch("http://foo.com/")
    first_resp_data = await first_resp.text()
    assert first_resp_data == "alarm False"

    # Wait for alarm to get triggered.
    while True:
        await sleep(0.2)
        resp = await obj.fetch("http://foo.com/")

        alarm_triggered = await resp.text() == "alarm True"
        if alarm_triggered:
            break
