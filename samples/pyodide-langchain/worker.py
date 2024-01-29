from js import Response


def fetch(request):
    return Response.new("hello world")


from langchain.chat_models import ChatOpenAI
import openai

API_KEY = "sk-abcdefgh"


def test():
    ChatOpenAI(openai_api_key=API_KEY)
    print("OK?")
