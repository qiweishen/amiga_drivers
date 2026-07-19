#!/usr/bin/env bash
# Post-process a recorded SBF file with the prebuilt RxTools CLI converters:
#
#   IMU (ExtSensorMeas), PRIMARY -> bin2asc -m ExtSensorMeas1 -> <base>_imu.csv
#   IMU (ExtSensorMeas), legacy  -> sbf2asc -j                -> <base>_imu.asc
#   Attitude / aux-ant baseline  -> sbf2asc -a -s -u          -> <base>_att.asc
#   Main-antenna raw obs         -> sbf2rin -a 1              -> <base>_ant1.obs
#   Aux-antenna raw obs          -> sbf2rin -a 2              -> <base>_ant2.obs
#   Mixed navigation (ephem.)    -> sbf2rin -nP               -> <base>.nav
#
# Usage:
#   scripts/postprocess.sh <file.sbf | recordings-dir> [outdir]
#
# Environment:
#   RXTOOLS_BIN   path to RxTools/bin (default: <repo>/RxTools/bin)
#   KEEP_INVALID_TIME=0   add -E: drop rows/messages whose timestamp is
#                         invalid (IMU rows recorded in Boot mode before GNSS
#                         time was known). Default 1 = keep everything.
#
# Notes:
#   * The .csv from bin2asc is the IMU product to feed the estimator: it keeps
#     the raw TOW [ms] / WNc fields (millisecond resolution) and decodes each
#     sub-block type into named columns (title row via -t).
#   * The .asc from sbf2asc is kept for quick eyeballing only. Known
#     limitations of the vendor tool (do NOT parse it for INS work):
#       - the time column is printed with 0.01 s resolution, so 200 Hz IMU
#         rows get duplicate/aliased timestamps;
#       - every -13 row carries 6 trailing literal '0' columns (13 tokens);
#       - X/Y/Z are only meaningful for Type 0 (acceleration, m/s^2) and
#         Type 1 (angular rate, deg/s); Type 3/4 rows are misdecoded by the
#         tool and must be taken from the .csv instead.
#   * sbf2rin/sbf2asc refuse SBF inputs >= 2 GB; the driver rotates recordings
#     at 1 GiB (config: output.rotate_bytes), safely below the limit.
#   * Each rotated segment is converted independently; pass a directory to
#     convert every *.sbf inside it.
set -euo pipefail

usage() {
    echo "usage: $0 <file.sbf | recordings-dir> [outdir]" >&2
    exit 3
}

[[ $# -ge 1 && $# -le 2 ]] || usage

# readlink -m: canonicalize without requiring existence (plain -f dies
# silently under `set -e` when a parent component is missing).
INPUT=$(readlink -m "$1")
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RXTOOLS_BIN="${RXTOOLS_BIN:-$SCRIPT_DIR/../RxTools/bin}"
KEEP_INVALID_TIME="${KEEP_INVALID_TIME:-1}"

for tool in sbf2rin sbf2asc bin2asc; do
    if [[ ! -x "$RXTOOLS_BIN/$tool" ]]; then
        echo "error: RxTools converter '$tool' not found at $RXTOOLS_BIN (set RXTOOLS_BIN)" >&2
        exit 1
    fi
done

# Scoped LD_LIBRARY_PATH per child process only: the current CLI tools are
# statically self-contained (they need only libc/libm), but this keeps the
# wrapper working if a future RxTools drop links the bundled Qt. Never export
# it globally.
run_tool() {
    local tool="$1"; shift
    LD_LIBRARY_PATH="$RXTOOLS_BIN${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$RXTOOLS_BIN/$tool" "$@"
}

convert_one() {
    local sbf="$1" outdir="$2"
    local base
    base=$(basename "$sbf" .sbf)

    local size
    size=$(stat -c %s "$sbf")
    if (( size >= 2147483648 )); then
        echo "error: $sbf is >= 2 GB; sbf2rin/sbf2asc cannot convert it." >&2
        echo "       Split it first, e.g.: $RXTOOLS_BIN/sbf2sbf -f <in> -Q <start:end> -o <out>" >&2
        return 1
    fi

    local e_flag=()
    if [[ "$KEEP_INVALID_TIME" == "0" ]]; then
        e_flag=(-E)
    fi

    echo "== $base (${size} bytes) -> $outdir"

    # 1) Raw IMU (ExtSensorMeas), PRIMARY: millisecond TOW/WNc preserved,
    #    per-type decoding, comma-separated with a title row.
    #    NOTE: unlike sbf2asc/sbf2rin, bin2asc's -o takes a BARE filename;
    #    the output directory goes in -p.
    run_tool bin2asc -f "$sbf" -m ExtSensorMeas1 -t "${e_flag[@]}" \
        -p "$outdir" -o "${base}_imu.csv"

    # 2) Raw IMU via sbf2asc (rows tagged -13) — quick-look only, see header.
    run_tool sbf2asc -f "$sbf" -j "${e_flag[@]}" -o "$outdir/${base}_imu.asc"

    # 3) Attitude + aux-antenna baseline: AttEuler (-4), AttCovEuler (-5),
    #    AuxPos (-12).
    run_tool sbf2asc -f "$sbf" -a -s -u "${e_flag[@]}" -o "$outdir/${base}_att.asc"

    # 4) RINEX v3.04 observations, main antenna, with SNR (-s) and Doppler (-D).
    run_tool sbf2rin -f "$sbf" -a 1 -nO -R304 -s -D -o "$outdir/${base}_ant1.obs"

    # 5) RINEX v3.04 observations, auxiliary antenna (dual-antenna heading).
    run_tool sbf2rin -f "$sbf" -a 2 -nO -R304 -s -D -o "$outdir/${base}_ant2.obs"

    # 6) RINEX v3.04 mixed navigation file (antenna-independent).
    run_tool sbf2rin -f "$sbf" -nP -R304 -o "$outdir/${base}.nav"

    ls -l "$outdir/${base}_imu.csv" "$outdir/${base}_imu.asc" \
          "$outdir/${base}_att.asc" \
          "$outdir/${base}_ant1.obs" "$outdir/${base}_ant2.obs" \
          "$outdir/${base}.nav"
}

if [[ -d "$INPUT" ]]; then
    OUTDIR="${2:-$INPUT}"
elif [[ -f "$INPUT" ]]; then
    OUTDIR="${2:-$(dirname "$INPUT")}"
else
    echo "error: $INPUT does not exist" >&2
    exit 1
fi

# Create before canonicalizing: readlink -f fails on paths that do not exist
# yet, and [outdir] is allowed to be a new (nested) directory.
mkdir -p "$OUTDIR"
OUTDIR=$(readlink -f "$OUTDIR")

# cd defensively: sbf2rin writes derived-name outputs to the CURRENT
# directory (we always pass -o, but do not depend on it).
cd "$OUTDIR"

if [[ -d "$INPUT" ]]; then
    shopt -s nullglob
    sbfs=("$INPUT"/*.sbf)
    if (( ${#sbfs[@]} == 0 )); then
        echo "error: no .sbf files in $INPUT" >&2
        exit 1
    fi
    for sbf in "${sbfs[@]}"; do
        convert_one "$sbf" "$OUTDIR"
    done
else
    convert_one "$INPUT" "$OUTDIR"
fi

echo "done."
