# Vendored: waybeam frame-shm ring

`venc_frame_ring.h` / `venc_frame_ring.c` are copied verbatim from waybeam
(`waybeam_venc`, branch `artosyn`, `505433e`: format version 2 with
`VENC_FRAME_FLAG_DISCONT` and `VENC_FRAME_CODEC_OPUS`), by way of
ar8030-transport's own copy (`third_party/waybeam_frame_ring/`).

PixelPilot is a consumer: with `--frame-shm <name>` it reads the frames
`ar8030-transport-rx -R <name>` publishes, instead of RTP on a UDP port
(ar8030-transport `docs/ground-frame-ring.md`). It only attaches, reads and
destroys; rx creates the ring.

- License: MIT (see `LICENSE` in this directory, copied from upstream)
