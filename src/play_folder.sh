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

# --- FIFOs for audio stream and RDS control ---------------------------------
CTL_FIFO="$(mktemp -u /tmp/pifm_ctl.XXXXXX)"
AUDIO_FIFO="$(mktemp -u /tmp/pifm_audio.XXXXXX)"
mkfifo -m 600 "$CTL_FIFO"
mkfifo -m 600 "$AUDIO_FIFO"

PIFM_PID=""
DECODER_PID=""
SHUTTING_DOWN=0

cleanup() {
  local rc=$?
  # Guard against the trap firing twice (INT then EXIT).
  if (( SHUTTING_DOWN )); then exit "$rc"; fi
  SHUTTING_DOWN=1

  echo
  echo "stopping..."

  # Kill the decoder first so it stops feeding the audio FIFO; this in turn
  # makes the current ffmpeg child die via SIGPIPE.
  if [[ -n "$DECODER_PID" ]] && kill -0 "$DECODER_PID" 2>/dev/null; then
    kill -TERM "$DECODER_PID" 2>/dev/null || true
  fi
  # Then ask pi_fm_rds to shut down cleanly (stops the DMA / carrier).
  if [[ -n "$PIFM_PID" ]] && kill -0 "$PIFM_PID" 2>/dev/null; then
    kill -TERM "$PIFM_PID" 2>/dev/null || true
  fi

  # Give them a moment; escalate to KILL if they're still around.
  local t=0
  while (( t < 20 )); do
    local alive=0
    [[ -n "$DECODER_PID" ]] && kill -0 "$DECODER_PID" 2>/dev/null && alive=1
    [[ -n "$PIFM_PID"    ]] && kill -0 "$PIFM_PID"    2>/dev/null && alive=1
    (( alive == 0 )) && break
    sleep 0.1
    t=$((t + 1))
  done
  [[ -n "$DECODER_PID" ]] && kill -KILL "$DECODER_PID" 2>/dev/null || true
  [[ -n "$PIFM_PID"    ]] && kill -KILL "$PIFM_PID"    2>/dev/null || true
  wait 2>/dev/null || true

  # Close the control-FIFO fd and remove both FIFOs.
  exec 3<&- 2>/dev/null || true
  exec 3>&- 2>/dev/null || true
  rm -f "$CTL_FIFO" "$AUDIO_FIFO"
  exit "$rc"
}
trap cleanup EXIT INT TERM HUP

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

# Extract title/artist with ffprobe, fall back to filename, then push a new
# radiotext line to the control FIFO. PS is intentionally NOT updated per
# track: it stays fixed at PS_DEFAULT (set via --ps at startup), which is
# how most real FM stations behave.
push_rds_for() {
  local f="$1" title artist rt
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
  rt="${rt:0:64}"

  printf 'RT %s\n' "$rt" >&3 || true
}

# --- start pi_fm_rds, reading the audio stream from the audio FIFO ----------
"$BIN" \
    --freq "$FREQ" \
    --pi   "$PI_CODE" \
    --ps   "$PS_DEFAULT" \
    --ctl  "$CTL_FIFO" \
    --audio - < "$AUDIO_FIFO" &
PIFM_PID=$!

# --- decoder loop: feeds concatenated WAV into the audio FIFO ---------------
# The inline `trap 'exit 130' INT TERM HUP` is crucial: without it, bash
# would swallow SIGINT whenever its ffmpeg child exits from SIGPIPE instead
# of SIGINT, and the loop would keep spinning after Ctrl-C (classic bash
# pipeline quirk).
(
  trap 'kill ${ffmpeg_pid:-0} 2>/dev/null; exit 130' INT TERM HUP
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
      # Decode this track into a WAV stream on stdout. Force a fixed sample
      # rate + channel count so concatenated tracks stay coherent.
      ffmpeg -hide_banner -loglevel error -nostdin \
             -i "$f" -f wav -ac "$CHANNELS" -ar "$SAMPLE_RATE" - &
      ffmpeg_pid=$!
      wait "$ffmpeg_pid"
      rc=$?
      # Exit codes >= 128 mean ffmpeg died from a signal (SIGPIPE, SIGTERM,
      # SIGINT, ...). In that case the downstream pi_fm_rds is gone or we're
      # shutting down: stop the loop instead of spinning through every file.
      if (( rc >= 128 )); then
        exit "$rc"
      elif (( rc != 0 )); then
        echo "   (skipping unreadable file, ffmpeg rc=$rc)" >&2
      fi
    done
  done
) > "$AUDIO_FIFO" &
DECODER_PID=$!

# Block on pi_fm_rds; the trap handles clean teardown of the decoder.
wait "$PIFM_PID"
