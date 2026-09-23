#!/usr/bin/env python3
"""Compare the frames shotcheck.sh grabbed for two arms.

Two arms that render the same thing should differ by nothing, but a naive comparison
says they always differ. The two arms are separate launches, so the grabs do not land
on the same frame number, and anything keyed to frame_count - the alpha dither, and
any particle the freeze does not stop - moves between them regardless of the arms.

So the comparison is masked. Grabs within one slot establish which pixels are unstable
on their own; only the pixels that hold still in both slots are asked about. What is
left is the systematic difference between the arms, which is the thing being tested.

Even that is not enough, because the scene is not reproducible across launches: a fairy
or a particle comes to rest somewhere slightly different each time, holds still there for
the whole run, and so survives the mask. At the Kokiri fixture that alone is ~10000 pixels
with a worst channel near 240 - which is why shotcheck.sh always runs a third slot with
arm A again. The verdict is B-against-A measured against A-against-A, and nothing else.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# A pixel is "moving" if two grabs of the same arm disagree by more than this on any
# channel. Deliberately low: it is better to exclude a pixel that might have been
# interesting than to report animation as a rendering change.
MOVE = 4
# What counts as a difference once the moving pixels are out of the way.
DIFF = 4


def load(out_dir: Path, slot: str, required: bool = True):
    paths = sorted(out_dir.glob(f"shot-{slot}-*.png"))
    if not paths:
        if not required:
            return [], None
        sys.exit(f"no frames for slot {slot} in {out_dir}")
    imgs = [np.asarray(Image.open(p).convert("RGB"), dtype=np.int16) for p in paths]
    return [p.name for p in paths], np.stack(imgs)


def moving(frames: np.ndarray) -> np.ndarray:
    """Pixels that differ between grabs of one slot: animation, not the arm."""
    if len(frames) < 2:
        return np.zeros(frames.shape[1:3], dtype=bool)
    return (np.abs(frames[1:] - frames[0]).max(axis=3) > MOVE).any(axis=0)


def main() -> None:
    out_dir = Path(sys.argv[1])
    val_a, val_b = sys.argv[2], sys.argv[3]
    names_a, fa = load(out_dir, "A")
    names_b, fb = load(out_dir, "B")
    names_n, fn = load(out_dir, "N", required=False)
    if fa.shape[1:] != fb.shape[1:]:
        sys.exit(f"size mismatch: {fa.shape[1:]} vs {fb.shape[1:]}")

    h, w = fa.shape[1:3]
    print(f"{w}x{h}, A ({val_a}) {len(fa)} frames, B ({val_b}) {len(fb)} frames"
          + (f", control ({val_a} again) {len(fn)} frames" if fn is not None else ""))

    def compare(x_frames, y_frames, label):
        stable = ~(moving(x_frames) | moving(y_frames))
        n_stable = int(stable.sum())
        best = None
        for xi, x in enumerate(x_frames):
            for yi, y in enumerate(y_frames):
                dmax = np.abs(x - y).max(axis=2)
                hits = int(((dmax > DIFF) & stable).sum())
                if best is None or hits < best[0]:
                    best = (hits, xi, yi, dmax)
        hits, xi, yi, dmax = best
        worst = int(dmax[stable].max())
        print(f"  {label}: {hits} stable pixels differ ({100 * hits / n_stable:.3f}%), "
              f"worst channel {worst}/255")
        return hits, worst, xi, yi, dmax, stable

    hits, worst, xi, yi, dmax, stable = compare(fa, fb, f"B vs A   ({val_b} vs {val_a})")

    vis = np.minimum(255, np.abs(fa[xi] - fb[yi]) * 16).astype(np.uint8)
    vis[~stable] = 0
    path = out_dir / f"diff-{val_a}-vs-{val_b}.png"
    Image.fromarray(vis).save(path)
    print(f"  wrote {path} (x16, moving pixels blacked out)")

    if fn is None:
        print("NO CONTROL: run shotcheck.sh, which captures one. A bare count is not a verdict.")
        sys.exit(1)

    null_hits, null_worst, *_ = compare(fa, fn, f"control  ({val_a} vs {val_a})")

    # The control is the same measurement with nothing changed, so the arms have to beat it
    # by a margin rather than merely be nonzero. Half again is deliberately loose: this
    # separates "a material renders differently" from "a fairy landed somewhere else", and
    # it is not trying to resolve anything finer than that.
    if hits <= null_hits * 1.5:
        print(f"PASS: {hits} against a control of {null_hits}. No difference this can see.")
    else:
        print(f"DIFFER: {hits} against a control of {null_hits}, worst {worst}/255 "
              f"(control {null_worst}). Look at the diff image.")
        sys.exit(2)


if __name__ == "__main__":
    main()
