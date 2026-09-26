import fs from 'node:fs';
import path from 'node:path';
import { sh, tail } from './util.mjs';

const has = (root, p) => fs.existsSync(path.join(root, p));
const read = (root, p) => {
  try {
    return fs.readFileSync(path.join(root, p), 'utf8');
  } catch {
    return '';
  }
};

/**
 * Autodetects project checks for Flutter/Dart, Node and Python.
 * C/C++ and ESP-IDF projects: define `checks` in .agents/review.config.json instead.
 */
export function detectChecks(root) {
  const checks = [];

  if (has(root, 'pubspec.yaml')) {
    const pub = read(root, 'pubspec.yaml');
    const tool = /sdk:\s*flutter/.test(pub) || /^flutter:/m.test(pub) ? 'flutter' : 'dart';
    checks.push({ name: `${tool} analyze`, cmd: `${tool} analyze`, timeoutSec: 600 });
    if (has(root, 'test')) checks.push({ name: `${tool} test`, cmd: `${tool} test`, timeoutSec: 900 });
  }

  if (has(root, 'package.json')) {
    let scripts = {};
    try {
      scripts = JSON.parse(read(root, 'package.json')).scripts ?? {};
    } catch {
      /* ignore */
    }
    const pm = has(root, 'pnpm-lock.yaml') ? 'pnpm' : has(root, 'yarn.lock') ? 'yarn' : 'npm';
    const run = (n) => (pm === 'npm' ? `npm run ${n}` : `${pm} ${n}`);
    if (scripts.lint) checks.push({ name: 'lint', cmd: run('lint'), timeoutSec: 300 });
    if (scripts.typecheck) checks.push({ name: 'typecheck', cmd: run('typecheck'), timeoutSec: 300 });
    else if (has(root, 'tsconfig.json')) checks.push({ name: 'tsc', cmd: 'npx tsc --noEmit', timeoutSec: 300 });
    if (scripts.test && !/no test specified/i.test(scripts.test)) {
      checks.push({ name: 'test', cmd: pm === 'npm' ? 'npm test' : `${pm} test`, timeoutSec: 900 });
    }
  }

  const pyproject = read(root, 'pyproject.toml');
  const isPython = has(root, 'pyproject.toml') || has(root, 'requirements.txt') || has(root, 'setup.py') || has(root, 'pytest.ini');
  if (isPython) {
    if (has(root, 'ruff.toml') || has(root, '.ruff.toml') || /\[tool\.ruff/.test(pyproject)) {
      checks.push({ name: 'ruff', cmd: 'ruff check .', timeoutSec: 300 });
    }
    if (has(root, 'mypy.ini') || /\[tool\.mypy/.test(pyproject)) {
      checks.push({ name: 'mypy', cmd: 'mypy .', timeoutSec: 600 });
    }
    if (has(root, 'pytest.ini') || has(root, 'tests') || has(root, 'test') || /\[tool\.pytest/.test(pyproject)) {
      checks.push({ name: 'pytest', cmd: 'python -m pytest -q', timeoutSec: 900 });
    }
  }

  return checks;
}

export function runChecks(root, defs) {
  return defs.map((d) => {
    const r = sh(d.cmd, { cwd: root, timeoutMs: (d.timeoutSec ?? 300) * 1000 });
    return {
      name: d.name,
      cmd: d.cmd,
      ok: r.ok,
      exitCode: r.exitCode,
      timedOut: r.timedOut,
      durationMs: r.durationMs,
      outputTail: r.ok ? '' : tail(r.error ?? r.output, 3000),
    };
  });
}
