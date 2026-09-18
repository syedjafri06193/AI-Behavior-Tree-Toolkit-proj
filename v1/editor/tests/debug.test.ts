/**
 * The debug protocol and the history buffer (design.md sections 7, 8.2).
 */

import { describe, expect, it } from "vitest";

import {
  CommandType,
  DetailLevel,
  MsgType,
  SCORE_EPSILON,
  applyFrame,
  decodeMessage,
  encodeCommand,
  encodeFrame,
  encodeHello,
  encodeSubscribe,
  estimateBytesPerSecond,
  frameStream,
  type FrameMessage,
  type Subscription,
} from "../src/debug/protocol.js";
import { AgentHistory, RingBuffer } from "../src/debug/ringbuffer.js";

describe("the arithmetic that shapes the protocol", () => {
  it("streaming everything really is 2.4 MB/s", () => {
    // Section 7.2: 200 agents x 30 actions x 5 considerations x 10 Hz =
    // 300,000 values/second. Asserting the premise keeps the design rationale
    // honest rather than quoted.
    const values = 200 * 30 * 5 * 10;
    expect(values).toBe(300_000);
    expect(values * 8).toBeGreaterThan(2_000_000);

    const full = estimateBytesPerSecond(200, 30, 5, 10, DetailLevel.FullBreakdown);
    expect(full).toBeGreaterThan(2_000_000);
  });

  it("subscribing to one agent is three orders of magnitude cheaper", () => {
    const all = estimateBytesPerSecond(200, 30, 5, 10, DetailLevel.FullBreakdown);
    const one = estimateBytesPerSecond(1, 30, 5, 10, DetailLevel.FullBreakdown);
    expect(one * 200).toBeCloseTo(all, 0);
    expect(one).toBeLessThan(20_000);
  });

  it("summary detail is cheap enough for every agent at once", () => {
    // "Tiny -- fine for all agents", and it has to be, because the agent list
    // is how you find the one worth subscribing to.
    const summary = estimateBytesPerSecond(
      200, 30, 5, 10, DetailLevel.Summary, 0.1,
    );
    expect(summary).toBeLessThan(50_000);
  });

  it("deltas collapse to nothing when the situation is stable", () => {
    // "An agent whose state hasn't changed sends nothing. Most agents are idle
    // most of the time, and the wire goes quiet accordingly."
    const stable = estimateBytesPerSecond(200, 30, 5, 10, DetailLevel.Scores, 0.05);
    const churning = estimateBytesPerSecond(200, 30, 5, 10, DetailLevel.Scores, 1);
    expect(stable).toBeLessThan(churning / 4);
  });
});

describe("framing", () => {
  it("splits a stream into messages regardless of read boundaries", () => {
    // TCP does not preserve message boundaries. A client that assumes one read
    // is one message works on localhost and fails against the dedicated server
    // this protocol exists for (section 7.1).
    const messages = [
      encodeHello({
        protocolVersion: 1,
        behaviorName: "GuardBehavior",
        behaviorVersion: 12345,
        agentCount: 42,
        cooked: true,
      }),
      encodeSubscribe({
        agentIds: [7],
        detail: DetailLevel.FullBreakdown,
        maxHz: 10,
        deltasOnly: true,
      }),
      encodeCommand(CommandType.ForceAction, 7, 2),
    ];

    const stream = new Uint8Array(
      messages.reduce((n, m) => n + m.length, 0),
    );
    let offset = 0;
    for (const m of messages) {
      stream.set(m, offset);
      offset += m.length;
    }

    // Feed it one byte at a time, which is the worst case a socket can produce.
    let pending: Uint8Array<ArrayBufferLike> = new Uint8Array(0);
    const decoded: MsgType[] = [];
    for (let i = 0; i < stream.length; i++) {
      const next = new Uint8Array(pending.length + 1);
      next.set(pending);
      next[pending.length] = stream[i]!;
      const { frames, rest } = frameStream(next);
      for (const f of frames) decoded.push(decodeMessage(f).type);
      pending = rest;
    }

    expect(decoded).toEqual([MsgType.Hello, MsgType.Subscribe, MsgType.Command]);
    expect(pending.length).toBe(0);
  });

  it("round-trips a subscription", () => {
    const sub: Subscription = {
      agentIds: [3, 9, 27],
      detail: DetailLevel.Scores,
      maxHz: 10,
      deltasOnly: true,
    };
    const { frames } = frameStream(encodeSubscribe(sub));
    const decoded = decodeMessage(frames[0]!).body as Subscription;
    expect(decoded).toEqual(sub);
  });

  it("round-trips the hello, including whether this is a cooked build", () => {
    // Section 11.3: editor performance is not shipping performance. The editor
    // shows which it is attached to, because it changes what the numbers mean.
    const hello = {
      protocolVersion: 1,
      behaviorName: "GuardBehavior",
      behaviorVersion: 0xdeadbeef,
      agentCount: 200,
      cooked: true,
    };
    const { frames } = frameStream(encodeHello(hello));
    expect(decodeMessage(frames[0]!).body).toEqual(hello);
  });

  it("round-trips every command, including hot reload", () => {
    // Section 7.4: hot reload "is the one that changes how it feels to work".
    for (const command of Object.values(CommandType).filter(
      (v): v is CommandType => typeof v === "number",
    )) {
      const { frames } = frameStream(
        encodeCommand(command, 11, 3, 0.25, "ai/guard.aib"),
      );
      const body = decodeMessage(frames[0]!).body as {
        command: CommandType;
        agentId: number;
        index: number;
        value: number;
        payload: string;
      };
      expect(body.command).toBe(command);
      expect(body.agentId).toBe(11);
      expect(body.index).toBe(3);
      expect(body.value).toBeCloseTo(0.25, 6);
      expect(body.payload).toBe("ai/guard.aib");
    }
  });
});

describe("delta encoding", () => {
  const scores = [0.81, 0.42, 0.17, 0.05];

  it("a keyframe carries every score", () => {
    const { frames } = frameStream(encodeFrame(1, 0, scores, 0, null));
    const msg = decodeMessage(frames[0]!).body as FrameMessage;
    expect(msg.changedMask).toBe(0b1111);
    expect(msg.scores.filter((s) => s !== undefined)).toHaveLength(4);
  });

  it("an unchanged agent sends no scores at all", () => {
    const { frames } = frameStream(encodeFrame(1, 0.1, scores, 0, scores));
    const msg = decodeMessage(frames[0]!).body as FrameMessage;
    expect(msg.changedMask).toBe(0);
    expect(msg.scores.every((s) => s === undefined)).toBe(true);
    // And the frame is small: header only.
    expect(frames[0]!.length).toBeLessThan(32);
  });

  it("jitter below the epsilon does not count as a change", () => {
    // Section 15.3's 0.001: "small numerical jitter doesn't count as a change,
    // which cuts traffic substantially on an agent whose situation is stable."
    const jittered = scores.map((s) => s + SCORE_EPSILON / 4);
    const { frames } = frameStream(encodeFrame(1, 0.1, jittered, 0, scores));
    expect((decodeMessage(frames[0]!).body as FrameMessage).changedMask).toBe(0);
  });

  it("a real change is carried and applied", () => {
    const changed = [0.81, 0.9, 0.17, 0.05];
    const { frames } = frameStream(encodeFrame(1, 0.2, changed, 1, scores));
    const msg = decodeMessage(frames[0]!).body as FrameMessage;
    expect(msg.changedMask).toBe(0b0010);
    expect(msg.selectedAction).toBe(1);

    // A score not on the wire is UNCHANGED, not zero. Getting this backwards
    // produces a breakdown view where every stable action reads 0.000, which
    // looks like a scoring bug and is not one.
    //
    // Compared with a tolerance because scores cross the wire as float32:
    // 0.9 comes back as 0.8999999761581421. That is the format doing what it
    // is supposed to -- four bytes per score is what keeps section 7.2's
    // arithmetic survivable -- and it is three orders of magnitude below the
    // 0.001 epsilon the protocol already treats as "unchanged".
    const applied = applyFrame(scores, msg);
    expect(applied).toHaveLength(changed.length);
    applied.forEach((v, i) => expect(v).toBeCloseTo(changed[i]!, 6));

    // The entries that were not on the wire came through exactly, because
    // they were never re-encoded.
    expect(applied[0]).toBe(scores[0]);
    expect(applied[2]).toBe(scores[2]);
  });

  it("applying a sequence of deltas reconstructs the truth exactly", () => {
    const sequence = [
      [0.1, 0.2, 0.3],
      [0.1, 0.9, 0.3],
      [0.5, 0.9, 0.3],
      [0.5, 0.9, 0.3],
      [0.5, 0.9, 0.0],
    ];

    let previous: number[] | null = null;
    let reconstructed: number[] = [];
    for (const [i, truth] of sequence.entries()) {
      const { frames } = frameStream(encodeFrame(1, i * 0.1, truth, 0, previous));
      const msg = decodeMessage(frames[0]!).body as FrameMessage;
      reconstructed = applyFrame(reconstructed, msg);
      for (let a = 0; a < truth.length; a++) {
        expect(reconstructed[a]).toBeCloseTo(truth[a]!, 5);
      }
      previous = truth;
    }
  });

  it("refuses to silently drop scores past the mask width", () => {
    // The mask is 32 bits. A behavior with 33 actions is legal, and quietly
    // never updating action 33 would present as an action whose score is
    // frozen -- a bug nobody would attribute to the wire format.
    const many = new Array(33).fill(0.5);
    expect(() => encodeFrame(1, 0, many, 0, null)).toThrow(/32 bits/);
  });
});

describe("ring buffer", () => {
  it("holds a fixed window and overwrites the oldest", () => {
    // "A debug buffer that grows will eventually be the reason a long play
    // session runs out of memory" (section 7.3).
    const buffer = new RingBuffer<{ time: number }>(600);
    for (let i = 0; i < 10_000; i++) buffer.push({ time: i * 0.1 });

    expect(buffer.size).toBe(600);
    expect(buffer.full).toBe(true);
    const frames = buffer.toArray();
    expect(frames).toHaveLength(600);
    expect(frames[0]!.time).toBeCloseTo((10_000 - 600) * 0.1, 5);
    expect(buffer.latest()!.time).toBeCloseTo(9999 * 0.1, 5);
  });

  it("600 frames at 10 Hz is the documented 60 seconds of scrubback", () => {
    const buffer = new RingBuffer<{ time: number }>(600);
    for (let i = 0; i < 600; i++) buffer.push({ time: i * 0.1 });
    expect(buffer.span()).toBeCloseTo(59.9, 5);
  });

  it("range queries stay correct after the buffer wraps", () => {
    // Wrapping is the only interesting case, and it only happens in a long
    // session -- which is the only kind this exists for.
    const buffer = new RingBuffer<{ time: number }>(10);
    for (let i = 0; i < 25; i++) buffer.push({ time: i });

    expect(buffer.range(0, 100).map((f) => f.time)).toEqual([
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    ]);
    expect(buffer.range(18, 20).map((f) => f.time)).toEqual([18, 19, 20]);
    expect(buffer.range(0, 5)).toEqual([]);
  });
});

describe("per-agent history", () => {
  it("keeps agents separate and bounds how many it tracks", () => {
    const history = new AgentHistory(100, 4);
    for (let agent = 0; agent < 10; agent++) {
      for (let i = 0; i < 5; i++) {
        history.push({
          time: i * 0.1,
          agentId: agent,
          selectedAction: 0,
          scores: [0.5],
        });
      }
    }
    // A session that spawns and kills agents for hours would otherwise
    // accumulate one buffer per agent that ever existed.
    expect(history.agentIds()).toHaveLength(4);
  });

  it("produces the score history graph from section 8.2", () => {
    // "Add a score history graph -- each action's score over the last few
    // seconds -- and oscillation becomes visible as crossing lines rather than
    // as a twitching character."
    const history = new AgentHistory();
    for (let i = 0; i < 50; i++) {
      history.push({
        time: i * 0.1,
        agentId: 3,
        selectedAction: i % 10 < 5 ? 0 : 1,
        scores: [0.5 + 0.1 * Math.sin(i), 0.5 + 0.1 * Math.cos(i)],
      });
    }

    const series = history.scoreHistory(3, 0);
    expect(series).toHaveLength(50);
    expect(series[0]!.time).toBeCloseTo(0, 5);

    // And the switch markers, which are what make the crossings legible.
    const switches = history.switchTimes(3);
    expect(switches.length).toBeGreaterThan(5);
  });
});
