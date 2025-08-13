from js import Response
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def fetch(self, request, env):
        print(env.secret)
        return Response.new("hello world")

    def test(self, ctx, env):
        print(env.secret)
        print("Hi there, this is a test")
