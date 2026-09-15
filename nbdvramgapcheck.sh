#!/usr/bin/env bash
#
# nbd-vram-gap-check.sh (v2) - quantify the stored/swapon gap on an nbd-vram
# swap device, and test the one thing about it that is actually falsifiable.
#
# Background
# ----------
# The daemon's `stored` is purely logical: g_blocks_live * 4096, a count of
# index entries with a non-zero clen (nbd-vram.c:2003). Heap fragmentation
# lives entirely on the VRAM side of the arrow and cannot move it. The only
# decrement site for g_blocks_live is trim_range() (nbd-vram.c:1553-1578),
# which decrements it one-for-one with `trimmed`.
#
# So when `stored` exceeds what `swapon --show` reports for the device, the
# excess is blocks the kernel freed without discarding: `--discard=pages` only
# emits a TRIM when an entire 2 MiB swap cluster becomes free, so scattered
# frees are never communicated at all.
#
# What this script checks
# -----------------------
#   A. `stored` never falls faster than trims explain:
#         d(stored) + 4096 * d(trimmed) >= 0
#      A violation means g_blocks_live is being decremented somewhere other
#      than trim_range(), i.e. a daemon accounting bug rather than a kernel
#      discard shortfall. This is the only invariant here with a real failure
#      mode, and it is the only thing that sets a non-zero exit status.
#
# What this script measures (no pass/fail - these are the numbers you want)
# ------------------------------------------------------------------------
#   Discard coverage. In any window where `swapon` USED fell, the kernel
#   released slots; `trimmed` says how many of those releases reached the
#   daemon as a TRIM.
#         coverage = 4096 * d(trimmed) / -d(kernel USED)
#   Both terms are exact - `swapon --bytes` is exact bytes and `trimmed` is an
#   exact block count - so this figure carries no quantization error. Coverage
#   well below 100% is the cluster-granularity shortfall, quantified. It is an
#   UPPER bound: d(kernel USED) is net of concurrent allocations, so the gross
#   number of frees is at least this large and true coverage is at most this.
#
#   Ghost-slot reuse. When the kernel writes to a slot it freed without
#   discarding, the index entry already has clen != 0, so wr_publish()
#   (nbd-vram.c:1218-1224) frees the old slot and allocates a new one WITHOUT
#   incrementing g_blocks_live. USED rises, `stored` does not, and the gap
#   closes with no TRIM anywhere. SWAP_HAS_CACHE entries do the same without
#   any NBD write arriving. This is normal, and it is how stale data actually
#   gets reclaimed on a device that never receives discards.
#
#   NOTE (v1 retraction): v1 also asserted "the gap only ever closes via trims"
#   as a check. That premise is false - ghost-slot reuse closes it too - and it
#   fired on live data that was behaving exactly as the diagnosis predicts. The
#   check has been removed rather than re-tuned; the gap has no sound one-sided
#   invariant.
#
# Precision note: hsize() (nbd-vram.c:1977-1982) prints "%.2f GiB", so `stored`
# carries a half-ulp of 5.37 MiB per sample and a delta up to 10.7 MiB of
# rounding error - which is larger than the per-minute movement on a settled
# system. Deltas smaller than that are printed as "unresolved" rather than
# asserted. Because a cumulative delta still spans only two endpoint roundings
# however many samples it covers, the script also reports every sample against
# sample 1: run it for an hour and `stored` movement resolves an order of
# magnitude better, with no daemon change. `trimmed` and `swapon --bytes` are
# exact and need none of this.
#
# Usage:
#   ./nbd-vram-gap-check.sh                      # 3 samples from the journal
#   ./nbd-vram-gap-check.sh -n 30 -t 3600        # an hour, for real resolution
#   ./nbd-vram-gap-check.sh -l /var/log/nbd.log  # read a log file instead
#   ./nbd-vram-gap-check.sh -d /dev/nbd0 -c out.csv
#
# Exit status: 0 check A held, 1 check A failed, 2 setup/usage error.

set -u

DEV=/dev/nbd0
SAMPLES=3
POLL=5
LOGFILE=""
CSV=""
TIMEOUT=9000

usage() {
    sed -n '2,70p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while getopts "d:n:p:l:c:t:h" opt; do
    case "$opt" in
        d) DEV=$OPTARG ;;
        n) SAMPLES=$OPTARG ;;
        p) POLL=$OPTARG ;;
        l) LOGFILE=$OPTARG ;;
        c) CSV=$OPTARG ;;
        t) TIMEOUT=$OPTARG ;;
        h|*) usage ;;
    esac
done

[ "$SAMPLES" -ge 2 ] 2>/dev/null || { echo "need -n >= 2" >&2; exit 2; }

command -v swapon >/dev/null || { echo "swapon not found" >&2; exit 2; }
if [ -n "$LOGFILE" ]; then
    [ -r "$LOGFILE" ] || { echo "cannot read $LOGFILE" >&2; exit 2; }
else
    command -v journalctl >/dev/null || {
        echo "journalctl not found; pass -l <logfile>" >&2; exit 2; }
fi

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# Most recent stats line, or empty if none yet.
last_stats_line() {
    if [ -n "$LOGFILE" ]; then
        grep -a -F -- ' stored ' "$LOGFILE" | grep -a -F -- ' trimmed ' | tail -n 1
    else
        journalctl -t nbd-vram -n 400 --no-pager -o cat 2>/dev/null \
            | grep -a -F -- ' stored ' | grep -a -F -- ' trimmed ' | tail -n 1
    fi
}

# Kernel's view, in bytes, for $DEV. Empty if the device is not a swap device.
kernel_used() {
    swapon --show=NAME,USED --bytes --noheadings 2>/dev/null \
        | awk -v d="$DEV" '$1 == d { print $2; found=1 } END { if (!found) print "" }'
}

# Parse one stats line into: stored_bytes stored_tol trimmed committed_bytes
# occupancy extents_used extents_total slack enospc
parse_stats() {
    awk '
    function tobytes(v, u) {
        if (u == "GiB") return v * 1073741824
        if (u == "MiB") return v * 1048576
        if (u == "KiB") return v * 1024
        return v
    }
    # Half-ulp of hsize(): %.2f GiB, %.1f MiB, integer-truncated KiB.
    function tol(u) {
        if (u == "GiB") return 0.005 * 1073741824
        if (u == "MiB") return 0.05  * 1048576
        return 1024
    }
    {
        if (match($0, /stored [0-9.]+ (GiB|MiB|KiB)/)) {
            split(substr($0, RSTART + 7, RLENGTH - 7), s, " ")
            stored = tobytes(s[1], s[2]); stol = tol(s[2])
        }
        if (match($0, /-> [0-9.]+ (GiB|MiB|KiB) VRAM/)) {
            split(substr($0, RSTART + 3, RLENGTH - 8), c, " ")
            commit = tobytes(c[1], c[2])
        }
        if (match($0, /occupancy [0-9.]+%/))
            occ = substr($0, RSTART + 10, RLENGTH - 11)
        if (match($0, /slack [0-9.]+%/))
            slack = substr($0, RSTART + 6, RLENGTH - 7)
        if (match($0, /extents [0-9]+\/[0-9]+/)) {
            split(substr($0, RSTART + 8, RLENGTH - 8), e, "/")
            eu = e[1]; et = e[2]
        }
        if (match($0, /trimmed [0-9]+/))
            trimmed = substr($0, RSTART + 8, RLENGTH - 8)
        if (match($0, /enospc [0-9]+/))
            enospc = substr($0, RSTART + 7, RLENGTH - 7)
        printf "%.0f %.0f %s %.0f %s %s %s %s %s\n",
               stored, stol, trimmed, commit, occ, eu, et, slack, enospc
    }'
}

human() { awk -v b="$1" 'BEGIN {
    a = (b < 0 ? -b : b); s = (b < 0 ? "-" : "")
    if (a >= 1073741824) printf "%s%.2f GiB", s, a/1073741824
    else if (a >= 1048576) printf "%s%.1f MiB", s, a/1048576
    else printf "%s%.0f KiB", s, a/1024
}'; }

# Same, but always signed, for deltas.
humans() { awk -v b="$1" 'BEGIN {
    a = (b < 0 ? -b : b); s = (b < 0 ? "-" : "+")
    if (a >= 1073741824) printf "%s%.2f GiB", s, a/1073741824
    else if (a >= 1048576) printf "%s%.1f MiB", s, a/1048576
    else printf "%s%.0f KiB", s, a/1024
}'; }

pct() { awk -v n="$1" -v d="$2" 'BEGIN{ printf "%.1f%%", d ? 100.0*n/d : 0 }'; }

# ---------------------------------------------------------------------------
# One window's worth of reporting. Args:
#   label span d_stored d_trim d_kused tolerance
# Sets global `fail` on a check A violation.
# ---------------------------------------------------------------------------
report_window() {
    local label="$1" span="$2" ds="$3" dt="$4" dk="$5" tol="$6"
    local tb=$((dt * 4096)) dg resA cov reuse_lo

    dg=$((ds - dk))

    echo
    printf '  %s (%ss)\n' "$label" "$span"
    printf '    kernel used   %-14s exact\n'  "$(humans "$dk")"
    printf '    trimmed       %-14s exact  (%s blocks)\n' "$(humans "$tb")" "$dt"
    if [ "$ds" -le "$tol" ] && [ "$ds" -ge "-$tol" ]; then
        printf '    stored        %-14s +/- %s  -> unresolved\n' \
            "$(humans "$ds")" "$(human "$tol")"
    else
        printf '    stored        %-14s +/- %s\n' "$(humans "$ds")" "$(human "$tol")"
    fi
    printf '    gap           %-14s\n' "$(humans "$dg")"
    echo

    # ---- Check A: stored never falls faster than trims explain.
    resA=$((ds + tb))
    if [ "$resA" -ge "-$tol" ]; then
        printf '    [A] PASS   d(stored) + 4096*d(trimmed) = %s  >= -%s\n' \
            "$(humans "$resA")" "$(human "$tol")"
    else
        printf '    [A] FAIL   d(stored) + 4096*d(trimmed) = %s  < -%s\n' \
            "$(humans "$resA")" "$(human "$tol")"
        echo  '               stored fell faster than trims account for, so'
        echo  '               g_blocks_live is being decremented outside'
        echo  '               trim_range(). That is a daemon accounting bug,'
        echo  '               not a discard shortfall.'
        fail=1
    fi

    # ---- Discard coverage: the headline number, exact quantities only.
    if [ "$dk" -lt 0 ]; then
        cov=$(pct "$tb" "$((-dk))")
        printf '    [coverage] kernel released %s, trims covered %s = %s\n' \
            "$(human "$((-dk))")" "$(human "$tb")" "$cov"
        echo  '               The remainder was freed without a TRIM: with'
        echo  '               --discard=pages the kernel only discards a whole'
        echo  '               2 MiB cluster, so scattered frees are silent.'
        echo  '               Upper bound - d(kernel used) is net of concurrent'
        echo  '               allocations, so true coverage is at most this.'
    elif [ "$dk" -gt 0 ] && [ "$resA" -lt "-$tol" ]; then
        # Check A failed, so first_writes == resA is not a usable quantity and
        # nothing can be attributed. Say so rather than printing a bound
        # derived from an accounting error.
        printf '    [reuse]    kernel USED rose %s, but check A failed in this\n' \
            "$(humans "$dk")"
        echo  '               window, so first writes cannot be separated from'
        echo  '               reuse here. Fix the accounting first.'
    elif [ "$dk" -gt 0 ]; then
        # allocated = first_writes + reuse, and first_writes == resA (+/- tol),
        # and allocated >= d(kernel used) since frees are non-negative.
        reuse_lo=$((dk - resA - tol))
        printf '    [reuse]    kernel USED rose %s while first writes account\n' \
            "$(humans "$dk")"
        printf '               for at most %s of it\n' "$(human "$((resA + tol))")"
        if [ "$reuse_lo" -gt 0 ]; then
            printf '               -> at least %s is the kernel rewriting slots the\n' \
                "$(human "$reuse_lo")"
            echo  '               daemon still holds data for (ghost-slot reuse),'
            echo  '               or SWAP_HAS_CACHE churn. Expected: this is how'
            echo  '               stale blocks get reclaimed without discards.'
        else
            echo  '               -> consistent with ordinary first writes; no'
            echo  '               reuse is resolvable at this precision.'
        fi
    else
        echo  '    [coverage] kernel USED flat; nothing to attribute.'
    fi

    # ---- Quiet-window equality, only where it says something.
    if [ "$dt" -gt 0 ] && [ "$resA" -le "$tol" ] && [ "$resA" -ge "-$tol" ]; then
        printf '    [ = ]      quiet window: d(stored) == -4096*d(trimmed) within %s\n' \
            "$(human "$tol")"
    elif [ "$resA" -gt "$tol" ]; then
        # first_writes == resA, but resA inherits stored's rounding, so state
        # the floor rather than the midpoint - a resA barely outside tolerance
        # is not evidence of that many first writes.
        printf '    [ i ]      first writes: at least %s  (%s +/- %s)\n' \
            "$(human "$((resA - tol))")" "$(human "$resA")" "$(human "$tol")"
    fi
}

# ---------------------------------------------------------------------------
# Collect
# ---------------------------------------------------------------------------

BLK=4096

if [ -z "$(kernel_used)" ]; then
    echo "error: $DEV is not listed by 'swapon --show'." >&2
    swapon --show >&2
    exit 2
fi

echo "device      : $DEV"
echo "stats source: ${LOGFILE:-journalctl -t nbd-vram}"
echo "collecting $SAMPLES stats lines (the daemon emits one every"
echo "VRAM_STATS_INTERVAL_SEC, default 60) ..."
echo

prev_stored=0; prev_tol=0; prev_trim=0; prev_kused=0; prev_ts=0
first_stored=0; first_tol=0; first_trim=0; first_kused=0; first_ts=0; first_gap=0
n=0; fail=0
start=$(date +%s)

[ -n "$CSV" ] && echo "ts,stored_bytes,kernel_used_bytes,gap_bytes,trimmed_blocks,committed_bytes,occupancy_pct,extents_used,extents_total,slack_pct,enospc,stored_tol_bytes,d_kernel_used_bytes,d_trimmed_blocks,coverage_pct" > "$CSV"

# Ignore whatever is already in the log: we want deltas between fresh lines.
prev_line_seen="$(last_stats_line)"
[ -n "$prev_line_seen" ] && echo "(waiting for the next stats line; ignoring the one already logged)"

while [ "$n" -lt "$SAMPLES" ]; do
    now=$(date +%s)
    if [ $((now - start)) -ge "$TIMEOUT" ]; then
        echo >&2
        echo "error: timed out after ${TIMEOUT}s with $n sample(s)." >&2
        echo "The daemon may have stats disabled (VRAM_STATS_INTERVAL_SEC=0)." >&2
        exit 2
    fi

    line="$(last_stats_line)"
    if [ -z "$line" ] || [ "$line" = "$prev_line_seen" ]; then
        sleep "$POLL"
        continue
    fi
    prev_line_seen="$line"

    # Read the kernel's view as close to the log line as we can get.
    kused="$(kernel_used)"
    ts=$(date +%s)

    set -- $(printf '%s\n' "$line" | parse_stats)
    stored=$1; stol=$2; trimmed=$3; commit=$4; occ=$5; eu=$6; et=$7; slack=$8; enospc=$9

    if [ -z "${trimmed:-}" ] || [ -z "${stored:-}" ]; then
        echo "warning: could not parse stats line, skipping:" >&2
        echo "  $line" >&2
        continue
    fi

    gap=$((stored - kused))
    n=$((n + 1))

    echo "=== sample $n  ($(date -d "@$ts" '+%H:%M:%S' 2>/dev/null || date '+%H:%M:%S')) ==="
    printf '  stored          %s   (+/- %s, print precision)\n' \
        "$(human "$stored")" "$(human "$stol")"
    printf '  swapon used     %s   (exact)\n'  "$(human "$kused")"
    printf '  gap             %s   (%s of stored)\n' "$(human "$gap")" \
        "$(pct "$gap" "$stored")"
    printf '  trimmed         %s blocks (%s cumulative)\n' "$trimmed" \
        "$(human $((trimmed * BLK)))"
    printf '  committed       %s   occupancy %s%%  slack %s%%  extents %s/%s  enospc %s\n' \
        "$(human "$commit")" "$occ" "$slack" "$eu" "$et" "$enospc"
    # Fragmentation cost, for contrast: committed VRAM minus the compressed
    # payload actually inside it. committed*occ is g_slot_bytes; removing slack
    # leaves g_codec_bytes. This is the entire (2) hypothesis, in bytes.
    printf '  frag cost       %s   (intra-extent holes + size-class rounding)\n' \
        "$(human "$(awk -v c="$commit" -v o="$occ" -v s="$slack" \
            'BEGIN{printf "%.0f", c - c*(o/100)*(1-s/100)}')")"
    # Occupancy corrected for blocks the kernel has already forgotten.
    printf '  true occupancy  %s%%  (reported %s%% scaled by kernel-live share)\n' \
        "$(awk -v o="$occ" -v k="$kused" -v s="$stored" \
            'BEGIN{printf "%.1f", s ? o*k/s : 0}')" "$occ"

    if [ "$n" -eq 1 ]; then
        first_stored=$stored; first_tol=$stol; first_trim=$trimmed
        first_kused=$kused; first_ts=$ts; first_gap=$gap
        [ -n "$CSV" ] && printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,,,\n' \
            "$ts" "$stored" "$kused" "$gap" "$trimmed" "$commit" "$occ" \
            "$eu" "$et" "$slack" "$enospc" "$stol" >> "$CSV"
    else
        d_stored=$((stored - prev_stored))
        d_trim=$((trimmed - prev_trim))
        d_kused=$((kused - prev_kused))
        tolerance=$(awk -v a="$stol" -v b="$prev_tol" 'BEGIN{printf "%.0f", a + b + 2}')

        if [ -n "$CSV" ]; then
            covcsv=""
            [ "$d_kused" -lt 0 ] && covcsv=$(awk -v n="$((d_trim * BLK))" \
                -v d="$((-d_kused))" 'BEGIN{printf "%.2f", 100.0*n/d}')
            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "$ts" "$stored" "$kused" "$gap" "$trimmed" "$commit" "$occ" \
                "$eu" "$et" "$slack" "$enospc" "$stol" "$d_kused" "$d_trim" \
                "$covcsv" >> "$CSV"
        fi

        report_window "window $((n-1)) -> $n" "$((ts - prev_ts))" \
            "$d_stored" "$d_trim" "$d_kused" "$tolerance"

        # Cumulative against sample 1. Same two endpoint roundings however
        # many samples it spans, so this is the resolving measurement.
        if [ "$n" -ge 3 ]; then
            ctol=$(awk -v a="$stol" -v b="$first_tol" 'BEGIN{printf "%.0f", a + b + 2}')
            report_window "cumulative 1 -> $n" "$((ts - first_ts))" \
                "$((stored - first_stored))" "$((trimmed - first_trim))" \
                "$((kused - first_kused))" "$ctol"
        fi
        echo
    fi

    prev_stored=$stored; prev_tol=$stol; prev_trim=$trimmed
    prev_kused=$kused; prev_ts=$ts
done

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------

tot_kused=$((prev_kused - first_kused))
tot_trim=$(( (prev_trim - first_trim) * BLK ))
tot_span=$((prev_ts - first_ts))
last_gap=$((prev_stored - prev_kused))

echo "==========================================================="
printf 'RUN TOTAL over %ss (%s samples)\n' "$tot_span" "$n"
printf '  kernel used   %s\n' "$(humans "$tot_kused")"
printf '  trimmed       %s (%s blocks)\n' "$(humans "$tot_trim")" \
    "$((prev_trim - first_trim))"
printf '  gap           %s -> %s  (%s)\n' "$(human "$first_gap")" \
    "$(human "$last_gap")" "$(humans "$((last_gap - first_gap))")"
if [ "$tot_kused" -lt 0 ]; then
    printf '  discard coverage over the run: %s (upper bound)\n' \
        "$(pct "$tot_trim" "$((-tot_kused))")"
fi
echo

if [ "$fail" -eq 0 ]; then
    cat <<'EOF'
VERDICT: check A held. `stored` never fell faster than `trimmed` explains, so
g_blocks_live is only being decremented by trim_range() and the daemon's logical
accounting is sound.

That leaves the gap as blocks the kernel freed without discarding. Read the
[coverage] lines above for the size of the shortfall: coverage well under 100%
is the --discard=pages cluster granularity, and no swapon option changes it.
Fragmentation is not a candidate - `stored` is g_blocks_live*4096 while
fragmentation is denominated in committed VRAM bytes, printed above as
"frag cost" - so the two cannot trade against each other at all.

Ordering consequence: trimming the stale blocks would not return the VRAM,
because slot_free() only releases an extent when every slot in it is free and
there is no compaction (nbd-vram.c:816-817). It would drop occupancy to roughly
the "true occupancy" printed above, converting a stale-data problem into a
fragmentation one.
EOF
    exit 0
else
    cat <<'EOF'
VERDICT: check A failed - see [A] FAIL above.

`stored` dropped by more than the trims in that window account for. Since
trim_range() (nbd-vram.c:1553-1578) is the only decrement site for
g_blocks_live, that points at the daemon's own accounting rather than at the
kernel's discard behaviour.

Before concluding: a single failure close to the printed tolerance may just be
rounding in hsize()'s "%.2f GiB" (nbd-vram.c:1977-1982). Re-run with more
samples and check the cumulative 1 -> N lines, which span the same two endpoint
roundings however long the run, and capture a CSV with -c.
EOF
    exit 1
fi
