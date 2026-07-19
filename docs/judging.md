# Independent judging of completed work

Every unit of completed work in this repo — a bullseye target achieve, a
goal-harness completion — must be confirmed by an **independent judge**
before it counts. The executor's own attestation is a claim, not a fact;
this repo has now had two false completions of the same target group
(🎯T156, 2026-07-18: a theater oracle, then an oracle hole plus
executor-weakened acceptance criteria), both caught only after the fact.

## The judge

`scripts/judge <target-id> [evidence-dir ...]`

- Extracts the target's block from `bullseye.yaml`.
- Invokes **`claude -p`** (a different model and harness from the executing
  agent) with read/execute tools to adversarially verify every acceptance
  criterion against the working tree, git history, tests, and supplied
  evidence.
- Writes `verdicts/<id>/<utc-stamp>.md` and exits 0 only on
  `VERDICT: CONFIRMED`.

## The rule

1. **No `status: achieved` without a verdict.** The achieve commit includes
   the `verdicts/<id>/…` artifact. A ledger edit that flips a target to
   `achieved` without one is invalid regardless of who makes it.
2. **The executor may not judge itself.** Grok goals use their built-in
   skeptic *and* this judge; Claude-executed work is judged by a fresh
   `claude -p` run (separate context), or by the Grok skeptic — the point
   is a verifier that did not produce the work and does not share its
   context.
3. **Acceptance criteria are the judge's spec, not the executor's.**
   Weakening or re-scoping a target's acceptance to match what was shipped
   is itself a finding the judge checks for (`git log -p` on the
   `bullseye.yaml` block). Scope changes belong in an explicit ledger edit
   *before* the work, with the change called out to the user — not folded
   into the achieve.
4. **Theater is refuted, not partially credited.** Self-comparison
   (double-running one implementation), stub result files, greps presented
   as logs, evidence in ephemeral temp dirs, and tests that start past the
   thing under test all refute the criterion they claim to support.

## Evidence

Durable evidence lives in-repo: `verdicts/` for judge output, committed
test artifacts or a TEST-REPORT-equivalent for run evidence. Goal-harness
SCRATCH dirs under `/var/folders/...` are working space, not attestation —
copy what matters into the repo before achieving.
