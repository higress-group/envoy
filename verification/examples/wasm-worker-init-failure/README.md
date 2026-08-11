# Request-only worker Wasm initialization recovery

This package verifies the Stage B HTTP behavior after a worker-local Wasm plugin fails to
initialize. Recovery is request driven: an idle worker does no work, the first later request may
make an immediate attempt, and later actual attempts for one worker/VM key are separated by a fixed
one-second guard. There is no initialization timer, backoff sequence, retry cap, or exhausted state.

All runners require an explicit absolute `ENVOY_BIN`; none falls back to a worktree build. The Go
plugin provides deterministic `configure-reject`, `trap-once`, `trap-always`, and `healthy`
initialization modes plus the `x-worker-init-request-trap: true` Ready-state trap.

## Functional matrix

Build the plugin once and run the static matrix under both request policies:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static FAIL_OPEN=true CONCURRENCY=4 \
  WORKDIR=/tmp/wasm-worker-init-static-open \
  verification/examples/wasm-worker-init-failure/run.sh all

ENVOY_BIN=/absolute/path/to/envoy-static FAIL_OPEN=false CONCURRENCY=4 \
  WORKDIR=/tmp/wasm-worker-init-static-closed \
  verification/examples/wasm-worker-init-failure/run.sh all
```

The runner records the Envoy/Wasm/config hashes, all responses, ordered plugin attempts, relevant
stats, and host logs. Its Stage B assertions cover:

- `configure-reject` is terminal and never attempts again on later requests;
- `trap-once` has no idle activity and the request that triggers recovery is itself processed by
  the recovered plugin (generation and plugin markers are present);
- `trap-always` has no background activity, immediate requests inside the guard do not invoke the
  factory, and more than five one-second-separated requests continue to make attempts;
- `healthy` has no initialization-failure state.

`CONCURRENCY=1` is the most deterministic trace. `CONCURRENCY=4` is the required worker-isolation
matrix. The N=4 run waits until all worker-local wrappers recover; `RECOVERY_REQUEST_LIMIT` bounds
connection steering. `FAIL_OPEN=true` bypasses a still-uninitialized wrapper, while
`FAIL_OPEN=false` returns local 503 until a request successfully initializes that worker.

## Replacement and guard ownership

Run the file-ECDS replacement matrix (N defaults to 4):

```bash
ENVOY_BIN=/absolute/path/to/envoy-static FAIL_OPEN=true \
  WORKDIR=/tmp/wasm-worker-init-ecds-open \
  verification/examples/wasm-worker-init-failure/ecds-replacement/run.sh
```

It verifies terminal-to-healthy replacement, retryable-to-healthy replacement and gauge drain,
request-only recovery of a fresh generation, no stale retry after replacement, and failure/healthy
churn with bounded active VMs, RSS, file descriptors, and `server.memory_allocated`. Configuration
updates retain the same Wasm bytes and VM key; only plugin configuration/generation changes.

The common unit tests provide the exact same-key/different-key guard matrix, including a no-handle
entry surviving cross-key registry sweeping. The static and ECDS N=4 runs provide the real-worker
evidence; line-level guard ownership is therefore tested without relying on scheduler timing.

## Ready recovery

The existing request-stage runner verifies the Ready-state `rebuild(true)` path and the shared fixed
guard for more than five cycles:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static CYCLES=6 \
  WORKDIR=/tmp/wasm-worker-init-request-recovery \
  verification/examples/wasm-worker-init-failure/request-recovery/run.sh
```

Each cycle traps in `OnHttpRequestHeaders`, waits beyond the one-second guard, and confirms that the
next normal request rebuilds the VM. Worker-initialization retry counters remain unchanged.

## Time-series soak

`soak/run.sh` produces 10-second CSV samples and 5-minute window summaries for initialization
recovery (`trap-once` generations), Ready recovery (request traps), persistent initialization
failure (`trap-always`), and a paired healthy control using the same Envoy binary, request rate,
worker count, and ECDS update cadence.

Smoke example:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static PROFILE=smoke SCENARIO=initialization \
  WORKDIR=/tmp/wasm-worker-init-soak-init \
  verification/examples/wasm-worker-init-failure/soak/run.sh
```

Formal defaults are deliberately declared before the run starts:

- one repetition per scenario and paired healthy control, with mandatory `CONCURRENCY=4`;
- 300-second warm-up, then 900 measured seconds;
- at least 1000 recoveries for `initialization`, at least 600 recoveries for `ready`, and at least
  600 actual attempts for `persistent`;
- each Ready recovery request starts at least 1050 ms after its trap response; crash-counter
  observation runs inside that deadline rather than extending the round after the guard wait;
- 10-second samples and non-overlapping 300-second windows;
- final-vs-first window median growth: FD <= 8, threads <= 0, allocated and heap memory <= 32 MiB,
  and RSS/RssAnon/Pss/Private_Dirty <= 64 MiB;
- per-100-event median trend: allocated and heap memory <= 1 MiB and
  RSS/RssAnon/Pss/Private_Dirty <= 4 MiB;
- the paired healthy control must satisfy the same absolute bounds.
- recovery-minus-control stable-window growth is additionally limited to FD <= 4, threads <= 0,
  allocated and heap memory <= 16 MiB, and RSS/RssAnon/Pss/Private_Dirty <= 32 MiB; its relative
  trend is limited to 512 KiB and 2 MiB per 100 events respectively.

Growth uses the median of up to the first two versus last two fixed windows; the three-window formal
profile uses the first and last window. Trend uses least-squares over all fixed-window medians and
each run's own event sequence, normalized per 100 events. Formal
sampling begins only after the full 300-second warm-up; warm-up data is intentionally excluded from
`samples.csv`. Actual and control runs each append exactly one `elapsed=duration` terminal sample;
the event-count gate reads that row, while fixed resource windows continue to use only samples
strictly inside the measured duration. The two-window smoke profile checks absolute and paired
growth but does not use its single edge-to-edge segment as a per-100-event regression; the formal
profile has at least three windows and enforces the trend bounds.

Each initialization generation recovers all four workers, checks every counter-changing response
for the same generation marker, and returns the worker gauge to zero. Because `trap-once` also
fails the neutral main-thread TLS slot, actual runs stabilize at one active VM below their healthy
baseline; healthy controls remain at that baseline. Both values are asserted in every measured
sample. Its control performs the same ECDS update and fixed request burst. Each Ready
round sends one trap and asserts `crash_total + 1`, then adaptively scans with normal requests (cap
200) until exact `recover_total + 1`; the counter-changing response carries the marker. The actual
run anchors the 1050 ms deadline after the trap response, performs crash-counter observation inside
that wait, and records each recovery request's guard elapsed time. A request below the declared
guard fails immediately. Exact admin-stat filters avoid serializing the complete stats set for each
counter read, and each post-request recovery counter becomes the next request's baseline instead of
being fetched twice. This keeps observational admin work off the Ready critical path where
possible. The run also records every round offset, phase, scan count, recovery latency, and
per-request offset. Its healthy control validates the complete schedule and replays the same round
timing, request count, and per-request cadence; missing or misaligned schedule data fails closed.
Ready also keeps zero `recover_error`, leaves initialization counters unchanged, and returns to
baseline active VMs.
Persistent rounds send a fixed high-QPS burst and record the actual
retry delta and timestamp in `attempt-rate.csv`, enforce at most N attempts per guard window, retain
gauge N, and retain the run-local active VM baseline; its healthy control retains gauge zero and
its own active baseline. Every measured sample is checked against the scenario's expected gauge and
active values. `server.memory_heap_size` is sampled when Envoy exposes it; otherwise it is recorded
as `-1` and excluded from that metric's gates.

Run a formal campaign with `PROFILE=formal SCENARIO=all CONCURRENCY=4`. Every threshold, resolved
parameter, binary hash, config hash, raw CSV, and window summary remains under `WORKDIR`. The
analysis fails closed if fewer events than declared are observed or if `/proc`/admin samples are
missing.

## Adjacent regressions

- `verification/examples/wasm-vm-reclaim/ecds-file-update/run.sh` covers binary A/B changes,
  in-flight request isolation, proactive rebuild, and reclaim.
- `test/extensions/common/wasm/wasm_test.cc` covers the shared rebuild guard, no-handle cleanup,
  replacement state ownership, terminal classification, and the no-cap request sequence.
- `verification/examples/wasm-canary-skip/` isolates identical-filter canary de-duplication.

Keep these adjacent regressions in the final evidence set; they exercise different ownership edges
and are intentionally not copied into this directory.
