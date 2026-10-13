import { fetchClassified, gitCodeConfigFromEnv } from "./gitcode.js";
import {
  freshPulls,
  renderDigest,
  stateForPending,
  type NotifiedState,
  type PendingPull,
} from "./pending.js";
import { sendMail, smtpConfigFromEnv, type SmtpConfig } from "./smtp.js";

export type NotifyReason = "sent" | "none" | "smtp_not_configured" | "eval";

export type NotifyResult = {
  pendingCount: number;
  freshCount: number;
  draftsSkipped: number;
  accepted: number;
  emailed: boolean;
  reason: NotifyReason;
  to?: string;
  fresh: Array<{
    number: number;
    title: string;
    url: string;
    headSha: string;
  }>;
};

export function summarize(pulls: readonly PendingPull[]): NotifyResult["fresh"] {
  return pulls.map((pull) => ({
    number: pull.number,
    title: pull.title,
    url: pull.url,
    headSha: pull.headSha,
  }));
}

export async function runNotify(options: {
  purpose?: string;
  loadState: () => Promise<NotifiedState>;
  saveState: (state: NotifiedState) => Promise<void>;
  env?: NodeJS.ProcessEnv;
  smtp?: SmtpConfig;
}): Promise<NotifyResult> {
  if (options.purpose === "eval") {
    return {
      pendingCount: 0,
      freshCount: 0,
      draftsSkipped: 0,
      accepted: 0,
      emailed: false,
      reason: "eval",
      fresh: [],
    };
  }

  const env = options.env ?? process.env;
  const config = gitCodeConfigFromEnv(env);
  const classified = await fetchClassified(config);
  const state = await options.loadState();
  const fresh = freshPulls(classified.pending, state);
  const base = {
    pendingCount: classified.pending.length,
    freshCount: fresh.length,
    draftsSkipped: classified.draftsSkipped,
    accepted: classified.accepted,
    fresh: summarize(fresh),
  };

  if (fresh.length === 0) {
    await options.saveState(stateForPending(classified.pending));
    return { ...base, emailed: false, reason: "none" };
  }

  const smtp = options.smtp ?? smtpConfigFromEnv(env);
  if (smtp === undefined) {
    return { ...base, emailed: false, reason: "smtp_not_configured" };
  }

  const digest = renderDigest(fresh, config.login);
  await sendMail(smtp, digest);
  await options.saveState(stateForPending(classified.pending));
  return { ...base, emailed: true, reason: "sent", to: smtp.to };
}
