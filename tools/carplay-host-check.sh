#!/usr/bin/env bash
# Run from the repository root with GCC and Python 3 on Linux.
set -eu
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /path/to/carplay-deps" >&2
    exit 2
fi
temp=$(mktemp -d)
trap 'rm -f "$temp/test_ncm" "$temp/test_airplay" "$temp/test_video"; rmdir "$temp"' EXIT
cp=src/carplay
lw=$1/lwip/src
mono=$1/monocypher/src
tls=third_party/mbedtls
build() {
    gcc -O1 '-DMBEDTLS_CONFIG_FILE=<carplay_tls_config.h>' \
        -I"$cp" -I"$cp/lwip_port" -I"$lw/include" -I"$mono" \
        -I"$mono/optional" -I"$tls/include" -I"$tls/library" -Isrc/libcicaa \
        -pthread "$@" "$tls"/library/*.c
}
build "$cp"/{test_ncm_host,cp_ncm,cp_net_lwip,cp_util}.c \
    "$cp/lwip_port/sys_arch.c" "$lw"/core/*.c "$lw"/core/ipv6/*.c \
    "$lw"/api/{api_lib,api_msg,err,netbuf,sockets,tcpip}.c "$lw/netif/ethernet.c" -o "$temp/test_ncm"
timeout 45 "$temp/test_ncm"
gcc -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Isrc/libcicaa "$cp/test_video_host.c" -o "$temp/test_video"
"$temp/test_video"
build "$cp"/{test_airplay_host,cp_airplay,cp_audio,cp_bplist,cp_crypto,cp_input,cp_mic,cp_net_posix,cp_screen,cp_util}.c \
    "$mono/monocypher.c" "$mono/optional/monocypher-ed25519.c" -o "$temp/test_airplay"
python3 tools/carplay-airplay-check.py "$temp/test_airplay"
