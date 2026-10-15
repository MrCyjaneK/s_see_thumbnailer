# thumbnailer format

## v1.0.0

JPEG thumbnail sidecar for images and videos. Long edge is at most 400px (never upscaled). Quality ≈ 50.

On success the artifact is a JPEG file (SOI `FF D8`).

On failure the same `.jpg` path is filled with UTF-8 error text (non-empty) so the host still uploads it. The module always reports `progressString == "done"` unless writing the output file itself fails.