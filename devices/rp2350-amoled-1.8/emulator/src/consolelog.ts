// A rolling buffer of the firmware's own log lines (env.js_log, emu_abi.h:
// "This is the firmware's printf"), shown in an on-page console pane. Also
// mirrored to the real devtools console, and readable back out for a freeze
// bundle (see freeze.ts).

export interface LogLine {
  t: number;
  text: string;
}

export class ConsoleLog {
  lines: LogLine[] = [];
  private max: number;
  private onLine?: (line: LogLine) => void;

  constructor(max = 500, onLine?: (line: LogLine) => void) {
    this.max = max;
    this.onLine = onLine;
  }

  push(text: string): void {
    const line: LogLine = { t: performance.now(), text };
    this.lines.push(line);
    if (this.lines.length > this.max) this.lines.shift();
    console.log("[fw]", text);
    this.onLine?.(line);
  }

  recent(n: number): LogLine[] {
    return this.lines.slice(-n);
  }
}
