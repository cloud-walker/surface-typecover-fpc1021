#!/usr/bin/env bash
#
# Follows the newest fpc_probe/fpc_capture JSONL trace and renders it as it
# is written -- the live view over a session driven in another terminal.
#
# Usage: tools/trace-watch.sh [glob]      (default: fpc-trace-*.jsonl)
#
# Switches to a newer trace by itself when one appears, so the probe can be
# restarted as often as an experiment needs without touching this pane.

set -uo pipefail
set -m   # each pipeline gets its own process group, so it can be killed as one

pattern="${1:-fpc-trace-*.jsonl}"

render='
  def pad(n): (tostring | if length < n then . + (" " * (n - length)) else . end);
  def ms: (.t * 1000 | round / 1000 | tostring);

  if .ev then
    "\(ms | pad(11))  *  \(.ev | pad(14)) \(.msg)"
  else
    (if .dir == "in" then "<" else ">" end) as $d
    | "\(ms | pad(11))  \($d)  \(.tag | pad(14))"
      + ("\(.len)B" | pad(6))
      + (if .rc != 0 then "  \(.rc_name // "rc=\(.rc)")" else "" end)
      + (if .substatus != null then "  sub=\(.substatus)" else "" end)
      + (if .length != null then " len=\(.length)" else "" end)
      + (if .chip_id != null then " chip_id=\(.chip_id)" else "" end)
      + (if .dur > 100 then "  [\(.dur | round)ms]" else "" end)
  end'

newest() {
    # shellcheck disable=SC2086  # the glob is the point
    ls -t $pattern 2>/dev/null | head -1
}

follower=0
stop_follower() {
    [ "$follower" -ne 0 ] || return 0
    kill -- "-$follower" 2>/dev/null
    wait "$follower" 2>/dev/null
    follower=0
}
trap 'stop_follower; exit 0' INT TERM EXIT

while :; do
    file="$(newest)"
    while [ -z "$file" ]; do
        printf '\r\033[Kwaiting for a trace matching %s ...' "$pattern"
        sleep 1
        file="$(newest)"
    done
    printf '\r\033[K--- following %s\n\n' "$file"

    tail -F -n +1 -- "$file" | jq -r --unbuffered "$render" &
    follower=$!

    # Hand over as soon as a newer trace appears: the probe gets restarted a
    # lot during an experiment, and each restart auto-names a new file.
    while [ "$(newest)" = "$file" ]; do
        sleep 1
    done
    stop_follower
done
