"""
This just tests that we can import all the dependencies and set up the
ChatOpenAI session. We don't make any requests here, so it doesn't check that
part.

TODO: update this to test that something happened
"""


def test():
    from langchain.chat_models import ChatOpenAI
    import openai

    API_KEY = "sk-abcdefgh"

    ChatOpenAI(openai_api_key=API_KEY)
