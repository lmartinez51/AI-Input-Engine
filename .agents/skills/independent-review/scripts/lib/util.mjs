import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const SKILL_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

/** Run a shell command (shell: true so `flutter`, `npm`, `.bat` shims work on Windows). */
export function sh(cmd, { cwd, timeoutMs = 300_000 } = {}) {
  const t0 = Date.now();
  const r = spawnSync(cmd, {
    cwd,
    shell: true,
    encoding: 'utf8',
    timeout: timeoutMs,
    maxBuffer: 32 * 1024 * 1024,
  });
  const timedOut = r.error?.code === 'ETIMEDOUT';
  return {
    ok: r.status === 0 && !r.error,
    exitCode: r.status,
    timedOut,
    error: r.error && !timedOut ? String(r.error.message) : null,
    output: `${r.stdout ?? ''}${r.stderr ?? ''}`,
    durationMs: Date.now() - t0,
  };
}

/** Run git without a shell (no quoting issues). */
export function git(args, { cwd, allowFail = false } = {}) {
  const r = spawnSync('git', args, { cwd, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  const ok = r.status === 0;
  if (!ok && !allowFail) {
    throw new Error(`git ${args.join(' ')} failed: ${(r.stderr ?? '').trim()}`);
  }
  return { ok, out: r.stdout ?? '', err: r.stderr ?? '' };
}

export const tail = (s, max) => (s.length <= max ? s : `…${s.slice(-max)}`);

const ENV_WHITELIST = new Set([
  'OPENAI_API_KEY',
  'OPENAI_REVIEW_MODEL',
  'OPENAI_BASE_URL',
  'TYPESAFE_API_KEY',
  'TYPESAFE_BASE_URL',
]);

/**
 * Minimal .env loader limited to the variables this skill needs.
 * Reads <skill>/.env first, then <repo>/.env. Real environment variables always win.
 * (Inside Antigravity's terminal sandbox `.env` files are blocked, so exported env vars
 * or an unsandboxed run are the reliable options.)
 */
export function loadEnvFiles(root) {
  for (const file of [path.join(SKILL_DIR, '.env'), path.join(root, '.env')]) {
    let txt;
    try {
      txt = fs.readFileSync(file, 'utf8');
    } catch {
      continue;
    }
    for (const line of txt.split(/\r?\n/)) {
      const m = line.match(/^\s*(?:export\s+)?([A-Z0-9_]+)\s*=\s*(.*?)\s*$/);
      if (!m || !ENV_WHITELIST.has(m[1]) || process.env[m[1]]) continue;
      let v = m[2];
      if ((v.startsWith('"') && v.endsWith('"')) || (v.startsWith("'") && v.endsWith("'"))) {
        v = v.slice(1, -1);
      }
      process.env[m[1]] = v;
    }
  }
}

export const DEFAULTS = {
  maxRounds: 3,
  requireChecks: true,
  checks: null, // null = autodetect (Flutter/Dart, Node, Python). Set an array for C/C++/ESP-IDF.
  diff: {
    base: 'HEAD',
    maxChars: 80_000,
    exclude: [
      '.agents/**',
      'package-lock.json',
      'yarn.lock',
      'pnpm-lock.yaml',
      'pubspec.lock',
      'poetry.lock',
      'uv.lock',
      '*.min.js',
      '*.map',
    ],
  },
  openai: { model: null, timeoutSec: 180 },
  jev: { model: 'jev-1.13.0' },
  thresholds: {
    sensitiveArea: 0.6, // P(yes) at/above -> human_review
    publicInterface: null, // number -> human_review; null -> note only
    unrelatedChanges: 0.7, // P(yes) at/above -> revise
    riskScoreHuman: 1.5, // expected risk score (0..2) at/above -> human_review
    riskMinConfidence: 0.5, // Jev confidence on risk below this -> human_review
  },
};

function merge(base, extra) {
  if (Array.isArray(base) || Array.isArray(extra) || typeof base !== 'object' || base === null) {
    return extra === undefined ? base : extra;
  }
  const out = { ...base };
  for (const [k, v] of Object.entries(extra ?? {})) {
    out[k] = k in base && v && typeof v === 'object' && !Array.isArray(v) ? merge(base[k], v) : v;
  }
  return out;
}

export function loadConfig(root) {
  const candidates = [
    path.join(root, '.agents', 'review.config.json'),
    path.join(SKILL_DIR, 'review.config.json'),
  ];
  for (const file of candidates) {
    if (fs.existsSync(file)) {
      try {
        return { cfg: merge(DEFAULTS, JSON.parse(fs.readFileSync(file, 'utf8'))), file };
      } catch (e) {
        throw new Error(`Invalid JSON in ${file}: ${e.message}`);
      }
    }
  }
  return { cfg: DEFAULTS, file: null };
}

export function appendLog(root, record) {
  try {
    const dir = path.join(root, '.agents');
    fs.mkdirSync(dir, { recursive: true });
    fs.appendFileSync(path.join(dir, 'review-log.jsonl'), `${JSON.stringify(record)}\n`);
  } catch {
    /* logging must never break a review */
  }
}
