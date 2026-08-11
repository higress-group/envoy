# Bounded rolling Wasm reclaim verification

`run.sh` is a self-verdict runtime harness for the HIGRESS worker-local reclaim timer. It uses a
real Envoy binary, a real Go Proxy-Wasm module and held upstream calls. It never treats an RSS
sample as proof that an old generation retired: the verdict is based on response generation
markers, per-source success counters, the reclaim-latency sample count and the live-old guard.

The module exposes the retained bytes captured by each stream in
`x-reclaim-retained-bytes`. A is deliberately marked with retained memory before it is held; a
successful A -> B cutover therefore produces new responses with `0` while the eventual held A
response still reports A's non-zero marker. Every worker Plugin Context generates a per-start
token; those response tokens are collected for every phase, so `CONCURRENCY=4` fails unless
baseline, B and C probes, both eligibility triggers and the held A/B sets visit all four workers.

## Smoke run

Build the changed Envoy first, then run:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static \
  verification/examples/wasm-vm-reclaim/timer-reclaim/run.sh all
```

The default single-worker campaign runs:

- explicit eligibility with active A and idle B;
- memory-threshold eligibility with active A and active B;
- a sustained healthy control with no eligibility signal;
- repeated cutovers under continuous request traffic.

The soak traffic loop is part of the verdict, not background noise: it publishes a successful
request count, must make progress in every cycle, and must exit cleanly after the runner writes
its stop marker. Any early curl exit fails the campaign and is retained in `traffic.error`.

Each rolling case proves that active A can cut over to B, A continues with its original marker,
new requests use B, live A rejects B -> C without changing counters or latency samples, and B can
cut over exactly once after A retires. The memory case also proves that B activity is not a
quiescence gate. Explicit and memory counters must remain source-exclusive; `recover_total` must
stay zero. `guard-window.txt` records wall-clock start/end timestamps and elapsed milliseconds;
the runner requires held A requests to remain live across at least two complete one-second timer
intervals.

## Formal and near-OOM profiles

The formal profile declares its duration and worker count before Envoy starts and refuses a
shortened campaign:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static PROFILE=formal CONCURRENCY=4 \
  SOAK_CYCLES=600 CONTROL_SECONDS=300 \
  WORKDIR=/tmp/wasm-timer-reclaim-formal \
  verification/examples/wasm-vm-reclaim/timer-reclaim/run.sh all
```

Both evidence profiles bind the executable to its sources. The runner parses the exact
40-character commit and `Clean` state from `ENVOY_BIN --version`, verifies that commit in
`ENVOY_SOURCE_REPO` (the current Envoy checkout by default), reads the Host dependency pin from
that exact commit, and resolves its tree from `HOST_SOURCE_REPO` (the sibling Host checkout by
default). Formal and near-OOM runs fail closed if any part cannot be verified. Smoke runs record
`unknown` instead of associating an unverified binary with the current checkout. Set
`EXPECTED_ENVOY_HEAD` to add an exact binary-head assertion.

Near-OOM testing must additionally declare the VM limit, estimated runtime overhead, retained
target and minimum remaining headroom. The runner rejects a target outside the declared safety
envelope and records every resolved value before allocation:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static PROFILE=near-oom \
  MEMORY_THRESHOLD_BYTES=805306368 MEMORY_TARGET_MB=800 \
  DECLARED_VM_LIMIT_BYTES=1073741824 DECLARED_VM_OVERHEAD_BYTES=67108864 \
  MIN_HEADROOM_BYTES=134217728 \
  VM_LIMIT_PROVENANCE='proxy-wasm-cpp-host d9c5587 include/proxy-wasm/limits.h PROXY_WASM_HOST_MAX_WASM_MEMORY_SIZE_BYTES=1GiB' \
  verification/examples/wasm-vm-reclaim/timer-reclaim/run.sh memory-active
```

`PROFILE=near-oom` fails closed unless the threshold, all four sizing inputs and
`VM_LIMIT_PROVENANCE` are explicitly supplied; smoke defaults are never promoted into near-OOM
evidence. The declared peak includes the target allocation, the pre-existing A marker and the
runtime overhead. For every covered worker in both A-eligible and B-eligible phases, the runner
then validates the response's actual `x-reclaim-vm-memory-bytes`: it must cross the configured
threshold, not exceed the declared peak, and leave between one and two times the declared minimum
headroom. The per-worker observations are written to `near-oom-vm-memory.csv`; maxima and minimum
observed headroom are also appended to the manifest. Choose values for the compiled runtime's
actual linear-memory limit; the example leaves 156 MiB after including the 4 MiB marker and 64 MiB
runtime estimate. An operator should lower the target when host memory pressure or other
concurrent builds make that envelope unsafe.

The module grows one target-sized retained backing slice and touches its new pages directly. It
does not create a second target-sized temporary allocation before copying, which would invalidate
the declared headroom near the Host hard limit.

The harness writes config, binary hashes, binary/source/Host provenance, resolved safety inputs,
response headers/bodies, per-phase worker IDs, stats/resource samples and Envoy logs under
`WORKDIR`. `resources.csv` is
useful for the long-run memory assessment, but the expected product behavior is bounded rolling
generation ownership and quick creation of the latest VM, not immediate RSS decrease while a
long-lived old Context exists.

Before normal shutdown, each case requires the active VM gauge to return to its startup baseline.
After SIGTERM it also requires the debug lifecycle log to drain to at most the process-global
cached base; depending on shutdown/log ordering Envoy may log either one remaining base or the
subsequent zero.

See [TRACEABILITY.md](TRACEABILITY.md) for the complete TASK/SPEC evidence map, including the
separate old/new Host pin NACK and fail-recovery guard campaigns.
