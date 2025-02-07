import numpy as np
from js import Response


def on_fetch(request):
  res = np.arange(12).reshape((3, -1))[::-2, ::-2]
  return Response.new(str(res))

