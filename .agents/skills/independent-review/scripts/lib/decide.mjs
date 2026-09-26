const pct = (p) => `${Math.round(p * 100)}%`;

/**
 * Pure function: turns evidence into a verdict. No I/O, so it is easy to test and to tune.
 * verdict: approve | revise | human_review | no_changes
 *   revise       -> the implementer should fix things and ask for another round
 *   human_review -> stop; a person must look (sensitive/risky, or a gate could not run)
 */
export function decide({ round, cfg, diff, checks, openai, jev, errors, skipped }) {
  const revise = [];
  const human = [];
  const notes = [];

  if (!diff.text.trim()) return { verdict: 'no_changes', reasons: ['No changes to review.'], notes };

  if (round > cfg.maxRounds) {
    human.push(`Reached the maximum of ${cfg.maxRounds} review rounds; a person should decide how to proceed.`);
  }

  // 1) Deterministic checks are the source of truth.
  for (const c of checks.filter((x) => !x.ok)) {
    revise.push(`Check failed: ${c.name} (${c.timedOut ? 'timeout' : `exit code ${c.exitCode}`})`);
  }
  if (checks.length === 0) {
    (cfg.requireChecks ? human : notes).push(
      'No deterministic checks ran (nothing autodetected and none configured). Define "checks" in .agents/review.config.json.',
    );
  }

  if (diff.truncated) {
    human.push(`Diff is too large to review in full (${diff.chars} chars > ${cfg.diff.maxChars}). Split the change into smaller steps.`);
  }

  // 2) Cross-vendor LLM review. A gate that could not run never counts as a pass.
  if (skipped.openai === 'flag') human.push('Cross-model review was skipped by flag; the change cannot be approved without it.');
  else if (errors.openai) human.push(`Cross-model review unavailable: ${errors.openai}`);
  else if (openai) {
    const high = openai.review.findings.filter((f) => f.severity === 'high');
    if (high.length || openai.review.verdict === 'revise') {
      revise.push(`Cross-model review requests changes (${high.length} high-severity finding(s)).`);
    }
  }

  // 3) Jev triage with thresholds owned by this code.
  if (skipped.jev === 'flag') human.push('Jev triage was skipped by flag; the change cannot be approved without it.');
  else if (errors.jev) human.push(`Jev triage unavailable: ${errors.jev}`);
  else if (jev) {
    const a = jev.answers;
    const t = cfg.thresholds;
    if (a.unrelated_changes.noul >= t.unrelatedChanges) {
      revise.push(`Jev: the diff likely contains changes unrelated to the task (${pct(a.unrelated_changes.noul)}).`);
    }
    if (a.sensitive_area.noul >= t.sensitiveArea) {
      human.push(`Jev: touches a sensitive area (auth, secrets, payments, crypto or schema) (${pct(a.sensitive_area.noul)}).`);
    }
    if (t.publicInterface != null && a.public_interface.noul >= t.publicInterface) {
      human.push(`Jev: changes a public interface or storage format (${pct(a.public_interface.noul)}).`);
    } else if (a.public_interface.noul >= 0.5) {
      notes.push(`Jev: possible public interface change (${pct(a.public_interface.noul)}).`);
    }
    if (a.risk.score >= t.riskScoreHuman) {
      human.push(`Jev: risk score ${a.risk.score.toFixed(2)} of 2 (threshold ${t.riskScoreHuman}).`);
    }
    if (a.risk.confidence < t.riskMinConfidence) {
      human.push(`Jev is unsure about the risk level (confidence ${pct(a.risk.confidence)}).`);
    }
  }

  const verdict = revise.length ? 'revise' : human.length ? 'human_review' : 'approve';
  return { verdict, reasons: [...revise, ...human], notes };
}
