export default {
  fetch(request) {
    console.log('Outbound Worker called');
    return fetch(request);
  },
};
