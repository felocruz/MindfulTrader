//! audit.rs -- deterministic GO/NO-GO auditor for Student-t HMM release artifacts.
//!
//! Port of `lbrnet/lbrnet/scripts/audit_hmm_release.py`'s `_evaluate()` (spec §5.1a: "the
//! cleanest keep-it-as-is case" -- pure deterministic JSON-in/JSON-out, no discretionary human
//! interpretation). Reads `ModelManifest.json` and `HMMGateReport.json` (as parsed `serde_json`
//! values, not file paths -- file I/O and CLI argument parsing are a future wrapper's job, kept
//! out of this pure-core module per "pure core, thin edges") and emits a strict GO/NO-GO decision
//! against the runbook-aligned gate contract.
//!
//! Plan §4 Stage G1: standalone, no-model-dependency port -- doesn't need Stage A/P9's golden
//! harness, doesn't need any of Stage G0's still-open blocking decisions resolved first.

use serde::Serialize;
use serde_json::Value;

/// One named pass/fail check in the audit, with whether it's required for a GO decision.
#[derive(Debug, Clone, Serialize)]
pub struct Check {
    pub name: String,
    pub passed: bool,
    pub required: bool,
    pub detail: String,
}

/// The completion-checklist keys every sign-off-quality gate report must carry. Required, by
/// design, matching the Python original's own comment: a gate report from before these
/// model-quality checks existed (missing these keys) correctly audits as NO-GO rather than
/// silently skipping the check -- regenerate via a fresh training run, don't relax this list.
/// Not `no_sustained_decline_at_convergence` (superseded): a detected L_MAP decline is no longer
/// a hard blocker by itself now that the trainer auto-restores the best-training-L_MAP
/// checkpoint -- `checkpoint_restore_consistent` fails only if a decline was detected but the
/// safety net did NOT fire.
const COMPLETION_CHECKLIST_KEYS: [&str; 10] = [
    "pipeline_completion_marker_emitted",
    "required_artifacts_present_and_non_empty",
    "manifest_regime_coverage_ok_for_run_class",
    "strict_mode_enabled_when_required",
    "memory_budget_gate_pass",
    "gemini_gate_coverage_reported",
    "em_converged_by_relative_tol",
    "dof_not_pinned_at_bounds",
    "state_occupancy_balanced",
    "checkpoint_restore_consistent",
];

/// The artifact filenames every release must have produced, present and non-empty.
const REQUIRED_ARTIFACTS: [&str; 4] =
    ["hmm_model.pkl", "HMMDiagnostics.png", "ModelManifest.json", "agent_state.bin"];

/// GO/NO-GO decision plus the full, itemized checklist that produced it.
#[derive(Debug, Clone, Serialize)]
pub struct AuditResult {
    pub decision: Decision,
    pub run_class: String,
    pub required_checks_passed: usize,
    pub required_checks_total: usize,
    pub failed_required_checks: Vec<Check>,
    pub all_checks: Vec<Check>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "SCREAMING-KEBAB-CASE")]
pub enum Decision {
    Go,
    NoGo,
}

/// Python's `_as_bool`: coerces a JSON value the same permissive way the original script does --
/// real booleans pass through, numbers are C-style truthy/falsy, and a fixed set of case-
/// insensitive strings map to true/false; anything else (including `null`/missing) is `false`,
/// never an error -- a gate report with an unexpected value type fails closed, not panics.
fn as_bool(value: &Value) -> bool {
    match value {
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().map(|f| f != 0.0).unwrap_or(false),
        Value::String(s) => {
            let v = s.trim().to_ascii_lowercase();
            matches!(v.as_str(), "1" | "true" | "yes" | "y" | "pass" | "ok")
        }
        _ => false,
    }
}

fn get<'a>(obj: &'a Value, key: &str) -> &'a Value {
    static NULL: Value = Value::Null;
    obj.get(key).unwrap_or(&NULL)
}

fn extract_manifest_taleb_flag(manifest: &Value) -> bool {
    let gang = get(manifest, "gang_validation");
    if !gang.is_object() {
        return false;
    }
    as_bool(get(gang, "taleb_extreme_state_present"))
}

/// Evaluate a manifest + gate-report pair against the runbook-aligned gate contract. Mirrors
/// `audit_hmm_release.py`'s `_evaluate()` check-by-check, including each check's `required`-ness.
pub fn evaluate(manifest: &Value, gate_report: &Value, expected_run_class: Option<&str>) -> AuditResult {
    let mut checks: Vec<Check> = Vec::new();
    let mut add = |name: &str, passed: bool, required: bool, detail: String| {
        checks.push(Check { name: name.to_string(), passed, required, detail });
    };

    let run_class = get(gate_report, "run_class")
        .as_str()
        .unwrap_or("unknown")
        .to_string();

    if let Some(expected) = expected_run_class {
        add(
            "run_class_matches_expected",
            run_class == expected,
            true,
            format!("expected={expected} actual={run_class}"),
        );
    }

    let overall_pass = as_bool(get(gate_report, "overall_pass"));
    add(
        "gate_report_overall_pass",
        overall_pass,
        true,
        format!("overall_pass={overall_pass}"),
    );

    let completion = get(gate_report, "completion_checklist");
    if completion.is_object() {
        for key in COMPLETION_CHECKLIST_KEYS {
            let value = as_bool(get(completion, key));
            add(&format!("completion_checklist.{key}"), value, true, format!("value={value}"));
        }
    } else {
        add(
            "completion_checklist_present",
            false,
            true,
            "completion_checklist missing or not an object".to_string(),
        );
    }

    let artifacts = get(gate_report, "artifacts");
    if artifacts.is_object() {
        for name in REQUIRED_ARTIFACTS {
            let value = as_bool(get(artifacts, name));
            add(&format!("artifacts.{name}"), value, true, format!("present_non_empty={value}"));
        }
    } else {
        add("artifacts_present", false, true, "artifacts missing or not an object".to_string());
    }

    let strict_enabled = as_bool(get(gate_report, "strict_enabled"));
    add(
        "strict_enabled_when_sign_off",
        if run_class == "sign-off" { strict_enabled } else { true },
        true,
        format!("run_class={run_class} strict_enabled={strict_enabled}"),
    );

    let taleb_flag_manifest = extract_manifest_taleb_flag(manifest);
    add(
        "manifest_taleb_extreme_state_present_for_sign_off",
        if run_class == "sign-off" { taleb_flag_manifest } else { true },
        true,
        format!("run_class={run_class} taleb_extreme_state_present={taleb_flag_manifest}"),
    );

    let manifest_summary = get(gate_report, "manifest_summary");
    if manifest_summary.is_object() && manifest_summary.get("taleb_extreme_state_present").is_some() {
        let taleb_flag_summary = as_bool(get(manifest_summary, "taleb_extreme_state_present"));
        add(
            "manifest_summary_consistent_with_manifest",
            taleb_flag_summary == taleb_flag_manifest,
            false,
            format!(
                "manifest_summary_taleb={taleb_flag_summary} manifest_taleb={taleb_flag_manifest}"
            ),
        );
    }

    let required_checks: Vec<&Check> = checks.iter().filter(|c| c.required).collect();
    let failed_required: Vec<Check> =
        required_checks.iter().filter(|c| !c.passed).map(|c| (*c).clone()).collect();
    let required_total = required_checks.len();
    let required_passed = required_total - failed_required.len();

    let decision = if failed_required.is_empty() { Decision::Go } else { Decision::NoGo };

    AuditResult {
        decision,
        run_class,
        required_checks_passed: required_passed,
        required_checks_total: required_total,
        failed_required_checks: failed_required,
        all_checks: checks,
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)] // test-only JSON-fixture mutation; documented workspace exception
mod tests {
    use super::*;
    use serde_json::json;

    /// A complete, all-passing sign-off-class gate report + manifest -- the baseline every other
    /// test perturbs one field of.
    fn passing_gate_report() -> Value {
        json!({
            "run_class": "sign-off",
            "overall_pass": true,
            "strict_enabled": true,
            "completion_checklist": {
                "pipeline_completion_marker_emitted": true,
                "required_artifacts_present_and_non_empty": true,
                "manifest_regime_coverage_ok_for_run_class": true,
                "strict_mode_enabled_when_required": true,
                "memory_budget_gate_pass": true,
                "gemini_gate_coverage_reported": true,
                "em_converged_by_relative_tol": true,
                "dof_not_pinned_at_bounds": true,
                "state_occupancy_balanced": true,
                "checkpoint_restore_consistent": true,
            },
            "artifacts": {
                "hmm_model.pkl": true,
                "HMMDiagnostics.png": true,
                "ModelManifest.json": true,
                "agent_state.bin": true,
            },
            "manifest_summary": { "taleb_extreme_state_present": true },
        })
    }

    fn passing_manifest() -> Value {
        json!({ "gang_validation": { "taleb_extreme_state_present": true } })
    }

    #[test]
    fn all_pass_yields_go() {
        let result = evaluate(&passing_manifest(), &passing_gate_report(), Some("sign-off"));
        assert_eq!(result.decision, Decision::Go);
        assert!(result.failed_required_checks.is_empty());
        assert_eq!(result.required_checks_passed, result.required_checks_total);
    }

    #[test]
    fn missing_completion_checklist_key_fails_closed() {
        // A gate report produced before a model-quality check existed: the key is simply absent,
        // not `false`. Must still audit as NO-GO -- the Python original's own documented intent.
        let mut gate_report = passing_gate_report();
        gate_report["completion_checklist"]
            .as_object_mut()
            .unwrap()
            .remove("em_converged_by_relative_tol");
        let result = evaluate(&passing_manifest(), &gate_report, Some("sign-off"));
        assert_eq!(result.decision, Decision::NoGo);
        assert!(result
            .failed_required_checks
            .iter()
            .any(|c| c.name == "completion_checklist.em_converged_by_relative_tol"));
    }

    #[test]
    fn missing_artifact_fails_closed() {
        let mut gate_report = passing_gate_report();
        gate_report["artifacts"].as_object_mut().unwrap().remove("agent_state.bin");
        let result = evaluate(&passing_manifest(), &gate_report, None);
        assert_eq!(result.decision, Decision::NoGo);
    }

    #[test]
    fn expected_run_class_mismatch_is_no_go() {
        let result = evaluate(&passing_manifest(), &passing_gate_report(), Some("smoke"));
        assert_eq!(result.decision, Decision::NoGo);
        assert!(result.failed_required_checks.iter().any(|c| c.name == "run_class_matches_expected"));
    }

    #[test]
    fn strict_enabled_only_required_for_sign_off() {
        let mut gate_report = passing_gate_report();
        gate_report["run_class"] = json!("smoke");
        gate_report["strict_enabled"] = json!(false);
        // Smoke runs don't require strict mode -- must still pass despite strict_enabled=false.
        let result = evaluate(&passing_manifest(), &gate_report, None);
        assert_eq!(result.decision, Decision::Go);
    }

    #[test]
    fn strict_enabled_required_for_sign_off() {
        let mut gate_report = passing_gate_report();
        gate_report["strict_enabled"] = json!(false);
        let result = evaluate(&passing_manifest(), &gate_report, None);
        assert_eq!(result.decision, Decision::NoGo);
    }

    #[test]
    fn taleb_flag_only_required_for_sign_off() {
        let mut gate_report = passing_gate_report();
        gate_report["run_class"] = json!("smoke");
        let manifest = json!({ "gang_validation": { "taleb_extreme_state_present": false } });
        let result = evaluate(&manifest, &gate_report, None);
        assert_eq!(result.decision, Decision::Go);
    }

    #[test]
    fn manifest_summary_mismatch_is_not_required() {
        // manifest_summary_consistent_with_manifest is explicitly NOT required (Python: `required=False`)
        // -- a mismatch here must not flip the decision to NO-GO by itself.
        let mut gate_report = passing_gate_report();
        gate_report["manifest_summary"]["taleb_extreme_state_present"] = json!(false);
        let result = evaluate(&passing_manifest(), &gate_report, Some("sign-off"));
        assert_eq!(result.decision, Decision::Go);
        assert!(result
            .all_checks
            .iter()
            .any(|c| c.name == "manifest_summary_consistent_with_manifest" && !c.passed && !c.required));
    }

    #[test]
    fn as_bool_coercion_matches_python_semantics() {
        assert!(as_bool(&json!(true)));
        assert!(as_bool(&json!(1)));
        assert!(as_bool(&json!(1.0)));
        assert!(as_bool(&json!("true")));
        assert!(as_bool(&json!("YES")));
        assert!(as_bool(&json!("pass")));
        assert!(as_bool(&json!("ok")));
        assert!(!as_bool(&json!(false)));
        assert!(!as_bool(&json!(0)));
        assert!(!as_bool(&json!("false")));
        assert!(!as_bool(&json!("no")));
        assert!(!as_bool(&json!(Value::Null)));
        assert!(!as_bool(&json!("garbage")));
        assert!(!as_bool(&json!([1, 2, 3])));
    }

    #[test]
    fn missing_completion_checklist_object_fails_closed() {
        let mut gate_report = passing_gate_report();
        gate_report["completion_checklist"] = json!("not an object");
        let result = evaluate(&passing_manifest(), &gate_report, None);
        assert_eq!(result.decision, Decision::NoGo);
        assert!(result.all_checks.iter().any(|c| c.name == "completion_checklist_present"));
    }
}
