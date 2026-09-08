#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Turn .env into a C header. Rewrites the header only when its contents change,
# so an unchanged .env does not force a rebuild.
set -eu

ENV_FILE="${1:-.env}"
OUT="${2:-main/env_config.h}"

die() { echo "gen_env_config: $*" >&2; exit 1; }

[ -f "$ENV_FILE" ] || die "$ENV_FILE not found (copy .env.example to .env)"

# Last uncommented assignment wins. Strips surrounding quotes and trailing space.
get() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$ENV_FILE" \
        | sed -e 's/[[:space:]]*$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'\$/\1/" \
        | tail -n 1
}

# Escape for a C string literal.
cesc() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

# newlib on the ESP32 has no timezone database, so it needs a POSIX TZ string.
# An IANA name is resolved by reading the POSIX footer out of the host's TZif
# file; anything containing a digit is passed through as an explicit TZ string.
resolve_tz() {
    tz="$1"
    for dir in /usr/share/zoneinfo /usr/lib/zoneinfo /etc/zoneinfo; do
        if [ -f "$dir/$tz" ]; then
            posix=$(tail -n 1 "$dir/$tz" | tr -d '\000\n')
            if [ -n "$posix" ]; then
                printf '%s' "$posix"
                return 0
            fi
        fi
    done
    case "$tz" in
        *[0-9]*) printf '%s' "$tz"; return 0 ;;
    esac
    return 1
}

WIFI_SSID=$(get WIFI_SSID)
WIFI_PASSWORD=$(get WIFI_PASSWORD)
WIFI_SSID_2=$(get WIFI_SSID_2)
WIFI_PASSWORD_2=$(get WIFI_PASSWORD_2)
WIFI_SSID_3=$(get WIFI_SSID_3)
WIFI_PASSWORD_3=$(get WIFI_PASSWORD_3)
TIMEZONE=$(get TIMEZONE)
HOURS=$(get HOURS)
TEMP_UNIT=$(get TEMP_UNIT)
SAMPLE_INTERVAL_SECONDS=$(get SAMPLE_INTERVAL_SECONDS)
NTP_SERVER=$(get NTP_SERVER)
REFRESH_INTERVAL_HOURS=$(get REFRESH_INTERVAL_HOURS)
REFRESH_TIME=$(get REFRESH_TIME)
ELEVATED=$(get ELEVATED)
ALERT=$(get ALERT)
WARNING=$(get WARNING)

[ -n "$WIFI_SSID" ] || die "WIFI_SSID is not set in $ENV_FILE"
[ -z "$WIFI_PASSWORD_2" ] || [ -n "$WIFI_SSID_2" ] || die "WIFI_PASSWORD_2 is set but WIFI_SSID_2 is not"
[ -z "$WIFI_PASSWORD_3" ] || [ -n "$WIFI_SSID_3" ] || die "WIFI_PASSWORD_3 is set but WIFI_SSID_3 is not"
[ -z "$WIFI_SSID_3" ] || [ -n "$WIFI_SSID_2" ] || die "WIFI_SSID_3 is set but WIFI_SSID_2 is not - fill networks in order"
[ -n "$TIMEZONE" ] || die "TIMEZONE is not set in $ENV_FILE"
[ -n "$NTP_SERVER" ] || NTP_SERVER="pool.ntp.org"
[ -n "$HOURS" ] || HOURS=24
[ -n "$TEMP_UNIT" ] || TEMP_UNIT=F
[ -n "$SAMPLE_INTERVAL_SECONDS" ] || SAMPLE_INTERVAL_SECONDS=15
[ -n "$REFRESH_INTERVAL_HOURS" ] || REFRESH_INTERVAL_HOURS=1
[ -n "$ELEVATED" ] || ELEVATED=1000
[ -n "$ALERT" ] || ALERT=2000
[ -n "$WARNING" ] || WARNING=5000

case "$HOURS" in
    12) CLOCK_24H=0 ;;
    24) CLOCK_24H=1 ;;
    *)  die "HOURS must be 12 or 24, got '$HOURS'" ;;
esac

case "$TEMP_UNIT" in
    C|c) TEMP_UNIT_F=0 ;;
    F|f) TEMP_UNIT_F=1 ;;
    *)   die "TEMP_UNIT must be C or F, got '$TEMP_UNIT'" ;;
esac

case "$SAMPLE_INTERVAL_SECONDS" in
    ''|*[!0-9]*) die "SAMPLE_INTERVAL_SECONDS must be a positive integer, got '$SAMPLE_INTERVAL_SECONDS'" ;;
esac
[ "$SAMPLE_INTERVAL_SECONDS" -ge 1 ] || die "SAMPLE_INTERVAL_SECONDS must be at least 1, got '$SAMPLE_INTERVAL_SECONDS'"
[ "$SAMPLE_INTERVAL_SECONDS" -le 1800 ] || die "SAMPLE_INTERVAL_SECONDS must be at most 1800, got '$SAMPLE_INTERVAL_SECONDS'"

case "$REFRESH_INTERVAL_HOURS" in
    ''|*[!0-9]*) die "REFRESH_INTERVAL_HOURS must be a positive integer, got '$REFRESH_INTERVAL_HOURS'" ;;
esac
[ "$REFRESH_INTERVAL_HOURS" -ge 1 ] || die "REFRESH_INTERVAL_HOURS must be at least 1, got '$REFRESH_INTERVAL_HOURS'"
[ "$REFRESH_INTERVAL_HOURS" -le 24 ] || die "REFRESH_INTERVAL_HOURS must be at most 24, got '$REFRESH_INTERVAL_HOURS'"

for var_name in ELEVATED ALERT WARNING; do
    eval "val=\$$var_name"
    case "$val" in
        ''|*[!0-9]*) die "$var_name must be a positive integer, got '$val'" ;;
    esac
done
[ "$ELEVATED" -lt "$ALERT" ] || die "ELEVATED must be less than ALERT, got ELEVATED=$ELEVATED ALERT=$ALERT"
[ "$ALERT" -lt "$WARNING" ] || die "ALERT must be less than WARNING, got ALERT=$ALERT WARNING=$WARNING"

# REFRESH_TIME anchors full refreshes to a time of day instead of to boot
# time; -1 means "no anchor, use boot time" (main.c's fallback behavior).
if [ -n "$REFRESH_TIME" ]; then
    case "$REFRESH_TIME" in
        [0-9]:[0-5][0-9]) REFRESH_TIME="0$REFRESH_TIME" ;;
    esac
    case "$REFRESH_TIME" in
        [0-2][0-9]:[0-5][0-9]) ;;
        *) die "REFRESH_TIME must be 24-hour HH:MM, got '$REFRESH_TIME'" ;;
    esac
    REFRESH_HOUR=${REFRESH_TIME%%:*}
    REFRESH_MIN=${REFRESH_TIME##*:}
    [ "$((10#$REFRESH_HOUR))" -le 23 ] || die "REFRESH_TIME hour must be 00-23, got '$REFRESH_TIME'"
    REFRESH_ANCHOR_MINUTES=$((10#$REFRESH_HOUR * 60 + 10#$REFRESH_MIN))
else
    REFRESH_ANCHOR_MINUTES=-1
fi

if [ -n "$WIFI_SSID_3" ]; then
    WIFI_COUNT=3
elif [ -n "$WIFI_SSID_2" ]; then
    WIFI_COUNT=2
else
    WIFI_COUNT=1
fi

POSIX_TZ=$(resolve_tz "$TIMEZONE") || die \
"cannot resolve TIMEZONE='$TIMEZONE'.
  Use an IANA name present in /usr/share/zoneinfo (e.g. America/New_York),
  or a literal POSIX TZ string (e.g. EST5EDT,M3.2.0,M11.1.0)."

TMP="$OUT.tmp"
mkdir -p "$(dirname "$OUT")"
cat > "$TMP" <<EOF
/* Generated from $ENV_FILE by scripts/gen_env_config.sh. Do not edit. */
#pragma once

#define CFG_WIFI_SSID_1 "$(cesc "$WIFI_SSID")"
#define CFG_WIFI_PASS_1 "$(cesc "$WIFI_PASSWORD")"
#define CFG_WIFI_SSID_2 "$(cesc "$WIFI_SSID_2")"
#define CFG_WIFI_PASS_2 "$(cesc "$WIFI_PASSWORD_2")"
#define CFG_WIFI_SSID_3 "$(cesc "$WIFI_SSID_3")"
#define CFG_WIFI_PASS_3 "$(cesc "$WIFI_PASSWORD_3")"
#define CFG_WIFI_COUNT $WIFI_COUNT
#define CFG_NTP_SERVER "$(cesc "$NTP_SERVER")"

/* TIMEZONE=$(cesc "$TIMEZONE") */
#define CFG_TZ         "$(cesc "$POSIX_TZ")"

/* HOURS=$HOURS */
#define CFG_CLOCK_24H  $CLOCK_24H

/* TEMP_UNIT=$TEMP_UNIT */
#define CFG_TEMP_UNIT_F $TEMP_UNIT_F

/* SAMPLE_INTERVAL_SECONDS=$SAMPLE_INTERVAL_SECONDS */
#define CFG_SAMPLE_INTERVAL_SECONDS $SAMPLE_INTERVAL_SECONDS

/* REFRESH_INTERVAL_HOURS=$REFRESH_INTERVAL_HOURS */
#define CFG_REFRESH_INTERVAL_MINUTES $((REFRESH_INTERVAL_HOURS * 60))

/* REFRESH_TIME=$(cesc "$REFRESH_TIME") (-1 below means unset: refresh from boot time) */
#define CFG_REFRESH_ANCHOR_MINUTES $REFRESH_ANCHOR_MINUTES

/* ELEVATED=$ELEVATED, ALERT=$ALERT, WARNING=$WARNING */
#define CFG_CO2_ELEVATED_PPM $ELEVATED
#define CFG_CO2_ALERT_PPM $ALERT
#define CFG_CO2_WARNING_PPM $WARNING
EOF

if [ -f "$OUT" ] && cmp -s "$TMP" "$OUT"; then
    rm -f "$TMP"
else
    mv "$TMP" "$OUT"
fi

if [ "$REFRESH_ANCHOR_MINUTES" -ge 0 ]; then
    REFRESH_DESC="every ${REFRESH_INTERVAL_HOURS}h anchored at $REFRESH_TIME"
else
    REFRESH_DESC="every ${REFRESH_INTERVAL_HOURS}h from boot"
fi

if [ "$WIFI_COUNT" -eq 1 ]; then
    WIFI_DESC="ssid=$WIFI_SSID"
else
    WIFI_DESC="ssids=$WIFI_SSID,$WIFI_SSID_2"
    if [ "$WIFI_COUNT" -eq 3 ]; then
        WIFI_DESC="$WIFI_DESC,$WIFI_SSID_3"
    fi
fi

echo "env: TZ=$TIMEZONE -> $POSIX_TZ, ${HOURS}-hour, temp=$TEMP_UNIT, sample every ${SAMPLE_INTERVAL_SECONDS}s, $WIFI_DESC, ntp=$NTP_SERVER, full refresh $REFRESH_DESC, CO2 LED elevated=$ELEVATED alert=$ALERT warning=$WARNING"
