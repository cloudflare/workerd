import {inherits} from 'node-internal:http_util';
import {Readable} from 'node-internal:streams_readable';
import {Buffer} from 'node-internal:internal_buffer';
import * as process from 'node-internal:process';

var rStates = {
  UNSENT: 0,
  OPENED: 1,
  HEADERS_RECEIVED: 2,
  LOADING: 3,
  DONE: 4
}
export {
  rStates as readyStates
};
export function IncomingMessage(xhr, response, mode) {
  var self = this
  Readable.call(self)

  self._mode = mode
  self.headers = {}
  self.rawHeaders = []
  self.trailers = {}
  self.rawTrailers = []

  // Fake the 'close' event, but only once 'end' fires
  self.on('end', function() {
    // The nextTick is necessary to prevent the 'request' module from causing an infinite loop
    process.nextTick(function() {
      self.emit('close')
    })
  })
  var read;
  self._fetchResponse = response

  self.url = response.url
  self.statusCode = response.status
  self.statusMessage = response.statusText
    // backwards compatible version of for (<item> of <iterable>):
    // for (var <item>,_i,_it = <iterable>[Symbol.iterator](); <item> = (_i = _it.next()).value,!_i.done;)
  for (var header, _i, _it = response.headers[Symbol.iterator](); header = (_i = _it.next()).value, !_i.done;) {
    self.headers[header[0].toLowerCase()] = header[1]
    self.rawHeaders.push(header[0], header[1])
  }

  // TODO: this doesn't respect backpressure. Once WritableStream is available, this can be fixed
  var reader = response.body.getReader()

  read = function () {
    reader.read().then(function(result) {
      if (self._destroyed)
        return
      if (result.done) {
        self.push(null)
        return
      }
      self.push(new Buffer(result.value))
      read()
    })
  }
  read()
}

inherits(IncomingMessage, Readable)

IncomingMessage.prototype._read = function() {}
