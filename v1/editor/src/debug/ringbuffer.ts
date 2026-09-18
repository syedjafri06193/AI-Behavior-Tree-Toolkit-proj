/**
 * Fixed-capacity history (design.md section 7.3).
 *
 * "The editor should be able to scrub backward a few seconds without a full
 * recording session. Keep a fixed-size ring buffer in the game process and let
 * the editor request history on demand."
 *
 * Fixed capacity is the point, and the document says why: "a debug buffer that
 * grows will eventually be the reason a long play session runs out of memory."
 * That is not hypothetical -- an overnight soak test is exactly when the
 * interesting AI bugs appear and exactly when an unbounded buffer kills the
 * process before anyone reads it.
 *
 * This is the editor-side mirror of the same structure, holding what has been
 * received so the timeline can scrub without asking the game again.
 */

export interface TimestampedFrame {
  time: number;
  agentId: number;
  selectedAction: number;
  scores: number[];
}

export class RingBuffer<T extends { time: number }> {
  private items: (T | undefined)[];
  private head = 0;
  private count = 0;

  /** 600 frames is 60 seconds at 10 Hz -- the design document's own figure. */
  constructor(readonly capacity: number = 600) {
    if (capacity <= 0) throw new Error("capacity must be positive");
    this.items = new Array<T | undefined>(capacity);
  }

  get size(): number {
    return this.count;
  }

  get full(): boolean {
    return this.count === this.capacity;
  }

  push(item: T): void {
    this.items[this.head] = item;
    this.head = (this.head + 1) % this.capacity;
    if (this.count < this.capacity) this.count++;
  }

  /** Oldest to newest. */
  toArray(): T[] {
    const out: T[] = [];
    const start = (this.head - this.count + this.capacity) % this.capacity;
    for (let i = 0; i < this.count; i++) {
      out.push(this.items[(start + i) % this.capacity]!);
    }
    return out;
  }

  /**
   * Frames within [startTime, endTime].
   *
   * Linear rather than binary search: at 600 entries the scan is free, and a
   * binary search over a ring buffer is a well-known place to put an off-by-one
   * that only shows up after the buffer wraps -- which is to say, only in a
   * long session, which is the only kind this exists for.
   */
  range(startTime: number, endTime: number): T[] {
    return this.toArray().filter((f) => f.time >= startTime && f.time <= endTime);
  }

  latest(): T | undefined {
    if (this.count === 0) return undefined;
    return this.items[(this.head - 1 + this.capacity) % this.capacity];
  }

  /** The window the editor can scrub over, in seconds. */
  span(): number {
    const frames = this.toArray();
    if (frames.length < 2) return 0;
    return frames[frames.length - 1]!.time - frames[0]!.time;
  }

  clear(): void {
    this.items = new Array<T | undefined>(this.capacity);
    this.head = 0;
    this.count = 0;
  }
}

/**
 * Per-agent history, so the editor can scrub one agent without walking every
 * frame of every other one.
 *
 * Bounded in both dimensions: a session that spawns and kills agents over
 * hours would otherwise accumulate one buffer per agent that ever existed,
 * which is the same unbounded-growth failure one level up.
 */
export class AgentHistory {
  private buffers = new Map<number, RingBuffer<TimestampedFrame>>();
  private order: number[] = [];

  constructor(
    readonly framesPerAgent = 600,
    readonly maxAgents = 64,
  ) {}

  push(frame: TimestampedFrame): void {
    let buffer = this.buffers.get(frame.agentId);
    if (!buffer) {
      if (this.buffers.size >= this.maxAgents) {
        const evict = this.order.shift();
        if (evict !== undefined) this.buffers.delete(evict);
      }
      buffer = new RingBuffer<TimestampedFrame>(this.framesPerAgent);
      this.buffers.set(frame.agentId, buffer);
      this.order.push(frame.agentId);
    }
    buffer.push(frame);
  }

  agent(agentId: number): readonly TimestampedFrame[] {
    return this.buffers.get(agentId)?.toArray() ?? [];
  }

  agentIds(): number[] {
    return [...this.buffers.keys()].sort((a, b) => a - b);
  }

  /**
   * Score history for one action over time -- the input to section 8.2's
   * score history graph, where "oscillation becomes visible as crossing lines
   * rather than as a twitching character".
   */
  scoreHistory(agentId: number, actionIndex: number): { time: number; score: number }[] {
    return this.agent(agentId).map((f) => ({
      time: f.time,
      score: f.scores[actionIndex] ?? 0,
    }));
  }

  /** Where the agent changed its mind, for the timeline markers. */
  switchTimes(agentId: number): number[] {
    const frames = this.agent(agentId);
    const out: number[] = [];
    for (let i = 1; i < frames.length; i++) {
      if (frames[i]!.selectedAction !== frames[i - 1]!.selectedAction) {
        out.push(frames[i]!.time);
      }
    }
    return out;
  }
}
