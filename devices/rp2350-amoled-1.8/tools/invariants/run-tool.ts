// Runs a binutils subprocess and returns its stdout as text, or throws. A
// nonzero exit or empty output is a hard failure here, for the same reason
// decision 0004 gives for the parser itself: an instrument that is skipped
// because its input is missing is indistinguishable, from the caller's
// point of view, from one that ran and passed.

export function runTool(bin: string, args: string[]): string {
  const result = Bun.spawnSync([bin, ...args], { stdout: "pipe", stderr: "pipe" });
  if (result.exitCode !== 0) {
    throw new Error(
      `${bin} ${args.join(" ")} exited ${result.exitCode}: ${result.stderr.toString()}`
    );
  }
  const out = result.stdout.toString();
  if (out.length === 0) {
    throw new Error(`${bin} ${args.join(" ")} produced no output`);
  }
  return out;
}
