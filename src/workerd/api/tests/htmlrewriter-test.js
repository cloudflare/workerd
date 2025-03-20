import {
  strictEqual,
  deepStrictEqual,
  notStrictEqual,
  rejects,
  throws,
} from 'node:assert';

const arrayIterator = [][Symbol.iterator]();
const arrayIteratorPrototype = Object.getPrototypeOf(
  Object.getPrototypeOf(arrayIterator)
);

export const passthroughWithContent = {
  async test() {
    const response = new Response('hello', {
      headers: {
        foo: 'bar',
      },
    });
    strictEqual(response.headers.get('foo'), 'bar');

    const rewriter = new HTMLRewriter();

    const newResponse = rewriter.transform(response);

    notStrictEqual(response, newResponse);
    strictEqual(newResponse.headers.get('foo'), 'bar');
    strictEqual(await newResponse.text(), 'hello');
  },
};

export const passthroughWithContentAndHandler = {
  async test() {
    const response = new Response('<h3>hello</h3>', {
      headers: {
        foo: 'bar',
      },
    });
    strictEqual(response.headers.get('foo'), 'bar');

    const rewriter = new HTMLRewriter().on('h3', {
      element() {},
      comments() {},
      text() {},
    });

    const newResponse = rewriter.transform(response);

    notStrictEqual(response, newResponse);
    strictEqual(newResponse.headers.get('foo'), 'bar');
    strictEqual(await newResponse.text(), '<h3>hello</h3>');
  },
};

export const passthroughWithContentAndAsyncHandler = {
  async test() {
    const response = new Response('<h3>hello</h3>', {
      headers: {
        foo: 'bar',
      },
    });
    strictEqual(response.headers.get('foo'), 'bar');

    const rewriter = new HTMLRewriter().on('h3', {
      async test() {
        await scheduler.wait(10);
      },
      async element() {
        await scheduler.wait(10);
      },
      async comments() {
        await scheduler.wait(10);
      },
    });

    const newResponse = rewriter.transform(response);

    notStrictEqual(response, newResponse);
    strictEqual(newResponse.headers.get('foo'), 'bar');
    strictEqual(await newResponse.text(), '<h3>hello</h3>');
  },
};

export const passthroughWithContentAndAsyncHandler2 = {
  async test() {
    const response = new Response('<h3>hello</h3>', {
      headers: {
        foo: 'bar',
      },
    });
    strictEqual(response.headers.get('foo'), 'bar');

    const { promise, resolve } = Promise.withResolvers();

    const rewriter = new HTMLRewriter().on('h3', {
      async test() {
        await promise;
      },
      async element() {
        await scheduler.wait(10);
      },
      async comments() {
        await scheduler.wait(10);
      },
    });

    const newResponse = rewriter.transform(response);

    // We can resolve the promise after calling transform and the transform
    // will complete as expected.
    resolve();

    notStrictEqual(response, newResponse);
    strictEqual(newResponse.headers.get('foo'), 'bar');
    strictEqual(await newResponse.text(), '<h3>hello</h3>');
  },
};

export const passthroughWithContentStream = {
  async test() {
    const { readable, writable } = new TransformStream();

    const rewriter = new HTMLRewriter().on('h3', {
      async text(content) {
        await scheduler.wait(10);
      },
    });

    const response = rewriter.transform(new Response(readable));

    const writer = writable.getWriter();
    const enc = new TextEncoder();
    const results = await Promise.all([
      response.text(),
      writer.write(enc.encode('<h3>hello</h3>')),
      writer.close(),
    ]);

    strictEqual(results[0], '<h3>hello</h3>');
  },
};

export const passthroughWithEmptyStream = {
  async test() {
    const { readable, writable } = new TransformStream();

    const rewriter = new HTMLRewriter().on('h3', {
      async text(content) {
        await scheduler.wait(10);
      },
    });

    const response = rewriter.transform(new Response(readable));

    const writer = writable.getWriter();
    const results = await Promise.all([response.text(), writer.close()]);

    strictEqual(results[0], '');
  },
};

export const asyncElementHandler = {
  async test() {
    const rewriter = new HTMLRewriter().on('body', {
      async element(e) {
        await scheduler.wait(10);
        e.setInnerContent('world');
      },
    });

    const response = rewriter.transform(new Response('<body>hello</body>'));

    strictEqual(await response.text(), '<body>world</body>');
  },
};

export const asyncCommentHandler = {
  async test() {
    const rewriter = new HTMLRewriter().on('body', {
      async comments(comment) {
        await scheduler.wait(10);
        if (comment.text == 'hello') {
          comment.text = 'world';
        }
      },
    });

    const response = rewriter.transform(
      new Response('<body><!--hello--></body>')
    );

    strictEqual(await response.text(), '<body><!--world--></body>');
  },
};

export const objectHandlers = {
  async test() {
    class DocumentContentHandlers {
      deadTokens = {};
      doctypeCount = 0;
      commentCount = 0;
      textCount = 0;
      expectedErrors = [];

      doctype(token) {
        if (!this.deadTokens.doctype) {
          this.deadTokens.doctype = token;
        }
        ++this.doctypeCount;

        this.sawDoctype = JSON.stringify(token);
      }

      comments(token) {
        if (!this.deadTokens.comment) {
          this.deadTokens.comment = token;
        }
        ++this.commentCount;
      }

      text(token) {
        if (!this.deadTokens.text) {
          this.deadTokens.text = token;
        }
        ++this.textCount;
      }

      end(token) {
        this.reachedEnd = true;
      }
    }

    class ElementContentHandlers {
      deadTokens = {};
      elementCount = 0;
      commentCount = 0;
      textCount = 0;
      expectedErrors = [];

      element(token) {
        if (!this.deadTokens.element) {
          this.deadTokens.element = token;
        }
        if (!this.deadTokens.attributesIterator) {
          this.deadTokens.attributesIterator = token.attributes;
        }
        ++this.elementCount;

        // Exercise all the different methods on Element.
        if (
          token.tagName === 'body' &&
          token.hasAttribute('foo') &&
          !token.hasAttribute('baz') &&
          token.getAttribute('foo') === 'bar'
        ) {
          token.removeAttribute('foo');
          token.setAttribute('baz', 'qux');

          try {
            token.tagName = 'should throw';
            throw new Error('should have thrown');
          } catch (e) {
            this.expectedErrors.push(e.message);
          }

          token.tagName = 'tail';

          // These will show up in order in the response body.
          token.before('<1>');
          token.before('<2>', { html: false });
          token.before('<3>\n', null);
          token.before('<html>', { html: true });

          // These will show up in reverse order in the response body.
          token.prepend('<em>hello</em>, ', { html: true });
          token.prepend('<6>\n');
          token.prepend('<5>', { html: false });
          token.prepend('\n<4>', null);

          // Iterator tests.

          this.sawAttributes = JSON.stringify([...token.attributes]);

          let iterator = token.attributes;
          let iteratorPrototype = Object.getPrototypeOf(
            Object.getPrototypeOf(iterator)
          );
          if (iteratorPrototype !== arrayIteratorPrototype) {
            throw new Error(
              'attributes iterator does not have iterator prototype'
            );
          }

          // Run the iterator down until it's done.
          for (let [k, v] of iterator) {
          }
          // .next() should now be idempotent.
          let result = iterator.next();
          let result2 = iterator.next();
          if (
            result.done !== result2.done ||
            result.value !== result2.value ||
            !result.done ||
            result.value
          ) {
            throw new Error(
              'exhausted iterator should continually return done'
            );
          }
        } else if (token.tagName === 'remove') {
          let mode = token.getAttribute('mode');
          if (mode === null) {
            throw new Error("missing attribute on 'remove' element");
          }
          if (token.removed) {
            throw new Error('element should not have been removed yet');
          }

          if (mode === 'all') {
            token.remove();
          } else {
            token.removeAndKeepContent();
          }

          if (!token.removed) {
            throw new Error('element should have been removed now');
          }
        } else if (token.tagName === 'after') {
          let isHtml = token.getAttribute('is-html');
          let html = isHtml === 'true' ? true : false;
          token.after('<after>', { html });
        } else if (token.tagName === 'append') {
          let isHtml = token.getAttribute('is-html');
          let html = isHtml === 'true' ? true : false;
          token.append('<append>', { html });
        } else if (token.tagName === 'replace') {
          let isHtml = token.getAttribute('is-html');
          let html = isHtml === 'true' ? true : false;
          token.replace('<replace>', { html });
        } else if (token.tagName === 'set-inner-content') {
          let isHtml = token.getAttribute('is-html');
          let html = isHtml === 'true' ? true : false;
          token.setInnerContent('<set-inner-content>', { html });
        } else if (token.tagName === 'set-attribute') {
          if (!token.hasAttribute('foo')) {
            throw new Error('element should have had attribute');
          }
          let attr = token.getAttribute('foo');
          if (attr !== '') {
            throw new Error('element attribute should have been empty');
          }
          token.setAttribute('foo', 'bar');

          if (token.getAttribute('nonexistent')) {
            throw new Error('attribute should not exist');
          }
        }
      }

      comments(token) {
        if (!this.deadTokens.comment) {
          this.deadTokens.comment = token;
        }
        ++this.commentCount;

        // Exercise all the different methods on Comment.
        if (token.text === ' SET TEXT PROPERTY ') {
          token.text = ' text property has been set ';
        } else if (token.text === ' REMOVE ME ') {
          if (token.removed) {
            throw new Error("Shouldn't be removed yet");
          }

          token.remove();

          if (!token.removed) {
            throw new Error('Should be removed now');
          }
        } else if (token.text === ' REPLACE ME ') {
          if (token.removed) {
            throw new Error("Shouldn't be removed yet");
          }

          token.replace('this will get overwritten');

          if (!token.removed) {
            throw new Error('Should be removed now');
          }

          token.replace('<REPLACED>', null);

          if (!token.removed) {
            throw new Error('Should still be removed');
          }

          token.before('<!-- ', { html: true });
          token.after(' -->', { html: true });
        }
      }

      text(token) {
        if (!this.deadTokens.text) {
          this.deadTokens.text = token;
        }
        ++this.textCount;

        if (token.lastInTextNode && token.text.length > 0) {
          throw new Error('last text chunk has non-zero length');
        } else if (!token.lastInTextNode && token.text.length === 0) {
          throw new Error('non-last text chunk has zero length');
        }

        if (token.text === 'world') {
          token.before('again, ');
          token.after('...');

          if (token.removed) {
            throw new Error("Shouldn't be removed yet");
          }

          token.replace('this will get overwritten');

          if (!token.removed) {
            throw new Error('Should be removed now');
          }

          token.replace('<WORLD>', { html: true });

          if (!token.removed) {
            throw new Error('Should still be removed');
          }
        } else if (token.text === 'REMOVE ME\n') {
          if (token.removed) {
            throw new Error("Shouldn't be removed yet");
          }

          token.remove();

          if (!token.removed) {
            throw new Error('Should be removed now');
          }
        }
      }
    }

    let documentHandlers = new DocumentContentHandlers();
    let elementHandlers = new ElementContentHandlers();

    const rewriter = new HTMLRewriter()
      .onDocument(documentHandlers)
      .on('*', elementHandlers);

    let count = 0;

    const enc = new TextEncoder();
    const kInput = [
      '<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01//EN">',
      '<!-- document-level comment -->',
      'document-level text',
      '<body foo="bar" an-attr="ibute?">world<p>REMOVE ME',
      '<remove mode="all">inner content</remove>',
      '<remove mode="keep">inner content</remove>',
      '<after is-html="true">inner content</after>',
      '<after is-html="false">inner content</after>',
      '<append is-html="true">inner content</append>',
      '<append is-html="false">inner content</append>',
      '<replace is-html="true">inner content</replace>',
      '<replace is-html="false">inner content</replace>',
      '<set-inner-content is-html="true">inner content</set-inner-content>',
      '<set-inner-content is-html="false">inner content</set-inner-content>',
      '<set-attribute foo></set-attribute>',
      '<set-attribute foo=></set-attribute>',
      '<set-attribute foo=""></set-attribute>',
      '<!-- REMOVE ME -->',
      '<!-- REPLACE ME -->',
      '<!-- SET TEXT PROPERTY -->',
      '</body>',
    ];

    const kResult = `<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01//EN"><!-- document-level comment -->document-level text&lt;1&gt;&lt;2&gt;&lt;3&gt;
<html><tail an-attr="ibute?" baz="qux">
&lt;4&gt;&lt;5&gt;&lt;6&gt;
<em>hello</em>, again, <WORLD>...<p>REMOVE MEinner content<after is-html="true">inner content</after><after><after is-html="false">inner content</after>&lt;after&gt;<append is-html="true">inner content<append></append><append is-html="false">inner content&lt;append&gt;</append><replace>&lt;replace&gt;<set-inner-content is-html="true"><set-inner-content></set-inner-content><set-inner-content is-html="false">&lt;set-inner-content&gt;</set-inner-content><set-attribute foo="bar"></set-attribute><set-attribute foo="bar"></set-attribute><set-attribute foo="bar"></set-attribute><!-- &lt;REPLACED&gt; --><!-- text property has been set --></tail>`;

    const readable = new ReadableStream({
      async pull(controller) {
        await scheduler.wait(1);
        if (kInput.length > 0) {
          controller.enqueue(enc.encode(kInput.shift()));
        } else {
          controller.close();
        }
      },
    });

    const response = rewriter.transform(new Response(readable));

    // At this point, we should not have seen any tokens.
    strictEqual(elementHandlers.deadTokens.element, undefined);

    strictEqual(await response.text(), kResult);

    // Now we've seen tokens
    notStrictEqual(elementHandlers.deadTokens.element, undefined);

    // Verify that tokens are invalidated outside handler execution scope.
    throws(() => documentHandlers.deadTokens.doctype.publicId, {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });
    throws(() => documentHandlers.deadTokens.comment.text, {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });
    throws(() => documentHandlers.deadTokens.text.text, {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });
    throws(() => elementHandlers.deadTokens.element.getAttribute('foo'), {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });
    throws(() => elementHandlers.deadTokens.attributesIterator.next(), {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });
    throws(() => elementHandlers.deadTokens.comment.text, {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });
    throws(() => elementHandlers.deadTokens.text.text, {
      message:
        'This content token is no longer valid. Content tokens are only ' +
        'valid during the execution of the relevant content handler.',
    });

    strictEqual(documentHandlers.doctypeCount, 1);
    strictEqual(documentHandlers.commentCount, 4);
    strictEqual(documentHandlers.textCount, 26);
    strictEqual(documentHandlers.reachedEnd, true);
    strictEqual(elementHandlers.elementCount, 15);
    strictEqual(elementHandlers.commentCount, 3);
    strictEqual(elementHandlers.textCount, 24);
    deepStrictEqual(elementHandlers.expectedErrors, [
      'Parser error: ` ` character is forbidden in the tag name',
    ]);
  },
};

export const manualWriting = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const response = new HTMLRewriter()
      .on('*', {
        element(element) {
          element.prepend('foo ');
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    // Because this variation uses IdentityTransformStream, we must
    // initiate the read before doing the writes.
    const promise = response.text();

    await writer.write(encoder.encode('<html>'));
    await writer.write(encoder.encode('bar'));
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    strictEqual(await promise, '<html>foo bar</html>');
  },
};

export const manualWriting2 = {
  async test() {
    const { readable, writable } = new TransformStream();

    const response = new HTMLRewriter()
      .on('*', {
        element(element) {
          element.prepend('foo ');
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    await writer.write(encoder.encode('<html>'));
    await writer.write(encoder.encode('bar'));
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    // This variation uses the JavaScript TransformStream, so we can
    // initiate the read after doing the writes.
    const promise = response.text();

    strictEqual(await promise, '<html>foo bar</html>');
  },
};

export const streamingReplacement = {
  async test() {
    const { readable, writable } = new TransformStream();

    const response = new HTMLRewriter()
      .on('*', {
        async element(element) {
          const dataStream = (
            await fetch('data:,the quick brown fox jumped over the lazy dog%20')
          ).body;
          element.prepend(dataStream, { html: false });
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    await writer.write(encoder.encode('<html>'));
    await writer.write(encoder.encode('bar'));
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    // This variation uses the JavaScript TransformStream, so we can
    // initiate the read after doing the writes.
    const promise = response.text();
    strictEqual(
      await promise,
      `<html>the quick brown fox jumped over the lazy dog bar</html>`
    );
  },
};

export const streamingReplacementHTML = {
  async test() {
    const { readable, writable } = new TransformStream();

    const response = new HTMLRewriter()
      .on('*', {
        async element(element) {
          const dataStream = (
            await fetch('data:,<b>such markup <i>much wow</i></b> ')
          ).body;
          element.prepend(dataStream, { html: true });
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    await writer.write(encoder.encode('<html>'));
    await writer.write(encoder.encode('bar'));
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    // This variation uses the JavaScript TransformStream, so we can
    // initiate the read after doing the writes.
    const promise = response.text();
    strictEqual(
      await promise,
      `<html><b>such markup <i>much wow</i></b>bar</html>`
    );
  },
};

export const streamingReplacementReplace = {
  async test() {
    const { readable, writable } = new TransformStream();

    const response = new HTMLRewriter()
      .on('.dinosaur', {
        async element(element) {
          const dataStream = (await fetch('data:,goodbye world')).body;
          element.replace(dataStream, { html: false });
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    await writer.write(encoder.encode('<html>'));
    await writer.write(
      encoder.encode('<div class="dinosaur">hello world</div>')
    );
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    // This variation uses the JavaScript TransformStream, so we can
    // initiate the read after doing the writes.
    const promise = response.text();
    strictEqual(await promise, `<html>goodbye world</html>`);
  },
};

export const streamingReplacementMultiple = {
  async test() {
    const { readable, writable } = new TransformStream();

    const response = new HTMLRewriter()
      .on('*', {
        async element(element) {
          element.prepend(await fetch('data:,alpha%20'));
          element.append(await fetch('data:,%20gamma'));
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    await writer.write(encoder.encode('<html>'));
    await writer.write(encoder.encode('beta'));
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    // This variation uses the JavaScript TransformStream, so we can
    // initiate the read after doing the writes.
    const promise = response.text();
    strictEqual(await promise, `<html>alpha beta gamma</html>`);
  },
};

export const streamingReplacementBadUTF8 = {
  async test() {
    const { readable, writable } = new TransformStream();

    const response = new HTMLRewriter()
      .on('*', {
        async element(element) {
          element.prepend(await fetch('data:,garbage%e2%28%a1'));
        },
      })
      .transform(new Response(readable));

    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    await writer.write(encoder.encode('<html>'));
    await writer.write(encoder.encode('bar'));
    await writer.write(encoder.encode('</html>'));
    await writer.close();

    // This variation uses the JavaScript TransformStream, so we can
    // initiate the read after doing the writes.
    await rejects(response.text(), { message: 'Parser error: Invalid UTF-8' });
  },
};

export const appendOnEnd = {
  async test() {
    const kInput =
      '<!doctype html><html><head></head><body><!----></body></html>';
    const kSuffix = '<!-- an end comment -->';
    const result = new HTMLRewriter()
      .onDocument({
        end(end) {
          end.append(kSuffix, { html: true });
        },
      })
      .transform(new Response(kInput));
    strictEqual(await result.text(), kInput + kSuffix);
  },
};

export const interleavedAsyncHandlers = {
  async test() {
    class OneTimeBarrier {
      constructor(limit) {
        this.limit = limit;
        this.current = 0;
        let resolve;
        this.promise = new Promise((r) => (resolve = r));
        this.resolver = resolve;
      }

      async wait() {
        this.current += 1;
        if (this.current >= this.limit) {
          this.resolver();
        } else {
          await this.promise;
        }
      }
    }

    const barrier = new OneTimeBarrier(2);
    const responses = await Promise.all([
      new HTMLRewriter()
        .on('body', {
          async element(e) {
            await barrier.wait();
            e.setInnerContent('foo bar');
          },
        })
        .transform(new Response('<body>body value</body>'))
        .text(),
      new HTMLRewriter()
        .on('body', {
          async element(e) {
            await barrier.wait();
            e.remove();
          },
        })
        .transform(
          new Response('<!doctype html><html><head></head><body></body></html>')
        )
        .arrayBuffer(),
    ]);

    const body = responses[0];
    const blank = responses[1];

    const inserted = new HTMLRewriter()
      .on('html', {
        element(e) {
          e.append(body, { html: true });
        },
      })
      .transform(new Response(blank));

    strictEqual(
      await inserted.text(),
      '<!doctype html><html><head></head><body>foo bar</body></html>'
    );
  },
};

export const exceptionInHandler = {
  async test() {
    const response = new HTMLRewriter()
      .on('*', {
        text() {
          throw new Error('boom');
        },
      })
      .transform(new Response('<body>hello</body>'));

    await rejects(response.text(), {
      message: 'boom',
    });
  },
};

export const exceptionInAsyncHandler = {
  async test() {
    const response = new HTMLRewriter()
      .on('*', {
        async text() {
          throw new Error('boom');
        },
      })
      .transform(new Response('<body>hello</body>'));

    await rejects(response.text(), {
      message: 'boom',
    });
  },
};

export const invalidEncoding = {
  async test() {
    const response = new Response('hello', {
      headers: {
        'content-type': 'text/html; charset=invalid',
      },
    });

    throws(
      () => {
        new HTMLRewriter().on('*', {}).transform(response);
      },
      {
        message: 'Parser error: Unknown character encoding has been provided.',
      }
    );
  },
};

export const exceptionPropagation = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const response = new HTMLRewriter().transform(new Response(readable));
    response.body.cancel(new Error('boom'));

    const writer = writable.getWriter();

    const enc = new TextEncoder();

    await writer.write(enc.encode('test'));

    rejects(writer.write(enc.encode('test')), {
      message: 'boom',
    });
  },
};

export const sameToken = {
  async test() {
    const obj = {};
    let element;

    const r = new HTMLRewriter()
      .on('*', {
        element(e) {
          element = e;
          strictEqual(e.hi, undefined);

          e.hi = 'test';
          strictEqual(e.hi, 'test');

          e.hi = 'hi';
          e.obj = obj;

          e.replace('foo');
        },
      })
      .on('img', {
        element(e) {
          notStrictEqual(e, element);
          notStrictEqual(e.hi, 'hi');
          notStrictEqual(e.obj, obj);

          // The HTMLRewriter creates a fresh new Element/Doctype/Text
          // object for each handler, thus `e.hi` will yield undefined even if we
          // assigned it in the first handler.
          // See https://jira.cfdata.org/browse/EW-2200.
          e.replace(e.hi);
        },
      })
      .transform(new Response('<img />'))
      .text();

    await r;
  },
};

export const svgNamespace = {
  async test() {
    const response = new Response(`
    <svg><a /></svg>
  `);
    let namespace;

    await new HTMLRewriter()
      .on('a', {
        element(e) {
          namespace = e.namespaceURI;
        },
      })
      .transform(response)
      .text();

    strictEqual(namespace, 'http://www.w3.org/2000/svg');
  },
};

// This test is commented out because it will cause a stack overflow in workerd if enabled.
// It is included for reference.

/*
export const stackOverflow1 = {
  // One big handler
  async test() {
    const promiseDepth = 128 * 100;
    const numReads = 2;
    const input = new Response('<div></div>');

    const output = new HTMLRewriter()
      .onDocument({
        end(e) {
          for (let i = 0; i < promiseDepth; i++) {
            e.append('<div></div>', { html: true });
          }
        },
      })
      .transform(input);

    // Begin reading but do not fully consume
    const reader = output.body.getReader();
    for (let i = 0; i < numReads; i++) {
      await reader.read();
    }
  },
};*/

export const stackOverflow2 = {
  // Many small handlers
  async test() {
    const promiseDepth = 128 * 100;
    const numReads = promiseDepth;

    const input = new Response('<div></div>'.repeat(promiseDepth));

    const output = new HTMLRewriter()
      .on('div', {
        element(e) {
          e.append('<div></div>', { html: true });
        },
      })
      .transform(input);

    // Begin reading but do not fully consume
    const reader = output.body.getReader();
    for (let i = 0; i < numReads; i++) {
      await reader.read();
    }
  },
};
