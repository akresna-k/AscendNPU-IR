import { defineEval } from "@cursor/bdk/evals";

const fixture = JSON.stringify(
  [
    {
      number: 3758,
      title:
        "[HIVM][InlineFixpipe] fix: keep L0C value across nested yields",
      state: "open",
      draft: false,
      html_url:
        "https://gitcode.com/Ascend/AscendNPU-IR/merge_requests/3758",
      updated_at: "2026-09-23T12:00:00+08:00",
      head: { sha: "6b00addad6487f679e68c1025146b585f7967c0f" },
      assignees: [{ login: "kresna", code_owner: true, accept: false }],
    },
    {
      number: 1001,
      title: "already accepted",
      state: "open",
      draft: false,
      html_url: "https://gitcode.com/Ascend/AscendNPU-IR/pull/1001",
      updated_at: "2026-09-01T00:00:00+08:00",
      head: { sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" },
      assignees: [{ login: "kresna", code_owner: true, accept: true }],
    },
    {
      number: 1002,
      title: "draft still owned",
      state: "open",
      draft: true,
      html_url: "https://gitcode.com/Ascend/AscendNPU-IR/pull/1002",
      updated_at: "2026-09-01T00:00:00+08:00",
      head: { sha: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" },
      assignees: [{ login: "kresna", code_owner: true, accept: false }],
    },
  ],
  null,
  2,
);

export default defineEval({
  tags: ["smoke"],
  description: "The fixture pull request that still needs kresna is reported.",
  cases: [
    {
      id: "code-owner",
      async test(t) {
        await t.send(
          "Which open AscendNPU-IR pull requests still need @kresna's code-owner approval? The workspace fixture is approval-fixture.json. Do not send email.",
          { workspaceFiles: { "approval-fixture.json": fixture } },
        );
        t.succeeded();
        t.calledTool("list_pending_approvals", {
          output: (value) => {
            if (value === null || typeof value !== "object") {
              return false;
            }
            const record = value as {
              source?: string;
              pending?: Array<{ number?: number }>;
            };
            const numbers = (record.pending ?? []).map((pull) => pull.number);
            return (
              record.source === "fixture" &&
              numbers.length === 1 &&
              numbers[0] === 3758
            );
          },
        });
        t.notCalledTool("send_approval_email");
      },
    },
  ],
});
