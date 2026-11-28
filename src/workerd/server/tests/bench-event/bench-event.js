export default {
  bench(b) {
    b.run('empty', () => {}, { warmup: 0, batch: 1, samples: 1 });
  },
};
