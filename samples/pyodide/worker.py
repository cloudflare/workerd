from js import Response


def onfetch(request):
  return Response.new("hello world")
