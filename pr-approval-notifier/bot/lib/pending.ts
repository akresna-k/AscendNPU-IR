// Code-owner approval selection for GitCode pull requests.
// A pull request needs approval when it is open, not a draft, and the
// watched login is an assignee with code_owner set and accept not true.

export type Assignee = {
  login?: string;
  code_owner?: boolean;
  accept?: boolean;
};

export type PullRecord = {
  number?: number;
  title?: string;
  state?: string;
  draft?: boolean;
  html_url?: string;
  updated_at?: string;
  head?: { sha?: string };
  assignees?: Assignee[];
};

export type PendingPull = {
  number: number;
  title: string;
  url: string;
  updatedAt: string;
  headSha: string;
};

export type ClassifiedPulls = {
  pending: PendingPull[];
  draftsSkipped: number;
  accepted: number;
  ignored: number;
};

export type NotifiedState = {
  notified: string[];
};

export const DEFAULT_LOGIN = "kresna";
export const DEFAULT_EMAIL = "kusjantono.kresna.kirana@huawei.com";
export const STATE_KEY = "approval-notifier:notified";

export function notificationKey(pull: PendingPull): string {
  return `${pull.number}:${pull.headSha}`;
}

export function normalizeState(value: unknown): NotifiedState {
  if (value === null || typeof value !== "object" || !("notified" in value)) {
    return { notified: [] };
  }
  const notified = value.notified;
  if (!Array.isArray(notified)) {
    return { notified: [] };
  }
  return {
    notified: notified.filter((item): item is string => typeof item === "string"),
  };
}

export function classifyPulls(
  pulls: readonly PullRecord[],
  login: string = DEFAULT_LOGIN,
): ClassifiedPulls {
  const pending: PendingPull[] = [];
  let draftsSkipped = 0;
  let accepted = 0;
  let ignored = 0;
  const wanted = login.toLowerCase();

  for (const pull of pulls) {
    if (pull.state !== "open" || typeof pull.number !== "number") {
      ignored += 1;
      continue;
    }
    const mine = (pull.assignees ?? []).find(
      (assignee) => (assignee.login ?? "").toLowerCase() === wanted,
    );
    if (mine === undefined || mine.code_owner !== true) {
      ignored += 1;
      continue;
    }
    if (pull.draft === true) {
      draftsSkipped += 1;
      continue;
    }
    if (mine.accept === true) {
      accepted += 1;
      continue;
    }
    const headSha = pull.head?.sha;
    if (headSha === undefined || headSha.length === 0) {
      ignored += 1;
      continue;
    }
    pending.push({
      number: pull.number,
      title: pull.title ?? "",
      url:
        pull.html_url ??
        `https://gitcode.com/Ascend/AscendNPU-IR/pull/${pull.number}`,
      updatedAt: pull.updated_at ?? "",
      headSha,
    });
  }

  pending.sort((left, right) => right.updatedAt.localeCompare(left.updatedAt));
  return { pending, draftsSkipped, accepted, ignored };
}

export function freshPulls(
  pending: readonly PendingPull[],
  state: NotifiedState,
): PendingPull[] {
  const seen = new Set(state.notified);
  return pending.filter((pull) => !seen.has(notificationKey(pull)));
}

export function stateForPending(pending: readonly PendingPull[]): NotifiedState {
  return { notified: pending.map(notificationKey) };
}

export function renderDigest(
  pulls: readonly PendingPull[],
  login: string = DEFAULT_LOGIN,
): { subject: string; text: string } {
  const noun = pulls.length === 1 ? "pull request needs" : "pull requests need";
  const subject = `AscendNPU-IR: ${pulls.length} ${noun} your approval`;
  const lines = [
    `These open AscendNPU-IR pull requests list @${login} as a code owner, and you have not accepted them yet.`,
    "",
  ];
  for (const pull of pulls) {
    lines.push(`- #${pull.number} ${pull.title}`);
    lines.push(`  ${pull.url}`);
    lines.push(
      `  updated ${pull.updatedAt}  head ${pull.headSha.slice(0, 12)}`,
    );
    lines.push("");
  }
  lines.push(
    "A commit that was already emailed is not repeated. A new head commit is emailed again while you still have not accepted it.",
  );
  return { subject, text: lines.join("\n") };
}
