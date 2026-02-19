export const HtmlRewriterCancelBeforeRead = {
  async test() {
    for (let i = 0; i < 50; i++) {
      const rewriter = new HTMLRewriter().on('div', {
        element(element) {
          element.setInnerContent('<span>x</span>', { html: true });
        },
      });

      const inputStream = new ReadableStream({
        start(controller) {
          controller.enqueue(
            new TextEncoder().encode(
              '<html><body><div>content</div></body></html>'
            )
          );
          controller.close();
        },
      });

      const transformed = rewriter.transform(new Response(inputStream));
      transformed.body.cancel();
    }
  },
};
