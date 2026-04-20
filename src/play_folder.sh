#!/usr/bin/env bash
#
# play_folder.sh — stream a folder of audio files through PiFmRds, forever.
#
# Usage:
#   sudo ./play_folder.sh /path/to/music
#
# Environment variables (all optional):
#   FREQ         carrier frequency in MHz          (default: 107.9)
#   PI_CODE      RDS PI code, 4 hex digits         (default: FFFF)
#   PS_DEFAULT   RDS station name, <= 8 chars      (default: PiFmRds)
#   SHUFFLE      1 = shuffle the playlist          (default: 0)
#   SAMPLE_RATE  forced sample rate (Hz)           (default: 44100)
#   CHANNELS     1 = mono, 2 = stereo              (default: 2)
#   BIN          path to the pi_fm_rds binary      (default: ./pi_fm_rds)
#
# Examples:
#   sudo ./play_folder.sh ~/Music
#   sudo FREQ=98.5 SHUFFLE=1 ./play_folder.sh ~/Music
#
# Dependencies: ffmpeg, ffprobe, and a built pi_fm_rds binary.
# Must be run as root (needed by pi_fm_rds for DMA/mailbox access).

set -euo pipefail

# --- defaults ----------------------------------------------------------------
FOLDER="${1:-}"
FREQ="${FREQ:-107.9}"
PI_CODE="${PI_CODE:-FFFF}"
PS_DEFAULT="${PS_DEFAULT:-PiFmRds}"
SHUFFLE="${SHUFFLE:-0}"
SAMPLE_RATE="${SAMPLE_RATE:-44100}"
CHANNELS="${CHANNELS:-2}"
BIN="${BIN:-./pi_fm_rds}"

# --- sanity checks -----------------------------------------------------------
if [[ -z "$FOLDER" ]]; then
  echo "usage: sudo $0 <folder-with-audio-files>" >&2
  exit 64
fi
if [[ ! -d "$FOLDER" ]]; then
  echo "error: folder '$FOLDER' does not exist." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "error: '$BIN' not found or not executable." >&2
  echo "       cd into PiFmRds/src and run 'make' first, or set BIN=..." >&2
  exit 1
fi
if [[ $EUID -ne 0 ]]; then
  echo "error: must be run as root (DMA/mailbox access). Use sudo." >&2
  exit 1
fi
for cmd in ffmpeg ffprobe find; do
  command -v "$cmd" >/dev/null || {
    echo "error: '$cmd' is not installed (apt install ffmpeg)." >&2
    exit 1
  }
done

# --- build playlist ----------------------------------------------------------
mapfile -d '' -t PLAYLIST < <(
  find "$FOLDER" -type f \( \
        -iname '*.mp3'  -o -iname '*.flac' -o -iname '*.m4a' \
     -o -iname '*.ogg'  -o -iname '*.opus' -o -iname '*.wav' \
     -o -iname '*.aac'  -o -iname '*.wma' \
  \) -print0 | sort -z
)
if (( ${#PLAYLIST[@]} == 0 )); then
  echo "error: no audio files found under '$FOLDER'." >&2
  exit 1
fi
echo "found ${#PLAYLIST[@]} track(s) in $FOLDER"
echo "transmitting on ${FREQ} MHz (Ctrl-C to stop)"

# --- RDS control FIFO --------------------------------------------------------
CTL_FIFO="$(mktemp -u /tmp/pifm_ctl.XXXXXX)"
mkfifo -m 600 "$CTL_FIFO"

PIFM_PID=""
cleanup() {
  local rc=$?
  echo
  echo "stopping..."
  if [[ -n "$PIFM_PID" ]] && kill -0 "$PIFM_PID" 2>/dev/null; then
    kill -TERM "$PIFM_PID" 2>/dev/null || true
    wait "$PIFM_PID" 2>/dev/null || true
  fi
  # Close the read+write fd and remove the FIFO.
  exec 3<&- 2>/dev/null || true
  exec 3>&- 2>/dev/null || true
  rm -f "$CTL_FIFO"
  exit "$rc"
}
trap cleanup EXIT INT TERM

# Keep the FIFO open so pi_fm_rds never sees EOF on --ctl.
# NB: open read+write (`<>`), not write-only (`>`). A write-only open of
# a FIFO blocks until a reader shows up, which would deadlock the script
# because the reader (pi_fm_rds) is started further down.
exec 3<>"$CTL_FIFO"

# --- helpers -----------------------------------------------------------------
# Sanitize a string for RDS: strip CR/LF, collapse whitespace, trim.
sanitize() {
  local s="${1-}"
  s="${s//$'\r'/ }"
  s="${s//$'\n'/ }"
  s="$(printf '%s' "$s" | awk '{$1=$1; print}')"
  printf '%s' "$s"
}

# Extract title/artist with ffprobe, fall back to filename, then push to FIFO.
push_rds_for() {
  local f="$1" title artist ps rt
  title="$(ffprobe -v error -show_entries format_tags=title  -of default=nw=1:nk=1 "$f" 2>/dev/null || true)"
  artist="$(ffprobe -v error -show_entries format_tags=artist -of default=nw=1:nk=1 "$f" 2>/dev/null || true)"
  title="$(sanitize "$title")"
  artist="$(sanitize "$artist")"
  if [[ -z "$title" ]]; then
    title="$(basename "$f")"
    title="${title%.*}"
  fi

  if [[ -n "$artist" ]]; then
    rt="${artist} - ${title}"
  else
    rt="$title"
  fi
  ps="${title:0:8}"
  rt="${rt:0:64}"

  printf 'PS %s\n' "$ps" >&3 || true
  printf 'RT %s\n' "$rt" >&3 || true
}

# --- start pi_fm_rds, fed by the decoder loop via a pipe ---------------------
(
  while :; do
    if (( SHUFFLE )); then
      mapfile -t ORDER < <(printf '%s\n' "${!PLAYLIST[@]}" | shuf)
    else
      ORDER=("${!PLAYLIST[@]}")
    fi
    for i in "${ORDER[@]}"; do
      f="${PLAYLIST[$i]}"
      echo ">> now playing: $f" >&2
      push_rds_for "$f"
      # Decode this track into a raw WAV stream on stdout. Force a fixed
      # sample rate + channel count so the stream concatenates cleanly and
      # pi_fm_rds doesn't change pitch between tracks.
      ffmpeg -hide_banner -loglevel error -nostdin \
             -i "$f" -f wav -ac "$CHANNELS" -ar "$SAMPLE_RATE" - \
        || echo "   (skipping unreadable file)" >&2
    done
  done
) | "$BIN" \
      --freq "$FREQ" \
      --pi   "$PI_CODE" \
      --ps   "$PS_DEFAULT" \
      --ctl  "$CTL_FIFO" \
      --audio - &

PIFM_PID=$!
wait "$PIFM_PID"
