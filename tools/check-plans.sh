#!/bin/bash
# checks every plan in docs/plans: dated name, matching Date line, a checklist, a Context without quoted conversation, open boxes that wait for a person ending in the marker (the user) or (the user to decide), a row in the index with current counts
# usage: check-plans.sh [--fix] [plans-dir]     --fix rewrites the Done and Open columns of README.md
# env: ALLOW (space-separated file names exempt from the name rule)
FIX=0; [ "$1" = "--fix" ] && { FIX=1; shift; }
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-$(dirname "$HERE")/docs/plans}
ALLOW=${ALLOW-}
FAIL=0
bad(){ echo "FAIL $1: $2"; FAIL=1; }
[ -f "$DIR/README.md" ] || bad README.md "the index is missing"
for f in "$DIR"/*; do
  n=$(basename "$f")
  [ "$n" = "README.md" ] && continue
  case " $ALLOW " in *" $n "*) continue;; esac
  if ! echo "$n" | grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{4}-[a-z0-9]+(-[a-z0-9]+)*\.(md|html)$'; then
    bad "$n" "name is not YYYY-MM-DD-HHMM-<slug>.md"; continue
  fi
  want="Date: ${n:0:10} ${n:11:2}:${n:13:2}"
  head -5 "$f" | grep -q "^$want " || bad "$n" "no line '$want <tz>' under the title"
  grep -qE '^[[:space:]]*- \[[ x]\]' "$f" || bad "$n" "no checkbox list"
  awk '/^## Context/{c=1; next} /^## /{c=0} c && /^>/' "$f" | grep -q . \
    && bad "$n" "Context quotes the conversation: state the goal instead (writing-context skill)"
  while IFS=$'\t' read -r loc kind _; do
    [ "$kind" = stray ] && bad "$loc" "a box that waits for a person or a decision ends with (the user) or (the user to decide), nothing else"
  done < <("$HERE/plan-items.sh" "$f")
  d=$(grep -cE '^[[:space:]]*- \[x\]' "$f"); o=$(grep -cE '^[[:space:]]*- \[ \]' "$f")
  row=$(grep -F "]($n)" "$DIR/README.md" 2>/dev/null | head -1)
  if [ -z "$row" ]; then bad "$n" "not in README.md"; continue; fi
  if [ $FIX = 1 ]; then
    N="$n" D="$d" O="$o" perl -0pi -e 's/^(\| \[\Q$ENV{N}\E\]\(\Q$ENV{N}\E\) \|[^|\n]*\|)[^|\n]*\|[^|\n]*\|$/$1 $ENV{D} | $ENV{O} |/m' "$DIR/README.md"
  else
    echo "$row" | grep -qE "\| $d \| $o \|[[:space:]]*$" || bad "$n" "README.md counts are stale (now $d done, $o open): run check-plans.sh --fix"
  fi
done
[ $FAIL = 0 ] && echo "OK $(ls "$DIR" | grep -cE '^[0-9]{4}-') plans"
exit $FAIL
