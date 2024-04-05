from js import Response
from langchain_core.prompts import PromptTemplate
from langchain_openai import OpenAI

API_KEY = "sk-abcdefg"

async def test(request):
  prompt = PromptTemplate.from_template("Complete the following sentence: I am a {profession} and ")
  llm = OpenAI(api_key=API_KEY)
  chain = prompt | llm

  res = await chain.ainvoke({"profession": "electrician"})
  print(res)
