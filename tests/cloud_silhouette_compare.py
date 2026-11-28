"""Compare full-volume optical coverage with legacy and directional proxy.

This is a software diagnostic, not exact raymarch ground truth or GPU timing.
Reference uses the same volume and clipped world-axis slice lattice without
canopy or far billboards. No change to the sampled pose or cloud state.
"""
import os
import sys
from pathlib import Path

import numpy as np
from banks_preview import render

def capture(exe, scene, mode, output):
    for name in ("VC_FULL_VOLUME", "VC_LEGACY", "VC_HALF_RES", "VC_NO_TRIM"):
        os.environ.pop(name, None)
    os.environ["VC_ALPHA"] = "1"
    if mode == "reference":
        os.environ["VC_FULL_VOLUME"] = "1"
    elif mode == "legacy":
        os.environ["VC_LEGACY"] = "1"
    im = render(scene, exe)
    if output:
        im.save(output / f"{scene}_{mode}_alpha.png")
    # Exclude the diagnostic heading, which is not part of the sky.
    return np.asarray(im, dtype=np.float32)[25:, :, 0] / 255.

def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: cloud_silhouette_compare.py BAKED_PREVIEW OUTPUT [SCENES...]")
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    for scene in sys.argv[3:] or ["ground", "zenith", "handover0", "entry"]:
        ref = capture(sys.argv[1], scene, "reference", output)
        for mode in ("legacy", "directional"):
            img = capture(sys.argv[1], scene, mode, output)
            print(f"{scene} {mode}: alpha MAE={np.mean(np.abs(img-ref)):.5f}", flush=True)
            for threshold in (.03, .1, .25, .5):
                m, r = img > threshold, ref > threshold
                union = np.count_nonzero(m | r)
                iou = np.count_nonzero(m & r) / max(1, union)
                missing, extra = np.mean(~m & r), np.mean(m & ~r)
                print(f"  alpha>{threshold:g}: IoU={iou:.4f}, missing={100*missing:.2f}%, extra={100*extra:.2f}%", flush=True)

if __name__ == "__main__":
    main()
