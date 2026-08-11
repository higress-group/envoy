#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENVOY_BIN="${ENVOY_BIN:-}"
WASM_BINARY="${WASM_BINARY:-}"
WORKDIR="${WORKDIR:-/tmp/wasm-worker-init-request-recovery}"
LISTENER_PORT="${LISTENER_PORT:-10006}"
ADMIN_PORT="${ADMIN_PORT:-9909}"
UPSTREAM_PORT="${UPSTREAM_PORT:-3086}"
BASE_ID="${BASE_ID:-125}"
CYCLES="${CYCLES:-6}"
RECOVERY_WAIT_SECONDS="${RECOVERY_WAIT_SECONDS:-1.25}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-262144}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-8}"
MAX_SERVER_MEMORY_GROWTH_BYTES="${MAX_SERVER_MEMORY_GROWTH_BYTES:-268435456}"
WORKDIR_MARKER=".envoy-verification-workdir"
ENVOY_PID=""
UPSTREAM_PID=""

if [[ -z "${ENVOY_BIN}" || "${ENVOY_BIN}" != /* || ! -x "${ENVOY_BIN}" ]]; then
  echo "ENVOY_BIN must explicitly name an absolute executable Envoy binary" >&2
  exit 1
fi
if [[ ! "${CYCLES}" =~ ^[1-9][0-9]*$ || "${CYCLES}" -lt 6 ]]; then
  echo "CYCLES must be an integer of at least 6 to cross the former retry-cap boundary" >&2
  exit 2
fi

TMP_ROOT="$(realpath -m /tmp)"
WORKDIR="$(realpath -m "${WORKDIR}")"
case "${WORKDIR}" in
  "${TMP_ROOT}"/?*) ;;
  *)
    echo "WORKDIR must resolve to a dedicated directory below ${TMP_ROOT}: ${WORKDIR}" >&2
    exit 1
    ;;
esac
if [[ -d "${WORKDIR}" && ! -f "${WORKDIR}/${WORKDIR_MARKER}" ]] &&
  find "${WORKDIR}" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
  echo "refusing to clean unmarked non-empty WORKDIR: ${WORKDIR}" >&2
  exit 1
fi
mkdir -p "${WORKDIR}"
touch "${WORKDIR}/${WORKDIR_MARKER}"
find "${WORKDIR}" -mindepth 1 -depth ! -path "${WORKDIR}/${WORKDIR_MARKER}" -delete

cleanup_envoy() {
  if [[ -n "${ENVOY_PID}" ]]; then
    kill "${ENVOY_PID}" >/dev/null 2>&1 || true
    wait "${ENVOY_PID}" >/dev/null 2>&1 || true
    ENVOY_PID=""
  fi
}

cleanup_upstream() {
  if [[ -n "${UPSTREAM_PID}" ]]; then
    kill "${UPSTREAM_PID}" >/dev/null 2>&1 || true
    wait "${UPSTREAM_PID}" >/dev/null 2>&1 || true
    UPSTREAM_PID=""
  fi
}

cleanup_all() {
  cleanup_envoy
  cleanup_upstream
}
trap cleanup_all EXIT

if [[ -n "${WASM_BINARY}" ]]; then
  if [[ "${WASM_BINARY}" != /* || ! -f "${WASM_BINARY}" ]]; then
    echo "WASM_BINARY must name an absolute existing file when provided" >&2
    exit 1
  fi
else
  WASM_BINARY="${WORKDIR}/worker-init-repro.wasm"
  (cd "${PARENT_DIR}/wasm" && GOWORK=off GOOS=wasip1 GOARCH=wasm \
    go build -buildmode=c-shared -o "${WASM_BINARY}" .)
fi

sed \
  -e "s#__ADMIN_PORT__#${ADMIN_PORT}#g" \
  -e "s#__LISTENER_PORT__#${LISTENER_PORT}#g" \
  -e "s#__UPSTREAM_PORT__#${UPSTREAM_PORT}#g" \
  -e "s#__FAIL_OPEN__#true#g" \
  -e "s#__WASM_BINARY__#${WASM_BINARY}#g" \
  -e "s#__MODE__#healthy#g" \
  -e "s#__RUN_ID__#request-recovery#g" \
  -e "s#__WORKER_COUNT__#1#g" \
  -e "s#__GENERATION__#request-recovery#g" \
  "${PARENT_DIR}/config.yaml.template" >"${WORKDIR}/config.yaml"

"${ENVOY_BIN}" --mode validate -c "${WORKDIR}/config.yaml" --log-level error \
  >"${WORKDIR}/validate.log" 2>&1

UPSTREAM_PORT="${UPSTREAM_PORT}" python3 "${PARENT_DIR}/upstream_server.py" \
  >"${WORKDIR}/upstream.log" 2>&1 &
UPSTREAM_PID=$!
upstream_ready=false
for _ in $(seq 1 40); do
  if curl --fail --silent --show-error --max-time 1 \
    "http://127.0.0.1:${UPSTREAM_PORT}/" >/dev/null 2>&1; then
    upstream_ready=true
    break
  fi
  sleep 0.25
done
if [[ "${upstream_ready}" != "true" ]]; then
  echo "timed out waiting for upstream" >&2
  exit 1
fi

"${ENVOY_BIN}" -c "${WORKDIR}/config.yaml" --concurrency 1 --base-id "${BASE_ID}" \
  --log-level warn --component-log-level wasm:debug \
  --log-path "${WORKDIR}/envoy.log" >"${WORKDIR}/console.log" 2>&1 &
ENVOY_PID=$!
envoy_ready=false
for _ in $(seq 1 80); do
  if kill -0 "${ENVOY_PID}" >/dev/null 2>&1 &&
    curl --fail --silent --show-error --max-time 1 \
      "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null 2>&1; then
    envoy_ready=true
    break
  fi
  sleep 0.25
done
if [[ "${envoy_ready}" != "true" ]]; then
  echo "timed out waiting for Envoy" >&2
  exit 1
fi

stat_value() {
  local stat="$1"
  curl --fail --silent --show-error --max-time 2 \
    "http://127.0.0.1:${ADMIN_PORT}/stats" | \
    awk -F': ' -v stat="${stat}" '$1 == stat { print $2; found=1 } END { if (!found) print 0 }'
}

wait_stat_eq() {
  local stat="$1"
  local expected="$2"
  local value=0
  for _ in $(seq 1 80); do
    value="$(stat_value "${stat}")"
    if [[ "${value}" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timed out waiting for ${stat}=${expected}; last value=${value}" >&2
  return 1
}

wait_active_at_most() {
  local maximum="$1"
  local value=0
  for _ in $(seq 1 80); do
    value="$(stat_value wasm.envoy.wasm.runtime.v8.active)"
    if (( value <= maximum )); then
      return 0
    fi
    sleep 0.25
  done
  echo "active VM count stayed at ${value}, expected <= ${maximum}" >&2
  return 1
}

request_normal() {
  local label="$1"
  curl --silent --show-error --max-time 5 \
    -D "${WORKDIR}/response-${label}.headers" -o "${WORKDIR}/response-${label}.body" \
    "http://127.0.0.1:${LISTENER_PORT}/request-recovery/${label}"
  grep -q '^HTTP/1.1 200 ' "${WORKDIR}/response-${label}.headers"
  grep -qi '^x-plugin-marker: ready' "${WORKDIR}/response-${label}.headers"
  grep -qi '^x-worker-init-generation: request-recovery' \
    "${WORKDIR}/response-${label}.headers"
}

proc_rss_kb() {
  awk '/VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' \
    "/proc/${ENVOY_PID}/status" 2>/dev/null || echo 0
}

proc_fd_count() {
  find "/proc/${ENVOY_PID}/fd" -maxdepth 1 -type l 2>/dev/null | wc -l
}

require_growth_max() {
  local label="$1"
  local before="$2"
  local after="$3"
  local maximum="$4"
  local growth=$((after - before))
  if (( growth > maximum )); then
    echo "${label} growth ${growth}, expected <= ${maximum}" >&2
    return 1
  fi
}

prefix="wasm.envoy.wasm.runtime.v8.plugin.worker_init_repro"
crash_stat="${prefix}.crash_total"
recover_stat="${prefix}.recover_total"
recover_error_stat="${prefix}.recover_error"
retry_stat="${prefix}.worker_init_retry_total"
uninitialized_stat="${prefix}.worker_uninitialized"
active_stat="wasm.envoy.wasm.runtime.v8.active"

request_normal baseline
active_baseline="$(stat_value "${active_stat}")"
if (( active_baseline < 1 )); then
  echo "invalid active VM baseline: ${active_baseline}" >&2
  exit 1
fi
crash_before="$(stat_value "${crash_stat}")"
recover_before="$(stat_value "${recover_stat}")"
retry_before="$(stat_value "${retry_stat}")"
rss_before="$(proc_rss_kb)"
fd_before="$(proc_fd_count)"
server_memory_before="$(stat_value server.memory_allocated)"

for cycle in $(seq 1 "${CYCLES}"); do
  curl --silent --show-error --max-time 5 \
    -H 'x-worker-init-request-trap: true' \
    -D "${WORKDIR}/response-trap-${cycle}.headers" \
    -o "${WORKDIR}/response-trap-${cycle}.body" \
    "http://127.0.0.1:${LISTENER_PORT}/request-recovery/trap-${cycle}" || true
  wait_stat_eq "${crash_stat}" "$((crash_before + cycle))"
  sleep "${RECOVERY_WAIT_SECONDS}"
  request_normal "recovered-${cycle}"
  wait_stat_eq "${recover_stat}" "$((recover_before + cycle))"
  [[ "$(stat_value "${retry_stat}")" == "${retry_before}" ]]
  [[ "$(stat_value "${uninitialized_stat}")" == 0 ]]
  wait_active_at_most "${active_baseline}"
done

request_normal final
rss_after="$(proc_rss_kb)"
fd_after="$(proc_fd_count)"
server_memory_after="$(stat_value server.memory_allocated)"
require_growth_max "VmRSS(kB)" "${rss_before}" "${rss_after}" "${MAX_RSS_GROWTH_KB}"
require_growth_max "fd count" "${fd_before}" "${fd_after}" "${MAX_FD_GROWTH}"
require_growth_max "server.memory_allocated(bytes)" "${server_memory_before}" \
  "${server_memory_after}" "${MAX_SERVER_MEMORY_GROWTH_BYTES}"
[[ "$(stat_value "${recover_error_stat}")" == 0 ]]
[[ "$(stat_value "${retry_stat}")" == "${retry_before}" ]]
wait_active_at_most "${active_baseline}"

{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "envoy_bin=${ENVOY_BIN}"
  echo "envoy_sha256=$(sha256sum "${ENVOY_BIN}" | awk '{print $1}')"
  echo "wasm_binary=${WASM_BINARY}"
  echo "wasm_sha256=$(sha256sum "${WASM_BINARY}" | awk '{print $1}')"
  echo "cycles=${CYCLES}"
  echo "active_baseline=${active_baseline}"
  echo "active_final=$(stat_value "${active_stat}")"
  echo "crash_total=$(stat_value "${crash_stat}")"
  echo "recover_total=$(stat_value "${recover_stat}")"
  echo "recover_error=$(stat_value "${recover_error_stat}")"
  echo "worker_init_retry_total_before=${retry_before}"
  echo "worker_init_retry_total_after=$(stat_value "${retry_stat}")"
  echo "rss_kb=${rss_before}->${rss_after}"
  echo "fd_count=${fd_before}->${fd_after}"
  echo "server_memory_allocated=${server_memory_before}->${server_memory_after}"
} | tee "${WORKDIR}/evidence.txt"

cleanup_envoy
[[ "$(grep -c 'worker-init-repro: intentional request trap' "${WORKDIR}/envoy.log")" == \
  "${CYCLES}" ]]
[[ "$(grep -c 'wasm vm recover from crash success' "${WORKDIR}/envoy.log")" == \
  "${CYCLES}" ]]
if ! grep -q '~Wasm 0 remaining active' "${WORKDIR}/console.log"; then
  echo "Envoy shutdown did not report all Wasm VMs released" >&2
  tail -n 80 "${WORKDIR}/console.log" >&2 || true
  exit 1
fi
cleanup_upstream
trap - EXIT

echo "evidence: ${WORKDIR}/evidence.txt"
echo "PASS"
