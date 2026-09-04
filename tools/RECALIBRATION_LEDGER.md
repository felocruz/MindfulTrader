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
| 2026-09-04 09:00 | observation_vector_recalibration | dims=amihud | tools/output/observation_vector_recalibration_amihud_20260904_090016.txt | ACTED ON (commit 814a9fd, 2026-09-04 — dim16 mean_rev_z recalibrated to 7.8; second pass, same file, dim11 amihud_illiquidity fixed 3036.0→191703.9 after finding a ζ_u subsampling-correction bug, dim12 liq_fragility recalibrated 21.26→1100.8 against the a76ec00 formula, see FeatureScaler.h's own comments) |
| 2026-09-04 10:35 | observation_vector_recalibration | dims=activity | tools/output/observation_vector_recalibration_activity_20260904_103524.txt | SUPERSEDED (pre-NaN-carry-forward-fix; dim10/dim13 show mean\|z\|=nan, do not use) |
| 2026-09-04 12:23 | observation_vector_recalibration | dims=activity | tools/output/observation_vector_recalibration_activity_20260904_122334.txt | ACTED ON (commit 77efe1c, 2026-09-04 — dim8 fast_hurst_exponent recalibrated to 10.9, dim13 fast_taleb_kurtosis recalibrated to 97.0; dim10 skewness_idx finding used to design the localMad-vs-\|z\| collapse-signature diagnostic, full resolution pending the diagnostic-enabled re-run below) |
| 2026-09-04 12:30 | observation_vector_recalibration | dims=activity | (wasted — process launched on the pre-diagnostic binary, see background_process_rebuild_race.md) | SUPERSEDED (produced identical numbers to the 12:23 run, zero new information; relaunched at ~14:18 with the correct binary) |
| 2026-09-04 15:57 | observation_vector_recalibration_activity | dims=activity | tools/output/observation_vector_recalibration_activity_20260904_155731.txt | ACTED ON (commit b94522c, 2026-09-04 -- confirmed skewness_idx collapse signature, corr(localMad,\|z\|)=-0.2459, enabled SHRINKAGE_SCALE_MIN[10]; also used to close dim8/dim13 winsor bounds, commit 77efe1c) |
| 2026-09-04 17:40 | observation_vector_recalibration_activity | dims=activity | tools/output/observation_vector_recalibration_activity_20260904_174019.txt | ACTED ON (commit 350f620, 2026-09-04 -- shrinkage-corrected refit, DIM_WINSOR_SIGMA_OVERRIDE[10] finalized at 269.0; last open item in the observation-vector winsorization-bound audit thread) |
