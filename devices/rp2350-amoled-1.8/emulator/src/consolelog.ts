// A rolling buffer of log lines shown in the on-page console pane, from two
// sources: the emulated firmware's own printf (env.js_log, emu_abi.h) and,
// once a real board is connected (devlink.ts), the board's own serial
// output. Both land in the same pane, which is exactly the confusion this
// task exists to prevent (see main.ts's device-line wiring): "source"
// defaults to "fw" so every existing call site (main.ts's consoleLog.push,
// unchanged) needs no update and reads exactly as it did before a device
// was ever part of this page. Also mirrored to the real devtools console,
// and readable back out for a freeze bundle (see freeze.ts).

export type LogSource = "fw" | "device";

export interface LogLine {
  t: number;
  text: string;
  source: LogSource;
}

export class ConsoleLog {
  lines: LogLine[] = [];
  private max: number;
  private onLine?: (line: LogLine) => void;

  constructor(max = 500, onLine?: (line: LogLine) => void) {
    this.max = max;
    this.onLine = onLine;
  }

  push(text: string, source: LogSource = "fw"): void {
    const line: LogLine = { t: performance.now(), text, source };
    this.lines.push(line);
    if (this.lines.length > this.max) this.lines.shift();
    console.log(source === "device" ? "[dev]" : "[fw]", text);
    this.onLine?.(line);
  }

  recent(n: number): LogLine[] {
    return this.lines.slice(-n);
  }
}
