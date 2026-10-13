import { defineTool } from "@cursor/bdk/tools";
import { z } from "zod";

import { normalizeState, STATE_KEY } from "../lib/pending.js";
import { runNotify } from "../lib/run-notify.js";

export default defineTool({
  description:
    "Email kusjantono.kresna.kirana@huawei.com a digest of AscendNPU-IR pull requests that still need @kresna's code-owner acceptance and have not been emailed for the current head commit. Re-reads GitCode before sending. Call only when the user explicitly asks to send the email; the schedule sends on its own.",
  effect: "write",
  inputSchema: z.object({}),
  dryRunResult: () => ({
    emailed: false,
    reason: "dry_run",
    pendingCount: 0,
    freshCount: 0,
    draftsSkipped: 0,
    accepted: 0,
    fresh: [],
  }),
  async execute(_input, ctx) {
    const result = await runNotify({
      purpose: ctx.session.purpose,
      loadState: async () => normalizeState(await ctx.host.kv.get(STATE_KEY)),
      saveState: async (state) => {
        await ctx.host.kv.put(STATE_KEY, { notified: state.notified });
      },
    });
    return {
      pendingCount: result.pendingCount,
      freshCount: result.freshCount,
      draftsSkipped: result.draftsSkipped,
      accepted: result.accepted,
      emailed: result.emailed,
      reason: result.reason,
      to: result.to ?? null,
      fresh: result.fresh.map((pull) => ({
        number: pull.number,
        title: pull.title,
        url: pull.url,
        headSha: pull.headSha,
      })),
    };
  },
});
