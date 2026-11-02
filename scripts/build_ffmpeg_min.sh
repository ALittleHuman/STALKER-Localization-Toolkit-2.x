#!/usr/bin/env bash
# 自建 minimal ffmpeg 7.1.5（P-798 之后体积的主解）。
# 口径（HA/DSH 2026-11-02）：--disable-everything + 显式 enable；不带 GPL；不出 ffprobe/ffplay。
# 产物：build/ffmpeg_min/ffmpeg.exe（staging，**不覆盖** dist 里的 exe；全绿后由人工替换）。
set -euo pipefail

SRC_VER="${FFMPEG_VER:-n7.1.5}"
WORK="${FFMPEG_WORK:-/e/Software/Games/STALKER/Localization/Tools/STALKER_Toolkit_2.x/build/ffmpeg_build}"
OUT="/e/Software/Games/STALKER/Localization/Tools/STALKER_Toolkit_2.x/build/ffmpeg_min"
mkdir -p "$WORK" "$OUT"

cd "$WORK"
if [ ! -d "FFmpeg-$SRC_VER" ]; then
    # github 可达；ffmpeg.org 在本机超时
    # 本机 msys curl 的 CA 链有问题（SSL verify failed）；由 PowerShell 预下载到同目录
    if [ ! -f "ffmpeg-$SRC_VER.tar.gz" ]; then
        curl -k -L --retry 3 -o "ffmpeg-$SRC_VER.tar.gz" \
            "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/$SRC_VER.tar.gz"
    fi
    tar -xzf "ffmpeg-$SRC_VER.tar.gz"
fi
cd "FFmpeg-$SRC_VER"

# 已编过就增量；要重来：rm -rf "$WORK/FFmpeg-$SRC_VER"（或 make distclean）
./configure \
    --prefix="$WORK/install" \
    --disable-everything \
    --disable-autodetect \
    --disable-doc \
    --disable-ffplay \
    --disable-ffprobe \
    --disable-network \
    --disable-debug \
    --disable-iconv \
    --disable-gpl \
    --disable-nonfree \
    --enable-small \
    --enable-static \
    --disable-shared \
    --enable-protocol=file \
    --enable-demuxer=avi,mov,matroska,ogg,wav,mp3 \
    --enable-muxer=ogg \
    --enable-decoder=h264,hevc,mpeg4,mjpeg,theora,vorbis,aac,mp3,ac3,eac3,pcm_s16le,vp8,vp9,opus,mpeg2video \
    --enable-parser=h264,hevc,mpeg4video,mjpeg,aac,theora,vorbis,vp8,vp9,opus \
    --enable-encoder=libtheora,libvorbis \
    --enable-filter=scale,fps,format,aresample,aformat,setpts \
    --enable-swscale \
    --enable-swresample \
    --enable-libtheora \
    --enable-libvorbis \
    --pkg-config-flags="--static" \
    --extra-cflags="-Os" \
    --extra-ldflags="-s -static -static-libgcc"   # 不静态链 gcc 运行时 ⇒ 空目录跑不起来（需 libwinpthread-1.dll）

make -j"$(nproc)"
make install
cp "$WORK/install/bin/ffmpeg.exe" "$OUT/ffmpeg.exe"
strip "$OUT/ffmpeg.exe" || true
ls -l "$OUT/ffmpeg.exe"
echo "BUILD-FFMPEG-MIN ok -> $OUT/ffmpeg.exe"
