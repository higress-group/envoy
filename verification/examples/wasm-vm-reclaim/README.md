# Proactive Wasm VM Reclaim — Runtime Artifacts

Reproducible local runtime assets for timer/memory reclaim, generation
replacement and ECDS churn. The original execution record is in
[`../../reports/proactive-wasm-vm-reclaim.md`](../../reports/proactive-wasm-vm-reclaim.md).

## Shared Upstream

```bash
python3 verification/examples/wasm-vm-reclaim/upstream_server.py
```

Listens on `127.0.0.1:3080`; supports `/slow?delay=N` to hold a downstream
request active (used by the active-stream / cap experiments).

## Timer-Reclaim Scenario (bounded rolling reclaim + memory threshold + observability)

The plugin (`timer-reclaim/wasm/main.go`) is a single flexible module driven by
request headers:

- `x-set-rebuild: true` → sets the `wasm_need_rebuild` property (host
  `shouldRebuild(true)`); explicit-flag trigger source.
- `x-alloc-mb: N` → idempotently grows a VM-retained buffer to at least `N` MiB
  and touches every new page so WASM linear memory grows past the reclaim threshold;
  memory-threshold trigger source (independent of the plugin flag).
- `x-hold-active: true` → holds a Context without implicitly selecting a rebuild
  source. It can be combined with either trigger.

The self-verdict runner lowers the reclaim memory threshold to **32 MiB** via a `layered_runtime`
override (`envoy.wasm.reclaim.memory_threshold_bytes`) so a modest allocation can
cross it without making the 4 MiB explicit-source marker eligible; the default is
800 MiB. The checked-in manual config retains its original 16 MiB threshold.

Build:

```bash
cd verification/examples/wasm-vm-reclaim/timer-reclaim/wasm
GOWORK=off GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o ../reclaim-verify.wasm .
```

Run the self-verdict harness against an explicitly selected Envoy binary:

```bash
ENVOY_BIN=/absolute/path/to/envoy-static \
  verification/examples/wasm-vm-reclaim/timer-reclaim/run.sh all
```

See `timer-reclaim/README.md` for the four-worker formal profile, declared
near-OOM safety envelope, evidence files and exact TASK/SPEC traceability.
Listener `10002`, admin `9905`. Stats scope:
`wasm.envoy.wasm.runtime.v8.plugin.reclaim_verify_plugin.<stat>`.

## Fail Recovery Crash Scenario (unchanged fail recovery — regression guard)

Copied verbatim from `optimize-wasm-rebuild-guard` (behavior is unchanged by this
change). Build:

```bash
cd verification/examples/wasm-vm-reclaim/fail-recovery-crash/wasm
GOWORK=off GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o ../crash-recovery.wasm .
```

Run from `fail-recovery-crash` (listener `10001`, admin `9904`).

## ECDS File Update Scenario (file xDS + cp/mv ECDS update)

`ecds-file-update/` loads LDS from a file, uses `config_discovery.path` for the
HTTP Wasm filter, and updates ECDS by copying a candidate to a temporary path
and atomically moving it into place.

Run from the repository root:

```bash
verification/examples/wasm-vm-reclaim/ecds-file-update/run.sh
```

The script verifies ECDS plugin configuration updates (`v1` -> `v2`), same-VM-key
wrapper replacement preserving reclaim state, Wasm binary A/B updates that change
`vm_key` through code bytes (`v4` / `v5`), pending rebuild plus binary-update
collision behavior, post-binary explicit-flag reclaim, no-residue active VM lifecycle
sanity, legal `vm_id` / `vm_key` change (`v3`), and memory-threshold reclaim
after the ECDS update.

## Ablation

The same `timer-reclaim` inputs (plugin + config) are run against two binaries:

- **baseline**: develop build (has `optimize-wasm-rebuild-guard` request-path
  rebuild + idle-allow; no timer, no memory trigger, no two-gen cap).
- **current**: post-implementation build of this change (timer-only non-fail
  reclaim + memory threshold + bounded rolling cutover + source counters + latency).

Generated Wasm binaries and runtime logs are intentionally not retained. Build
them from the checked-in Go sources and record the current checksums and
observations in the active VERIFY.
