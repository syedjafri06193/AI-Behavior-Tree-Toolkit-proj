/**
 * The debug wire protocol (design.md section 7).
 *
 * The transport has to work from a cooked build and a dedicated server, over a
 * network, with the editor as a separate process -- because "the bugs that
 * matter often don't reproduce" in PIE (7.1).
 *
 * THE ARITHMETIC THAT SHAPES EVERYTHING (7.2):
 *
 *     200 agents x 30 actions x 5 considerations x 10 Hz = 300,000 values/s
 *
 * At 8 bytes each that is 2.4 MB/s before framing, and it is useless data,
 * because nobody is looking at 200 agents. Two mechanisms keep the wire quiet:
 *
 *   - SUBSCRIPTION. Three detail levels, and the expensive one is scoped to
 *     one agent at a time.
 *   - DELTAS. An agent whose state has not changed sends nothing. Most agents
 *     are idle most of the time and the wire goes quiet accordingly.
 *
 * Messages are length-prefixed so a stream can be framed without a parser, and
 * everything is little-endian. This is a binary protocol rather than JSON for
 * the reason in section 12: it has to be cheap enough to leave enabled in a
 * cooked build.
 */

export enum MsgType {
  /** editor -> game */
  Subscribe = 1,
  Unsubscribe = 2,
  Command = 3,
  RequestHistory = 4,
  /** game -> editor */
  Frame = 16,
  Breakdown = 17,
  Hello = 18,
  History = 19,
  AbortEvent = 20,
}

export enum DetailLevel {
  /** Current action per agent, on change only. Tiny -- fine for all agents. */
  Summary = 0,
  /** All action scores for subscribed agents. Moderate. */
  Scores = 1,
  /** Per-consideration values and curve outputs. Large -- one agent at a time. */
  FullBreakdown = 2,
}

export enum CommandType {
  /** Pause the AI without pausing the game. */
  Pause = 1,
  Resume = 2,
  Step = 3,
  /** Force an action, to test a branch without engineering the situation. */
  ForceAction = 4,
  /** Override an input, to see what the agent would do if health were 0.1. */
  OverrideInput = 5,
  ClearOverrides = 6,
  /**
   * Recompile the YAML and swap it in without restarting.
   *
   * Section 7.4 singles this one out: "Hot reload is the one that changes how
   * it feels to work. Tuning a curve and seeing the agent's behavior change
   * within a second, without leaving the running game, is the difference
   * between iteration and a build cycle."
   */
  HotReload = 7,
}

export interface Subscription {
  /** Usually exactly one. */
  agentIds: number[];
  detail: DetailLevel;
  maxHz: number;
  deltasOnly: boolean;
}

export interface FrameMessage {
  agentId: number;
  time: number;
  /** Indexed by action; only changed entries are on the wire. */
  scores: (number | undefined)[];
  selectedAction: number;
  /** Bitmask of which scores this message actually carries. */
  changedMask: number;
}

export interface BreakdownMessage {
  agentId: number;
  time: number;
  actionIndex: number;
  considerations: { normalized: number; curveOutput: number }[];
  rawProduct: number;
  compensation: number;
  finalScore: number;
  /** -1 when the action was fully evaluated. */
  stoppedAt: number;
}

export interface HelloMessage {
  protocolVersion: number;
  behaviorName: string;
  behaviorVersion: number;
  agentCount: number;
  /** True in a cooked build; the editor shows it, because it changes what the
   *  numbers mean (section 11.3: editor performance is not shipping
   *  performance). */
  cooked: boolean;
}

export const PROTOCOL_VERSION = 1;

/**
 * Scores closer than this count as unchanged.
 *
 * Section 15.3 uses 0.001 and notes it "cuts traffic substantially on an agent
 * whose situation is stable". It is also below what any breakdown view can
 * display, so nothing visible is lost.
 */
export const SCORE_EPSILON = 0.001;

// --------------------------------------------------------------- framing

class Writer {
  private bytes: number[] = [];

  u8(v: number): this {
    this.bytes.push(v & 0xff);
    return this;
  }
  u16(v: number): this {
    this.bytes.push(v & 0xff, (v >>> 8) & 0xff);
    return this;
  }
  u32(v: number): this {
    this.bytes.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
    return this;
  }
  f32(v: number): this {
    const dv = new DataView(new ArrayBuffer(4));
    dv.setFloat32(0, v, true);
    for (let i = 0; i < 4; i++) this.bytes.push(dv.getUint8(i));
    return this;
  }
  f64(v: number): this {
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, v, true);
    for (let i = 0; i < 8; i++) this.bytes.push(dv.getUint8(i));
    return this;
  }
  str(s: string): this {
    const enc = new TextEncoder().encode(s);
    this.u16(enc.length);
    for (const b of enc) this.bytes.push(b);
    return this;
  }
  /** Length-prefixed frame: u32 length, u8 type, payload. */
  finish(type: MsgType): Uint8Array {
    const payload = Uint8Array.from(this.bytes);
    const out = new Uint8Array(5 + payload.length);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, payload.length + 1, true);
    out[4] = type;
    out.set(payload, 5);
    return out;
  }
}

class Reader {
  private pos = 0;
  private view: DataView;
  constructor(private bytes: Uint8Array) {
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }
  private need(n: number): void {
    if (this.pos + n > this.bytes.length) {
      throw new Error(`truncated message at offset ${this.pos}`);
    }
  }
  u8(): number {
    this.need(1);
    return this.view.getUint8(this.pos++);
  }
  u16(): number {
    this.need(2);
    const v = this.view.getUint16(this.pos, true);
    this.pos += 2;
    return v;
  }
  u32(): number {
    this.need(4);
    const v = this.view.getUint32(this.pos, true);
    this.pos += 4;
    return v;
  }
  f32(): number {
    this.need(4);
    const v = this.view.getFloat32(this.pos, true);
    this.pos += 4;
    return v;
  }
  f64(): number {
    this.need(8);
    const v = this.view.getFloat64(this.pos, true);
    this.pos += 8;
    return v;
  }
  str(): string {
    const n = this.u16();
    this.need(n);
    const s = new TextDecoder().decode(this.bytes.subarray(this.pos, this.pos + n));
    this.pos += n;
    return s;
  }
  get remaining(): number {
    return this.bytes.length - this.pos;
  }
}

// -------------------------------------------------------------- encoding

export function encodeSubscribe(sub: Subscription): Uint8Array {
  const w = new Writer();
  w.u8(sub.detail).u8(sub.deltasOnly ? 1 : 0).f32(sub.maxHz);
  w.u16(sub.agentIds.length);
  for (const id of sub.agentIds) w.u32(id);
  return w.finish(MsgType.Subscribe);
}

export function encodeCommand(
  command: CommandType,
  agentId: number,
  index = 0,
  value = 0,
  payload = "",
): Uint8Array {
  const w = new Writer();
  w.u8(command).u32(agentId).u16(index).f32(value).str(payload);
  return w.finish(MsgType.Command);
}

export function encodeHello(hello: HelloMessage): Uint8Array {
  const w = new Writer();
  w.u16(hello.protocolVersion)
    .str(hello.behaviorName)
    .u32(hello.behaviorVersion)
    .u32(hello.agentCount)
    .u8(hello.cooked ? 1 : 0);
  return w.finish(MsgType.Hello);
}

/**
 * Encode a frame, sending only the scores that changed (section 15.3).
 *
 * `previous` is the last frame sent for this agent, or null for a keyframe. A
 * receiver that misses a delta would carry a stale score forever, so the
 * client tracks what it has and the server sends a keyframe on subscribe.
 */
export function encodeFrame(
  agentId: number,
  time: number,
  scores: readonly number[],
  selectedAction: number,
  previous: readonly number[] | null,
): Uint8Array {
  if (scores.length > 32) {
    // The mask is a u32. More than 32 actions in one behavior is possible and
    // would need a longer mask; failing loudly beats silently dropping action
    // 33's score, which would present as an action that never updates.
    throw new Error(
      `delta mask is 32 bits but this behavior has ${scores.length} actions`,
    );
  }

  let mask = 0;
  scores.forEach((s, i) => {
    if (previous === null || Math.abs(s - (previous[i] ?? 0)) >= SCORE_EPSILON) {
      mask |= 1 << i;
    }
  });

  const w = new Writer();
  w.u32(agentId).f64(time).u16(selectedAction).u16(scores.length).u32(mask);
  scores.forEach((s, i) => {
    if (mask & (1 << i)) w.f32(s);
  });
  return w.finish(MsgType.Frame);
}

// -------------------------------------------------------------- decoding

export interface DecodedMessage {
  type: MsgType;
  body: unknown;
}

/**
 * Split a byte stream into complete frames.
 *
 * TCP does not preserve message boundaries, so a client that assumes one read
 * equals one message works on localhost and fails the moment it is pointed at
 * a real dedicated server -- which is the case section 7.1 says this exists
 * for. Incomplete trailing bytes are returned for the caller to prepend to the
 * next read.
 */
export function frameStream(buffer: Uint8Array): {
  frames: Uint8Array[];
  rest: Uint8Array;
} {
  const frames: Uint8Array[] = [];
  let offset = 0;
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);

  while (offset + 4 <= buffer.length) {
    const length = view.getUint32(offset, true);
    if (offset + 4 + length > buffer.length) break;
    frames.push(buffer.subarray(offset + 4, offset + 4 + length));
    offset += 4 + length;
  }
  return { frames, rest: buffer.subarray(offset) };
}

export function decodeMessage(frame: Uint8Array): DecodedMessage {
  const r = new Reader(frame);
  const type = r.u8() as MsgType;

  switch (type) {
    case MsgType.Subscribe: {
      const detail = r.u8() as DetailLevel;
      const deltasOnly = r.u8() !== 0;
      const maxHz = r.f32();
      const count = r.u16();
      const agentIds: number[] = [];
      for (let i = 0; i < count; i++) agentIds.push(r.u32());
      return { type, body: { agentIds, detail, maxHz, deltasOnly } as Subscription };
    }
    case MsgType.Command: {
      return {
        type,
        body: {
          command: r.u8() as CommandType,
          agentId: r.u32(),
          index: r.u16(),
          value: r.f32(),
          payload: r.str(),
        },
      };
    }
    case MsgType.Hello: {
      return {
        type,
        body: {
          protocolVersion: r.u16(),
          behaviorName: r.str(),
          behaviorVersion: r.u32(),
          agentCount: r.u32(),
          cooked: r.u8() !== 0,
        } as HelloMessage,
      };
    }
    case MsgType.Frame: {
      const agentId = r.u32();
      const time = r.f64();
      const selectedAction = r.u16();
      const count = r.u16();
      const changedMask = r.u32();
      const scores: (number | undefined)[] = new Array(count).fill(undefined);
      for (let i = 0; i < count; i++) {
        if (changedMask & (1 << i)) scores[i] = r.f32();
      }
      return {
        type,
        body: { agentId, time, scores, selectedAction, changedMask } as FrameMessage,
      };
    }
    default:
      throw new Error(`unknown message type ${type}`);
  }
}

/**
 * Apply a delta frame onto the last known scores.
 *
 * The counterpart to the encoder: a score not on the wire is unchanged, not
 * zero. Getting this backwards produces a breakdown view where every stable
 * action reads 0.000, which looks like a scoring bug and is not one.
 */
export function applyFrame(
  previous: readonly number[],
  frame: FrameMessage,
): number[] {
  const out = previous.slice();
  out.length = frame.scores.length;
  frame.scores.forEach((s, i) => {
    if (s !== undefined) out[i] = s;
    else if (out[i] === undefined) out[i] = 0;
  });
  return out as number[];
}

/**
 * Estimated wire cost, for reasoning about section 7.2's arithmetic without
 * running a game. Used by the tests to assert the subscription model actually
 * fixes the problem it exists for.
 */
export function estimateBytesPerSecond(
  agents: number,
  actions: number,
  considerations: number,
  hz: number,
  detail: DetailLevel,
  fractionChanging = 1,
): number {
  const headerBytes = 5 + 4 + 8 + 2 + 2 + 4; // frame + agent + time + sel + n + mask
  switch (detail) {
    case DetailLevel.Summary:
      // On change only, and a change is just the selected action.
      return agents * hz * fractionChanging * (5 + 4 + 8 + 2);
    case DetailLevel.Scores:
      return agents * hz * (headerBytes + actions * 4 * fractionChanging);
    case DetailLevel.FullBreakdown:
      return (
        agents * hz * (headerBytes + actions * (4 + considerations * 8))
      );
  }
}
