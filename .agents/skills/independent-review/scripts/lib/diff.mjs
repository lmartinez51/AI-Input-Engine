import fs from 'node:fs';
import path from 'node:path';
import { git } from './util.mjs';

const MAX_UNTRACKED_BYTES = 200_000;

function looksBinary(buf) {
  return buf.subarray(0, 8000).includes(0);
}

function renderNewFile(rel, abs) {
  const stat = fs.statSync(abs);
  if (stat.size > MAX_UNTRACKED_BYTES) {
    return `diff --git a/${rel} b/${rel}\nnew file (omitted: ${stat.size} bytes, too large)\n`;
  }
  const buf = fs.readFileSync(abs);
  if (looksBinary(buf)) return `diff --git a/${rel} b/${rel}\nnew binary file (omitted)\n`;
  const lines = buf.toString('utf8').split(/\r?\n/);
  if (lines.at(-1) === '') lines.pop();
  return [
    `diff --git a/${rel} b/${rel}`,
    'new file mode 100644',
    '--- /dev/null',
    `+++ b/${rel}`,
    `@@ -0,0 +1,${lines.length} @@`,
    ...lines.map((l) => `+${l}`),
    '',
  ].join('\n');
}

/**
 * Working tree vs `base` (staged + unstaged) plus untracked files rendered as new files.
 */
export function collectDiff(root, cfg, baseOverride) {
  const base = baseOverride || cfg.diff.base;
  const hasHead = git(['rev-parse', '--verify', 'HEAD'], { cwd: root, allowFail: true }).ok;
  const excludes = cfg.diff.exclude.map((p) => `:(exclude,glob)**/${p}`).concat(
    cfg.diff.exclude.map((p) => `:(exclude,glob)${p}`),
  );

  const baseArgs = ['diff', '--no-color', '--unified=3'];
  if (hasHead || baseOverride) baseArgs.push(base);
  else baseArgs.push('--cached');

  const tracked = git([...baseArgs, '--', '.', ...excludes], { cwd: root }).out;
  const names = git([...baseArgs, '--name-only', '--', '.', ...excludes], { cwd: root })
    .out.split(/\r?\n/)
    .filter(Boolean);

  const untracked = git(
    ['ls-files', '--others', '--exclude-standard', '--', '.', ...excludes],
    { cwd: root },
  )
    .out.split(/\r?\n/)
    .filter(Boolean);

  const parts = [tracked];
  for (const rel of untracked) {
    try {
      parts.push(renderNewFile(rel, path.join(root, rel)));
    } catch {
      /* unreadable file: skip */
    }
  }

  let text = parts.join('\n');
  const chars = text.length;
  const truncated = chars > cfg.diff.maxChars;
  if (truncated) text = `${text.slice(0, cfg.diff.maxChars)}\n[... diff truncated ...]\n`;

  return { text, chars, truncated, files: [...names, ...untracked] };
}
