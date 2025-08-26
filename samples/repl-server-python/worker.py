import code
import sys
from io import StringIO

from js import Response
from workers import WorkerEntrypoint

sys.stdout = StringIO()

ii = code.InteractiveInterpreter()


class Default(WorkerEntrypoint):
    async def fetch(self, request):
        cmd = (await request.json()).cmd

        ii.runsource(cmd)

        res = sys.stdout.getvalue()
        sys.stdout = StringIO()

        return Response.new(res)
