from js import Response


def fetch(request):
  return Response.new("hello world")

def test():
  import aiohttp
  print("hi")
