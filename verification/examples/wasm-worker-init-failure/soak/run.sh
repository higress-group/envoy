#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ECDS_DIR="${PARENT_DIR}/ecds-replacement"
ENVOY_BIN="${ENVOY_BIN:-}"
WASM_BINARY="${WASM_BINARY:-}"
WORKDIR="${WORKDIR:-/tmp/wasm-worker-init-soak}"
PROFILE="${PROFILE:-smoke}"
SCENARIO="${SCENARIO:-all}"
CONCURRENCY="${CONCURRENCY:-1}"
LISTENER_PORT="${LISTENER_PORT:-10007}"
ADMIN_PORT="${ADMIN_PORT:-9910}"
UPSTREAM_PORT="${UPSTREAM_PORT:-3087}"
BASE_ID="${BASE_ID:-127}"
EVENT_INTERVAL_MS="${EVENT_INTERVAL_MS:-1100}"
READY_GUARD_WAIT_MS="${READY_GUARD_WAIT_MS:-1050}"
REQUESTS_PER_EVENT="${REQUESTS_PER_EVENT:-}"
READY_SCAN_LIMIT="${READY_SCAN_LIMIT:-200}"
MAX_REPLAY_LAG_MS="${MAX_REPLAY_LAG_MS:-5000}"
READY_SCHEDULE_VALIDATE_ONLY_DIR="${READY_SCHEDULE_VALIDATE_ONLY_DIR:-}"
INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR="${INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR:-}"
SAMPLE_INTERVAL_SECONDS="${SAMPLE_INTERVAL_SECONDS:-10}"
WINDOW_SECONDS="${WINDOW_SECONDS:-}"
WARMUP_SECONDS="${WARMUP_SECONDS:-}"
DURATION_SECONDS="${DURATION_SECONDS:-}"
REPETITIONS="${REPETITIONS:-}"
MIN_INITIALIZATION_RECOVERIES="${MIN_INITIALIZATION_RECOVERIES:-}"
MIN_READY_RECOVERIES="${MIN_READY_RECOVERIES:-}"
MIN_PERSISTENT_ATTEMPTS="${MIN_PERSISTENT_ATTEMPTS:-}"
WORKDIR_MARKER=.envoy-verification-workdir
ENVOY_PID=""
UPSTREAM_PID=""
LAST_LATENCY_MS=0
EVENT_SEQUENCE=0
MEASURED_EVENTS=0
EVENT_START_OFFSET_MS=0
EXPECTED_ACTIVE_STEADY=0
EXPECTED_UNINITIALIZED_STEADY=0
READY_REPLAY_SCAN_COUNT=0
READY_SCHEDULE_ROWS=()
READY_ROUND_SCAN_COUNTS=()
declare -A READY_REQUEST_OFFSETS
INITIALIZATION_REPLAY_SCAN_COUNT=0
INITIALIZATION_SCHEDULE_ROWS=()
INITIALIZATION_ROUND_SCAN_COUNTS=()
declare -A INITIALIZATION_REQUEST_OFFSETS

if [[ -z "${ENVOY_BIN}" || "${ENVOY_BIN}" != /* || ! -x "${ENVOY_BIN}" ]]; then
  echo "ENVOY_BIN must explicitly name an absolute executable Envoy binary" >&2
  exit 1
fi
case "${PROFILE}" in
  smoke)
    WARMUP_SECONDS="${WARMUP_SECONDS:-10}"
    DURATION_SECONDS="${DURATION_SECONDS:-60}"
    WINDOW_SECONDS="${WINDOW_SECONDS:-30}"
    REPETITIONS="${REPETITIONS:-1}"
    MIN_INITIALIZATION_RECOVERIES="${MIN_INITIALIZATION_RECOVERIES:-6}"
    MIN_READY_RECOVERIES="${MIN_READY_RECOVERIES:-6}"
    MIN_PERSISTENT_ATTEMPTS="${MIN_PERSISTENT_ATTEMPTS:-6}"
    ;;
  formal)
    WARMUP_SECONDS="${WARMUP_SECONDS:-300}"
    DURATION_SECONDS="${DURATION_SECONDS:-900}"
    WINDOW_SECONDS="${WINDOW_SECONDS:-300}"
    REPETITIONS="${REPETITIONS:-1}"
    MIN_INITIALIZATION_RECOVERIES="${MIN_INITIALIZATION_RECOVERIES:-1000}"
    MIN_READY_RECOVERIES="${MIN_READY_RECOVERIES:-600}"
    MIN_PERSISTENT_ATTEMPTS="${MIN_PERSISTENT_ATTEMPTS:-600}"
    ;;
  *) echo "PROFILE must be smoke or formal" >&2; exit 2 ;;
esac
if [[ "${PROFILE}" == formal && "${CONCURRENCY}" != 4 ]]; then
  echo "formal profile requires CONCURRENCY=4" >&2
  exit 2
fi
if [[ -z "${REQUESTS_PER_EVENT}" ]]; then
  REQUESTS_PER_EVENT=$((2 * CONCURRENCY))
fi
case "${SCENARIO}" in all|initialization|ready|persistent) ;; *) echo "invalid SCENARIO" >&2; exit 2;; esac
for pair in "CONCURRENCY:${CONCURRENCY}" "EVENT_INTERVAL_MS:${EVENT_INTERVAL_MS}" \
  "READY_GUARD_WAIT_MS:${READY_GUARD_WAIT_MS}" \
  "SAMPLE_INTERVAL_SECONDS:${SAMPLE_INTERVAL_SECONDS}" "WINDOW_SECONDS:${WINDOW_SECONDS}" \
  "WARMUP_SECONDS:${WARMUP_SECONDS}" "DURATION_SECONDS:${DURATION_SECONDS}" \
  "REPETITIONS:${REPETITIONS}"; do
  name="${pair%%:*}"; value="${pair#*:}"
  [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${name} must be positive" >&2; exit 2; }
done
(( READY_GUARD_WAIT_MS >= 1000 )) || {
  echo "READY_GUARD_WAIT_MS must preserve the 1-second recovery guard" >&2
  exit 2
}
for pair in "READY_SCAN_LIMIT:${READY_SCAN_LIMIT}" "MAX_REPLAY_LAG_MS:${MAX_REPLAY_LAG_MS}"; do
  name="${pair%%:*}"; value="${pair#*:}"
  [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${name} must be positive" >&2; exit 2; }
done
[[ "${REQUESTS_PER_EVENT}" =~ ^[1-9][0-9]*$ ]] || {
  echo "REQUESTS_PER_EVENT must be positive" >&2; exit 2;
}
(( REQUESTS_PER_EVENT >= 2 * CONCURRENCY )) || {
  echo "REQUESTS_PER_EVENT must be at least 2*CONCURRENCY" >&2; exit 2;
}
(( DURATION_SECONDS >= 2 * WINDOW_SECONDS )) || {
  echo "DURATION_SECONDS must cover at least two analysis windows" >&2; exit 2;
}
(( DURATION_SECONDS % WINDOW_SECONDS == 0 )) || {
  echo "DURATION_SECONDS must be an integer multiple of WINDOW_SECONDS" >&2; exit 2;
}
(( SAMPLE_INTERVAL_SECONDS < WINDOW_SECONDS )) || {
  echo "SAMPLE_INTERVAL_SECONDS must be smaller than WINDOW_SECONDS" >&2; exit 2;
}

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

if [[ -n "${WASM_BINARY}" ]]; then
  [[ "${WASM_BINARY}" == /* && -f "${WASM_BINARY}" ]] || { echo "invalid WASM_BINARY" >&2; exit 1; }
else
  WASM_BINARY="${WORKDIR}/worker-init-repro.wasm"
  (cd "${PARENT_DIR}/wasm" && GOWORK=off GOOS=wasip1 GOARCH=wasm \
    go build -buildmode=c-shared -o "${WASM_BINARY}" .)
fi

cat_manifest="${WORKDIR}/campaign.txt"
{
  echo "declared_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "profile=${PROFILE}"
  echo "scenario=${SCENARIO}"
  echo "envoy_bin=${ENVOY_BIN}"
  echo "envoy_sha256=$(sha256sum "${ENVOY_BIN}" | awk '{print $1}')"
  echo "wasm_binary=${WASM_BINARY}"
  echo "wasm_sha256=$(sha256sum "${WASM_BINARY}" | awk '{print $1}')"
  echo "concurrency=${CONCURRENCY}"
  echo "warmup_seconds=${WARMUP_SECONDS}"
  echo "duration_seconds=${DURATION_SECONDS}"
  echo "sample_interval_seconds=${SAMPLE_INTERVAL_SECONDS}"
  echo "window_seconds=${WINDOW_SECONDS}"
  echo "event_interval_ms=${EVENT_INTERVAL_MS}"
  echo "ready_guard_wait_ms=${READY_GUARD_WAIT_MS}"
  echo "requests_per_event=${REQUESTS_PER_EVENT}"
  echo "ready_scan_limit=${READY_SCAN_LIMIT}"
  echo "max_replay_lag_ms=${MAX_REPLAY_LAG_MS}"
  echo "repetitions=${REPETITIONS}"
  echo "minimum_initialization_recoveries=${MIN_INITIALIZATION_RECOVERIES}"
  echo "minimum_ready_recoveries=${MIN_READY_RECOVERIES}"
  echo "minimum_persistent_attempts=${MIN_PERSISTENT_ATTEMPTS}"
  echo "absolute_fd_growth=8"
  echo "absolute_thread_growth=0"
  echo "absolute_allocated_growth_bytes=33554432"
  echo "absolute_heap_growth_bytes=33554432"
  echo "absolute_rss_pss_dirty_growth_kb=65536"
  echo "trend_allocated_growth_bytes_per_100_events=1048576"
  echo "trend_heap_growth_bytes_per_100_events=1048576"
  echo "trend_rss_pss_dirty_growth_kb_per_100_events=4096"
  echo "paired_extra_fd_growth=4"
  echo "paired_extra_thread_growth=0"
  echo "paired_extra_allocated_growth_bytes=16777216"
  echo "paired_extra_heap_growth_bytes=16777216"
  echo "paired_extra_rss_pss_dirty_growth_kb=32768"
  echo "paired_extra_allocated_growth_bytes_per_100_events=524288"
  echo "paired_extra_heap_growth_bytes_per_100_events=524288"
  echo "paired_extra_rss_pss_dirty_growth_kb_per_100_events=2048"
  echo "growth_basis=median_of_up_to_two_edge_windows"
  echo "growth_edge_count=max(1,min(2,floor(window_count/2)));formal_three_windows=first_vs_last"
  echo "trend_basis=least_squares_over_all_fixed_window_medians_per_100_events"
} >"${cat_manifest}"

cleanup_case() {
  if [[ -n "${ENVOY_PID}" ]]; then kill "${ENVOY_PID}" >/dev/null 2>&1 || true; wait "${ENVOY_PID}" >/dev/null 2>&1 || true; ENVOY_PID=""; fi
  if [[ -n "${UPSTREAM_PID}" ]]; then kill "${UPSTREAM_PID}" >/dev/null 2>&1 || true; wait "${UPSTREAM_PID}" >/dev/null 2>&1 || true; UPSTREAM_PID=""; fi
}
trap cleanup_case EXIT

prefix=wasm.envoy.wasm.runtime.v8.plugin.worker_init_repro
retryable_stat="${prefix}.worker_init_retryable_failure_total"
retry_stat="${prefix}.worker_init_retry_total"
recovered_stat="${prefix}.worker_init_recovered_total"
crash_stat="${prefix}.crash_total"
recover_stat="${prefix}.recover_total"
recover_error_stat="${prefix}.recover_error"
uninitialized_stat="${prefix}.worker_uninitialized"
active_stat=wasm.envoy.wasm.runtime.v8.active

stat_value() {
  local stat="$1" missing="${2:-0}" filter="${1//./\\.}"
  curl --fail --silent --show-error --max-time 2 --get \
    --data-urlencode "filter=^${filter}$" "http://127.0.0.1:${ADMIN_PORT}/stats" |
    awk -F': ' -v stat="${stat}" -v missing="${missing}" \
      '$1 == stat {print $2; found=1} END {if (!found) print missing}'
}
wait_until_ms() {
  local target_ms="$1" now_ms remaining_ms wait_seconds
  now_ms="$(date +%s%3N)"
  if (( now_ms < target_ms )); then
    remaining_ms=$((target_ms - now_ms))
    printf -v wait_seconds '%d.%03d' "$((remaining_ms / 1000))" "$((remaining_ms % 1000))"
    sleep "${wait_seconds}"
  fi
}
wait_stat_at_least() {
  local stat="$1" expected="$2" value=0
  for _ in $(seq 1 200); do
    value="$(stat_value "${stat}")"
    (( value >= expected )) && return 0
    sleep 0.05
  done
  echo "timed out waiting for ${stat} >= ${expected}; last=${value}" >&2
  return 1
}
wait_stat_eq() {
  local stat="$1" expected="$2" value=0
  for _ in $(seq 1 200); do
    value="$(stat_value "${stat}")"
    [[ "${value}" == "${expected}" ]] && return 0
    sleep 0.05
  done
  echo "timed out waiting for ${stat}=${expected}; last=${value}" >&2
  return 1
}
wait_active_at_most() {
  local expected="$1" value=0
  for _ in $(seq 1 200); do
    value="$(stat_value "${active_stat}")"
    (( value <= expected )) && return 0
    sleep 0.05
  done
  echo "active VM count ${value} exceeds baseline ${expected}" >&2
  return 1
}
request() {
  local run_dir="$1" label="$2" trap_header="${3:-false}"
  local start_ms end_ms
  start_ms="$(date +%s%3N)"
  if [[ "${trap_header}" == true ]]; then
    curl --silent --show-error --max-time 5 -H 'connection: close' \
      -H 'x-worker-init-request-trap: true' -D "${run_dir}/last.headers" \
      -o "${run_dir}/last.body" "http://127.0.0.1:${LISTENER_PORT}/${label}" || true
  else
    curl --silent --show-error --max-time 5 -H 'connection: close' \
      -D "${run_dir}/last.headers" -o "${run_dir}/last.body" \
      "http://127.0.0.1:${LISTENER_PORT}/${label}" || true
  fi
  end_ms="$(date +%s%3N)"
  LAST_LATENCY_MS=$((end_ms - start_ms))
}
install_generation() {
  local run_dir="$1" mode="$2" generation="$3" run_id="$4"
  local next="${run_dir}/ecds.yaml.next"
  sed -e 's#__FAIL_OPEN__#true#g' -e "s#__WASM_BINARY__#${WASM_BINARY}#g" \
    -e "s#__MODE__#${mode}#g" -e "s#__RUN_ID__#${run_id}#g" \
    -e "s#__WORKER_COUNT__#${CONCURRENCY}#g" -e "s#__GENERATION__#${generation}#g" \
    "${ECDS_DIR}/ecds.yaml.template" >"${next}"
  mv "${next}" "${run_dir}/ecds.yaml"
}
wait_log_generation() {
  local run_dir="$1" generation="$2"
  for _ in $(seq 1 200); do
    grep -Fq "generation=${generation} " "${run_dir}/envoy.log" 2>/dev/null && return 0
    sleep 0.05
  done
  echo "timed out waiting for ECDS generation ${generation}" >&2
  return 1
}
require_tls_roles() {
  local run_dir="$1" generation="$2"
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
      ' "${run_dir}/envoy.log")
    if [[ "${total}" == "${expected_total}" && "${main_records}" == 2 &&
      "${worker_tids}" == "${CONCURRENCY}" ]]; then
      printf '%s,%s,%s,%s,%s\n' "${generation}" "${ENVOY_PID}" "${total}" \
        "${main_records}" "${worker_tids}" >>"${run_dir}/tls-role-evidence.csv"
      return 0
    fi
    sleep 0.25
  done
  echo "TLS role mismatch for ${generation}: total=${total}, main=${main_records}, " \
    "worker_tids=${worker_tids}" >&2
  return 1
}
load_ready_schedule() {
  local schedule="$1"
  local expected_header='round,measured,start_offset_ms,normal_requests,round_duration_ms,recovery_latency_ms'
  [[ -f "${schedule}" ]] || { echo "missing Ready schedule: ${schedule}" >&2; return 1; }
  local header
  IFS= read -r header <"${schedule}"
  [[ "${header}" == "${expected_header}" ]] || {
    echo "invalid Ready schedule header in ${schedule}" >&2; return 1;
  }
  mapfile -t READY_SCHEDULE_ROWS < <(tail -n +2 "${schedule}")
  ((${#READY_SCHEDULE_ROWS[@]} > 0)) || { echo "Ready schedule has no rounds" >&2; return 1; }
  READY_ROUND_SCAN_COUNTS=()

  local expected_round=1 previous_offset=-1 saw_warmup=false saw_measured=false
  local round measured offset requests duration latency extra expected_phase
  local warmup_ms=$((WARMUP_SECONDS * 1000))
  local total_ms=$(((WARMUP_SECONDS + DURATION_SECONDS) * 1000))
  for row in "${READY_SCHEDULE_ROWS[@]}"; do
    IFS=, read -r round measured offset requests duration latency extra <<<"${row}"
    [[ -z "${extra:-}" ]] || { echo "Ready schedule row has extra fields: ${row}" >&2; return 1; }
    for pair in "round:${round}" "offset:${offset}" "requests:${requests}" \
      "duration:${duration}" "latency:${latency}"; do
      [[ "${pair#*:}" =~ ^[0-9]+$ ]] || {
        echo "Ready schedule has non-integer ${pair%%:*}: ${row}" >&2; return 1;
      }
    done
    [[ "${round}" == "${expected_round}" ]] || {
      echo "Ready schedule round gap: expected ${expected_round}, got ${round}" >&2; return 1;
    }
    (( offset > previous_offset && offset < total_ms )) || {
      echo "Ready schedule offset is missing, unordered, or out of range: ${row}" >&2; return 1;
    }
    (( requests >= 1 && requests <= READY_SCAN_LIMIT )) || {
      echo "Ready schedule scan count is out of range: ${row}" >&2; return 1;
    }
    (( duration >= 1 && latency >= 1000 )) || {
      echo "Ready schedule duration/latency is invalid: ${row}" >&2; return 1;
    }
    expected_phase=false
    (( offset >= warmup_ms )) && expected_phase=true
    [[ "${measured}" == "${expected_phase}" ]] || {
      echo "Ready schedule phase is misaligned with warmup boundary: ${row}" >&2; return 1;
    }
    [[ "${measured}" == true ]] && saw_measured=true || saw_warmup=true
    READY_ROUND_SCAN_COUNTS[round]="${requests}"
    expected_round=$((expected_round + 1))
    previous_offset="${offset}"
  done
  [[ "${saw_warmup}" == true && "${saw_measured}" == true ]] || {
    echo "Ready schedule must cover both warmup and measured phases" >&2; return 1;
  }
}

load_ready_request_schedule() {
  local schedule="$1"
  local expected_header='round,request,start_offset_ms,guard_elapsed_ms,recovered'
  [[ -f "${schedule}" ]] || {
    echo "missing Ready request schedule: ${schedule}" >&2; return 1;
  }
  local header
  IFS= read -r header <"${schedule}"
  [[ "${header}" == "${expected_header}" ]] || {
    echo "invalid Ready request schedule header in ${schedule}" >&2; return 1;
  }
  local rows
  mapfile -t rows < <(tail -n +2 "${schedule}")
  ((${#rows[@]} > 0)) || { echo "Ready request schedule has no rows" >&2; return 1; }
  READY_REQUEST_OFFSETS=()
  local expected_round=1 expected_request=1 previous_offset=0
  local round request_index offset guard_elapsed recovered extra expected_count
  for row in "${rows[@]}"; do
    IFS=, read -r round request_index offset guard_elapsed recovered extra <<<"${row}"
    [[ -z "${extra:-}" && "${round}" =~ ^[0-9]+$ && "${request_index}" =~ ^[0-9]+$ &&
      "${offset}" =~ ^[0-9]+$ && "${guard_elapsed}" =~ ^[0-9]+$ ]] || {
      echo "malformed Ready request schedule row: ${row}" >&2; return 1;
    }
    (( round == expected_round && request_index == expected_request )) || {
      echo "Ready request schedule gap or misordering: ${row}" >&2; return 1;
    }
    expected_count="${READY_ROUND_SCAN_COUNTS[round]:-0}"
    (( expected_count >= 1 && request_index <= expected_count &&
      guard_elapsed >= READY_GUARD_WAIT_MS && offset > previous_offset )) || {
      echo "Ready request schedule offset/count mismatch: ${row}" >&2; return 1;
    }
    if (( request_index == expected_count )); then
      [[ "${recovered}" == true ]] || {
        echo "Ready request schedule final scan did not recover: ${row}" >&2; return 1;
      }
      expected_round=$((expected_round + 1))
      expected_request=1
      previous_offset=0
    else
      [[ "${recovered}" == false ]] || {
        echo "Ready request schedule recovered before final scan: ${row}" >&2; return 1;
      }
      expected_request=$((expected_request + 1))
      previous_offset="${offset}"
    fi
    READY_REQUEST_OFFSETS["${round}:${request_index}"]="${offset}"
  done
  (( expected_round == ${#READY_SCHEDULE_ROWS[@]} + 1 && expected_request == 1 )) || {
    echo "Ready request schedule ended before all rounds were covered" >&2; return 1;
  }
}

load_initialization_schedule() {
  local schedule="$1"
  local expected_header='round,measured,start_offset_ms,normal_requests,round_duration_ms,recovery_latency_ms'
  [[ -f "${schedule}" ]] || {
    echo "missing initialization schedule: ${schedule}" >&2; return 1;
  }
  local header
  IFS= read -r header <"${schedule}"
  [[ "${header}" == "${expected_header}" ]] || {
    echo "invalid initialization schedule header in ${schedule}" >&2; return 1;
  }
  mapfile -t INITIALIZATION_SCHEDULE_ROWS < <(tail -n +2 "${schedule}")
  ((${#INITIALIZATION_SCHEDULE_ROWS[@]} > 0)) || {
    echo "initialization schedule has no rounds" >&2; return 1;
  }
  INITIALIZATION_ROUND_SCAN_COUNTS=()

  local expected_round=1 previous_offset=-1 saw_warmup=false saw_measured=false
  local round measured offset requests duration latency extra expected_phase
  local warmup_ms=$((WARMUP_SECONDS * 1000))
  local total_ms=$(((WARMUP_SECONDS + DURATION_SECONDS) * 1000))
  for row in "${INITIALIZATION_SCHEDULE_ROWS[@]}"; do
    IFS=, read -r round measured offset requests duration latency extra <<<"${row}"
    [[ -z "${extra:-}" ]] || {
      echo "initialization schedule row has extra fields: ${row}" >&2; return 1;
    }
    for pair in "round:${round}" "offset:${offset}" "requests:${requests}" \
      "duration:${duration}" "latency:${latency}"; do
      [[ "${pair#*:}" =~ ^[0-9]+$ ]] || {
        echo "initialization schedule has non-integer ${pair%%:*}: ${row}" >&2; return 1;
      }
    done
    [[ "${round}" == "${expected_round}" ]] || {
      echo "initialization schedule round gap: expected ${expected_round}, got ${round}" >&2
      return 1
    }
    (( offset > previous_offset && offset < total_ms )) || {
      echo "initialization schedule offset is missing, unordered, or out of range: ${row}" >&2
      return 1
    }
    (( requests >= CONCURRENCY && requests <= READY_SCAN_LIMIT )) || {
      echo "initialization schedule scan count is out of range: ${row}" >&2; return 1;
    }
    (( duration >= 1 && latency >= 1000 )) || {
      echo "initialization schedule duration/latency is invalid: ${row}" >&2; return 1;
    }
    expected_phase=false
    (( offset >= warmup_ms )) && expected_phase=true
    [[ "${measured}" == "${expected_phase}" ]] || {
      echo "initialization schedule phase is misaligned with warmup boundary: ${row}" >&2
      return 1
    }
    [[ "${measured}" == true ]] && saw_measured=true || saw_warmup=true
    INITIALIZATION_ROUND_SCAN_COUNTS[round]="${requests}"
    expected_round=$((expected_round + 1))
    previous_offset="${offset}"
  done
  [[ "${saw_warmup}" == true && "${saw_measured}" == true ]] || {
    echo "initialization schedule must cover both warmup and measured phases" >&2; return 1;
  }
}

load_initialization_request_schedule() {
  local schedule="$1"
  local expected_header='round,request,start_offset_ms,recovered_delta'
  [[ -f "${schedule}" ]] || {
    echo "missing initialization request schedule: ${schedule}" >&2; return 1;
  }
  local header
  IFS= read -r header <"${schedule}"
  [[ "${header}" == "${expected_header}" ]] || {
    echo "invalid initialization request schedule header in ${schedule}" >&2; return 1;
  }
  local rows
  mapfile -t rows < <(tail -n +2 "${schedule}")
  ((${#rows[@]} > 0)) || {
    echo "initialization request schedule has no rows" >&2; return 1;
  }
  INITIALIZATION_REQUEST_OFFSETS=()
  local expected_round=1 expected_request=1 previous_offset=0 recovered_sum=0
  local round request_index offset recovered_delta extra expected_count
  for row in "${rows[@]}"; do
    IFS=, read -r round request_index offset recovered_delta extra <<<"${row}"
    [[ -z "${extra:-}" && "${round}" =~ ^[0-9]+$ && "${request_index}" =~ ^[0-9]+$ &&
      "${offset}" =~ ^[0-9]+$ && "${recovered_delta}" =~ ^[0-9]+$ ]] || {
      echo "malformed initialization request schedule row: ${row}" >&2; return 1;
    }
    (( round == expected_round && request_index == expected_request )) || {
      echo "initialization request schedule gap or misordering: ${row}" >&2; return 1;
    }
    expected_count="${INITIALIZATION_ROUND_SCAN_COUNTS[round]:-0}"
    (( expected_count >= CONCURRENCY && request_index <= expected_count && offset >= 1000 &&
      offset > previous_offset && recovered_delta <= CONCURRENCY )) || {
      echo "initialization request schedule offset/count mismatch: ${row}" >&2; return 1;
    }
    recovered_sum=$((recovered_sum + recovered_delta))
    (( recovered_sum <= CONCURRENCY )) || {
      echo "initialization request schedule recovered too many workers: ${row}" >&2; return 1;
    }
    INITIALIZATION_REQUEST_OFFSETS["${round}:${request_index}"]="${offset}"
    if (( request_index == expected_count )); then
      (( recovered_sum == CONCURRENCY )) || {
        echo "initialization request schedule final scan did not recover every worker: ${row}" >&2
        return 1
      }
      expected_round=$((expected_round + 1))
      expected_request=1
      previous_offset=0
      recovered_sum=0
    else
      (( recovered_sum < CONCURRENCY )) || {
        echo "initialization request schedule recovered before final scan: ${row}" >&2; return 1;
      }
      expected_request=$((expected_request + 1))
      previous_offset="${offset}"
    fi
  done
  (( expected_round == ${#INITIALIZATION_SCHEDULE_ROWS[@]} + 1 && expected_request == 1 )) || {
    echo "initialization request schedule ended before all rounds were covered" >&2; return 1;
  }
}

sample() {
  local run_dir="$1" elapsed="$2"
  local status="/proc/${ENVOY_PID}/status" rollup="/proc/${ENVOY_PID}/smaps_rollup"
  local rss rss_anon threads pss dirty fd cpu heap
  rss="$(awk '/VmRSS:/ {print $2}' "${status}")"
  rss_anon="$(awk '/RssAnon:/ {print $2}' "${status}")"
  threads="$(awk '/Threads:/ {print $2}' "${status}")"
  pss="$(awk '/^Pss:/ {print $2}' "${rollup}")"
  dirty="$(awk '/^Private_Dirty:/ {print $2}' "${rollup}")"
  fd="$(find "/proc/${ENVOY_PID}/fd" -maxdepth 1 -type l | wc -l)"
  cpu="$(awk '{print $14 + $15}' "/proc/${ENVOY_PID}/stat")"
  heap="$(stat_value server.memory_heap_size -1)"
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "${elapsed}" "${MEASURED_EVENTS}" "$(stat_value "${retryable_stat}")" \
    "$(stat_value "${retry_stat}")" "$(stat_value "${recovered_stat}")" \
    "$(stat_value "${crash_stat}")" "$(stat_value "${recover_stat}")" \
    "$(stat_value "${uninitialized_stat}")" "$(stat_value "${active_stat}")" \
    "$(stat_value server.memory_allocated)" "${heap}" "${rss}" "${rss_anon}" "${pss}" "${dirty}" \
    "${fd}" "${threads}" "${cpu}" "${LAST_LATENCY_MS}" >>"${run_dir}/samples.csv"
}

do_event() {
  local run_dir="$1" scenario="$2" control="$3" measured="$4"
  EVENT_SEQUENCE=$((EVENT_SEQUENCE + 1))
  local generation="${scenario}-${control}-${EVENT_SEQUENCE}"
  local before after response_before retry_delta request_index recovery_delta
  local round_start_ms recovery_end_ms round_end_ms recovery_latency_ms scan_requests
  local request_start_offset_ms request_target_ms request_now_ms request_lag_ms recovery_changed
  local ready_guard_anchor_ms ready_guard_elapsed_ms
  local request_key
  case "${scenario}:${control}" in
    initialization:false)
      round_start_ms="$(date +%s%3N)"
      before="$(stat_value "${retryable_stat}")"
      install_generation "${run_dir}" trap-once "${generation}" "${generation}"
      wait_stat_at_least "${retryable_stat}" "$((before + CONCURRENCY))"
      [[ "$(stat_value "${retryable_stat}")" == "$((before + CONCURRENCY))" ]] || {
        echo "initialization round ${EVENT_SEQUENCE} did not fail exactly ${CONCURRENCY} workers" >&2
        return 1
      }
      require_tls_roles "${run_dir}" "${generation}"
      sleep 1.05
      before="$(stat_value "${recovered_stat}")"
      scan_requests=0
      for request_index in $(seq 1 "${READY_SCAN_LIMIT}"); do
        scan_requests="${request_index}"
        request_start_offset_ms=$(($(date +%s%3N) - round_start_ms))
        response_before="$(stat_value "${recovered_stat}")"
        request "${run_dir}" "${generation}-${request_index}"
        after="$(stat_value "${recovered_stat}")"
        recovery_delta=$((after - response_before))
        (( recovery_delta >= 0 && recovery_delta <= CONCURRENCY &&
          after <= before + CONCURRENCY )) || {
          echo "initialization round ${EVENT_SEQUENCE} recovery counter changed unexpectedly" >&2
          return 1
        }
        if (( recovery_delta > 0 )); then
          grep -qi '^x-plugin-marker: ready' "${run_dir}/last.headers"
          grep -qi "^x-worker-init-generation: ${generation}" "${run_dir}/last.headers"
        fi
        printf '%s,%s,%s,%s\n' "${EVENT_SEQUENCE}" "${request_index}" \
          "${request_start_offset_ms}" "${recovery_delta}" \
          >>"${run_dir}/initialization-requests.csv"
        (( after == before + CONCURRENCY )) && break
      done
      after="$(stat_value "${recovered_stat}")"
      [[ "${after}" == "$((before + CONCURRENCY))" ]] || {
        echo "initialization round ${EVENT_SEQUENCE} exhausted ${READY_SCAN_LIMIT} requests " \
          "before recovering every worker" >&2
        return 1
      }
      recovery_end_ms="$(date +%s%3N)"
      wait_stat_eq "${uninitialized_stat}" 0
      wait_stat_eq "${active_stat}" "${EXPECTED_ACTIVE_STEADY}"
      round_end_ms="$(date +%s%3N)"
      recovery_latency_ms=$((recovery_end_ms - round_start_ms))
      printf '%s,%s,%s,%s,%s,%s\n' "${EVENT_SEQUENCE}" "${measured}" \
        "${EVENT_START_OFFSET_MS}" "${scan_requests}" "$((round_end_ms - round_start_ms))" \
        "${recovery_latency_ms}" >>"${run_dir}/initialization-schedule.csv"
      if [[ "${measured}" == true ]]; then
        MEASURED_EVENTS=$((MEASURED_EVENTS + after - before))
      fi
      ;;
    initialization:true)
      (( INITIALIZATION_REPLAY_SCAN_COUNT >= CONCURRENCY &&
        INITIALIZATION_REPLAY_SCAN_COUNT <= READY_SCAN_LIMIT ))
      round_start_ms="$(date +%s%3N)"
      install_generation "${run_dir}" healthy "${generation}" "${generation}"
      wait_log_generation "${run_dir}" "${generation}"
      require_tls_roles "${run_dir}" "${generation}"
      for request_index in $(seq 1 "${INITIALIZATION_REPLAY_SCAN_COUNT}"); do
        request_key="${EVENT_SEQUENCE}:${request_index}"
        request_start_offset_ms="${INITIALIZATION_REQUEST_OFFSETS[$request_key]:-}"
        [[ "${request_start_offset_ms}" =~ ^[0-9]+$ ]] || {
          echo "missing initialization request replay offset for " \
            "${EVENT_SEQUENCE}:${request_index}" >&2
          return 1
        }
        request_target_ms=$((round_start_ms + request_start_offset_ms))
        request_now_ms="$(date +%s%3N)"
        while (( request_now_ms < request_target_ms )); do
          sleep 0.01
          request_now_ms="$(date +%s%3N)"
        done
        request_lag_ms=$((request_now_ms - request_target_ms))
        (( request_lag_ms <= MAX_REPLAY_LAG_MS )) || {
          echo "initialization request replay ${EVENT_SEQUENCE}:${request_index} lag " \
            "${request_lag_ms}ms exceeds ${MAX_REPLAY_LAG_MS}ms" >&2
          return 1
        }
        printf '%s,%s,%s,%s,%s\n' "${EVENT_SEQUENCE}" "${request_index}" \
          "${request_start_offset_ms}" "$((request_now_ms - round_start_ms))" \
          "${request_lag_ms}" >>"${run_dir}/initialization-request-replay.csv"
        request "${run_dir}" "${generation}-${request_index}"
        grep -qi '^x-plugin-marker: ready' "${run_dir}/last.headers"
        grep -qi "^x-worker-init-generation: ${generation}" "${run_dir}/last.headers"
      done
      wait_stat_eq "${uninitialized_stat}" 0
      wait_stat_eq "${active_stat}" "${EXPECTED_ACTIVE_STEADY}"
      if [[ "${measured}" == true ]]; then
        MEASURED_EVENTS=$((MEASURED_EVENTS + CONCURRENCY))
      fi
      ;;
    ready:false)
      round_start_ms="$(date +%s%3N)"
      before="$(stat_value "${crash_stat}")"
      request "${run_dir}" "ready-trap-${EVENT_SEQUENCE}" true
      ready_guard_anchor_ms="$(date +%s%3N)"
      wait_stat_at_least "${crash_stat}" "$((before + 1))"
      [[ "$(stat_value "${crash_stat}")" == "$((before + 1))" ]]
      # Crash observation is part of the guard wait, not additional serial delay. Anchoring after
      # the trap response ensures every recovery request remains at least one second after crash.
      wait_until_ms "$((ready_guard_anchor_ms + READY_GUARD_WAIT_MS))"
      before="$(stat_value "${recover_stat}")"
      response_before="${before}"
      scan_requests=0
      for request_index in $(seq 1 "${READY_SCAN_LIMIT}"); do
        scan_requests="${request_index}"
        request_now_ms="$(date +%s%3N)"
        request_start_offset_ms=$((request_now_ms - round_start_ms))
        ready_guard_elapsed_ms=$((request_now_ms - ready_guard_anchor_ms))
        (( ready_guard_elapsed_ms >= READY_GUARD_WAIT_MS ))
        request "${run_dir}" "ready-recover-${EVENT_SEQUENCE}-${request_index}"
        after="$(stat_value "${recover_stat}")"
        recovery_changed=false
        if (( after > response_before )); then
          recovery_changed=true
          grep -qi '^x-plugin-marker: ready' "${run_dir}/last.headers"
          grep -qi '^x-worker-init-generation: initial' "${run_dir}/last.headers"
        fi
        printf '%s,%s,%s,%s,%s\n' "${EVENT_SEQUENCE}" "${request_index}" \
          "${request_start_offset_ms}" "${ready_guard_elapsed_ms}" "${recovery_changed}" \
          >>"${run_dir}/ready-requests.csv"
        [[ "${recovery_changed}" == true ]] && break
        # The request path is the only recovery trigger. Reuse its post-request value as the next
        # baseline instead of adding another admin round trip before the following request.
        response_before="${after}"
      done
      after="$(stat_value "${recover_stat}")"
      [[ "${after}" == "$((before + 1))" ]]
      recovery_end_ms="$(date +%s%3N)"
      [[ "$(stat_value "${recover_error_stat}")" == 0 ]]
      [[ "$(stat_value "${retry_stat}")" == "${INIT_RETRY_BASELINE}" ]]
      [[ "$(stat_value "${recovered_stat}")" == "${INIT_RECOVERED_BASELINE}" ]]
      wait_stat_eq "${uninitialized_stat}" 0
      wait_active_at_most "${ACTIVE_BASELINE}"
      round_end_ms="$(date +%s%3N)"
      recovery_latency_ms=$((recovery_end_ms - round_start_ms))
      printf '%s,%s,%s,%s,%s,%s\n' "${EVENT_SEQUENCE}" "${measured}" \
        "${EVENT_START_OFFSET_MS}" "${scan_requests}" "$((round_end_ms - round_start_ms))" \
        "${recovery_latency_ms}" >>"${run_dir}/ready-schedule.csv"
      if [[ "${measured}" == true ]]; then MEASURED_EVENTS=$((MEASURED_EVENTS + 1)); fi
      ;;
    ready:true)
      (( READY_REPLAY_SCAN_COUNT >= 1 && READY_REPLAY_SCAN_COUNT <= READY_SCAN_LIMIT ))
      round_start_ms="$(date +%s%3N)"
      request "${run_dir}" "ready-control-a-${EVENT_SEQUENCE}"
      ready_guard_anchor_ms="$(date +%s%3N)"
      wait_until_ms "$((ready_guard_anchor_ms + READY_GUARD_WAIT_MS))"
      for request_index in $(seq 1 "${READY_REPLAY_SCAN_COUNT}"); do
        request_key="${EVENT_SEQUENCE}:${request_index}"
        request_start_offset_ms="${READY_REQUEST_OFFSETS[$request_key]:-}"
        [[ "${request_start_offset_ms}" =~ ^[0-9]+$ ]] || {
          echo "missing Ready request replay offset for ${EVENT_SEQUENCE}:${request_index}" >&2
          return 1
        }
        request_target_ms=$((round_start_ms + request_start_offset_ms))
        request_now_ms="$(date +%s%3N)"
        while (( request_now_ms < request_target_ms )); do
          sleep 0.01
          request_now_ms="$(date +%s%3N)"
        done
        request_lag_ms=$((request_now_ms - request_target_ms))
        (( request_lag_ms <= MAX_REPLAY_LAG_MS )) || {
          echo "Ready request replay ${EVENT_SEQUENCE}:${request_index} lag ${request_lag_ms}ms" >&2
          return 1
        }
        printf '%s,%s,%s,%s,%s\n' "${EVENT_SEQUENCE}" "${request_index}" \
          "${request_start_offset_ms}" "$((request_now_ms - round_start_ms))" \
          "${request_lag_ms}" >>"${run_dir}/ready-request-replay.csv"
        request "${run_dir}" "ready-control-b-${EVENT_SEQUENCE}-${request_index}"
        grep -qi '^x-plugin-marker: ready' "${run_dir}/last.headers"
        grep -qi '^x-worker-init-generation: initial' "${run_dir}/last.headers"
      done
      wait_stat_eq "${uninitialized_stat}" 0
      wait_active_at_most "${ACTIVE_BASELINE}"
      if [[ "${measured}" == true ]]; then MEASURED_EVENTS=$((MEASURED_EVENTS + 1)); fi
      ;;
    persistent:false)
      before="$(stat_value "${retry_stat}")"
      for request_index in $(seq 1 "${REQUESTS_PER_EVENT}"); do
        request "${run_dir}" "persistent-${EVENT_SEQUENCE}-${request_index}"
      done
      after="$(stat_value "${retry_stat}")"
      retry_delta=$((after - before))
      (( retry_delta >= 0 && retry_delta <= CONCURRENCY ))
      printf '%s,%s,%s,%s,%s\n' "$(date +%s%3N)" "${EVENT_SEQUENCE}" "${before}" \
        "${after}" "${retry_delta}" >>"${run_dir}/attempt-rate.csv"
      wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
      wait_active_at_most "${ACTIVE_BASELINE}"
      if [[ "${measured}" == true ]]; then
        MEASURED_EVENTS=$((MEASURED_EVENTS + retry_delta))
      fi
      ;;
    persistent:true)
      for request_index in $(seq 1 "${REQUESTS_PER_EVENT}"); do
        request "${run_dir}" "persistent-control-${EVENT_SEQUENCE}-${request_index}"
      done
      wait_stat_eq "${uninitialized_stat}" 0
      wait_active_at_most "${ACTIVE_BASELINE}"
      if [[ "${measured}" == true ]]; then
        MEASURED_EVENTS=$((MEASURED_EVENTS + CONCURRENCY))
      fi
      ;;
  esac
}

run_one() {
  local scenario="$1" control="$2" repetition="$3"
  local label="${scenario}-rep${repetition}"
  [[ "${control}" == true ]] && label="${label}-healthy-control"
  local run_dir="${WORKDIR}/${label}"
  mkdir -p "${run_dir}"
  if [[ "${scenario}" == ready || "${scenario}" == initialization ]]; then
    if [[ "${control}" == true ]]; then
      if [[ "${scenario}" == ready ]]; then
        load_ready_schedule "${WORKDIR}/ready-rep${repetition}/ready-schedule.csv"
        load_ready_request_schedule "${WORKDIR}/ready-rep${repetition}/ready-requests.csv"
      else
        load_initialization_schedule \
          "${WORKDIR}/initialization-rep${repetition}/initialization-schedule.csv"
        load_initialization_request_schedule \
          "${WORKDIR}/initialization-rep${repetition}/initialization-requests.csv"
      fi
      printf '%s\n' \
        'round,measured,scheduled_offset_ms,actual_offset_ms,replay_lag_ms,normal_requests' \
        >"${run_dir}/${scenario}-replay.csv"
      printf '%s\n' 'round,request,scheduled_offset_ms,actual_offset_ms,replay_lag_ms' \
        >"${run_dir}/${scenario}-request-replay.csv"
    else
      printf '%s\n' \
        'round,measured,start_offset_ms,normal_requests,round_duration_ms,recovery_latency_ms' \
        >"${run_dir}/${scenario}-schedule.csv"
      if [[ "${scenario}" == ready ]]; then
        printf '%s\n' 'round,request,start_offset_ms,guard_elapsed_ms,recovered' \
          >"${run_dir}/ready-requests.csv"
      else
        printf '%s\n' 'round,request,start_offset_ms,recovered_delta' \
          >"${run_dir}/initialization-requests.csv"
      fi
    fi
  fi
  EVENT_SEQUENCE=0; MEASURED_EVENTS=0; LAST_LATENCY_MS=0
  local initial_mode=healthy
  [[ "${scenario}" == persistent && "${control}" == false ]] && initial_mode=trap-always
  install_generation "${run_dir}" "${initial_mode}" initial "${label}-initial"
  sed -e "s#__ADMIN_PORT__#${ADMIN_PORT}#g" -e "s#__UPSTREAM_PORT__#${UPSTREAM_PORT}#g" \
    -e "s#__WORKDIR__#${run_dir}#g" "${ECDS_DIR}/config.yaml.template" >"${run_dir}/config.yaml"
  sed -e "s#__LISTENER_PORT__#${LISTENER_PORT}#g" -e "s#__WORKDIR__#${run_dir}#g" \
    "${ECDS_DIR}/lds.yaml.template" >"${run_dir}/lds.yaml"
  "${ENVOY_BIN}" --mode validate -c "${run_dir}/config.yaml" --log-level error \
    >"${run_dir}/validate.log" 2>&1
  UPSTREAM_PORT="${UPSTREAM_PORT}" python3 "${PARENT_DIR}/upstream_server.py" \
    >"${run_dir}/upstream.log" 2>&1 &
  UPSTREAM_PID=$!
  for _ in $(seq 1 80); do
    curl --fail --silent --max-time 1 "http://127.0.0.1:${UPSTREAM_PORT}/" >/dev/null 2>&1 && break
    sleep 0.25
  done
  "${ENVOY_BIN}" -c "${run_dir}/config.yaml" --concurrency "${CONCURRENCY}" \
    --base-id "${BASE_ID}" --file-flush-interval-msec 100 \
    --log-level warn --component-log-level wasm:debug \
    --log-path "${run_dir}/envoy.log" >"${run_dir}/console.log" 2>&1 &
  ENVOY_PID=$!
  for _ in $(seq 1 120); do
    if kill -0 "${ENVOY_PID}" >/dev/null 2>&1 &&
      curl --fail --silent --max-time 1 "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null 2>&1; then break; fi
    sleep 0.25
  done
  curl --fail --silent --show-error --max-time 2 "http://127.0.0.1:${ADMIN_PORT}/ready" >/dev/null

  printf '%s\n' 'elapsed_seconds,events,retryable_failures,init_retries,init_recovered,crashes,ready_recovered,uninitialized,active,server_memory_allocated,server_memory_heap_size,rss_kb,rss_anon_kb,pss_kb,private_dirty_kb,fd_count,thread_count,cpu_ticks,last_event_latency_ms' \
    >"${run_dir}/samples.csv"
  printf '%s\n' 'timestamp_ms,round,retry_before,retry_after,actual_attempts' \
    >"${run_dir}/attempt-rate.csv"
  printf '%s\n' 'generation,main_thread_tid,plugin_start_records,main_thread_records,worker_tids' \
    >"${run_dir}/tls-role-evidence.csv"
  if [[ "${scenario}" == persistent && "${control}" == false ]]; then
    wait_stat_eq "${uninitialized_stat}" "${CONCURRENCY}"
  else
    wait_stat_eq "${uninitialized_stat}" 0
  fi
  require_tls_roles "${run_dir}" initial
  request "${run_dir}" initial-baseline
  ACTIVE_BASELINE="$(stat_value "${active_stat}")"
  (( ACTIVE_BASELINE >= 1 ))
  EXPECTED_ACTIVE_STEADY="${ACTIVE_BASELINE}"
  EXPECTED_UNINITIALIZED_STEADY=0
  if [[ "${scenario}" == initialization && "${control}" == false ]]; then
    (( ACTIVE_BASELINE >= 2 ))
    EXPECTED_ACTIVE_STEADY=$((ACTIVE_BASELINE - 1))
  elif [[ "${scenario}" == persistent && "${control}" == false ]]; then
    EXPECTED_UNINITIALIZED_STEADY="${CONCURRENCY}"
  fi
  INIT_RETRY_BASELINE="$(stat_value "${retry_stat}")"
  INIT_RECOVERED_BASELINE="$(stat_value "${recovered_stat}")"
  local start_ms measure_start_ms end_ms next_event_ms next_sample_ms now_ms measured elapsed
  local row round scheduled_offset scan_requests scheduled_duration scheduled_latency extra
  local target_ms actual_offset replay_lag
  start_ms="$(date +%s%3N)"
  measure_start_ms=$((start_ms + WARMUP_SECONDS * 1000))
  end_ms=$((measure_start_ms + DURATION_SECONDS * 1000))
  next_event_ms="${start_ms}"
  next_sample_ms="${measure_start_ms}"
  if [[ ( "${scenario}" == ready || "${scenario}" == initialization ) &&
    "${control}" == true ]]; then
    local schedule_rows_name="${scenario^^}_SCHEDULE_ROWS"
    local -n scheduled_rows="${schedule_rows_name}"
    for row in "${scheduled_rows[@]}"; do
      IFS=, read -r round measured scheduled_offset scan_requests scheduled_duration \
        scheduled_latency extra <<<"${row}"
      [[ "${round}" == "$((EVENT_SEQUENCE + 1))" && -z "${extra:-}" ]]
      target_ms=$((start_ms + scheduled_offset))
      now_ms="$(date +%s%3N)"
      while (( now_ms < target_ms )); do
        if (( now_ms >= next_sample_ms && next_sample_ms < end_ms )); then
          elapsed=$(((now_ms - measure_start_ms) / 1000))
          (( elapsed >= 0 && elapsed < DURATION_SECONDS )) && sample "${run_dir}" "${elapsed}"
          next_sample_ms=$((next_sample_ms + SAMPLE_INTERVAL_SECONDS * 1000))
        fi
        sleep 0.05
        now_ms="$(date +%s%3N)"
      done
      actual_offset=$((now_ms - start_ms))
      replay_lag=$((actual_offset - scheduled_offset))
      (( replay_lag >= 0 && replay_lag <= MAX_REPLAY_LAG_MS )) || {
        echo "${scenario} replay round ${round} lag ${replay_lag}ms exceeds " \
          "${MAX_REPLAY_LAG_MS}ms" >&2
        return 1
      }
      EVENT_START_OFFSET_MS="${scheduled_offset}"
      if [[ "${scenario}" == ready ]]; then
        READY_REPLAY_SCAN_COUNT="${scan_requests}"
      else
        INITIALIZATION_REPLAY_SCAN_COUNT="${scan_requests}"
      fi
      do_event "${run_dir}" "${scenario}" "${control}" "${measured}"
      printf '%s,%s,%s,%s,%s,%s\n' "${round}" "${measured}" "${scheduled_offset}" \
        "${actual_offset}" "${replay_lag}" "${scan_requests}" \
        >>"${run_dir}/${scenario}-replay.csv"
      now_ms="$(date +%s%3N)"
      if (( now_ms >= next_sample_ms && next_sample_ms < end_ms )); then
        elapsed=$(((now_ms - measure_start_ms) / 1000))
        (( elapsed >= 0 && elapsed < DURATION_SECONDS )) && sample "${run_dir}" "${elapsed}"
        next_sample_ms=$((next_sample_ms + SAMPLE_INTERVAL_SECONDS * 1000))
      fi
    done
    [[ "${EVENT_SEQUENCE}" == "${#scheduled_rows[@]}" ]]
    now_ms="$(date +%s%3N)"
    while (( now_ms < end_ms )); do
      if (( now_ms >= next_sample_ms )); then
        elapsed=$(((now_ms - measure_start_ms) / 1000))
        (( elapsed < DURATION_SECONDS )) && sample "${run_dir}" "${elapsed}"
        next_sample_ms=$((next_sample_ms + SAMPLE_INTERVAL_SECONDS * 1000))
      fi
      sleep 0.05
      now_ms="$(date +%s%3N)"
    done
  else
    while true; do
      now_ms="$(date +%s%3N)"
      (( now_ms >= end_ms )) && break
      measured=false
      (( now_ms >= measure_start_ms )) && measured=true
      if (( now_ms >= next_event_ms )); then
        EVENT_START_OFFSET_MS=$((now_ms - start_ms))
        do_event "${run_dir}" "${scenario}" "${control}" "${measured}"
        now_ms="$(date +%s%3N)"
        next_event_ms=$((next_event_ms + EVENT_INTERVAL_MS))
        if (( next_event_ms < now_ms )); then next_event_ms="${now_ms}"; fi
      fi
      if (( now_ms >= next_sample_ms )); then
        elapsed=$(((now_ms - measure_start_ms) / 1000))
        (( elapsed < DURATION_SECONDS )) && sample "${run_dir}" "${elapsed}"
        next_sample_ms=$((next_sample_ms + SAMPLE_INTERVAL_SECONDS * 1000))
      fi
      sleep 0.05
    done
    if [[ "${scenario}" == ready ]]; then
      load_ready_schedule "${run_dir}/ready-schedule.csv"
      load_ready_request_schedule "${run_dir}/ready-requests.csv"
    elif [[ "${scenario}" == initialization ]]; then
      load_initialization_schedule "${run_dir}/initialization-schedule.csv"
      load_initialization_request_schedule "${run_dir}/initialization-requests.csv"
    fi
  fi
  # Periodic samples intentionally stop before the measured boundary. Record one terminal sample
  # for the final event count without adding it to any fixed resource window.
  sample "${run_dir}" "${DURATION_SECONDS}"
  local minimum
  case "${scenario}" in
    initialization) minimum="${MIN_INITIALIZATION_RECOVERIES}" ;;
    ready) minimum="${MIN_READY_RECOVERIES}" ;;
    persistent) minimum="${MIN_PERSISTENT_ATTEMPTS}" ;;
  esac
  python3 "${SCRIPT_DIR}/analyze.py" --input "${run_dir}/samples.csv" \
    --output "${run_dir}/windows.csv" --window-seconds "${WINDOW_SECONDS}" \
    --minimum-events "${minimum}" --duration-seconds "${DURATION_SECONDS}" \
    --sample-interval-seconds "${SAMPLE_INTERVAL_SECONDS}" 2>&1 |
    tee "${run_dir}/analysis.txt"
  awk -F, -v duration="${DURATION_SECONDS}" -v scenario="${scenario}" \
    -v expected_active="${EXPECTED_ACTIVE_STEADY}" \
    -v expected_uninitialized="${EXPECTED_UNINITIALIZED_STEADY}" '
      NR == 1 { next }
      $1 < duration {
        rows++
        if ($8 != expected_uninitialized || $9 != expected_active) {
          printf "%s steady-state contract failed: %s\n", scenario, $0 > "/dev/stderr"
          failed = 1
        }
      }
      END { if (rows == 0 || failed) exit 1 }
    ' "${run_dir}/samples.csv"
  {
    echo "scenario=${scenario}"
    echo "healthy_control=${control}"
    echo "repetition=${repetition}"
    echo "minimum_events=${minimum}"
    echo "observed_events=${MEASURED_EVENTS}"
    echo "run_active_baseline=${ACTIVE_BASELINE}"
    echo "expected_active_steady=${EXPECTED_ACTIVE_STEADY}"
    echo "expected_worker_uninitialized_steady=${EXPECTED_UNINITIALIZED_STEADY}"
    echo "config_sha256=$(sha256sum "${run_dir}/config.yaml" | awk '{print $1}')"
    echo "tls_role_evidence_sha256=$(sha256sum "${run_dir}/tls-role-evidence.csv" | awk '{print $1}')"
    if [[ "${scenario}" == ready || "${scenario}" == initialization ]]; then
      if [[ "${control}" == false ]]; then
        echo "${scenario}_schedule_sha256=$(sha256sum \
          "${run_dir}/${scenario}-schedule.csv" | awk '{print $1}')"
        echo "${scenario}_requests_sha256=$(sha256sum \
          "${run_dir}/${scenario}-requests.csv" | awk '{print $1}')"
      else
        echo "source_${scenario}_schedule_sha256=$(sha256sum \
          "${WORKDIR}/${scenario}-rep${repetition}/${scenario}-schedule.csv" | awk '{print $1}')"
        echo "source_${scenario}_requests_sha256=$(sha256sum \
          "${WORKDIR}/${scenario}-rep${repetition}/${scenario}-requests.csv" | awk '{print $1}')"
        echo "${scenario}_replay_sha256=$(sha256sum \
          "${run_dir}/${scenario}-replay.csv" | awk '{print $1}')"
        echo "${scenario}_request_replay_sha256=$(sha256sum \
          "${run_dir}/${scenario}-request-replay.csv" | awk '{print $1}')"
      fi
    fi
  } >"${run_dir}/evidence.txt"
  kill "${ENVOY_PID}" >/dev/null 2>&1 || true
  wait "${ENVOY_PID}" >/dev/null 2>&1 || true
  ENVOY_PID=""
  grep -q '~Wasm 0 remaining active' "${run_dir}/console.log"
  kill "${UPSTREAM_PID}" >/dev/null 2>&1 || true
  wait "${UPSTREAM_PID}" >/dev/null 2>&1 || true
  UPSTREAM_PID=""
}

if [[ -n "${READY_SCHEDULE_VALIDATE_ONLY_DIR}" ]]; then
  READY_SCHEDULE_VALIDATE_ONLY_DIR="$(realpath -m "${READY_SCHEDULE_VALIDATE_ONLY_DIR}")"
  load_ready_schedule "${READY_SCHEDULE_VALIDATE_ONLY_DIR}/ready-schedule.csv"
  load_ready_request_schedule "${READY_SCHEDULE_VALIDATE_ONLY_DIR}/ready-requests.csv"
  echo "PASS Ready schedule validation: ${#READY_SCHEDULE_ROWS[@]} rounds"
  trap - EXIT
  exit 0
fi

if [[ -n "${INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR}" ]]; then
  INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR="$(realpath -m \
    "${INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR}")"
  load_initialization_schedule \
    "${INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR}/initialization-schedule.csv"
  load_initialization_request_schedule \
    "${INITIALIZATION_SCHEDULE_VALIDATE_ONLY_DIR}/initialization-requests.csv"
  echo "PASS initialization schedule validation: ${#INITIALIZATION_SCHEDULE_ROWS[@]} rounds"
  trap - EXIT
  exit 0
fi

scenarios=("${SCENARIO}")
[[ "${SCENARIO}" == all ]] && scenarios=(initialization ready persistent)
for scenario in "${scenarios[@]}"; do
  for repetition in $(seq 1 "${REPETITIONS}"); do
    run_one "${scenario}" false "${repetition}"
    run_one "${scenario}" true "${repetition}"
    actual_dir="${WORKDIR}/${scenario}-rep${repetition}"
    control_dir="${WORKDIR}/${scenario}-rep${repetition}-healthy-control"
    python3 "${SCRIPT_DIR}/compare.py" --actual "${actual_dir}/windows.csv" \
      --control "${control_dir}/windows.csv" --output "${actual_dir}/paired-comparison.csv" 2>&1 |
      tee "${actual_dir}/paired-analysis.txt"
  done
done
cleanup_case
trap - EXIT
echo "campaign: ${cat_manifest}"
echo PASS
