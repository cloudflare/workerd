"""
This just tests that we can import all the dependencies and set up the
ChatOpenAI session. We don't make any requests here, so it doesn't check that
part.

TODO: update this to test that something happened
"""
from langchain_core.prompts import PromptTemplate
from langchain_openai import OpenAI

def test():
    prompt = PromptTemplate.from_template("Complete the following sentence: I am a {profession} and ")
    llm = OpenAI(api_key="abc123")
    chain = prompt | llm
