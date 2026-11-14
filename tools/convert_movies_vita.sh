#!/usr/bin/env bash
# Converts the game's Bink movies to MP4 files for the hardware decoder of
# the PS Vita (sceAvPlayer): H.264 Main profile, level 3.1, 4:2:0, at the
# size and frame rate of the source, and AAC-LC at 48 kHz (the Vita's output
# rate). Each audio track of a movie stays, in its order (the country
# movies have one for each language).
#
# ffmpeg runs in Docker: nothing is installed on the host.
#
#   tools/convert_movies_vita.sh GAME_DIR OUT_DIR
#
# GAME_DIR is the folder with FrontEnd/ and CountrySpecific/ (iso/CMR2).
# OUT_DIR gets the same folders, with .mp4 in place of .bik.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 GAME_DIR OUT_DIR" >&2
    exit 2
fi
game=$(cd "$1" && (pwd -W 2>/dev/null || pwd))
mkdir -p "$2"
out=$(cd "$2" && (pwd -W 2>/dev/null || pwd))
image=jrottenberg/ffmpeg:7.1-ubuntu2404

run() {
    MSYS_NO_PATHCONV=1 docker run --rm --platform linux/amd64 -v "$game:/in:ro" -v "$out:/out" "$@"
}

find "$1" -iname '*.bik' | sort | while read -r path; do
    rel=${path#"$1"/}
    dest=${rel%.*}.mp4
    mkdir -p "$2/$(dirname "$rel")"
    # A keyframe each 2 s: the frame rate of the source, twice.
    fps=$(run --entrypoint ffprobe "$image" -v error -select_streams v:0 \
        -show_entries stream=r_frame_rate -of default=nw=1:nk=1 "/in/$rel")
    gop=$(( 2 * ${fps%/*} / ${fps#*/} ))
    echo "$rel -> $dest ($fps fps)"
    run "$image" -hide_banner -loglevel error -y -i "/in/$rel" \
        -map 0:v:0 -map '0:a?' \
        -c:v libx264 -profile:v main -level:v 3.1 -pix_fmt yuv420p \
        -preset slow -crf 18 -maxrate 4M -bufsize 8M -g "$gop" -bf 2 \
        -c:a aac -b:a 128k -ar 48000 -ac 2 \
        -movflags +faststart "/out/$dest"
done
