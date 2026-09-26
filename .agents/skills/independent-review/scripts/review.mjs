#!/usr/bin/env node
import { parseArgs } from 'node:util';
import { appendLog, git, loadConfig, loadEnvFiles } from './lib/util.mjs';
import { collectDiff } from './lib/diff.mjs';
import { detectChecks, runChecks } from './lib/checks.mjs';
import { crossReview } from './lib/openai.mjs';
import { jevGate } from './lib/jev.mjs';
import { decide } from './lib/decide.mjs';

const HELP = `independent-review: review gate for the current working-tree changes.

Usage:
  node .agents/skills/independent-review/scripts/review.mjs --task "<what the change is meant to do>" [options]

Options:
  --task <text>      (required) The goal of the change, in the user's words.
  --round <n>        Review round, starting at 1. After the configured maximum the verdict is human_review.
  --base <ref>       Diff against this git ref instead of HEAD.
  --skip-openai      Skip the cross-vendor review (the verdict can then never be "approve").
  --skip-jev         Skip Jev triage (the verdict can then never be "approve").
  --force-review     Call the LLMs even when deterministic checks already failed.
  -h, --help         Show this help.

Output: one JSON object on stdout. Read "verdict": approve | revise | human_review | no_changes.
Exit code is 0 whenever the gate ran; non-zero only for usage errors or crashes.
Environment: OPENAI_API_KEY, OPENAI_REVIEW_MODEL, TYPESAFE_API_KEY (see README).`;

const die = (msg) => {
  console.error(`review.mjs: ${msg}`);
  process.exit(1);
};
const errMsg = (e) => (e instanceof Error ? e.message : String(e));

async function main() {
  let values;
  try {
    ({ values } = parseArgs({
      options: {
        task: { type: 'string' },
        round: { type: 'string', default: '1' },
        base: { type: 'string' },
        'skip-openai': { type: 'boolean', default: false },
        'skip-jev': { type: 'boolean', default: false },
        'force-review': { type: 'boolean', default: false },
        help: { type: 'boolean', short: 'h', default: false },
      },
    }));
  } catch (e) {
    die(`${errMsg(e)}\n\n${HELP}`);
  }
  if (values.help) {
    console.log(HELP);
    return;
  }
  if (!values.task?.trim()) die(`--task is required\n\n${HELP}`);

  const top = git(['rev-parse', '--show-toplevel'], { allowFail: true });
  if (!top.ok) die('not inside a git repository');
  const root = top.out.trim();

  loadEnvFiles(root);
  const { cfg, file: cfgFile } = loadConfig(root);
  const round = Number.parseInt(values.round, 10) || 1;
  const task = values.task.trim();

  const diff = collectDiff(root, cfg, values.base);
  const checkDefs = cfg.checks ?? detectChecks(root);
  const checks = diff.text.trim() ? runChecks(root, checkDefs) : [];

  const checksFailed = checks.some((c) => !c.ok);
  const early = (round > cfg.maxRounds || (checksFailed && !values['force-review'])) && diff.text.trim();

  const skipped = {
    openai: values['skip-openai'] ? 'flag' : early || !diff.text.trim() ? 'early' : null,
    jev: values['skip-jev'] ? 'flag' : early || !diff.text.trim() ? 'early' : null,
  };
  const errors = {};

  const [openai, jev] = await Promise.all([
    skipped.openai
      ? null
      : crossReview({ task, diffText: diff.text, checks, model: cfg.openai.model, timeoutSec: cfg.openai.timeoutSec }).catch((e) => {
          errors.openai = errMsg(e);
          return null;
        }),
    skipped.jev
      ? null
      : jevGate({ task, diffText: diff.text, model: cfg.jev.model }).catch((e) => {
          errors.jev = errMsg(e);
          return null;
        }),
  ]);

  const { verdict, reasons, notes } = decide({ round, cfg, diff, checks, openai, jev, errors, skipped });

  const report = {
    verdict,
    round,
    reasons,
    notes,
    checks: checks.map(({ name, cmd, ok, exitCode, timedOut, durationMs, outputTail }) => ({
      name, cmd, ok, exitCode, timedOut, durationMs, ...(ok ? {} : { outputTail }),
    })),
    review: openai ? { model: openai.model, summary: openai.review.summary, verdict: openai.review.verdict, findings: openai.review.findings } : null,
    jev: jev
      ? {
          model: jev.model,
          answers: {
            sensitive_area: { p_yes: jev.answers.sensitive_area.noul },
            public_interface: { p_yes: jev.answers.public_interface.noul },
            unrelated_changes: { p_yes: jev.answers.unrelated_changes.noul },
            risk: { score_0_to_2: jev.answers.risk.score, confidence: jev.answers.risk.confidence },
          },
        }
      : null,
    errors,
    diff: { files: diff.files, chars: diff.chars, truncated: diff.truncated },
    config: cfgFile ?? 'defaults',
  };

  appendLog(root, {
    ts: new Date().toISOString(),
    task,
    ...report,
    thresholds: cfg.thresholds,
    usage: { openai: openai?.usage ?? null, jev: jev?.usage ?? null },
  });

  console.log(JSON.stringify(report, null, 2));
}

main().catch((e) => die(errMsg(e)));
