/**
 * The authoring format (design.md section 5).
 *
 * `.uasset` is binary. Two designers editing the same behavior tree in the
 * same sprint produces a merge conflict that git cannot resolve and nobody
 * can read, and the workarounds -- asset locking, "only one person touches
 * AI" -- are workflow damage. The design document calls this the least
 * glamorous of the three gaps and possibly the most valuable.
 *
 * Two structural decisions make it work, and both are testable rather than
 * aspirational:
 *
 *   1. ORDERING IS STABLE (5.2). A format that serializes in hash order
 *      produces spurious diffs on every save. Round-trip byte equality is the
 *      test that catches it.
 *   2. LAYOUT LIVES IN A SIDECAR (5.3). Node positions are editor state, not
 *      behavior. In the logic file, moving a node produces a diff and the
 *      signal-to-noise ratio of code review collapses.
 */

export type CurveType = "linear" | "polynomial" | "logistic" | "logit";

/** Wire values, matching aib::CurveType in core/include/aib/curve.h. */
export const CURVE_TYPE_ID: Readonly<Record<CurveType, number>> = {
  linear: 0,
  polynomial: 1,
  logistic: 2,
  logit: 3,
};

export const CURVE_TYPES: readonly CurveType[] = [
  "linear",
  "polynomial",
  "logistic",
  "logit",
];

export interface Curve {
  type: CurveType;
  /** slope */
  m: number;
  /** exponent (linear/polynomial) or amplitude (logistic) */
  k: number;
  /** vertical shift */
  b: number;
  /** horizontal shift */
  c: number;
}

export type InputSource = "blackboard" | "attribute" | "constant" | "custom";

export interface InputDef {
  name: string;
  source: InputSource;
  key: string;
  /** e.g. "distance", "elapsed" -- resolved by the runtime's input sources. */
  transform?: string;
}

export interface Consideration {
  input: string;
  /** [min, max]. Inverted is legal and means "less is better". */
  range: [number, number];
  curve: Curve;
}

export interface TaskRef {
  type: string;
  params?: Record<string, unknown>;
}

export interface Action {
  name: string;
  bucket: number;
  weight?: number;
  interruptible?: boolean;
  considerations: Consideration[];
  task: TaskRef;
}

export interface Policy {
  evaluationHz: number;
  currentActionBonus: number;
  minCommitmentSeconds: number;
  switchThreshold: number;
  bucketSatisfiedThreshold: number;
}

export interface Behavior {
  version: number;
  type: "utility";
  name: string;
  inputs: InputDef[];
  actions: Action[];
  policy: Policy;
}

/**
 * Editor state, kept in a separate file (section 5.3). A team that gitignores
 * this entirely and relies on auto-layout gets conflict-free AI authoring,
 * which is a real and unusual property -- so the format has to work when it is
 * absent, not merely tolerate it.
 */
export interface Layout {
  version: number;
  nodes: Record<string, { x: number; y: number; color?: string }>;
  comments?: { x: number; y: number; w: number; h: number; text: string }[];
  collapsed?: string[];
}

export const DEFAULT_POLICY: Policy = {
  evaluationHz: 5,
  currentActionBonus: 1.1,
  minCommitmentSeconds: 0.5,
  switchThreshold: 0.05,
  bucketSatisfiedThreshold: 0.3,
};

export const DEFAULT_CURVE: Curve = {
  type: "linear",
  m: 1,
  k: 1,
  b: 0,
  c: 0,
};

/** Bucket names from section 3.6, for display only. */
export const BUCKET_NAMES = [
  "Emergency",
  "Combat",
  "Investigate",
  "Ambient",
] as const;

export function bucketName(bucket: number): string {
  return BUCKET_NAMES[bucket] ?? `Bucket ${bucket}`;
}
