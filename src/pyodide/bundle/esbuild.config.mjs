export default {
  logOverride: {
    // Suppress warning about CommonJS 'module' variable in pyodide.asm.js (ES module context)
    // The pyodide.asm.js file contains embedded libraries (like MiniLZ4) that use CommonJS
    // style module.exports checks for compatibility. These are safe to ignore as they won't
    // execute in our ES module context.
    'commonjs-variable-in-esm': 'silent',
  },
};
