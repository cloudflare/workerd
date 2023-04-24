// process is a global in Node.js so comes as a global for nodeJsCompatModules.
process.nextTick(() => {
  console.log('this should work', __filename, __dirname, module.path);
});

module.exports = {
  // Buffer is a global in Node.js so comes as a global for nodeJsCompatModules.
  Buffer,
  // require can be called synchronously inline.
  util: require('util'),
}

try {
  // Requiring Node.js built-ins that we do not implement will fail,
  // even if there are similarly named modules in the worker bundle.
  require('fs');
} catch (err) {
  console.log('attempt was made to load the fs module, which is not implemented');
}

// As long as the module being required is not a Node.js built-in, requiring
// it from the worker bundle works just fine.
const bar = require('bar');
