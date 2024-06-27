
from js import Response

import io

import code

from io import StringIO
import sys

sys.stdout = StringIO()

ii = code.InteractiveInterpreter()

async def on_fetch(request, env):
  cmd = (await request.json()).cmd

  ii.runsource(cmd)

  res = sys.stdout.getvalue()
  sys.stdout = StringIO()

  return Response.new(res)
