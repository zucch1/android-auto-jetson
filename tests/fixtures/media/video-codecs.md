# Synthetic codec fixtures

The three `*_480p30_gray_1f` files are one synthetic gray 800x480 frame at
30 fps, generated locally with FFmpeg. No captured phone or third-party media.
IVF contains a 32-byte file header and one 12-byte frame header; the decoder
test strips those and submits the encoded frame payload. HEVC is Annex B.

```
ffmpeg -f lavfi -i color=gray:size=800x480:rate=30 -frames:v 1 -c:v libx265 -threads 1 -x265-params pools=1:frame-threads=1:log-level=error -f hevc h265_480p30_gray_1f.hevc
ffmpeg -f lavfi -i color=gray:size=800x480:rate=30 -frames:v 1 -c:v libvpx-vp9 -threads 1 -f ivf vp9_480p30_gray_1f.ivf
ffmpeg -f lavfi -i color=gray:size=800x480:rate=30 -frames:v 1 -c:v libaom-av1 -cpu-used 8 -threads 1 -f ivf av1_480p30_gray_1f.ivf
```

These verify software parsing, decoding and I420 delivery when the respective
runtime elements are installed. They do not qualify phone interoperability,
higher-resolution performance or hardware decode.
