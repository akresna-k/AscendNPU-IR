import assert from "node:assert/strict";
import test from "node:test";

import {
  classifyPulls,
  freshPulls,
  notificationKey,
  renderDigest,
  stateForPending,
} from "./pending.js";

const head = "6b00addad6487f679e68c1025146b585f7967c0f";

const needsApproval = {
  number: 3758,
  title: "[HIVM][InlineFixpipe] fix: keep L0C value across nested yields",
  state: "open",
  draft: false,
  html_url: "https://gitcode.com/Ascend/AscendNPU-IR/merge_requests/3758",
  updated_at: "2026-09-23T12:00:00+08:00",
  head: { sha: head },
  assignees: [{ login: "kresna", code_owner: true, accept: false }],
};

test("keeps an open code owner who has not accepted", () => {
  const classified = classifyPulls([
    needsApproval,
    {
      ...needsApproval,
      number: 1001,
      title: "already accepted",
      assignees: [{ login: "kresna", code_owner: true, accept: true }],
    },
    {
      ...needsApproval,
      number: 1002,
      title: "draft",
      draft: true,
    },
    {
      ...needsApproval,
      number: 1003,
      title: "not a code owner",
      assignees: [{ login: "kresna", code_owner: false, accept: false }],
    },
    {
      ...needsApproval,
      number: 1004,
      title: "someone else",
      assignees: [{ login: "other", code_owner: true, accept: false }],
    },
  ]);
  assert.deepEqual(
    classified.pending.map((pull) => pull.number),
    [3758],
  );
  assert.equal(classified.draftsSkipped, 1);
  assert.equal(classified.accepted, 1);
  assert.equal(
    classified.pending[0]?.url,
    "https://gitcode.com/Ascend/AscendNPU-IR/merge_requests/3758",
  );
});

test("emails only commits that have not been sent", () => {
  const classified = classifyPulls([needsApproval]);
  const pending = classified.pending;
  assert.equal(freshPulls(pending, { notified: [] }).length, 1);
  const seen = stateForPending(pending);
  assert.deepEqual(freshPulls(pending, seen), []);
  const moved = pending.map((pull) => ({
    ...pull,
    headSha: "abc123abc123abc123abc123abc123abc123abcd",
  }));
  assert.equal(freshPulls(moved, seen).length, 1);
  assert.equal(notificationKey(pending[0]!), `3758:${head}`);
});

test("digest names the pull request and the recipient role", () => {
  const { subject, text } = renderDigest(classifyPulls([needsApproval]).pending);
  assert.match(subject, /1 pull request needs your approval/);
  assert.match(text, /#3758/);
  assert.match(text, /@kresna/);
  assert.match(text, /merge_requests\/3758/);
});
