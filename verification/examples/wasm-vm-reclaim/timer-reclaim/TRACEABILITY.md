# TASK-11001 verification traceability

| Contract | Focused evidence | Runtime evidence |
| --- | --- | --- |
| Active current with no old generation may cut over | `WasmHttpFilterTest.ProactiveRebuildRollsActiveCurrentAndBoundsLiveOldGeneration` | `run.sh explicit-idle` and `run.sh memory-active`: non-zero held A marker, zero B marker, first counter/latency delta before A response |
| At most one live old generation | active/idle live-old unit tests plus the rolling test's rejected second cutover | both rolling cases require no B -> C counter or latency change while A is held across at least two complete timer intervals, record the actual guard start/end/elapsed, then allow exactly one cutover after A retires |
| Old Context remains on A; new requests use B | rolling unit test compares the Context's `wasm()` with A while the wrapper points at B | held A response retains A bytes; per-worker B/C probes report zero |
| Weak old tracking does not retain A | `ProactiveRebuildPrunesExpiredOldGenerations` | B -> C becomes possible without an explicit old-generation cleanup API after held A responses complete |
| Timer explicit and memory sources; current activity is not a gate | reclaim timer source tests and active rolling test | explicit-idle and memory-active cases; the latter holds B during B -> C |
| Success-only source counters and reclaim latency | timer skip/success tests and counter assertions | every successful cutover increments exactly one source counter and latency count; live-old skips change neither |
| Fail recovery remains under the existing one-second guard | `FailRecoveryBypassesProactiveRollingGuard` and worker initialization/recovery unit tests | `verification/examples/wasm-worker-init-failure/request-recovery/run.sh` (first ready recovery and later >1 s attempts) |
| Same-key worker clone failure does not poison the healthy base or cause later config NACK | `//test/extensions/common/wasm:wasm_test`, `WorkerCloneFailureDoesNotPoisonBaseOrNackNextSameVmKeyConfig` | compile Envoy once with the branch's previous Host pin for the red baseline and once with Higress Host revision `d9c558753df6781388e36e0d51adc98c0b6835a0` for green; run the common test and the `wasm-worker-init-failure` static/ECDS matrix against each binary, retaining LDS/config logs |
| Continuous trap/recover and healthy-control memory stability | worker-init recovery unit tests | `verification/examples/wasm-worker-init-failure/soak/run.sh` formal `SCENARIO=all`; pair with `run.sh soak` here, whose continuous-traffic heartbeat must advance in every cycle and exit cleanly |
| Near-OOM envelope applies to actual VM memory | response header reports the Host `plugin_vm_memory` property | `PROFILE=near-oom` checks every A/B eligibility worker observation against threshold, declared peak and 1x-2x minimum remaining headroom; CSV plus manifest retain the actual values |
| Non-HIGRESS behavior unchanged | production diff is inside the existing HIGRESS implementation | build/test a non-HIGRESS configuration; this harness requires the HIGRESS counters/timer and therefore fails closed when they are absent |

The old/new Host pin comparison is intentionally a two-binary campaign. Higress loads this
dependency from the GitHub archive selected by `version`; `sha256` validates that archive.
Evidence records the resolved 40-character `version` from the exact Envoy source commit and
verifies the corresponding Host commit tree.

For Higress Host revision `d9c558753df6781388e36e0d51adc98c0b6835a0`, the expected commit
tree is `9cdf1a9ed40d945f94af5c8ea458892c383b818e`. The runner first extracts the exact
Envoy source commit from the selected binary, reads `proxy_wasm_cpp_host.version` from that
commit's `bazel/repository_locations.bzl`, and resolves the Host tree from the configured checkout.
Formal and near-OOM evidence fails if this chain is incomplete; smoke evidence records `unknown`
rather than borrowing the current checkout's dependency pin.
