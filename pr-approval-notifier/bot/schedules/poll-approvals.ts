import { defineSchedule } from "@cursor/bdk/schedules";

export default defineSchedule({
  cron: "*/30 * * * *",
  async run({ callTool }) {
    const outcome = await callTool("send_approval_email", {});
    if (outcome.isError) {
      throw new Error("send_approval_email failed");
    }
  },
});
