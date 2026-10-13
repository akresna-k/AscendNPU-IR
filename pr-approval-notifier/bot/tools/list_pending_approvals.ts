import { readFile } from "node:fs/promises";
import path from "node:path";

import { defineTool } from "@cursor/bdk/tools";
import { z } from "zod";

import { fetchClassified, gitCodeConfigFromEnv } from "../lib/gitcode.js";
import {
  classifyPulls,
  type ClassifiedPulls,
  type PullRecord,
} from "../lib/pending.js";

const FIXTURE_NAME = "approval-fixture.json";

function resolveInside(root: string, relativePath: string): string {
  const base = path.resolve(root);
  const full = path.resolve(base, relativePath);
  if (full !== base && !full.startsWith(`${base}${path.sep}`)) {
    throw new Error("fixturePath must stay inside the workspace");
  }
  return full;
}

async function readFixture(file: string): Promise<PullRecord[]> {
  const parsed: unknown = JSON.parse(await readFile(file, "utf8"));
  if (!Array.isArray(parsed)) {
    throw new Error("approval fixture must be a JSON array of pull requests");
  }
  return parsed.filter(
    (item): item is PullRecord => item !== null && typeof item === "object",
  );
}

export default defineTool({
  description:
    "List open AscendNPU-IR pull requests that still need @kresna's code-owner acceptance. Uses approval-fixture.json in the workspace when that file exists, otherwise reads GitCode. Does not send email.",
  effect: "read",
  inputSchema: z.object({
    fixturePath: z
      .string()
      .optional()
      .describe(
        "Workspace-relative JSON file of GitCode pull objects. Omit to auto-use approval-fixture.json when present, or GitCode otherwise.",
      ),
  }),
  async execute({ fixturePath }, ctx) {
    const config = gitCodeConfigFromEnv();
    const workspace = ctx.workspaceDir;
    const relative = fixturePath ?? FIXTURE_NAME;
    const candidate =
      fixturePath !== undefined || workspace.length > 0
        ? resolveInside(workspace, relative)
        : undefined;
    let source: "fixture" | "gitcode" = "gitcode";
    let classified: ClassifiedPulls | undefined;
    if (candidate !== undefined && (fixturePath !== undefined || relative === FIXTURE_NAME)) {
      try {
        const pulls = await readFixture(candidate);
        classified = classifyPulls(pulls, config.login);
        source = "fixture";
      } catch (error) {
        const missing =
          error !== null &&
          typeof error === "object" &&
          "code" in error &&
          error.code === "ENOENT";
        if (!missing || fixturePath !== undefined) {
          throw error;
        }
      }
    }
    if (classified === undefined) {
      classified = await fetchClassified(config);
      source = "gitcode";
    }
    return {
      source,
      login: config.login,
      pendingCount: classified.pending.length,
      draftsSkipped: classified.draftsSkipped,
      accepted: classified.accepted,
      pending: classified.pending.map((pull) => ({
        number: pull.number,
        title: pull.title,
        url: pull.url,
        updatedAt: pull.updatedAt,
        headSha: pull.headSha,
      })),
    };
  },
});
