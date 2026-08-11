#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
MEMORY_TARGET_EXPLICIT="${MEMORY_TARGET_MB+x}"
MEMORY_THRESHOLD_EXPLICIT="${MEMORY_THRESHOLD_BYTES+x}"
VM_LIMIT_EXPLICIT="${DECLARED_VM_LIMIT_BYTES+x}"
VM_OVERHEAD_EXPLICIT="${DECLARED_VM_OVERHEAD_BYTES+x}"
HEADROOM_EXPLICIT="${MIN_HEADROOM_BYTES+x}"
ENVOY_BIN="${ENVOY_BIN:-}"
ENVOY_SOURCE_REPO="${ENVOY_SOURCE_REPO:-${REPO_ROOT}}"
HOST_SOURCE_REPO="${HOST_SOURCE_REPO:-${REPO_ROOT}/../proxy-wasm-cpp-host}"
EXPECTED_ENVOY_HEAD="${EXPECTED_ENVOY_HEAD:-}"
WASM_BINARY="${WASM_BINARY:-}"
WORKDIR="${WORKDIR:-/tmp/wasm-timer-reclaim}"
PROFILE="${PROFILE:-smoke}"
CASE="${1:-all}"
LISTENER_PORT="${LISTENER_PORT:-10002}"
ADMIN_PORT="${ADMIN_PORT:-9905}"
UPSTREAM_PORT="${UPSTREAM_PORT:-3082}"
BASE_ID="${BASE_ID:-192}"
CONCURRENCY="${CONCURRENCY:-1}"
MEMORY_THRESHOLD_BYTES="${MEMORY_THRESHOLD_BYTES:-33554432}"
MEMORY_TARGET_MB="${MEMORY_TARGET_MB:-40}"
EXPLICIT_MARKER_MB="${EXPLICIT_MARKER_MB:-4}"
DECLARED_VM_LIMIT_BYTES="${DECLARED_VM_LIMIT_BYTES:-134217728}"
DECLARED_VM_OVERHEAD_BYTES="${DECLARED_VM_OVERHEAD_BYTES:-33554432}"
MIN_HEADROOM_BYTES="${MIN_HEADROOM_BYTES:-33554432}"
VM_LIMIT_PROVENANCE="${VM_LIMIT_PROVENANCE:-}"
HOLD_SECONDS="${HOLD_SECONDS:-20}"
GUARD_OBSERVE_SECONDS="${GUARD_OBSERVE_SECONDS:-2.25}"
RECLAIM_TIMER_INTERVAL_MS=1000
MIN_GUARD_OBSERVE_MS=$((2 * RECLAIM_TIMER_INTERVAL_MS))
CUTOVER_DEADLINE_SECONDS="${CUTOVER_DEADLINE_SECONDS:-8}"
STEERING_REQUEST_LIMIT="${STEERING_REQUEST_LIMIT:-80}"
HOLD_REQUEST_MULTIPLIER="${HOLD_REQUEST_MULTIPLIER:-2}"
CONTROL_SECONDS="${CONTROL_SECONDS:-5}"
SOAK_CYCLES="${SOAK_CYCLES:-4}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-262144}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-12}"
TRAFFIC_FAIL_AFTER="${TRAFFIC_FAIL_AFTER:-0}"
WORKDIR_MARKER=.envoy-verification-workdir
ENVOY_PID=""
UPSTREAM_PID=""
TRAFFIC_PID=""
TRAFFIC_STOP_FILE=""
TRAFFIC_COUNT_FILE=""
TRAFFIC_ERROR_FILE=""
ENVOY_VERSION_OUTPUT=""
BINARY_ENVOY_HEAD="unknown"
BINARY_BUILD_STATE="unknown"
ENVOY_SOURCE_VERIFIED="false"
RESOLVED_HOST_PIN="unknown"
RESOLVED_HOST_PIN_SOURCE="unavailable"
RESOLVED_HOST_TREE="unknown"
RESOLVED_HOST_TREE_SOURCE="unavailable"
declare -a A_HOLD_PIDS=()
declare -a B_HOLD_PIDS=()

case "${CASE}" in
  all|explicit-idle|memory-active|control|soak) ;;
  *) echo "usage: $0 [all|explicit-idle|memory-active|control|soak]" >&2; exit 2 ;;
esac
case "${PROFILE}" in
  smoke|formal|near-oom) ;;
  *) echo "PROFILE must be smoke, formal, or near-oom" >&2; exit 2 ;;
esac
if [[ -z "${ENVOY_BIN}" || "${ENVOY_BIN}" != /* || ! -x "${ENVOY_BIN}" ]]; then
  echo "ENVOY_BIN must explicitly name an absolute executable Envoy binary" >&2
  exit 1
fi
for pair in "CONCURRENCY:${CONCURRENCY}" "MEMORY_THRESHOLD_BYTES:${MEMORY_THRESHOLD_BYTES}" \
  "MEMORY_TARGET_MB:${MEMORY_TARGET_MB}" "EXPLICIT_MARKER_MB:${EXPLICIT_MARKER_MB}" \
  "DECLARED_VM_LIMIT_BYTES:${DECLARED_VM_LIMIT_BYTES}" \
  "DECLARED_VM_OVERHEAD_BYTES:${DECLARED_VM_OVERHEAD_BYTES}" \
  "MIN_HEADROOM_BYTES:${MIN_HEADROOM_BYTES}" \
  "HOLD_SECONDS:${HOLD_SECONDS}" "CUTOVER_DEADLINE_SECONDS:${CUTOVER_DEADLINE_SECONDS}" \
  "STEERING_REQUEST_LIMIT:${STEERING_REQUEST_LIMIT}" \
  "HOLD_REQUEST_MULTIPLIER:${HOLD_REQUEST_MULTIPLIER}" \
  "CONTROL_SECONDS:${CONTROL_SECONDS}" \
  "SOAK_CYCLES:${SOAK_CYCLES}"; do
  name="${pair%%:*}"
  value="${pair#*:}"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${name} must be a positive integer" >&2
    exit 2
  fi
done
if [[ ! "${TRAFFIC_FAIL_AFTER}" =~ ^[0-9]+$ ]]; then
  echo "TRAFFIC_FAIL_AFTER must be a non-negative integer" >&2
  exit 2
fi
if [[ "${PROFILE}" == formal && "${CONCURRENCY}" != 4 ]]; then
  echo "PROFILE=formal requires CONCURRENCY=4" >&2
  exit 2
fi
if [[ "${PROFILE}" == formal ]] && (( SOAK_CYCLES < 600 || CONTROL_SECONDS < 300 )); then
  echo "PROFILE=formal requires SOAK_CYCLES>=600 and CONTROL_SECONDS>=300" >&2
  exit 2
fi
if [[ "${PROFILE}" == near-oom ]]; then
  if [[ "${CASE}" != memory-active && "${CASE}" != all ]]; then
    echo "PROFILE=near-oom requires CASE=memory-active or CASE=all" >&2
    exit 2
  fi
  if [[ -z "${MEMORY_TARGET_EXPLICIT}" || -z "${MEMORY_THRESHOLD_EXPLICIT}" ||
        -z "${VM_LIMIT_EXPLICIT}" ||
        -z "${VM_OVERHEAD_EXPLICIT}" || -z "${HEADROOM_EXPLICIT}" ||
        -z "${VM_LIMIT_PROVENANCE}" ]]; then
    echo "PROFILE=near-oom requires explicit MEMORY_THRESHOLD_BYTES, MEMORY_TARGET_MB, DECLARED_VM_LIMIT_BYTES, DECLARED_VM_OVERHEAD_BYTES, MIN_HEADROOM_BYTES, and VM_LIMIT_PROVENANCE" >&2
    exit 2
  fi
fi
if [[ ! "${GUARD_OBSERVE_SECONDS}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "GUARD_OBSERVE_SECONDS must be a non-negative number" >&2
  exit 2
fi
guard_observe_ms="$(awk -v seconds="${GUARD_OBSERVE_SECONDS}" \
  'BEGIN { printf "%.0f", seconds * 1000 }')"
case "${CASE}" in
  all|explicit-idle|memory-active)
    if (( guard_observe_ms < MIN_GUARD_OBSERVE_MS )); then
      echo "rolling evidence requires GUARD_OBSERVE_SECONDS to cover at least two full 1-second timer intervals (${MIN_GUARD_OBSERVE_MS}ms)" >&2
      exit 2
    fi
    ;;
esac

memory_target_bytes=$((MEMORY_TARGET_MB * 1024 * 1024))
marker_bytes=$((EXPLICIT_MARKER_MB * 1024 * 1024))
declared_peak_bytes=$((memory_target_bytes + marker_bytes + DECLARED_VM_OVERHEAD_BYTES))
if (( memory_target_bytes < MEMORY_THRESHOLD_BYTES )); then
  echo "MEMORY_TARGET_MB must cross MEMORY_THRESHOLD_BYTES" >&2
  exit 2
fi
if (( declared_peak_bytes + MIN_HEADROOM_BYTES > DECLARED_VM_LIMIT_BYTES )); then
  echo "memory target violates declared VM headroom: target=${memory_target_bytes}, overhead=${DECLARED_VM_OVERHEAD_BYTES}, headroom=${MIN_HEADROOM_BYTES}, limit=${DECLARED_VM_LIMIT_BYTES}" >&2
  exit 2
fi
if [[ "${PROFILE}" == near-oom ]] && (( DECLARED_VM_LIMIT_BYTES - declared_peak_bytes > MIN_HEADROOM_BYTES * 2 )); then
  echo "PROFILE=near-oom requires the target to finish within 2x MIN_HEADROOM_BYTES of DECLARED_VM_LIMIT_BYTES" >&2
  exit 2
fi

resolve_binary_provenance() {
  local version_payload locations
  if ! ENVOY_VERSION_OUTPUT="$("${ENVOY_BIN}" --version 2>&1)"; then
    echo "failed to read Envoy binary version from ${ENVOY_BIN}" >&2
    return 1
  fi
  version_payload="$(printf '%s\n' "${ENVOY_VERSION_OUTPUT}" |
    sed -n 's#^.* version: ##p' | head -1)"
  BINARY_ENVOY_HEAD="${version_payload%%/*}"
  BINARY_BUILD_STATE="$(printf '%s\n' "${version_payload}" | awk -F/ '{print $3}')"
  if [[ ! "${BINARY_ENVOY_HEAD}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Envoy --version did not expose an exact 40-character source commit" >&2
    return 1
  fi
  if [[ -n "${EXPECTED_ENVOY_HEAD}" && "${BINARY_ENVOY_HEAD}" != "${EXPECTED_ENVOY_HEAD}" ]]; then
    echo "Envoy binary commit ${BINARY_ENVOY_HEAD} does not match EXPECTED_ENVOY_HEAD=${EXPECTED_ENVOY_HEAD}" >&2
    return 1
  fi
  if git -C "${ENVOY_SOURCE_REPO}" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
    git -C "${ENVOY_SOURCE_REPO}" cat-file -e "${BINARY_ENVOY_HEAD}^{commit}" 2>/dev/null; then
    ENVOY_SOURCE_VERIFIED=true
  fi
  if [[ "${ENVOY_SOURCE_VERIFIED}" == true && "${BINARY_BUILD_STATE}" == Clean ]]; then
    locations="$(git -C "${ENVOY_SOURCE_REPO}" \
      show "${BINARY_ENVOY_HEAD}:bazel/repository_locations.bzl" 2>/dev/null || true)"
    RESOLVED_HOST_PIN="$(printf '%s\n' "${locations}" |
      awk '/proxy_wasm_cpp_host = dict\(/ {in_host=1} in_host && /version =/ {gsub(/[",]/, "", $3); print $3; exit}')"
    if [[ "${RESOLVED_HOST_PIN}" =~ ^[0-9a-f]{40}$ ]]; then
      RESOLVED_HOST_PIN_SOURCE=envoy-binary-source-commit
    else
      RESOLVED_HOST_PIN=unknown
    fi
  fi
  if [[ "${RESOLVED_HOST_PIN}" != unknown ]] &&
    git -C "${HOST_SOURCE_REPO}" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
    git -C "${HOST_SOURCE_REPO}" cat-file -e "${RESOLVED_HOST_PIN}^{commit}" 2>/dev/null; then
    RESOLVED_HOST_TREE="$(git -C "${HOST_SOURCE_REPO}" show -s --format=%T \
      "${RESOLVED_HOST_PIN}")"
    RESOLVED_HOST_TREE_SOURCE=host-source-repo
  fi
  if [[ -n "${HOST_COMMIT_TREE:-}" && "${RESOLVED_HOST_TREE}" != unknown &&
        "${HOST_COMMIT_TREE}" != "${RESOLVED_HOST_TREE}" ]]; then
    echo "resolved Host tree ${RESOLVED_HOST_TREE} does not match HOST_COMMIT_TREE=${HOST_COMMIT_TREE}" >&2
    return 1
  fi
  if [[ "${PROFILE}" == formal || "${PROFILE}" == near-oom ]]; then
    if [[ "${BINARY_BUILD_STATE}" != Clean || "${ENVOY_SOURCE_VERIFIED}" != true ||
          "${RESOLVED_HOST_PIN}" == unknown || "${RESOLVED_HOST_TREE}" == unknown ]]; then
      echo "${PROFILE} evidence requires a clean exact Envoy binary commit plus verifiable Envoy-source Host pin and Host commit tree" >&2
      return 1
    fi
  fi
}

resolve_binary_provenance

tmp_root="$(realpath -m /tmp)"
WORKDIR="$(realpath -m "${WORKDIR}")"
case "${WORKDIR}" in
  "${tmp_root}"/?*) ;;
  *) echo "WORKDIR must be a dedicated directory below ${tmp_root}" >&2; exit 1 ;;
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
  if [[ -n "${TRAFFIC_PID}" ]]; then
    [[ -n "${TRAFFIC_STOP_FILE}" ]] && touch "${TRAFFIC_STOP_FILE}"
    kill "${TRAFFIC_PID}" >/dev/null 2>&1 || true
    wait "${TRAFFIC_PID}" >/dev/null 2>&1 || true
    TRAFFIC_PID=""
    TRAFFIC_STOP_FILE=""
    TRAFFIC_COUNT_FILE=""
    TRAFFIC_ERROR_FILE=""
  fi
  if [[ -n "${ENVOY_PID}" ]]; then
    kill "${ENVOY_PID}" >/dev/null 2>&1 || true
    wait "${ENVOY_PID}" >/dev/null 2>&1 || true
    ENVOY_PID=""
  fi
}
stop_envoy_and_assert_lifecycle() {
  local case_dir="$1"
  if [[ -n "${ENVOY_PID}" ]]; then
    kill "${ENVOY_PID}" >/dev/null 2>&1 || true
    wait "${ENVOY_PID}" >/dev/null 2>&1 || true
    ENVOY_PID=""
  fi
  local remaining
  remaining="$({ grep '~Wasm [0-9][0-9]* remaining active' "${case_dir}/envoy.log" || true; } |
    tail -1 | sed -n 's/.*~Wasm \([0-9][0-9]*\) remaining active.*/\1/p')"
  if [[ -z "${remaining}" ]] || (( remaining > 1 )); then
    echo "normal shutdown left more than the process-global cached base: remaining=${remaining:-unknown}" >&2
    return 1
  fi
}
cleanup_all() {
  cleanup_envoy
  if [[ -n "${UPSTREAM_PID}" ]]; then
    kill "${UPSTREAM_PID}" >/dev/null 2>&1 || true
    wait "${UPSTREAM_PID}" >/dev/null 2>&1 || true
    UPSTREAM_PID=""
  fi
}
trap cleanup_all EXIT

if [[ -n "${WASM_BINARY}" ]]; then
  if [[ "${WASM_BINARY}" != /* || ! -f "${WASM_BINARY}" ]]; then
    echo "WASM_BINARY must name an absolute existing file" >&2
    exit 1
  fi
else
  WASM_BINARY="${WORKDIR}/reclaim-verify.wasm"
  (cd "${SCRIPT_DIR}/wasm" && GOWORK=off GOOS=wasip1 GOARCH=wasm \
    go build -buildmode=c-shared -o "${WASM_BINARY}" .)
fi
WASM_BINARY="$(realpath "${WASM_BINARY}")"

UPSTREAM_PORT="${UPSTREAM_PORT}" python3 "${SCRIPT_DIR}/../upstream_server.py" \
  >"${WORKDIR}/upstream.log" 2>&1 &
UPSTREAM_PID=$!
for _ in $(seq 1 80); do
  if curl --fail --silent --max-time 1 "http://127.0.0.1:${UPSTREAM_PORT}/" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done
curl --fail --silent --show-error --max-time 2 \
  "http://127.0.0.1:${UPSTREAM_PORT}/" >/dev/null

prefix=wasm.envoy.wasm.runtime.v8.plugin.reclaim_verify_plugin
rebuild_stat="${prefix}.rebuild_total"
explicit_stat="${prefix}.rebuild_explicit_total"
memory_stat="${prefix}.rebuild_memory_total"
recover_stat="${prefix}.recover_total"
active_stat=wasm.envoy.wasm.runtime.v8.active

render_config() {
  local output="$1"
  sed -e "s#__ADMIN_PORT__#${ADMIN_PORT}#g" \
    -e "s#__LISTENER_PORT__#${LISTENER_PORT}#g" \
    -e "s#__UPSTREAM_PORT__#${UPSTREAM_PORT}#g" \
    -e "s#__MEMORY_THRESHOLD_BYTES__#${MEMORY_THRESHOLD_BYTES}#g" \
    -e "s#__WASM_BINARY__#${WASM_BINARY}#g" \
    "${SCRIPT_DIR}/config.yaml.template" >"${output}"
}
stat_value() {
  local stat="$1"
  curl --fail --silent --show-error --max-time 2 \
    "http://127.0.0.1:${ADMIN_PORT}/stats?filter=^${stat}$" |
    awk -F': ' -v stat="${stat}" '$1 == stat {print $2; found=1} END {if (!found) print 0}'
}
latency_count() {
  curl --fail --silent --show-error --max-time 2 \
    "http://127.0.0.1:${ADMIN_PORT}/stats/prometheus?filter=reclaim_latency" |
    awk '$1 ~ /reclaim_latency_count/ {sum += $2} END {print sum + 0}'
}
wait_stat_at_least() {
  local stat="$1" expected="$2" deadline=$((SECONDS + CUTOVER_DEADLINE_SECONDS)) value=0
  while (( SECONDS <= deadline )); do
    value="$(stat_value "${stat}")"
    if (( value >= expected )); then
      echo "${stat}=${value} reached expected minimum ${expected}"
      return 0
    fi
    sleep 0.20
  done
  echo "timed out waiting for ${stat}>=${expected}; last=${value}" >&2
  return 1
}
wait_latency_at_least() {
  local expected="$1" deadline=$((SECONDS + CUTOVER_DEADLINE_SECONDS)) value=0
  while (( SECONDS <= deadline )); do
    value="$(latency_count)"
    if (( value >= expected )); then
      echo "reclaim_latency_count=${value} reached expected minimum ${expected}"
      return 0
    fi
    sleep 0.20
  done
  echo "timed out waiting for reclaim_latency_count>=${expected}; last=${value}" >&2
  return 1
}
proc_rss_kb() {
  awk '/VmRSS:/ {print $2; found=1} END {if (!found) print 0}' \
    "/proc/${ENVOY_PID}/status" 2>/dev/null || echo 0
}
proc_fd_count() {
  find "/proc/${ENVOY_PID}/fd" -maxdepth 1 -type l 2>/dev/null | wc -l
}
record_resources() {
  local case_dir="$1" phase="$2"
  printf '%s,%s,%s,%s,%s,%s,%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${phase}" \
    "$(proc_rss_kb)" "$(proc_fd_count)" "$(stat_value "${active_stat}")" \
    "$(stat_value "${rebuild_stat}")" "$(latency_count)" >>"${case_dir}/resources.csv"
}
traffic_success_count() {
  local count=0
  if [[ -f "${TRAFFIC_COUNT_FILE}" ]]; then
    count="$(<"${TRAFFIC_COUNT_FILE}")"
  fi
  [[ "${count}" =~ ^[0-9]+$ ]] || count=0
  printf '%s\n' "${count}"
}
start_continuous_traffic() {
  local case_dir="$1"
  if [[ -n "${TRAFFIC_PID}" ]]; then
    echo "continuous traffic is already running" >&2
    return 1
  fi
  TRAFFIC_STOP_FILE="${case_dir}/traffic.stop"
  TRAFFIC_COUNT_FILE="${case_dir}/traffic-success-count"
  TRAFFIC_ERROR_FILE="${case_dir}/traffic.error"
  rm -f "${TRAFFIC_STOP_FILE}" "${TRAFFIC_ERROR_FILE}"
  printf '0\n' >"${TRAFFIC_COUNT_FILE}"
  (
    local count=0 temporary_count="${TRAFFIC_COUNT_FILE}.tmp"
    while [[ ! -e "${TRAFFIC_STOP_FILE}" ]]; do
      if ! curl --fail --silent --show-error --max-time 2 -H 'connection: close' \
        "http://127.0.0.1:${LISTENER_PORT}/soak-traffic-$((count + 1))" >/dev/null; then
        printf 'continuous traffic curl failed after %s successful requests\n' "${count}" \
          >"${TRAFFIC_ERROR_FILE}"
        exit 1
      fi
      count=$((count + 1))
      printf '%s\n' "${count}" >"${temporary_count}"
      mv "${temporary_count}" "${TRAFFIC_COUNT_FILE}"
      if (( TRAFFIC_FAIL_AFTER > 0 && count >= TRAFFIC_FAIL_AFTER )); then
        printf 'injected continuous traffic failure after %s successful requests\n' "${count}" \
          >"${TRAFFIC_ERROR_FILE}"
        exit 42
      fi
    done
  ) &
  TRAFFIC_PID=$!
}
assert_continuous_traffic_progress() {
  local previous="$1" current message deadline=$((SECONDS + 4))
  while (( SECONDS <= deadline )); do
    current="$(traffic_success_count)"
    if (( current > previous )); then
      printf '%s\n' "${current}"
      return 0
    fi
    if ! kill -0 "${TRAFFIC_PID}" >/dev/null 2>&1; then
      message=no-error-recorded
      [[ -s "${TRAFFIC_ERROR_FILE}" ]] && message="$(<"${TRAFFIC_ERROR_FILE}")"
      echo "continuous traffic exited before making progress: ${message}" >&2
      return 1
    fi
    sleep 0.10
  done
  echo "continuous traffic made no progress in 4 seconds (success_count=${current})" >&2
  return 1
}
stop_continuous_traffic() {
  local case_dir="$1" status=0 final_count message
  touch "${TRAFFIC_STOP_FILE}"
  if wait "${TRAFFIC_PID}"; then
    status=0
  else
    status=$?
  fi
  TRAFFIC_PID=""
  final_count="$(traffic_success_count)"
  if (( status != 0 )) || [[ -s "${TRAFFIC_ERROR_FILE}" ]]; then
    message=no-error-recorded
    [[ -s "${TRAFFIC_ERROR_FILE}" ]] && message="$(<"${TRAFFIC_ERROR_FILE}")"
    echo "continuous traffic failed (status=${status}, success_count=${final_count}): ${message}" >&2
    return 1
  fi
  if (( final_count == 0 )); then
    echo "continuous traffic stopped without a successful request" >&2
    return 1
  fi
  {
    echo "stopped_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "successful_requests=${final_count}"
    echo "exit_status=${status}"
  } >"${case_dir}/traffic-summary.txt"
  TRAFFIC_STOP_FILE=""
  TRAFFIC_COUNT_FILE=""
  TRAFFIC_ERROR_FILE=""
}
start_envoy() {
  local case_dir="$1"
  mkdir -p "${case_dir}"
  render_config "${case_dir}/config.yaml"
  "${ENVOY_BIN}" --mode validate -c "${case_dir}/config.yaml" --log-level error \
    >"${case_dir}/config-validate.log" 2>&1
  "${ENVOY_BIN}" -c "${case_dir}/config.yaml" --concurrency "${CONCURRENCY}" \
    --base-id "${BASE_ID}" --log-level warn --component-log-level wasm:debug \
    --file-flush-interval-msec 10 \
    --log-path "${case_dir}/envoy.log" >"${case_dir}/envoy-console.log" 2>&1 &
  ENVOY_PID=$!
  for _ in $(seq 1 120); do
    if curl --fail --silent --max-time 1 \
      "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null 2>&1; then
      printf '%s\n' 'timestamp,phase,rss_kb,fd_count,active_vm,rebuild_total,reclaim_latency_count' \
        >"${case_dir}/resources.csv"
      return 0
    fi
    if ! kill -0 "${ENVOY_PID}" >/dev/null 2>&1; then
      echo "Envoy exited before ready" >&2
      cat "${case_dir}/envoy-console.log" >&2 || true
      return 1
    fi
    sleep 0.25
  done
  echo "timed out waiting for Envoy readiness" >&2
  return 1
}
require_response_marker() {
  local headers="$1" expected_bytes="$2"
  grep -qi '^HTTP/1.1 200' "${headers}"
  grep -qi "^x-reclaim-retained-bytes: ${expected_bytes}" "${headers}"
  grep -Eqi '^x-reclaim-worker-token: [0-9a-f]{16}' "${headers}"
}
request() {
  local case_dir="$1" label="$2" expected_bytes="$3"
  shift 3
  curl --fail --silent --show-error --http1.1 --max-time 10 -H 'connection: close' \
    -H "x-reclaim-request-id: ${label}" "$@" -D "${case_dir}/${label}.headers" \
    -o "${case_dir}/${label}.body" "http://127.0.0.1:${LISTENER_PORT}/${label}"
  require_response_marker "${case_dir}/${label}.headers" "${expected_bytes}"
}
worker_ids() {
  local case_dir="$1" request_prefix="$2"
  find "${case_dir}" -maxdepth 1 -type f -name "${request_prefix}*.headers" \
    -exec grep -hi '^x-reclaim-worker-token: ' {} + 2>/dev/null |
    tr -d '\r' | awk -F': ' '{print $2}' | sort -u
}
validate_near_oom_memory() {
  local case_dir="$1" request_prefix="$2" phase="$3" headers token bytes remaining
  local output="${case_dir}/near-oom-vm-memory.csv" max_bytes=0 min_remaining
  local -A observed_by_worker=()
  [[ "${PROFILE}" == near-oom ]] || return 0
  if [[ ! -f "${output}" ]]; then
    printf '%s\n' \
      'phase,worker_token,observed_vm_memory_bytes,threshold_bytes,declared_peak_bytes,vm_limit_bytes,remaining_headroom_bytes,minimum_headroom_bytes' \
      >"${output}"
  fi
  while IFS= read -r -d '' headers; do
    token="$(tr -d '\r' <"${headers}" |
      awk -F': ' 'tolower($1) == "x-reclaim-worker-token" {print $2; exit}')"
    bytes="$(tr -d '\r' <"${headers}" |
      awk -F': ' 'tolower($1) == "x-reclaim-vm-memory-bytes" {print $2; exit}')"
    if [[ ! "${token}" =~ ^[0-9a-f]{16}$ || ! "${bytes}" =~ ^[0-9]+$ ]]; then
      echo "near-OOM response is missing a valid worker token or VM memory observation: ${headers}" >&2
      return 1
    fi
    if [[ -z "${observed_by_worker[${token}]:-}" ]] ||
      (( bytes > observed_by_worker[${token}] )); then
      observed_by_worker[${token}]="${bytes}"
    fi
  done < <(find "${case_dir}" -maxdepth 1 -type f -name "${request_prefix}*.headers" \
    -print0 | sort -z)
  if (( ${#observed_by_worker[@]} < CONCURRENCY )); then
    echo "near-OOM VM-memory observations covered ${#observed_by_worker[@]} workers, expected ${CONCURRENCY}" >&2
    return 1
  fi
  min_remaining="${DECLARED_VM_LIMIT_BYTES}"
  for token in "${!observed_by_worker[@]}"; do
    bytes="${observed_by_worker[${token}]}"
    remaining=$((DECLARED_VM_LIMIT_BYTES - bytes))
    if (( bytes < MEMORY_THRESHOLD_BYTES )); then
      echo "near-OOM observed VM memory ${bytes} did not cross threshold ${MEMORY_THRESHOLD_BYTES}" >&2
      return 1
    fi
    if (( bytes > declared_peak_bytes )); then
      echo "near-OOM observed VM memory ${bytes} exceeded declared peak ${declared_peak_bytes}" >&2
      return 1
    fi
    if (( remaining < MIN_HEADROOM_BYTES || remaining > MIN_HEADROOM_BYTES * 2 )); then
      echo "near-OOM observed remaining headroom ${remaining} is outside [${MIN_HEADROOM_BYTES}, $((MIN_HEADROOM_BYTES * 2))]" >&2
      return 1
    fi
    (( bytes > max_bytes )) && max_bytes="${bytes}"
    (( remaining < min_remaining )) && min_remaining="${remaining}"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "${phase}" "${token}" "${bytes}" \
      "${MEMORY_THRESHOLD_BYTES}" "${declared_peak_bytes}" "${DECLARED_VM_LIMIT_BYTES}" \
      "${remaining}" "${MIN_HEADROOM_BYTES}" >>"${output}"
  done
  {
    echo "near_oom_${phase}_max_observed_vm_memory_bytes=${max_bytes}"
    echo "near_oom_${phase}_min_observed_headroom_bytes=${min_remaining}"
  } >>"${case_dir}/manifest.txt"
}
live_hold_worker_ids() {
  local log="$1" request_prefix="$2"
  awk -v request="request_id=${request_prefix}" '
    index($0, "holding active request") && index($0, request) {
      line = $0
      sub(/^.*worker_token=/, "", line)
      sub(/ .*/, "", line)
      print line
    }
  ' "${log}" 2>/dev/null | sort -u
}
cover_workers() {
  local case_dir="$1" request_prefix="$2" expected_bytes="$3" trigger="$4" i count
  for i in $(seq 1 "${STEERING_REQUEST_LIMIT}"); do
    case "${trigger}" in
      none) request "${case_dir}" "${request_prefix}-${i}" "${expected_bytes}" ;;
      explicit) request "${case_dir}" "${request_prefix}-${i}" "${expected_bytes}" \
        -H 'x-set-rebuild: true' ;;
      memory) request "${case_dir}" "${request_prefix}-${i}" "${expected_bytes}" \
        -H "x-alloc-mb: ${MEMORY_TARGET_MB}" ;;
      *) echo "unknown trigger ${trigger}" >&2; return 2 ;;
    esac
    count="$(worker_ids "${case_dir}" "${request_prefix}-" | wc -l)"
    if (( count >= CONCURRENCY )); then
      worker_ids "${case_dir}" "${request_prefix}-" \
        >"${case_dir}/${request_prefix}-workers.txt"
      return 0
    fi
  done
  echo "failed to steer ${request_prefix} across ${CONCURRENCY} workers" >&2
  return 1
}
start_holds() {
  local -n hold_pids="$1"
  local case_dir="$2" request_prefix="$3" trigger="$4" marker_mb="$5"
  local expected_bytes=$((marker_mb * 1024 * 1024)) i headers body count
  local minimum_hold_count=$((CONCURRENCY * HOLD_REQUEST_MULTIPLIER))
  local -a args
  hold_pids=()
  for i in $(seq 1 "${STEERING_REQUEST_LIMIT}"); do
    headers="${case_dir}/${request_prefix}-${i}.headers"
    body="${case_dir}/${request_prefix}-${i}.body"
    args=(-H 'connection: close' -H "x-reclaim-request-id: ${request_prefix}-${i}" \
      -H 'x-hold-active: true' -H "x-hold-delay: ${HOLD_SECONDS}")
    if [[ "${trigger}" == marker ]]; then
      args+=(-H "x-alloc-mb: ${marker_mb}")
    fi
    curl --fail --silent --show-error --http1.1 --max-time "$((HOLD_SECONDS + 20))" \
      "${args[@]}" -D "${headers}" -o "${body}" \
      "http://127.0.0.1:${LISTENER_PORT}/${request_prefix}-${i}" &
    hold_pids+=("$!")
    sleep 0.05
    count="$(live_hold_worker_ids "${case_dir}/envoy.log" "${request_prefix}-" | wc -l)"
    if (( i >= minimum_hold_count && count >= CONCURRENCY )); then
      live_hold_worker_ids "${case_dir}/envoy.log" "${request_prefix}-" \
        >"${case_dir}/${request_prefix}-live-workers.txt"
      printf '%s\n' "${expected_bytes}" >"${case_dir}/${request_prefix}-expected-bytes"
      return 0
    fi
  done
  echo "failed to establish held ${request_prefix} Contexts on all ${CONCURRENCY} workers" >&2
  return 1
}
wait_holds() {
  local -n hold_pids="$1"
  local case_dir="$2" request_prefix="$3" expected_bytes="$4" index=1 pid
  for pid in "${hold_pids[@]}"; do
    if ! wait "${pid}"; then
      echo "held request ${request_prefix}-${index} failed" >&2
      return 1
    fi
    require_response_marker "${case_dir}/${request_prefix}-${index}.headers" "${expected_bytes}"
    index=$((index + 1))
  done
  if (( $(worker_ids "${case_dir}" "${request_prefix}-" | wc -l) < CONCURRENCY )); then
    echo "held requests ${request_prefix} did not cover all ${CONCURRENCY} worker plugin instances" >&2
    return 1
  fi
  worker_ids "${case_dir}" "${request_prefix}-" >"${case_dir}/${request_prefix}-workers.txt"
  hold_pids=()
}
assert_holds_alive() {
  local -n hold_pids="$1"
  local phase="$2" pid state
  if (( ${#hold_pids[@]} == 0 )); then
    echo "${phase}: no held requests remain to prove a live old generation" >&2
    return 1
  fi
  for pid in "${hold_pids[@]}"; do
    state="$(ps -o stat= -p "${pid}" 2>/dev/null | awk '{print $1}')"
    if ! kill -0 "${pid}" >/dev/null 2>&1 || [[ -z "${state}" || "${state}" == Z* ]]; then
      echo "${phase}: held request process ${pid} exited before the guard window completed" >&2
      return 1
    fi
  done
}
wait_active_cap() {
  local baseline="$1" value deadline=$((SECONDS + CUTOVER_DEADLINE_SECONDS))
  while (( SECONDS <= deadline )); do
    value="$(stat_value "${active_stat}")"
    if (( value <= baseline + CONCURRENCY )); then
      return 0
    fi
    sleep 0.20
  done
  echo "active generation cap did not settle: baseline=${baseline}, current=${value}, workers=${CONCURRENCY}" >&2
  return 1
}
wait_active_baseline() {
  local expected="$1" value deadline=$((SECONDS + CUTOVER_DEADLINE_SECONDS))
  while (( SECONDS <= deadline )); do
    value="$(stat_value "${active_stat}")"
    if [[ "${value}" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.20
  done
  echo "active VM count did not return to baseline: expected=${expected}, actual=${value}" >&2
  return 1
}
write_manifest() {
  local case_dir="$1" case_name="$2"
  {
    echo "started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "case=${case_name}"
    echo "profile=${PROFILE}"
    echo "envoy_bin=${ENVOY_BIN}"
    echo "envoy_sha256=$(sha256sum "${ENVOY_BIN}" | awk '{print $1}')"
    echo "envoy_version=$(printf '%s' "${ENVOY_VERSION_OUTPUT}" | tr '\n' ' ')"
    echo "envoy_binary_commit=${BINARY_ENVOY_HEAD}"
    echo "envoy_binary_build_state=${BINARY_BUILD_STATE}"
    echo "envoy_source_repo=${ENVOY_SOURCE_REPO}"
    echo "envoy_source_commit_verified=${ENVOY_SOURCE_VERIFIED}"
    echo "config_sha256=$(sha256sum "${case_dir}/config.yaml" | awk '{print $1}')"
    echo "wasm_sha256=$(sha256sum "${WASM_BINARY}" | awk '{print $1}')"
    echo "proxy_wasm_cpp_host_commit=${RESOLVED_HOST_PIN}"
    echo "proxy_wasm_cpp_host_commit_source=${RESOLVED_HOST_PIN_SOURCE}"
    echo "proxy_wasm_cpp_host_tree=${RESOLVED_HOST_TREE}"
    echo "proxy_wasm_cpp_host_tree_source=${RESOLVED_HOST_TREE_SOURCE}"
    echo "concurrency=${CONCURRENCY}"
    echo "memory_threshold_bytes=${MEMORY_THRESHOLD_BYTES}"
    echo "memory_target_bytes=${memory_target_bytes}"
    echo "explicit_marker_bytes=${marker_bytes}"
    echo "declared_vm_limit_bytes=${DECLARED_VM_LIMIT_BYTES}"
    echo "declared_vm_overhead_bytes=${DECLARED_VM_OVERHEAD_BYTES}"
    echo "declared_peak_bytes=${declared_peak_bytes}"
    echo "minimum_headroom_bytes=${MIN_HEADROOM_BYTES}"
    echo "vm_limit_provenance=${VM_LIMIT_PROVENANCE:-smoke-budget-not-runtime-limit}"
    echo "reclaim_latency_semantics=eligible-to-new-current-cutover"
    echo "rss_semantics=resource-sample-only-no-retirement-claim"
  } >"${case_dir}/manifest.txt"
}

run_rolling_case() {
  local source="$1" current_state="$2" case_name
  case_name="${source}-${current_state}"
  local case_dir="${WORKDIR}/${case_name}" marker_mb expected_a_bytes expected_b_bytes
  local rebuild_before source_before other_before latency_before latency_after_first
  local guard_rebuild active_baseline final_rebuild expected_final
  local guard_start_utc guard_end_utc guard_start_ms guard_end_ms guard_elapsed_ms
  marker_mb="${EXPLICIT_MARKER_MB}"
  expected_a_bytes=$((marker_mb * 1024 * 1024))
  expected_b_bytes=0
  [[ "${source}" == memory ]] && expected_b_bytes="${memory_target_bytes}"

  start_envoy "${case_dir}"
  write_manifest "${case_dir}" "${case_name}"
  cover_workers "${case_dir}" baseline 0 none
  active_baseline="$(stat_value "${active_stat}")"
  rebuild_before="$(stat_value "${rebuild_stat}")"
  latency_before="$(latency_count)"
  if [[ "${source}" == explicit ]]; then
    source_before="$(stat_value "${explicit_stat}")"
    other_before="$(stat_value "${memory_stat}")"
  else
    source_before="$(stat_value "${memory_stat}")"
    other_before="$(stat_value "${explicit_stat}")"
  fi
  record_resources "${case_dir}" baseline

  # Establish active A Contexts on every worker before making A eligible. This avoids
  # timer phase skew allowing late held requests to enter B in the N-worker profile.
  start_holds A_HOLD_PIDS "${case_dir}" A-hold marker "${marker_mb}"
  cover_workers "${case_dir}" A-eligible \
    "$([[ "${source}" == memory ]] && echo "${memory_target_bytes}" || echo "${expected_a_bytes}")" \
    "${source}"
  if [[ "${source}" == memory ]]; then
    validate_near_oom_memory "${case_dir}" A-eligible- A_eligible
  fi
  wait_stat_at_least "${rebuild_stat}" "$((rebuild_before + CONCURRENCY))"
  wait_latency_at_least "$((latency_before + CONCURRENCY))"
  latency_after_first="$(latency_count)"
  wait_active_cap "${active_baseline}"
  cover_workers "${case_dir}" B-probe 0 none
  record_resources "${case_dir}" A-and-B-live

  cover_workers "${case_dir}" B-eligible \
    "$([[ "${source}" == memory ]] && echo "${memory_target_bytes}" || echo 0)" "${source}"
  if [[ "${source}" == memory ]]; then
    validate_near_oom_memory "${case_dir}" B-eligible- B_eligible
  fi
  if [[ "${current_state}" == active ]]; then
    start_holds B_HOLD_PIDS "${case_dir}" B-hold none 0
  fi
  guard_rebuild="$(stat_value "${rebuild_stat}")"
  assert_holds_alive A_HOLD_PIDS guard-start
  guard_start_utc="$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)"
  guard_start_ms="$(date +%s%3N)"
  sleep "${GUARD_OBSERVE_SECONDS}"
  guard_end_ms="$(date +%s%3N)"
  guard_end_utc="$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)"
  guard_elapsed_ms=$((guard_end_ms - guard_start_ms))
  assert_holds_alive A_HOLD_PIDS guard-end
  if (( guard_elapsed_ms < MIN_GUARD_OBSERVE_MS )); then
    echo "actual live-old guard observation ${guard_elapsed_ms}ms did not cover two 1-second timer intervals" >&2
    return 1
  fi
  {
    echo "requested_seconds=${GUARD_OBSERVE_SECONDS}"
    echo "timer_interval_ms=${RECLAIM_TIMER_INTERVAL_MS}"
    echo "minimum_required_ms=${MIN_GUARD_OBSERVE_MS}"
    echo "started_at=${guard_start_utc}"
    echo "ended_at=${guard_end_utc}"
    echo "elapsed_ms=${guard_elapsed_ms}"
    echo "held_a_processes=${#A_HOLD_PIDS[@]}"
  } >"${case_dir}/guard-window.txt"
  {
    echo "guard_started_at=${guard_start_utc}"
    echo "guard_ended_at=${guard_end_utc}"
    echo "guard_elapsed_ms=${guard_elapsed_ms}"
  } >>"${case_dir}/manifest.txt"
  if [[ "$(stat_value "${rebuild_stat}")" != "${guard_rebuild}" ]]; then
    echo "live A failed to block the B -> C proactive cutover" >&2
    return 1
  fi
  if [[ "$(latency_count)" != "${latency_after_first}" ]]; then
    echo "live-old skip unexpectedly recorded reclaim latency" >&2
    return 1
  fi
  wait_active_cap "${active_baseline}"
  record_resources "${case_dir}" live-old-guard

  wait_holds A_HOLD_PIDS "${case_dir}" A-hold "${expected_a_bytes}"
  expected_final=$((rebuild_before + 2 * CONCURRENCY))
  wait_stat_at_least "${rebuild_stat}" "${expected_final}"
  wait_latency_at_least "$((latency_after_first + CONCURRENCY))"
  cover_workers "${case_dir}" C-probe 0 none
  final_rebuild="$(stat_value "${rebuild_stat}")"
  if [[ "${final_rebuild}" != "${expected_final}" ]]; then
    echo "unexpected successful rebuild count: expected=${expected_final}, actual=${final_rebuild}" >&2
    return 1
  fi
  if [[ "${source}" == explicit ]]; then
    [[ "$(stat_value "${explicit_stat}")" == "$((source_before + 2 * CONCURRENCY))" ]]
    [[ "$(stat_value "${memory_stat}")" == "${other_before}" ]]
  else
    [[ "$(stat_value "${memory_stat}")" == "$((source_before + 2 * CONCURRENCY))" ]]
    [[ "$(stat_value "${explicit_stat}")" == "${other_before}" ]]
  fi
  [[ "$(stat_value "${recover_stat}")" == 0 ]]
  wait_active_cap "${active_baseline}"
  record_resources "${case_dir}" B-to-C-cutover

  if [[ "${current_state}" == active ]]; then
    wait_holds B_HOLD_PIDS "${case_dir}" B-hold "${expected_b_bytes}"
  fi
  wait_active_baseline "${active_baseline}"
  record_resources "${case_dir}" complete
  stop_envoy_and_assert_lifecycle "${case_dir}"
  echo "PASS ${case_name}: active A rolled to B, live A blocked B -> C, then retirement enabled exactly one next cutover"
}

run_control() {
  local case_dir="${WORKDIR}/healthy-control" before active_baseline deadline i=0
  start_envoy "${case_dir}"
  write_manifest "${case_dir}" healthy-control
  cover_workers "${case_dir}" control-baseline 0 none
  before="$(stat_value "${rebuild_stat}")"
  active_baseline="$(stat_value "${active_stat}")"
  record_resources "${case_dir}" baseline
  deadline=$((SECONDS + CONTROL_SECONDS))
  while (( SECONDS < deadline )); do
    i=$((i + 1))
    request "${case_dir}" "control-${i}" 0
  done
  [[ "$(stat_value "${rebuild_stat}")" == "${before}" ]]
  [[ "$(stat_value "${recover_stat}")" == 0 ]]
  wait_active_baseline "${active_baseline}"
  record_resources "${case_dir}" complete
  stop_envoy_and_assert_lifecycle "${case_dir}"
  echo "PASS healthy-control: sustained requests produced no reclaim/recovery"
}

run_soak() {
  local case_dir="${WORKDIR}/rolling-soak" before target rss_before rss_after fd_before fd_after cycle
  local active_baseline traffic_count
  start_envoy "${case_dir}"
  write_manifest "${case_dir}" rolling-soak
  cover_workers "${case_dir}" soak-baseline 0 none
  active_baseline="$(stat_value "${active_stat}")"
  before="$(stat_value "${rebuild_stat}")"
  rss_before="$(proc_rss_kb)"
  fd_before="$(proc_fd_count)"
  start_continuous_traffic "${case_dir}"
  traffic_count="$(assert_continuous_traffic_progress 0)"
  for cycle in $(seq 1 "${SOAK_CYCLES}"); do
    cover_workers "${case_dir}" "soak-trigger-${cycle}" 0 explicit
    target=$((before + cycle * CONCURRENCY))
    wait_stat_at_least "${rebuild_stat}" "${target}"
    if [[ "$(stat_value "${rebuild_stat}")" != "${target}" ]]; then
      echo "soak cycle ${cycle} created an unbounded generation chain" >&2
      return 1
    fi
    record_resources "${case_dir}" "cycle-${cycle}"
    sleep 1.10
    traffic_count="$(assert_continuous_traffic_progress "${traffic_count}")"
  done
  stop_continuous_traffic "${case_dir}"
  rss_after="$(proc_rss_kb)"
  fd_after="$(proc_fd_count)"
  if (( rss_after - rss_before > MAX_RSS_GROWTH_KB )); then
    echo "soak RSS growth exceeded bound: before=${rss_before}, after=${rss_after}, max=${MAX_RSS_GROWTH_KB}" >&2
    return 1
  fi
  if (( fd_after - fd_before > MAX_FD_GROWTH )); then
    echo "soak FD growth exceeded bound: before=${fd_before}, after=${fd_after}, max=${MAX_FD_GROWTH}" >&2
    return 1
  fi
  wait_active_baseline "${active_baseline}"
  record_resources "${case_dir}" complete
  stop_envoy_and_assert_lifecycle "${case_dir}"
  echo "PASS rolling-soak: ${SOAK_CYCLES} bounded cutovers under continuous traffic; RSS was sampled, not treated as retirement proof"
}

{
  echo "profile=${PROFILE}"
  echo "case=${CASE}"
  echo "concurrency=${CONCURRENCY}"
  echo "memory_threshold_bytes=${MEMORY_THRESHOLD_BYTES}"
  echo "memory_target_bytes=${memory_target_bytes}"
  echo "explicit_marker_bytes=${marker_bytes}"
  echo "declared_vm_limit_bytes=${DECLARED_VM_LIMIT_BYTES}"
  echo "declared_vm_overhead_bytes=${DECLARED_VM_OVERHEAD_BYTES}"
  echo "declared_peak_bytes=${declared_peak_bytes}"
  echo "minimum_headroom_bytes=${MIN_HEADROOM_BYTES}"
  echo "vm_limit_provenance=${VM_LIMIT_PROVENANCE:-smoke-budget-not-runtime-limit}"
  echo "hold_seconds=${HOLD_SECONDS}"
  echo "cutover_deadline_seconds=${CUTOVER_DEADLINE_SECONDS}"
  echo "guard_observe_seconds=${GUARD_OBSERVE_SECONDS}"
  echo "minimum_guard_observe_ms=${MIN_GUARD_OBSERVE_MS}"
  echo "hold_request_multiplier=${HOLD_REQUEST_MULTIPLIER}"
  echo "soak_cycles=${SOAK_CYCLES}"
  echo "traffic_fail_after=${TRAFFIC_FAIL_AFTER}"
} >"${WORKDIR}/resolved-parameters.txt"

case "${CASE}" in
  explicit-idle) run_rolling_case explicit idle ;;
  memory-active) run_rolling_case memory active ;;
  control) run_control ;;
  soak) run_soak ;;
  all)
    run_rolling_case explicit idle
    run_rolling_case memory active
    run_control
    run_soak
    ;;
esac

trap - EXIT
cleanup_all
echo "all requested timer-reclaim checks passed; evidence=${WORKDIR}"
