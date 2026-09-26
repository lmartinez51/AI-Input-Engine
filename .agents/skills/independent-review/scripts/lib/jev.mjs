/**
 * Jev (TypeSafe AI "System One") triage over the diff.
 * Jev only answers narrow, typed questions; every threshold and every decision lives in decide.mjs.
 * Anything code can compute exactly (did the checks pass? was the diff truncated?) is NOT asked here.
 */
export async function jevGate({ task, diffText, model }) {
  let sdk;
  try {
    sdk = await import('@typesafe-ai/sdk');
  } catch {
    throw new Error('Package @typesafe-ai/sdk is not installed. Run `npm install` inside .agents/skills/independent-review');
  }
  if (!process.env.TYPESAFE_API_KEY) throw new Error('TYPESAFE_API_KEY is not set');

  const { TypeSafeClient, noul, score } = sdk;
  const client = new TypeSafeClient();

  const truncatedDiff = diffText.length > 60_000
    ? diffText.slice(0, 60_000) + '\n... [diff truncated for triage evaluation]'
    : diffText;

  const res = await client.systemOne(
    {
      model,
      state: { task, diff: truncatedDiff },
      questions: {
        sensitive_area: noul(
          'The change in `diff` modifies authentication, authorization, secrets or credentials handling, payments, cryptography, or database schema/migrations.',
        ),
        public_interface: noul(
          'The change in `diff` alters a public API, an exported function or class signature, a network protocol, or a file/storage format that other code or clients depend on.',
        ),
        unrelated_changes: noul(
          'The change in `diff` includes edits that are not needed to achieve the goal described in `task`.',
        ),
        risk: score('How risky is applying the change in `diff` for the goal in `task`?', [
          'Low: small, local change that is easy to revert',
          'Medium: changes shared logic, several modules, or behavior other code depends on',
          'High: could cause data loss, a security exposure, or an outage if it is wrong',
        ]),
      },
    },
    { timeout: 30_000 },
  );

  return res; // { model, answers, usage }
}
