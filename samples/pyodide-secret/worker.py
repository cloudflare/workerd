from js import Response


def on_fetch(request, env):
    print(env.secret)
    return Response.new("hello world")


def test(ctx, env):
    print(env.secret)
    print("Hi there, this is a test")
