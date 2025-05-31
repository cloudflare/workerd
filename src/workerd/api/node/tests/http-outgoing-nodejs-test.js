import { strictEqual, throws, deepStrictEqual } from 'node:assert';
import { OutgoingMessage, ClientRequest } from 'node:http';

// Test is taken from Node.js, test/parallel/test-http-outgoing-proto.js
export const testHttpOutgoingProto = {
  async test() {
    strictEqual(typeof ClientRequest.prototype._implicitHeader, 'function');

    // validateHeader
    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader();
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Header name must be a valid HTTP token ["undefined"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader('test');
      },
      {
        code: 'ERR_HTTP_INVALID_HEADER_VALUE',
        name: 'TypeError',
        message: 'Invalid value "undefined" for header "test"',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader(404);
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Header name must be a valid HTTP token ["404"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader.call({ _header: 'test' }, 'test', 'value');
      },
      {
        code: 'ERR_HTTP_HEADERS_SENT',
        name: 'Error',
        message: 'Cannot set headers after they are sent to the client',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader('200', 'あ');
      },
      {
        code: 'ERR_INVALID_CHAR',
        name: 'TypeError',
        message: 'Invalid character in header content ["200"]',
      }
    );

    // write
    {
      const outgoingMessage = new OutgoingMessage();

      throws(
        () => {
          outgoingMessage.write('');
        },
        {
          code: 'ERR_METHOD_NOT_IMPLEMENTED',
          name: 'Error',
          message: 'The _implicitHeader() method is not implemented',
        }
      );
    }

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.write.call({ _header: 'test', _hasBody: 'test' });
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
        message:
          'The "chunk" argument must be of type string or an instance of ' +
          'Buffer or Uint8Array. Received undefined',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.write.call({ _header: 'test', _hasBody: 'test' }, 1);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
        message:
          'The "chunk" argument must be of type string or an instance of ' +
          'Buffer or Uint8Array. Received type number (1)',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.write.call({ _header: 'test', _hasBody: 'test' }, null);
      },
      {
        code: 'ERR_STREAM_NULL_VALUES',
        name: 'TypeError',
      }
    );

    // addTrailers()
    // The `Error` comes from the JavaScript engine so confirm that it is a
    // `TypeError` but do not check the message. It will be different in different
    // JavaScript engines.
    throws(() => {
      const outgoingMessage = new OutgoingMessage();
      outgoingMessage.addTrailers();
    }, TypeError);

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.addTrailers({ あ: 'value' });
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Trailer name must be a valid HTTP token ["あ"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.addTrailers({ 404: 'あ' });
      },
      {
        code: 'ERR_INVALID_CHAR',
        name: 'TypeError',
        message: 'Invalid character in trailer content ["404"]',
      }
    );

    {
      const outgoingMessage = new OutgoingMessage();
      strictEqual(outgoingMessage.destroyed, false);
      outgoingMessage.destroy();
      strictEqual(outgoingMessage.destroyed, true);
    }
  },
};

// Test is taken from Node.js, test/parallel/test-http-outgoing-renderHeaders.js
export const testHttpOutgoingRenderHeaders = {
  async test() {
    const kOutHeaders = require('internal/http').kOutHeaders;
    const http = require('http');
    const OutgoingMessage = http.OutgoingMessage;

    {
      const outgoingMessage = new OutgoingMessage();
      outgoingMessage._header = {};
      throws(() => outgoingMessage._renderHeaders(), {
        code: 'ERR_HTTP_HEADERS_SENT',
        name: 'Error',
        message: 'Cannot render headers after they are sent to the client',
      });
    }

    {
      const outgoingMessage = new OutgoingMessage();
      outgoingMessage[kOutHeaders] = null;
      const result = outgoingMessage._renderHeaders();
      deepStrictEqual(result, {});
    }

    {
      const outgoingMessage = new OutgoingMessage();
      outgoingMessage[kOutHeaders] = {};
      const result = outgoingMessage._renderHeaders();
      deepStrictEqual(result, {});
    }

    {
      const outgoingMessage = new OutgoingMessage();
      outgoingMessage[kOutHeaders] = {
        host: ['host', 'nodejs.org'],
        origin: ['Origin', 'localhost'],
      };
      const result = outgoingMessage._renderHeaders();
      deepStrictEqual(result, {
        host: 'nodejs.org',
        Origin: 'localhost',
      });
    }
  },
};
