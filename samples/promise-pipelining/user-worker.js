export default {
  fetch() {},
  async foo(emoji) {
    return {
      bar: {
        buzz: () => `You made it! ${emoji}`,
      },
    };
  },
};
