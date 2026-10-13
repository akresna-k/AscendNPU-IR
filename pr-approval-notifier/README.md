# PR approval notifier

Emails [kusjantono.kresna.kirana@huawei.com](mailto:kusjantono.kresna.kirana@huawei.com)
when an open [AscendNPU-IR](https://gitcode.com/Ascend/AscendNPU-IR) pull
request still needs `@kresna`'s approval.

A pull request needs approval when GitCode lists `@kresna` as an assignee
with `code_owner: true` and `accept` is not true. Drafts are skipped.
[Pull request 3758](https://gitcode.com/Ascend/AscendNPU-IR/pull/3758) is
that case: InlineFixpipe changes, and `@kresna` is a code owner who has
not accepted.

Each email is one digest of pull requests whose current head commit has
not been emailed yet. A later commit is emailed again while acceptance
is still missing. The public GitCode API is enough; `GITCODE_ACCESS_TOKEN`
is optional.

## GitHub Actions

After this workflow is on `master`, GitHub runs it every 30 minutes.
Enable Actions on the fork, then add these repository secrets:

| Secret | Purpose |
| --- | --- |
| `APPROVAL_SMTP_HOST` | Outgoing SMTP host for the Huawei mailbox |
| `APPROVAL_SMTP_PORT` | Usually `587` |
| `APPROVAL_SMTP_USER` | SMTP username |
| `APPROVAL_SMTP_PASSWORD` | SMTP password or app password |
| `APPROVAL_SMTP_FROM` | From address the relay will accept |

`APPROVAL_SMTP_STARTTLS` defaults to on for port 587. Set the secret to
`false` only for a relay that does not use STARTTLS. The first successful
run sends the current backlog, then only new commits.

Notified commit ids are stored on the `notifier-state` branch as
`state.json`. That file is numbers and head SHAs, not mail credentials.

## Local check

```bash
cd pr-approval-notifier
npm test
npx bdk validate --dir .
npx bdk call list_pending_approvals --dir . --input '{}'
```

Set the same `APPROVAL_SMTP_*` variables and run `npx tsx scripts/poll.ts`
to send from this checkout. Without them the command lists what would be
sent and does not mark those commits as notified.

## Hosted schedule

`bot/schedules/poll-approvals.ts` runs the same send every 30 minutes on
a Cursor-hosted BDK deployment. Deploy only after the SMTP values are
available to that deployment as secrets of the same names.
