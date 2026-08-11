#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENVOY_BIN="${ENVOY_BIN:-}"
WASM_BINARY="${WASM_BINARY:-}"
WORKDIR="${WORKDIR:-/tmp/wasm-worker-init-ecds-replacement}"
LISTENER_PORT="${LISTENER_PORT:-10005}"
ADMIN_PORT="${ADMIN_PORT:-9908}"
UPSTREAM_PORT="${UPSTREAM_PORT:-3085}"
BASE_ID="${BASE_ID:-123}"
CONCURRENCY="${CONCURRENCY:-4}"
FAIL_OPEN="${FAIL_OPEN:-true}"
IDLE_SECONDS="${IDLE_SECONDS:-10}"
GUARD_WAIT_SECONDS="${GUARD_WAIT_SECONDS:-1.10}"
RECOVERY_REQUEST_LIMIT="${RECOVERY_REQUEST_LIMIT:-400}"
CHURN_CYCLES="${CHURN_CYCLES:-6}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-262144}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-8}"
MAX_SERVER_MEMORY_GROWTH_BYTES="${MAX_SERVER_MEMORY_GROWTH_BYTES:-268435456}"
WORKDIR_MARKER=.envoy-verification-workdir
ENVOY_PID=""
UPSTREAM_PID=""

if [[ -z "${ENVOY_BIN}" || "${ENVOY_BIN}" != /* || ! -x "${ENVOY_BIN}" ]]; then
  echo "ENVOY_BIN must explicitly name an absolute executable Envoy binary" >&2
  exit 1
fi
if [[ "${FAIL_OPEN}" != true && "${FAIL_OPEN}" != false ]]; then
  echo "FAIL_OPEN must be true or false" >&2
  exit 2
fi
for pair in "CONCURRENCY:${CONCURRENCY}" "IDLE_SECONDS:${IDLE_SECONDS}" \
  "RECOVERY_REQUEST_LIMIT:${RECOVERY_REQUEST_LIMIT}" "CHURN_CYCLES:${CHURN_CYCLES}"; do
  name="${pair%%:*}"; value="${pair#*:}"
  [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${name} must be positive" >&2; exit 2; }
done

tmp_root="$(realpath -m /tmp)"
WORKDIR="$(realpath -m "${WORKDIR}")"
case "${WORKDIR}" in "${tmp_root}"/?*) ;; *) echo "WORKDIR must be below /tmp" >&2; exit 1;; esac
if [[ -d "${WORKDIR}" && ! -f "${WORKDIR}/${WORKDIR_MARKER}" ]] &&
  find "${WORKDIR}" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
  echo "refusing to clean unmarked non-empty WORKDIR: ${WORKDIR}" >&2
  exit 1
fi
mkdir -p "${WORKDIR}"
touch "${WORKDIR}/${WORKDIR_MARKER}"
find "${WORKDIR}" -mindepth 1 -depth ! -path "${WORKDIR}/${WORKDIR_MARKER}" -delete

cleanup() {
  if [[ -n "${ENVOY_PID}" ]]; then kill "${ENVOY_PID}" >/dev/null 2>&1 || true; wait "${ENVOY_PID}" >/dev/null 2>&1 || true; fi
  if [[ -n "${UPSTREAM_PID}" ]]; then kill "${UPSTREAM_PID}" >/dev/null 2>&1 || true; wait "${UPSTREAM_PID}" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT

if [[ -n "${WASM_BINARY}" ]]; then
  [[ "${WASM_BINARY}" == /* && -f "${WASM_BINARY}" ]] || { echo "invalid WASM_BINARY" >&2; exit 1; }
else
  WASM_BINARY="${WORKDIR}/worker-init-repro.wasm"
  (cd "${PARENT_DIR}/wasm" && GOWORK=off GOOS=wasip1 GOARCH=wasm \
    go build -buildmode=c-shared -o "${WASM_BINARY}" .)
fi

sed -e "s#__ADMIN_PORT__#${ADMIN_PORT}#g" -e "s#__UPSTREAM_PORT__#${UPSTREAM_PORT}#g" \
  -e "s#__WORKDIR__#${WORKDIR}#g" "${SCRIPT_DIR}/config.yaml.template" \
  >"${WORKDIR}/config.yaml"
sed -e "s#__LISTENER_PORT__#${LISTENER_PORT}#g" -e "s#__WORKDIR__#${WORKDIR}#g" \
  "${SCRIPT_DIR}/lds.yaml.template" >"${WORKDIR}/lds.yaml"

install_generation() {
  local mode="$1" generation="$2" run_id="$3"
  local next="${WORKDIR}/ecds.yaml.next"
  sed -e "s#__FAIL_OPEN__#${FAIL_OPEN}#g" -e "s#__WASM_BINARY__#${WASM_BINARY}#g" \
    -e "s#__MODE__#${mode}#g" -e "s#__RUN_ID__#${run_id}#g" \
    -e "s#__WORKER_COUNT__#${CONCURRENCY}#g" -e "s#__GENERATION__#${generation}#g" \
    "${SCRIPT_DIR}/ecds.yaml.template" >"${next}"
  mv "${next}" "${WORKDIR}/ecds.yaml"
  printf '%s,%s,%s,%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${mode}" "${generation}" \
    "$(sha256sum "${WORKDIR}/ecds.yaml" | awk '{print $1}')" >>"${WORKDIR}/updates.csv"
}

prefix=wasm.envoy.wasm.runtime.v8.plugin.worker_init_repro
retryable_stat="${prefix}.worker_init_retryable_failure_total"
terminal_stat="${prefix}.worker_init_terminal_failure_total"
retry_stat="${prefix}.worker_init_retry_total"
recovered_stat="${prefix}.worker_init_recovered_total"
uninitialized_stat="${prefix}.worker_uninitialized"
active_stat=wasm.envoy.wasm.runtime.v8.active
stat_value() {
  local stat="$1"
  curl --fail --silent --show-error --max-time 2 "http://127.0.0.1:${ADMIN_PORT}/stats" |
    awk -F': ' -v stat="${stat}" '$1 == stat {print $2; found=1} END {if (!found) print 0}'
}
wait_stat_eq() {
  local stat="$1" expected="$2" value=0
  for _ in $(seq 1 200); do
    value="$(stat_value "${stat}")"
    [[ "${value}" == "${expected}" ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for ${stat}=${expected}; last=${value}" >&2
  return 1
}
wait_active_baseline() {
  local stage="$1" value=0 poll
  for poll in $(seq 1 200); do
    value="$(stat_value "${active_stat}")"
    printf '%s,%s,%s,%s,%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${stage}" "${poll}" \
      "${active_baseline}" "${value}" >>"${WORKDIR}/active-baseline.csv"
    [[ "${value}" == "${active_baseline}" ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for ${stage} active=${active_baseline}; last=${value}" >&2
  return 1
}
wait_generation() {
  local generation="$1"
  for i in $(seq 1 "${RECOVERY_REQUEST_LIMIT}"); do
    request "generation-${generation}-${i}"
    if [[ "$(<"${WORKDIR}/generation-${generation}-${i}.status")" == 200 ]] &&
      grep -qi "^x-worker-init-generation: ${generation}" \
        "${WORKDIR}/generation-${generation}-${i}.headers"; then
      return 0
    fi
  done
  echo "did not observe generation ${generation}" >&2
  return 1
}
request() {
  local label="$1"
  curl --silent --show-error --max-time 5 -H 'connection: close' \
    -D "${WORKDIR}/${label}.headers" -o "${WORKDIR}/${label}.body" -w '%{http_code}' \
    "http://127.0.0.1:${LISTENER_PORT}/${label}" >"${WORKDIR}/${label}.status" || true
}
require_failure_policy() {
  local label="$1"
  request "${label}"
  if [[ "${FAIL_OPEN}" == true ]]; then
    [[ "$(<"${WORKDIR}/${label}.status")" == 200 ]] &&
      grep -qi '^x-upstream-marker: reached' "${WORKDIR}/${label}.headers" &&
      grep -qi '^x-plugin-marker: absent' "${WORKDIR}/${label}.headers"
  else
    [[ "$(<"${WORKDIR}/${label}.status")" == 503 ]] &&
      ! grep -qi '^x-upstream-marker:' "${WORKDIR}/${label}.headers"
  fi
}
proc_rss_kb() { awk '/VmRSS:/ {print $2}' "/proc/${ENVOY_PID}/status"; }
proc_fd_count() { find "/proc/${ENVOY_PID}/fd" -maxdepth 1 -type l | wc -l; }
require_growth_max() {
  local label="$1" before="$2" after="$3" maximum="$4"
  (( after - before <= maximum )) || { echo "${label} growth exceeded ${maximum}" >&2; return 1; }
}

printf 'timestamp_utc,mode,generation,config_sha256\n' >"${WORKDIR}/updates.csv"
install_generation configure-reject terminal-old terminal-old
"${ENVOY_BIN}" --mode validate -c "${WORKDIR}/config.yaml" --log-level error \
  >"${WORKDIR}/validate.log" 2>&1
UPSTREAM_PORT="${UPSTREAM_PORT}" python3 "${PARENT_DIR}/upstream_server.py" \
  >"${WORKDIR}/upstream.log" 2>&1 &
UPSTREAM_PID=$!
for _ in $(seq 1 80); do
  curl --fail --silent --max-time 1 "http://127.0.0.1:${UPSTREAM_PORT}/" >/dev/null 2>&1 && break
  sleep 0.25
done
"${ENVOY_BIN}" -c "${WORKDIR}/config.yaml" --concurrency "${CONCURRENCY}" \
  --base-id "${BASE_ID}" --log-level warn --component-log-level wasm:debug,config:debug \
  --log-path "${WORKDIR}/envoy.log" >"${WORKDIR}/console.log" 2>&1 &
ENVOY_PID=$!
for _ in $(seq 1 120); do
  if kill -0 "${ENVOY_PID}" >/dev/null 2>&1 &&
    curl --fail --silent --max-time 1 "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done
curl --fail --silent --show-error --max-time 2 "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null

# Terminal state never retries; replacement owns and drains the old wrapper-local gauge.
wait_stat_eq "${terminal_stat}" "${CONCURRENCY}"
wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
terminal_retry="$(stat_value "${retry_stat}")"
require_failure_policy terminal-old
sleep "${IDLE_SECONDS}"
require_failure_policy terminal-old-after-idle
[[ "$(stat_value "${retry_stat}")" == "${terminal_retry}" ]]
install_generation healthy terminal-healthy terminal-healthy
wait_generation terminal-healthy
wait_stat_eq "${uninitialized_stat}" 0
active_baseline="$(stat_value "${active_stat}")"
printf 'timestamp_utc,stage,poll,expected_active,actual_active\n' \
  >"${WORKDIR}/active-baseline.csv"
printf '%s,%s,%s,%s,%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" baseline-established 0 \
  "${active_baseline}" "${active_baseline}" >>"${WORKDIR}/active-baseline.csv"

# A fresh retryable generation is idle until requests. Every counter-changing request must carry
# that same generation's marker, proving recovery precedes normal processing of that request.
retryable_before="$(stat_value "${retryable_stat}")"
retry_before="$(stat_value "${retry_stat}")"
recovered_before="$(stat_value "${recovered_stat}")"
install_generation trap-once request-recovery request-recovery
wait_stat_eq "${retryable_stat}" "$((retryable_before + CONCURRENCY))"
wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
sleep "${IDLE_SECONDS}"
[[ "$(stat_value "${retry_stat}")" == "${retry_before}" ]]
recovered="${recovered_before}"
for i in $(seq 1 "${RECOVERY_REQUEST_LIMIT}"); do
  before="${recovered}"
  request "request-recovery-${i}"
  recovered="$(stat_value "${recovered_stat}")"
  if (( recovered > before )); then
    [[ "$(<"${WORKDIR}/request-recovery-${i}.status")" == 200 ]]
    grep -qi '^x-plugin-marker: ready' "${WORKDIR}/request-recovery-${i}.headers"
    grep -qi '^x-worker-init-generation: request-recovery' \
      "${WORKDIR}/request-recovery-${i}.headers"
  fi
  (( recovered == recovered_before + CONCURRENCY )) && break
done
[[ "${recovered}" == "$((recovered_before + CONCURRENCY))" ]]
wait_stat_eq "${uninitialized_stat}" 0

# Replace a persistent failed generation before the guard opens. With no background timer, waiting
# past the old deadline cannot mutate counters or resurrect the replaced wrapper.
retryable_before="$(stat_value "${retryable_stat}")"
retry_before="$(stat_value "${retry_stat}")"
install_generation trap-always persistent-old persistent-old
wait_stat_eq "${retryable_stat}" "$((retryable_before + CONCURRENCY))"
wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
require_failure_policy persistent-old
retry_after_request="$(stat_value "${retry_stat}")"
[[ "${retry_after_request}" == "${retry_before}" ]]
install_generation healthy replacement-healthy replacement-healthy
wait_generation replacement-healthy
wait_stat_eq "${uninitialized_stat}" 0
wait_active_baseline persistent-replacement
sleep "${GUARD_WAIT_SECONDS}"
[[ "$(stat_value "${retry_stat}")" == "${retry_after_request}" ]]

# Repeat replacement/drain ownership while retaining bounded process resources.
rss_before="$(proc_rss_kb)"
fd_before="$(proc_fd_count)"
memory_before="$(stat_value server.memory_allocated)"
terminal_before="$(stat_value "${terminal_stat}")"
for cycle in $(seq 1 "${CHURN_CYCLES}"); do
  install_generation configure-reject "churn-failed-${cycle}" "churn-failed-${cycle}"
  wait_stat_eq "${terminal_stat}" "$((terminal_before + cycle * CONCURRENCY))"
  wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
  require_failure_policy "churn-failed-${cycle}"
  install_generation healthy "churn-healthy-${cycle}" "churn-healthy-${cycle}"
  wait_generation "churn-healthy-${cycle}"
  wait_stat_eq "${uninitialized_stat}" 0
  wait_active_baseline "churn-healthy-${cycle}"
done
sleep 2
wait_active_baseline churn-final
rss_after="$(proc_rss_kb)"
fd_after="$(proc_fd_count)"
memory_after="$(stat_value server.memory_allocated)"
require_growth_max VmRSS-kB "${rss_before}" "${rss_after}" "${MAX_RSS_GROWTH_KB}"
require_growth_max fd "${fd_before}" "${fd_after}" "${MAX_FD_GROWTH}"
require_growth_max server.memory_allocated "${memory_before}" "${memory_after}" \
  "${MAX_SERVER_MEMORY_GROWTH_BYTES}"

{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "envoy_bin=${ENVOY_BIN}"
  echo "envoy_sha256=$(sha256sum "${ENVOY_BIN}" | awk '{print $1}')"
  echo "wasm_sha256=$(sha256sum "${WASM_BINARY}" | awk '{print $1}')"
  echo "fail_open=${FAIL_OPEN}"
  echo "concurrency=${CONCURRENCY}"
  echo "idle_seconds=${IDLE_SECONDS}"
  echo "worker_init_retryable_failure_total=$(stat_value "${retryable_stat}")"
  echo "worker_init_terminal_failure_total=$(stat_value "${terminal_stat}")"
  echo "worker_init_retry_total=$(stat_value "${retry_stat}")"
  echo "worker_init_recovered_total=$(stat_value "${recovered_stat}")"
  echo "worker_uninitialized=$(stat_value "${uninitialized_stat}")"
  echo "active_baseline=${active_baseline}"
  echo "active_final=$(stat_value "${active_stat}")"
  echo "active_baseline_evidence=active-baseline.csv"
  echo "active_baseline_poll_samples=$(awk 'END { print NR - 1 }' "${WORKDIR}/active-baseline.csv")"
  echo "active_baseline_summary=$(awk -F, '
    NR > 1 {
      if (!seen[$2]++) order[++count] = $2
      last[$2] = $5
    }
    END {
      for (i = 1; i <= count; ++i) {
        if (i > 1) printf ";"
        printf "%s=%s", order[i], last[order[i]]
      }
    }
  ' "${WORKDIR}/active-baseline.csv")"
  echo "rss_kb=${rss_before}->${rss_after}"
  echo "fd_count=${fd_before}->${fd_after}"
  echo "server_memory_allocated=${memory_before}->${memory_after}"
} | tee "${WORKDIR}/evidence.txt"

kill "${ENVOY_PID}" >/dev/null 2>&1 || true
wait "${ENVOY_PID}" >/dev/null 2>&1 || true
ENVOY_PID=""
grep -q '~Wasm 0 remaining active' "${WORKDIR}/console.log"
grep 'worker-init-repro: plugin start' "${WORKDIR}/envoy.log" >"${WORKDIR}/attempts.log"
cleanup
trap - EXIT
echo "evidence: ${WORKDIR}/evidence.txt"
echo PASS
