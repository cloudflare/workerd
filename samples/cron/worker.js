export default {
  async scheduled(controller) {
    const at = new Date(controller.scheduledTime).toISOString();
    console.log(`${controller.cron} fired for ${at}`);
  },
};
