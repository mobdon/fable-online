// Structured event log. Prometheus only holds totals; anything tied to a specific
// hero (who minted the gold, who killed whom) goes here as one JSON object per line,
// so it can be searched later or loaded into ClickHouse / Loki without changes.

import { createWriteStream, mkdirSync, type WriteStream } from "node:fs";
import { dirname, join } from "node:path";

export interface AnalyticsEvent {
  /** ISO timestamp, set by the sink if missing. */
  ts?: string;
  type: string;
  [field: string]: unknown;
}

export interface EventSink {
  write(event: AnalyticsEvent): void;
  close?(): Promise<void>;
}

/** Keeps events in memory. For tests and the demo. */
export class MemorySink implements EventSink {
  readonly events: AnalyticsEvent[] = [];
  write(event: AnalyticsEvent) { this.events.push(event); }
  ofType(type: string) { return this.events.filter((e) => e.type === type); }
}

/** Appends JSON lines to one file per UTC day: <dir>/events-YYYY-MM-DD.jsonl */
export class JsonlFileSink implements EventSink {
  private day = "";
  private stream: WriteStream | undefined;
  constructor(private readonly dir: string) {}

  write(event: AnalyticsEvent) {
    const ts = event.ts ?? new Date().toISOString();
    const day = ts.slice(0, 10);
    if (day !== this.day || !this.stream) {
      this.stream?.end();
      const file = join(this.dir, `events-${day}.jsonl`);
      mkdirSync(dirname(file), { recursive: true });
      this.stream = createWriteStream(file, { flags: "a" });
      this.day = day;
    }
    this.stream.write(JSON.stringify({ ...event, ts }) + "\n");
  }

  close() {
    return new Promise<void>((resolve) => (this.stream ? this.stream.end(resolve) : resolve()));
  }
}

/** Sends every event to several sinks. */
export class FanoutSink implements EventSink {
  constructor(private readonly sinks: EventSink[]) {}
  write(event: AnalyticsEvent) { for (const s of this.sinks) s.write(event); }
  async close() { await Promise.all(this.sinks.map((s) => s.close?.())); }
}
