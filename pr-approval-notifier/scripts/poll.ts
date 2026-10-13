import { readFile, writeFile } from "node:fs/promises";

import { normalizeState } from "../bot/lib/pending.js";
import { runNotify } from "../bot/lib/run-notify.js";

const file = process.env.APPROVAL_STATE_FILE ?? ".notifier-state.json";

const result = await runNotify({
  loadState: async () => {
    try {
      return normalizeState(JSON.parse(await readFile(file, "utf8")));
    } catch (error) {
      if (
        error !== null &&
        typeof error === "object" &&
        "code" in error &&
        error.code === "ENOENT"
      ) {
        return { notified: [] };
      }
      throw error;
    }
  },
  saveState: async (state) => {
    await writeFile(file, `${JSON.stringify(state, null, 2)}\n`);
  },
});

console.log(JSON.stringify(result, null, 2));
if (result.reason === "smtp_not_configured") {
  console.error(
    "SMTP is not configured. Set APPROVAL_SMTP_HOST, APPROVAL_SMTP_PORT, APPROVAL_SMTP_USER, APPROVAL_SMTP_PASSWORD, and APPROVAL_SMTP_FROM.",
  );
}
