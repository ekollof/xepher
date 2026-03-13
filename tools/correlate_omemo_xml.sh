#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Correlate WeeChat OMEMO log events with raw XML trace entries by timestamp.

Usage:
  tools/correlate_omemo_xml.sh --account NAME [options]

Options:
  --account NAME         Account name (required).
  --event-log PATH       Event log path.
                        Default: ~/.local/share/weechat/logs/xmpp.account.NAME.weechatlog
  --raw-log PATH         Raw XML trace path.
                        Default: ~/.local/share/weechat/xmpp/raw_xml_NAME.log
  --pattern REGEX        Event filter regex.
                        Default matches common OMEMO diagnostics.
  --window SECONDS       Max timestamp distance for correlation (default: 2).
  --max-events N         Max matching events to print (default: 80).
  --max-hits N           Max raw stanza matches per event (default: 4).
  --direction MODE       Direction filter: any|send|recv (default: any).
  --stanza-name NAME     Raw stanza name filter: any|message|iq|presence|a
                        (default: any; auto-inferred per event when omitted).
  --help                 Show this help.

Examples:
  tools/correlate_omemo_xml.sh --account andrath
  tools/correlate_omemo_xml.sh --account andrath --direction recv --window 1
  tools/correlate_omemo_xml.sh --account andrath \
    --event-log ~/.local/share/weechat/logs/xmpp.andrath.andrath@movim.eu.weechatlog
EOF
}

account=""
event_log=""
raw_log=""
pattern='OMEMO|not encrypted for this device|Decryption Error|waiting for [0-9]+ recipient device|no session for|key-transport|queued message|not ready yet|message has no key for our device|encrypted payload attached|SEND message stanza'
window=2
max_events=80
max_hits=4
direction="any"
stanza_name="any"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --account)
      account="${2:-}"
      shift 2
      ;;
    --event-log)
      event_log="${2:-}"
      shift 2
      ;;
    --raw-log)
      raw_log="${2:-}"
      shift 2
      ;;
    --pattern)
      pattern="${2:-}"
      shift 2
      ;;
    --window)
      window="${2:-}"
      shift 2
      ;;
    --max-events)
      max_events="${2:-}"
      shift 2
      ;;
    --max-hits)
      max_hits="${2:-}"
      shift 2
      ;;
    --direction)
      direction="${2:-}"
      shift 2
      ;;
    --stanza-name)
      stanza_name="${2:-}"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$account" ]]; then
  echo "--account is required" >&2
  usage >&2
  exit 2
fi

if [[ -z "$event_log" ]]; then
  event_log="$HOME/.local/share/weechat/logs/xmpp.account.${account}.weechatlog"
fi

if [[ -z "$raw_log" ]]; then
  raw_log="$HOME/.local/share/weechat/xmpp/raw_xml_${account}.log"
fi

if [[ ! -f "$event_log" ]]; then
  echo "Event log not found: $event_log" >&2
  exit 1
fi

if [[ ! -f "$raw_log" ]]; then
  echo "Raw XML log not found: $raw_log" >&2
  exit 1
fi

if [[ "$direction" != "any" && "$direction" != "send" && "$direction" != "recv" ]]; then
  echo "Invalid --direction: $direction (use any|send|recv)" >&2
  exit 2
fi

if [[ "$stanza_name" != "any" && "$stanza_name" != "message" && "$stanza_name" != "iq" && "$stanza_name" != "presence" && "$stanza_name" != "a" ]]; then
  echo "Invalid --stanza-name: $stanza_name (use any|message|iq|presence|a)" >&2
  exit 2
fi

awk \
  -v event_pattern="$pattern" \
  -v max_window="$window" \
  -v max_events="$max_events" \
  -v max_hits="$max_hits" \
  -v dir_filter="$direction" \
  -v stanza_filter="$stanza_name" \
  '
function to_epoch(ts, parts, normalized) {
  split(ts, parts, /[- :]/)
  if (length(parts) < 6)
    return 0
  normalized = sprintf("%d %d %d %d %d %d", parts[1], parts[2], parts[3], parts[4], parts[5], parts[6])
  return mktime(normalized)
}

function flush_raw_entry() {
  if (!raw_collecting)
    return
  raw_count++
  raw_ts[raw_count] = raw_current_ts
  raw_epoch[raw_count] = raw_current_epoch
  raw_dir[raw_count] = raw_current_dir
  raw_name[raw_count] = raw_current_name
  raw_xml[raw_count] = raw_current_xml
  raw_collecting = 0
  raw_current_ts = ""
  raw_current_epoch = 0
  raw_current_dir = ""
  raw_current_name = ""
  raw_current_xml = ""
}

FNR == 1 && NR != 1 {
  flush_raw_entry()
}

NR == FNR {
  if (match($0, /^\[([0-9-]+ [0-9:]+)\] (SEND|RECV) ([^[:space:]]+)/, m)) {
    flush_raw_entry()
    raw_collecting = 1
    raw_current_ts = m[1]
    raw_current_epoch = to_epoch(raw_current_ts)
    raw_current_dir = tolower(m[2])
    raw_current_name = m[3]
    next
  }

  if (raw_collecting) {
    if ($0 == "") {
      flush_raw_entry()
    } else if (raw_current_xml == "") {
      raw_current_xml = $0
    } else {
      raw_current_xml = raw_current_xml "\\n" $0
    }
  }
  next
}

{
  ts = substr($0, 1, 19)
  if (ts !~ /^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$/)
    next

  if ($0 !~ event_pattern)
    next

  event_epoch = to_epoch(ts)
  if (event_epoch <= 0)
    next

  event_dir = "any"
  if ($0 ~ /SEND message stanza|attempting encrypted send|queued message|not ready yet|encrypted payload attached/) {
    event_dir = "send"
  } else if ($0 ~ /decode|Decryption Error|message has no key for our device|sender did not encrypt for us/) {
    event_dir = "recv"
  }

  if (dir_filter != "any")
    event_dir = dir_filter

  event_stanza = "any"
  if ($0 ~ /bundle|devicelist|requesting legacy bundle|requesting legacy device list|MAM query/) {
    event_stanza = "iq"
  } else if ($0 ~ /decode|message has no key for our device|sender did not encrypt for us|SEND message stanza|attempting encrypted send|encrypted payload attached|waiting for [0-9]+ recipient device/) {
    event_stanza = "message"
  }

  if (stanza_filter != "any")
    event_stanza = stanza_filter

  event_seen++
  if (event_seen > max_events)
    next

  printf("=== EVENT %d ===\n", event_seen)
  printf("time: %s\n", ts)
  printf("line: %s\n", $0)
  printf("filters: direction=%s stanza=%s window=%ss\n", event_dir, event_stanza, max_window)

  hit_count = 0
  for (i = 1; i <= raw_count; i++) {
    if (event_dir != "any" && raw_dir[i] != event_dir)
      continue

    if (event_stanza != "any" && raw_name[i] != event_stanza)
      continue

    delta = raw_epoch[i] - event_epoch
    if (delta < 0)
      delta = -delta

    if (delta > max_window)
      continue

    hit_count++
    printf("  -> hit %d (|dt|=%ss): [%s] %s %s\n", hit_count, delta, raw_ts[i], toupper(raw_dir[i]), raw_name[i])
    printf("     xml: %s\n", raw_xml[i])
    if (hit_count >= max_hits)
      break
  }

  if (hit_count == 0)
    printf("  -> no raw stanza match in +/- %ss window\n", max_window)

  printf("\n")
}

END {
  flush_raw_entry()
  if (event_seen == 0) {
    print "No matching events found."
  }
}
' "$raw_log" "$event_log"
