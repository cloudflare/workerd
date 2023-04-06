import {inherits} from 'node-internal:http_util';
import {IncomingMessage, readyStates as rStates} from 'node-internal:http_response';
import {Writable} from 'node-internal:streams_writable';
import {Buffer} from 'node-internal:internal_buffer';

export default ClientRequest;

function ClientRequest(opts) {
  var self = this
  Writable.call(self)

  self._opts = opts
  self._body = []
  self._headers = {}
  if (opts.auth)
    self.setHeader('Authorization', 'Basic ' + new Buffer(opts.auth).toString('base64'))
  Object.keys(opts.headers).forEach(function(name) {
    self.setHeader(name, opts.headers[name])
  })

  self._mode = 'fetch'

  self.on('finish', function() {
    self._onFinish()
  })
}

inherits(ClientRequest, Writable)
// Taken from http://www.w3.org/TR/XMLHttpRequest/#the-setrequestheader%28%29-method
var unsafeHeaders = [
  'accept-charset',
  'accept-encoding',
  'access-control-request-headers',
  'access-control-request-method',
  'connection',
  'content-length',
  'cookie',
  'cookie2',
  'date',
  'dnt',
  'expect',
  'host',
  'keep-alive',
  'origin',
  'referer',
  'te',
  'trailer',
  'transfer-encoding',
  'upgrade',
  'user-agent',
  'via'
]
ClientRequest.prototype.setHeader = function(name, value) {
  var self = this
  var lowerName = name.toLowerCase()
    // This check is not necessary, but it prevents warnings from browsers about setting unsafe
    // headers. To be honest I'm not entirely sure hiding these warnings is a good thing, but
    // http-browserify did it, so I will too.
  if (unsafeHeaders.indexOf(lowerName) !== -1)
    return

  self._headers[lowerName] = {
    name: name,
    value: value
  }
}

ClientRequest.prototype.getHeader = function(name) {
  var self = this
  return self._headers[name.toLowerCase()].value
}

ClientRequest.prototype.removeHeader = function(name) {
  var self = this
  delete self._headers[name.toLowerCase()]
}

ClientRequest.prototype._onFinish = function() {
  var self = this

  if (self._destroyed)
    return
  var opts = self._opts

  var headersObj = self._headers
  var body
  if (opts.method === 'POST' || opts.method === 'PUT' || opts.method === 'PATCH') {
    body = Buffer.concat(self._body).toString()
  }

  var headers = Object.keys(headersObj).map(function(name) {
    return [headersObj[name].name, headersObj[name].value]
  })


  fetch(self._opts.url, {
    method: self._opts.method,
    headers: headers,
    body: body,
    // mode: 'cors',
    // credentials: opts.withCredentials ? 'include' : 'same-origin'
  }).then(function(response) {
    self._fetchResponse = response
    self._connect()
  }, function(reason) {
    self.emit('error', reason)
  })
}

/**
 * Checks if xhr.status is readable and non-zero, indicating no error.
 * Even though the spec says it should be available in readyState 3,
 * accessing it throws an exception in IE8
 */
function statusValid(xhr) {
  try {
    var status = xhr.status
    return (status !== null && status !== 0)
  } catch (e) {
    return false
  }
}

ClientRequest.prototype._onXHRProgress = function() {
  var self = this

  if (!statusValid(self._xhr) || self._destroyed)
    return

  if (!self._response)
    self._connect()

  self._response._onXHRProgress()
}

ClientRequest.prototype._connect = function() {
  var self = this

  // TODO: ClientRequest.destroy() is being called somewhere?
  // if (self._destroyed)
  //   return

  self._response = new IncomingMessage(self._xhr, self._fetchResponse, self._mode)
  self.emit('response', self._response)
}

ClientRequest.prototype._write = function(chunk, encoding, cb) {
  var self = this

  self._body.push(chunk)
  cb()
}

ClientRequest.prototype.abort = ClientRequest.prototype.destroy = function() {
  var self = this
  self._destroyed = true
  if (self._response)
    self._response._destroyed = true
  if (self._xhr)
    self._xhr.abort()
    // Currently, there isn't a way to truly abort a fetch.
    // If you like bikeshedding, see https://github.com/whatwg/fetch/issues/27
}

ClientRequest.prototype.end = function(data, encoding, cb) {
  var self = this
  if (typeof data === 'function') {
    cb = data
    data = undefined
  }

  Writable.prototype.end.call(self, data, encoding, cb)
}

ClientRequest.prototype.flushHeaders = function() {}
ClientRequest.prototype.setTimeout = function() {}
ClientRequest.prototype.setNoDelay = function() {}
ClientRequest.prototype.setSocketKeepAlive = function() {}
