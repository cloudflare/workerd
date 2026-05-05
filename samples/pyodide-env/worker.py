from js import Response
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def fetch(self, request):
        print(self.env.MY_VAR)
        return Response.new("hello world")

    def test(self, ctx):
        print(self.env.MY_VAR)
        print("Hi there, this is a test")
