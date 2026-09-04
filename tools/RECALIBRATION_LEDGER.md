# Recalibration Ledger

Auto-appended by `ToolProgressLogger` on every `tools/` run (added 2026-09-04, see its own
TOP-LEVEL DIRECTIVE in `tools/ToolProgressLogger.h`). **Check PENDING REVIEW rows before
launching a new heavy recalibration pass** — a pending row may already answer the question a
new run would re-derive at real compute cost (this file exists because that happened: 3
completed archives sat unused for hours on 2026-09-04, only noticed by chance). Update the
Status column by hand once a row's finding is actually consumed (e.g. `ACTED ON (commit
abc1234, 2026-09-05)`) — this file is git-tracked specifically so that edit shows up in normal
code review, unlike the archives themselves (`tools/output/` is gitignored).

Rows below `2026-09-04 13:XX` were backfilled by hand for pre-existing archives that predate
this mechanism; everything after is auto-appended.

| Date | Tool | Scope | Archive | Status |
|------|------|-------|---------|--------|
| 2026-09-03 22:19 | observation_vector_recalibration | dims=burstiness | tools/output/observation_vector_recalibration_burstiness_20260903_221935.txt | SUPERSEDED (earlier/incomplete attempt, see the 23:49 run) |
| 2026-09-03 23:49 | observation_vector_recalibration | dims=burstiness | tools/output/observation_vector_recalibration_burstiness_20260903_234906.txt | ACTED ON (commit 814a9fd, 2026-09-04 — audited clean, no override needed) |
| 2026-09-03 22:21 | observation_vector_recalibration | dims=logscale | tools/output/observation_vector_recalibration_logscale_20260903_222110.txt | SUPERSEDED (earlier/incomplete attempt, see the 03:40 run) |
| 2026-09-04 03:40 | observation_vector_recalibration | dims=logscale | tools/output/observation_vector_recalibration_logscale_20260904_034035.txt | ACTED ON (commit 814a9fd, 2026-09-04 — dim3 recalibrated to 12.0, dim0 audited clean) |
| 2026-09-04 04:56 | observation_vector_recalibration | dims=amihud | tools/output/observation_vector_recalibration_amihud_20260904_045637.txt | SUPERSEDED (earlier/incomplete attempt, see the 09:00 run) |
| 2026-09-04 09:00 | observation_vector_recalibration | dims=amihud | tools/output/observation_vector_recalibration_amihud_20260904_090016.txt | ACTED ON (commit 814a9fd, 2026-09-04 — dim16 mean_rev_z recalibrated to 7.8) |
| 2026-09-04 10:35 | observation_vector_recalibration | dims=activity | tools/output/observation_vector_recalibration_activity_20260904_103524.txt | SUPERSEDED (pre-NaN-carry-forward-fix; dim10/dim13 show mean\|z\|=nan, do not use) |
| 2026-09-04 12:23 | observation_vector_recalibration | dims=activity | tools/output/observation_vector_recalibration_activity_20260904_122334.txt | ACTED ON (finding — dim10 skewness_idx anomalous, mean\|z\|=18.28, rate-at-bound=5.43% — used to design the localMad-vs-\|z\| collapse-signature diagnostic added the same day; full resolution pending the re-run below) |
