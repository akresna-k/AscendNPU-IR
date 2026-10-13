import {
  classifyPulls,
  type ClassifiedPulls,
  type PullRecord,
} from "./pending.js";

const API_ROOT = "https://api.gitcode.com/api/v5";

export type GitCodeConfig = {
  owner: string;
  repo: string;
  login: string;
  token?: string;
};

export function gitCodeConfigFromEnv(
  env: NodeJS.ProcessEnv = process.env,
): GitCodeConfig {
  const token =
    env.GITCODE_ACCESS_TOKEN?.trim() || env.GITCODE_TOKEN?.trim() || undefined;
  return {
    owner: env.APPROVAL_REPO_OWNER?.trim() || "Ascend",
    repo: env.APPROVAL_REPO?.trim() || "AscendNPU-IR",
    login: env.APPROVAL_LOGIN?.trim() || "kresna",
    token,
  };
}

function buildUrl(
  config: GitCodeConfig,
  path: string,
  query: Record<string, string>,
): URL {
  const url = new URL(`${API_ROOT}${path}`);
  for (const [key, value] of Object.entries(query)) {
    url.searchParams.set(key, value);
  }
  if (config.token !== undefined) {
    url.searchParams.set("access_token", config.token);
  }
  return url;
}

function safeUrl(url: URL): string {
  const copy = new URL(url.toString());
  copy.searchParams.delete("access_token");
  return copy.toString();
}

async function getJson(url: URL): Promise<unknown> {
  const response = await fetch(url, {
    headers: {
      accept: "application/json",
      "user-agent": "pr-approval-notifier",
    },
    signal: AbortSignal.timeout(30_000),
  });
  if (!response.ok) {
    throw new Error(`GitCode ${response.status} for ${safeUrl(url)}`);
  }
  return response.json() as Promise<unknown>;
}

function asPullArray(value: unknown): PullRecord[] {
  if (!Array.isArray(value)) {
    throw new Error("GitCode pull list was not an array");
  }
  return value.filter(
    (item): item is PullRecord => item !== null && typeof item === "object",
  );
}

async function mapPool<T, R>(
  items: readonly T[],
  limit: number,
  fn: (item: T) => Promise<R>,
): Promise<R[]> {
  const results = new Array<R>(items.length);
  let next = 0;
  const workers = Array.from(
    { length: Math.min(limit, items.length) },
    async () => {
      for (;;) {
        const index = next;
        next += 1;
        if (index >= items.length) {
          return;
        }
        results[index] = await fn(items[index] as T);
      }
    },
  );
  await Promise.all(workers);
  return results;
}

export async function fetchClassified(
  config: GitCodeConfig,
): Promise<ClassifiedPulls> {
  const numbers: number[] = [];
  for (let page = 1; page <= 20; page += 1) {
    const url = buildUrl(
      config,
      `/repos/${encodeURIComponent(config.owner)}/${encodeURIComponent(config.repo)}/pulls`,
      {
        state: "open",
        assignee: config.login,
        per_page: "100",
        page: String(page),
        sort: "updated",
        direction: "desc",
      },
    );
    const pagePulls = asPullArray(await getJson(url));
    for (const pull of pagePulls) {
      if (typeof pull.number === "number") {
        numbers.push(pull.number);
      }
    }
    if (pagePulls.length < 100) {
      break;
    }
  }

  const details = await mapPool(numbers, 6, async (number) => {
    const url = buildUrl(
      config,
      `/repos/${encodeURIComponent(config.owner)}/${encodeURIComponent(config.repo)}/pulls/${number}`,
      {},
    );
    const detail = await getJson(url);
    if (detail === null || typeof detail !== "object") {
      throw new Error(`GitCode pull ${number} was not an object`);
    }
    return detail as PullRecord;
  });
  return classifyPulls(details, config.login);
}
