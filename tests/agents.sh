#!/bin/sh
set -eu
think=$1
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
child=
cleanup() {
    if [ -n "$child" ]; then kill "$child" 2>/dev/null || :; wait "$child" 2>/dev/null || :; fi
    rm -rf "$tmp"
}
trap cleanup EXIT
wait_event() {
    for unused in $(seq 1 200); do
        if grep -Fq "$1" "$tmp/events" 2>/dev/null; then return; fi
        kill -0 "$child" 2>/dev/null || break
        sleep 0.02
    done
    cat "$tmp/events" >&2
    exit 1
}
expect() {
    code=$1
    shift
    got=0
    "$@" > "$tmp/out" 2> "$tmp/err" || got=$?
    if [ "$got" -ne "$code" ]; then cat "$tmp/out" "$tmp/err" >&2; exit 1; fi
}
# Options are deliberately independent of repository and current working directory.
run() {
    "$think" run --root "$tmp/root" --task 'مهمة 7' --timeout-ms 6000 --poll-ms 10 "$@"
}
mkdir "$tmp/root"
run --trigger publish --bind "artifact=size:$tmp/data" --bind "writer=file:$tmp/status" \
    --progress "file:$tmp/events" > "$tmp/stdout" &
child=$!
wait_event '"event":"waiting_trigger"'
mkdir "$tmp/root/artifact-ready"
cp "$root/agent/artifact-ready/agent.ini" "$tmp/root/artifact-ready/draft"
mv "$tmp/root/artifact-ready/draft" "$tmp/root/artifact-ready/agent.ini"
wait_event '"agent":"build-started","event":"waiting_manifest"'
cp -R "$root/agent/build-started" "$tmp/build-started"
# Publish the directory in one rename so its name and identity agree.
mv "$tmp/build-started" "$tmp/root/build-started"
wait_event '"agent":"build-started","event":"waiting_input"'
: > "$tmp/data"
wait_event '"agent":"build-started","event":"waiting_input","value":"0"'
printf '\000' > "$tmp/data"
wait_event '"agent":"build-started","event":"ready","value":"1"'
printf 'ready' > "$tmp/status"
# An unfinished line cannot mark the task ready. Change the active predicate while waiting.
sed 's/value = ready/value = sealed/' "$root/agent/artifact-ready/agent.ini" > "$tmp/new"
mv "$tmp/new" "$tmp/root/artifact-ready/agent.ini"
printf '\n' >> "$tmp/status"
wait_event '"agent":"artifact-ready","event":"waiting_input","value":"ready"'
printf 'sealed\r\n' > "$tmp/status.next"
mv "$tmp/status.next" "$tmp/status"
wait "$child"
child=
grep -Fq '"task":"مهمة 7","event":"completed"' "$tmp/events"
test ! -s "$tmp/stdout"
cp "$tmp/root/artifact-ready/agent.ini" "$tmp/before"
# Change the source to a pipe and the sink to stderr with identical manifests.
printf 'sealed\n' | run --agent artifact-ready --bind "artifact=size:$tmp/data" \
    --bind writer=stdin --progress stderr > "$tmp/stdout" 2> "$tmp/events"
cmp "$tmp/before" "$tmp/root/artifact-ready/agent.ini"
grep -Fq '"event":"completed"' "$tmp/events"
test ! -s "$tmp/stdout"

printf 'failed\n' > "$tmp/status"
expect 1 run --agent artifact-ready --bind "artifact=size:$tmp/data" --bind "writer=file:$tmp/status"
grep -Fq '"agent":"artifact-ready","event":"rejected","value":"failed"' "$tmp/out"
grep -Fq '"event":"failed"' "$tmp/out"

printf 'sealed' > "$tmp/status"
expect 124 "$think" run --root "$tmp/root" --agent artifact-ready --task partial \
    --bind "artifact=size:$tmp/data" --bind "writer=file:$tmp/status" --poll-ms 10000 --timeout-ms 100
grep -Fq '"event":"timed_out"' "$tmp/out"
# Hold both ends open without writing. A blocking read would miss the deadline.
mkfifo "$tmp/pipe"
exec 3<>"$tmp/pipe"
expect 124 timeout 3 "$think" run --root "$tmp/root" --agent artifact-ready --task idle \
    --bind "artifact=size:$tmp/data" --bind writer=stdin --timeout-ms 150 --poll-ms 10000 <&3
grep -Fq '"event":"timed_out"' "$tmp/out"
exec 3>&-

expect 2 run --agent ../escape
expect 2 run --agent artifact-ready --poll-ms 1
expect 2 "$think" run --root "$tmp/root" --agent a --task t --poll-ms -1 --timeout-ms 1
expect 2 "$think" run --root "$tmp/root" --agent a --task t --poll-ms 1 --timeout-ms 4294967295
expect 1 run --agent artifact-ready --bind "writer=file:$tmp/status"
grep -Fq 'missing artifact' "$tmp/err"
printf 'requires = artifact-ready\n' >> "$tmp/root/build-started/agent.ini"
expect 1 run --agent artifact-ready
grep -Fq 'dependency cycle' "$tmp/err"
printf 'value = 9\n' >> "$tmp/root/build-started/agent.ini"
expect 1 run --agent build-started
grep -Fq 'duplicate manifest field' "$tmp/err"

cp "$root/agent/build-started/agent.ini" "$tmp/root/build-started/agent.ini"
printf '18446744073709551616\n' > "$tmp/status"
expect 1 run --agent build-started --bind "artifact=file:$tmp/status"
grep -Fq 'expected unsigned decimal' "$tmp/err"
printf '1\n2\n' > "$tmp/status"
expect 1 run --agent build-started --bind "artifact=file:$tmp/status"
grep -Fq 'expected one status record' "$tmp/err"
expect 1 run --agent build-started --bind "artifact=file:$tmp/root"
expect 2 run --agent build-started --bind "artifact=size:$tmp/data" --progress "file:$tmp/data"
test "$(wc -c < "$tmp/data")" -eq 1
expect 1 run --agent build-started --bind "artifact=size:$tmp/data" --progress "file:$tmp/root"
if "$think" run --root "$tmp/root" --agent build-started --task write-error --poll-ms 1 \
    --timeout-ms 200 --bind "artifact=size:$tmp/data" > /dev/full 2>/dev/null; then exit 1; fi
printf '%s\n' 'Agent connector and live manifest checks passed'
