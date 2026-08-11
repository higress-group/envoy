#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENVOY_BIN="${ENVOY_BIN:-}"
WASM_BINARY="${WASM_BINARY:-}"
WORKDIR="${WORKDIR:-/tmp/wasm-worker-init-failure}"
LISTENER_PORT="${LISTENER_PORT:-10004}"
ADMIN_PORT="${ADMIN_PORT:-9907}"
UPSTREAM_PORT="${UPSTREAM_PORT:-3084}"
BASE_ID="${BASE_ID:-121}"
CONCURRENCY="${CONCURRENCY:-1}"
FAIL_OPEN="${FAIL_OPEN:-true}"
IDLE_SECONDS="${IDLE_SECONDS:-10}"
GUARD_WAIT_SECONDS="${GUARD_WAIT_SECONDS:-1.10}"
RECOVERY_REQUEST_LIMIT="${RECOVERY_REQUEST_LIMIT:-200}"
PERSISTENT_ROUNDS="${PERSISTENT_ROUNDS:-7}"
CASE="${1:-all}"
WORKDIR_MARKER=.envoy-verification-workdir
ENVOY_PID=""
UPSTREAM_PID=""

case "${CASE}" in
  all|configure-reject|trap-once|trap-always|healthy) ;;
  *) echo "usage: $0 [all|configure-reject|trap-once|trap-always|healthy]" >&2; exit 2 ;;
esac
if [[ -z "${ENVOY_BIN}" || "${ENVOY_BIN}" != /* || ! -x "${ENVOY_BIN}" ]]; then
  echo "ENVOY_BIN must explicitly name an absolute executable Envoy binary" >&2
  exit 1
fi
if [[ "${FAIL_OPEN}" != true && "${FAIL_OPEN}" != false ]]; then
  echo "FAIL_OPEN must be true or false" >&2
  exit 2
fi
for pair in "CONCURRENCY:${CONCURRENCY}" "IDLE_SECONDS:${IDLE_SECONDS}" \
  "RECOVERY_REQUEST_LIMIT:${RECOVERY_REQUEST_LIMIT}" "PERSISTENT_ROUNDS:${PERSISTENT_ROUNDS}"; do
  name="${pair%%:*}"
  value="${pair#*:}"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${name} must be a positive integer" >&2
    exit 2
  fi
done
if (( PERSISTENT_ROUNDS < 6 )); then
  echo "PERSISTENT_ROUNDS must be at least 6 to prove there is no retry cap" >&2
  exit 2
fi

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
  if [[ -n "${ENVOY_PID}" ]]; then
    kill "${ENVOY_PID}" >/dev/null 2>&1 || true
    wait "${ENVOY_PID}" >/dev/null 2>&1 || true
    ENVOY_PID=""
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
  WASM_BINARY="${WORKDIR}/worker-init-repro.wasm"
  (cd "${SCRIPT_DIR}/wasm" && GOWORK=off GOOS=wasip1 GOARCH=wasm \
    go build -buildmode=c-shared -o "${WASM_BINARY}" .)
fi

UPSTREAM_PORT="${UPSTREAM_PORT}" python3 "${SCRIPT_DIR}/upstream_server.py" \
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

prefix=wasm.envoy.wasm.runtime.v8.plugin.worker_init_repro
retryable_stat="${prefix}.worker_init_retryable_failure_total"
terminal_stat="${prefix}.worker_init_terminal_failure_total"
retry_stat="${prefix}.worker_init_retry_total"
recovered_stat="${prefix}.worker_init_recovered_total"
skip_stat="${prefix}.worker_fail_open_skip_total"
uninitialized_stat="${prefix}.worker_uninitialized"

render_config() {
  local mode="$1" output="$2"
  sed -e "s#__ADMIN_PORT__#${ADMIN_PORT}#g" \
    -e "s#__LISTENER_PORT__#${LISTENER_PORT}#g" \
    -e "s#__UPSTREAM_PORT__#${UPSTREAM_PORT}#g" \
    -e "s#__FAIL_OPEN__#${FAIL_OPEN}#g" \
    -e "s#__WASM_BINARY__#${WASM_BINARY}#g" \
    -e "s#__MODE__#${mode}#g" \
    -e "s#__RUN_ID__#static-${mode}#g" \
    -e "s#__WORKER_COUNT__#${CONCURRENCY}#g" \
    -e "s#__GENERATION__#static-${mode}#g" \
    "${SCRIPT_DIR}/config.yaml.template" >"${output}"
}
stat_value() {
  local stat="$1"
  curl --fail --silent --show-error --max-time 2 \
    "http://127.0.0.1:${ADMIN_PORT}/stats" |
    awk -F': ' -v stat="${stat}" '$1 == stat {print $2; found=1} END {if (!found) print 0}'
}
wait_stat_eq() {
  local stat="$1" expected="$2" value=0
  for _ in $(seq 1 120); do
    value="$(stat_value "${stat}")"
    [[ "${value}" == "${expected}" ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for ${stat}=${expected}; last=${value}" >&2
  return 1
}
request() {
  local case_dir="$1" label="$2"
  curl --silent --show-error --max-time 5 -H 'connection: close' \
    -D "${case_dir}/${label}.headers" -o "${case_dir}/${label}.body" \
    -w '%{http_code}' "http://127.0.0.1:${LISTENER_PORT}/${label}" \
    >"${case_dir}/${label}.status" || true
}
require_recovered_response() {
  local case_dir="$1" label="$2" generation="$3"
  [[ "$(<"${case_dir}/${label}.status")" == 200 ]] &&
    grep -qi '^x-upstream-marker: reached' "${case_dir}/${label}.headers" &&
    grep -qi '^x-plugin-marker: ready' "${case_dir}/${label}.headers" &&
    grep -qi "^x-worker-init-generation: ${generation}" "${case_dir}/${label}.headers"
}
require_failure_policy_response() {
  local case_dir="$1" label="$2"
  if [[ "${FAIL_OPEN}" == true ]]; then
    [[ "$(<"${case_dir}/${label}.status")" == 200 ]] &&
      grep -qi '^x-upstream-marker: reached' "${case_dir}/${label}.headers" &&
      grep -qi '^x-plugin-marker: absent' "${case_dir}/${label}.headers"
  else
    [[ "$(<"${case_dir}/${label}.status")" == 503 ]] &&
      ! grep -qi '^x-upstream-marker:' "${case_dir}/${label}.headers"
  fi
}
require_ready_or_failure_policy_response() {
  local case_dir="$1" label="$2" generation="$3"
  if require_recovered_response "${case_dir}" "${label}" "${generation}"; then
    return 0
  fi
  require_failure_policy_response "${case_dir}" "${label}"
}
require_tls_roles() {
  local case_dir="$1" generation="$2"
  local total=0 main_records=0 worker_tids=0 expected_total=$((CONCURRENCY + 2))
  for _ in $(seq 1 120); do
    read -r total main_records worker_tids < <(
      awk -v generation="generation=${generation} " -v main_tid="${ENVOY_PID}" '
        index($0, "worker-init-repro: plugin start") && index($0, generation) {
          total++
          split($0, fields, /\]\[/)
          tid = fields[2]
          if (tid == main_tid) {
            main_records++
          } else {
            worker[tid] = 1
          }
        }
        END {
          for (tid in worker) worker_tids++
          print total + 0, main_records + 0, worker_tids + 0
        }
      ' "${case_dir}/envoy.log")
    if [[ "${total}" == "${expected_total}" && "${main_records}" == 2 &&
      "${worker_tids}" == "${CONCURRENCY}" ]]; then
      printf 'generation=%s\nmain_thread_tid=%s\nplugin_start_records=%s\nmain_thread_records=%s\nworker_tids=%s\n' \
        "${generation}" "${ENVOY_PID}" "${total}" "${main_records}" "${worker_tids}" \
        >"${case_dir}/tls-role-evidence.txt"
      return 0
    fi
    sleep 0.25
  done
  echo "TLS role mismatch for ${generation}: total=${total}, main=${main_records}, " \
    "worker_tids=${worker_tids}" >&2
  return 1
}

run_case() {
  local mode="$1"
  local case_dir="${WORKDIR}/${mode}"
  local config="${case_dir}/envoy.yaml"
  mkdir -p "${case_dir}"
  render_config "${mode}" "${config}"
  "${ENVOY_BIN}" --mode validate -c "${config}" --log-level error \
    >"${case_dir}/validate.log" 2>&1
  "${ENVOY_BIN}" -c "${config}" --concurrency "${CONCURRENCY}" --base-id "${BASE_ID}" \
    --file-flush-interval-msec 100 \
    --log-level warn --component-log-level wasm:debug --log-path "${case_dir}/envoy.log" \
    >"${case_dir}/console.log" 2>&1 &
  ENVOY_PID=$!
  local ready=false
  for _ in $(seq 1 120); do
    if kill -0 "${ENVOY_PID}" >/dev/null 2>&1 &&
      curl --fail --silent --max-time 1 "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null 2>&1; then
      ready=true
      break
    fi
    sleep 0.25
  done
  [[ "${ready}" == true ]] || { echo "Envoy did not become ready for ${mode}" >&2; return 1; }
  require_tls_roles "${case_dir}" "static-${mode}"

  case "${mode}" in
    healthy)
      request "${case_dir}" healthy
      require_recovered_response "${case_dir}" healthy static-healthy
      [[ "$(stat_value "${retryable_stat}")" == 0 ]]
      [[ "$(stat_value "${terminal_stat}")" == 0 ]]
      [[ "$(stat_value "${retry_stat}")" == 0 ]]
      [[ "$(stat_value "${recovered_stat}")" == 0 ]]
      [[ "$(stat_value "${uninitialized_stat}")" == 0 ]]
      ;;
    configure-reject)
      wait_stat_eq "${terminal_stat}" "${CONCURRENCY}"
      wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
      local terminal_retry_before
      terminal_retry_before="$(stat_value "${retry_stat}")"
      sleep "${IDLE_SECONDS}"
      for i in $(seq 1 "$((2 * CONCURRENCY + 2))"); do
        request "${case_dir}" "terminal-${i}"
        require_failure_policy_response "${case_dir}" "terminal-${i}"
      done
      [[ "$(stat_value "${retry_stat}")" == "${terminal_retry_before}" ]]
      ;;
    trap-once)
      wait_stat_eq "${retryable_stat}" "${CONCURRENCY}"
      wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
      local idle_retry recovered before after label
      idle_retry="$(stat_value "${retry_stat}")"
      sleep "${IDLE_SECONDS}"
      [[ "$(stat_value "${retry_stat}")" == "${idle_retry}" ]]
      recovered="$(stat_value "${recovered_stat}")"
      for i in $(seq 1 "${RECOVERY_REQUEST_LIMIT}"); do
        label="recover-${i}"
        before="${recovered}"
        request "${case_dir}" "${label}"
        after="$(stat_value "${recovered_stat}")"
        if (( after > before )); then
          require_recovered_response "${case_dir}" "${label}" static-trap-once
        else
          require_ready_or_failure_policy_response "${case_dir}" "${label}" static-trap-once
        fi
        recovered="${after}"
        (( recovered == CONCURRENCY )) && break
      done
      [[ "${recovered}" == "${CONCURRENCY}" ]]
      wait_stat_eq "${uninitialized_stat}" 0
      [[ "$(stat_value "${retry_stat}")" == "${CONCURRENCY}" ]]
      ;;
    trap-always)
      wait_stat_eq "${retryable_stat}" "${CONCURRENCY}"
      wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
      local persistent_before immediate_after persistent_after round
      persistent_before="$(stat_value "${retry_stat}")"
      sleep "${IDLE_SECONDS}"
      [[ "$(stat_value "${retry_stat}")" == "${persistent_before}" ]]
      for i in $(seq 1 "${RECOVERY_REQUEST_LIMIT}"); do
        request "${case_dir}" "persistent-first-${i}"
        require_failure_policy_response "${case_dir}" "persistent-first-${i}"
        immediate_after="$(stat_value "${retry_stat}")"
        (( immediate_after == persistent_before + CONCURRENCY )) && break
      done
      [[ "${immediate_after}" == "$((persistent_before + CONCURRENCY))" ]]
      for i in $(seq 1 "$((2 * CONCURRENCY))"); do
        request "${case_dir}" "persistent-guard-${i}"
        require_failure_policy_response "${case_dir}" "persistent-guard-${i}"
      done
      [[ "$(stat_value "${retry_stat}")" == "${immediate_after}" ]]
      for round in $(seq 2 "${PERSISTENT_ROUNDS}"); do
        sleep "${GUARD_WAIT_SECONDS}"
        for i in $(seq 1 "$((2 * CONCURRENCY))"); do
          request "${case_dir}" "persistent-${round}-${i}"
          require_failure_policy_response "${case_dir}" "persistent-${round}-${i}"
        done
      done
      persistent_after="$(stat_value "${retry_stat}")"
      (( persistent_after - persistent_before >= PERSISTENT_ROUNDS ))
      [[ "$(stat_value "${recovered_stat}")" == 0 ]]
      [[ "$(stat_value "${uninitialized_stat}")" == "${CONCURRENCY}" ]]
      ;;
  esac

  {
    echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "mode=${mode}"
    echo "fail_open=${FAIL_OPEN}"
    echo "concurrency=${CONCURRENCY}"
    echo "idle_seconds=${IDLE_SECONDS}"
    echo "guard_wait_seconds=${GUARD_WAIT_SECONDS}"
    echo "envoy_bin=${ENVOY_BIN}"
    echo "envoy_sha256=$(sha256sum "${ENVOY_BIN}" | awk '{print $1}')"
    echo "wasm_binary=${WASM_BINARY}"
    echo "wasm_sha256=$(sha256sum "${WASM_BINARY}" | awk '{print $1}')"
    echo "config_sha256=$(sha256sum "${config}" | awk '{print $1}')"
    echo "worker_init_retryable_failure_total=$(stat_value "${retryable_stat}")"
    echo "worker_init_terminal_failure_total=$(stat_value "${terminal_stat}")"
    echo "worker_init_retry_total=$(stat_value "${retry_stat}")"
    echo "worker_init_recovered_total=$(stat_value "${recovered_stat}")"
    echo "worker_fail_open_skip_total=$(stat_value "${skip_stat}")"
    echo "worker_uninitialized=$(stat_value "${uninitialized_stat}")"
  } | tee "${case_dir}/evidence.txt"
  cleanup_envoy
  grep 'worker-init-repro: plugin start' "${case_dir}/envoy.log" >"${case_dir}/attempts.log"
  grep -E 'wasm worker initialization (failed|retry failed|retry became terminal|recovered)|cooldown' \
    "${case_dir}/envoy.log" >"${case_dir}/host-worker-init.log" || true
  grep -q '~Wasm 0 remaining active' "${case_dir}/console.log"
}

if [[ "${CASE}" == all ]]; then
  for mode in configure-reject trap-once trap-always healthy; do run_case "${mode}"; done
else
  run_case "${CASE}"
fi
cleanup_all
trap - EXIT
echo "evidence: ${WORKDIR}"
echo PASS
