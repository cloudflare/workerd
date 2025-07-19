import bs4


async def test():
    assert bs4.__version__
    # Use beautifulsoup4 to parse HTML
    soup = bs4.BeautifulSoup("<html></html>", "html.parser")
    print(soup.prettify())
    print("BeautifulSoup4 ran successfully!")
