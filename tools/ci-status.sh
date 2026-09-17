#!/usr/bin/env bash
# Answers one question about one commit: did CI actually pass on it?
#
#   tools/ci-status.sh              # HEAD
#   tools/ci-status.sh v5.2.0       # any tag, branch or sha
#   tools/ci-status.sh HEAD~2
#
# WHY THIS EXISTS
#
# `gh run watch --exit-status` returned exit code 0 for two separate runs that
# had FAILED, on 2026-09-16. Both were noticed by eye, and only because someone
# happened to look. A gate that reports success on a failed run is worse than
# no gate, and this is the gate between a tag and a repeat of v5.0.1: that tag
# was pushed before CI finished, the macOS job failed, `create-release` never
# ran because it needs all three, and the tag is permanently empty because a
# published tag cannot be moved.
#
# RELEASING.md used to answer the question by telling the reader to run
# `gh run view --json conclusion,jobs` and read the field. That is a control
# that works for as long as someone remembers to run it and reads it correctly.
# This is the same check, as a thing that can be run and whose exit code can be
# trusted.
#
# WHAT IT REFUSES TO CALL SUCCESS
#
# Exit 0 needs the run's `conclusion` to be exactly `success` AND every job in
# it to say the same. Anything else exits non-zero with the value printed:
# failure, cancelled, timed_out, action_required, skipped, neutral, stale, a
# conclusion that is empty or missing, a run still unfinished when the timeout
# expires, and -- the one that is easiest to get wrong -- NO RUN AT ALL for the
# commit.
#
# No evidence is not success. An unpushed commit, a workflow that did not
# trigger, a wrong workflow name and a deleted run all arrive as "no run", and
# every one of them is a reason to stop rather than to tag.
#
# The newest run for the commit is the one read, so re-running a failed job and
# waiting for it gives the answer you would expect.
#
# EXIT CODES
#
#   0  conclusion success, every job success
#   1  the run finished and did not succeed
#   2  no evidence: no run for the commit, no conclusion, or gh could not answer
#   3  still running when the timeout expired
#
# ENVIRONMENT
#
#   CI_STATUS_WORKFLOW   workflow file to look for   (default ci.yml)
#   CI_STATUS_TIMEOUT    seconds to wait for a run in progress (default 1800)
#   CI_STATUS_INTERVAL   seconds between polls       (default 15)
set -euo pipefail

ref=${1:-HEAD}
workflow=${CI_STATUS_WORKFLOW:-ci.yml}
timeout=${CI_STATUS_TIMEOUT:-1800}
interval=${CI_STATUS_INTERVAL:-15}

if ! command -v gh > /dev/null 2>&1; then
    echo "CI UNKNOWN: gh is not installed, so there is no evidence either way" >&2
    exit 2
fi

if ! sha=$(git rev-parse --verify "$ref^{commit}" 2> /dev/null); then
    echo "CI UNKNOWN: '$ref' is not a commit in this repository" >&2
    exit 2
fi

short=${sha:0:8}

# --commit takes the full sha. gh lists newest first, so .[0] is the latest
# attempt; `empty` rather than a null field, so an absent run arrives as an
# empty string instead of the four-character string "null".
if ! run_id=$(gh run list --workflow "$workflow" --commit "$sha" --limit 20 \
                  --json databaseId \
                  --jq 'if length == 0 then empty else .[0].databaseId end' 2> /dev/null); then
    echo "CI UNKNOWN: gh run list failed for $short, so there is no evidence either way" >&2
    exit 2
fi

if [[ -z "$run_id" ]]; then
    echo "CI FAIL: no $workflow run for $short ($ref)" >&2
    echo "  Nothing ran, so nothing passed. Push the commit, or check the workflow" >&2
    echo "  name with CI_STATUS_WORKFLOW=<file> if it is not $workflow." >&2
    exit 2
fi

echo "run $run_id for $short ($ref), workflow $workflow"

# Polled on `status` alone: `conclusion` is empty until the run completes, and
# reading it early is how a gate ends up treating "not finished" as "fine".
# gh failing is NOT the run failing, and the two must not share an exit code.
# Under `set -e` a bare `status=$(gh ...)` aborts the script with gh's own
# status, normally 1 -- which this script defines as "the run finished and did
# not succeed". A network blip while polling would therefore have reported a
# CI failure that never happened. Exit 2 is "gh could not answer", as the
# header promises.
query_run_jq()
{
    local field=$1 expression=$2 value

    if ! value=$(gh run view "$run_id" --json "$field" --jq "$expression" 2>&1); then
        echo "CI FAIL: gh could not read $field for run $run_id" >&2
        echo "  $value" >&2
        echo "  This is a tooling failure, not a verdict on the run." >&2
        exit 2
    fi

    printf '%s' "$value"
}

query_run()
{
    query_run_jq "$1" ".$1 // \"\""
}

waited=0
status=$(query_run status)

while [[ "$status" != "completed" ]]; do
    if (( waited >= timeout )); then
        echo "CI FAIL: run $run_id was still '$status' after ${timeout}s" >&2
        echo "  Unfinished is not passed. Raise CI_STATUS_TIMEOUT or look at the run." >&2
        exit 3
    fi

    echo "  ${status:-unknown} ... waiting (${waited}s of ${timeout}s)"
    sleep "$interval"
    waited=$(( waited + interval ))
    status=$(query_run status)
done

# The field, explicitly, rather than an exit code from something that watched
# the run. This line is the whole point of the script.
conclusion=$(query_run conclusion)

# Every job as well. A run whose conclusion says success while a job does not is
# the same shape of disagreement as the one that made this script necessary, so
# it is reported rather than averaged away. Pipe-delimited because a job name
# contains spaces and a conclusion can be empty.
failed_jobs=0
total_jobs=0

# Through query_run_jq, not a bare process substitution, which is how this read
# used to be written. A process substitution discards gh's exit status: a
# network blip produced no output, the loop never ran, total_jobs stayed 0, and
# the check below announced
#
#     CI FAIL: run <id> reports success with no jobs in it
#
# and exited 1 -- the code this script reserves for "the run finished and did
# not succeed". That is a tooling failure reported as a verdict on the run,
# which is the exact conflation the comment above query_run_jq forbids, and
# this was the one of the three gh calls left unguarded.
#
# A herestring rather than a pipe so the loop runs in THIS shell and its
# counters survive it. An empty jobs list feeds one blank line, which the
# job_name guard already skips, so total_jobs stays 0 and the real check fires.
jobs_raw=$(query_run_jq jobs '.jobs[] | "\(.conclusion // "")|\(.name)"')

while IFS='|' read -r job_conclusion job_name; do
    [[ -z "$job_name" ]] && continue

    total_jobs=$(( total_jobs + 1 ))

    echo "  job: ${job_conclusion:-<empty>}  $job_name"

    if [[ "$job_conclusion" != "success" ]]; then
        failed_jobs=$(( failed_jobs + 1 ))
    fi
done <<< "$jobs_raw"

if [[ "$conclusion" != "success" ]]; then
    echo "CI FAIL: conclusion is ${conclusion:-<empty>} (run $run_id)" >&2
    exit 1
fi

if (( total_jobs == 0 )); then
    echo "CI FAIL: run $run_id reports success with no jobs in it" >&2
    echo "  A run that did no work is not evidence that anything passed." >&2
    exit 1
fi

if (( failed_jobs > 0 )); then
    echo "CI FAIL: conclusion is success but $failed_jobs of $total_jobs jobs are not" >&2
    exit 1
fi

echo "CI OK: conclusion success, $total_jobs jobs, all success ($short)"
