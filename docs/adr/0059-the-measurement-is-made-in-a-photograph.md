# 0059 — The measurement is made in a photograph, and the photograph says where it came from

**Status:** accepted

## Context

ADR 0055 has the accuracy test render its own dataset. What it rendered *from* was
`_checkerboard_panorama`, and a checkerboard is the one world a feature matcher must not be scored
in: it is periodic, so a match onto the wrong square is agreed on by as many neighbours as a match
onto the right one, and it is infinitely sharp, so nothing a lens does to a real scene is present.
The roadmap already carried two consequences of that. ORB declined three of eleven pairs there
because hundreds of its corners were genuinely indistinguishable, and before the sensor prior
bounded the search, two detectors returned half-turn rotations that fit the pixels exactly, because
the checkerboard is invariant under one. The second was written down as "the panorama's symmetry is
itself worth removing, and is not removed yet."

So: a photograph. Which panorama is a licensing question before it is a technical one. A binary in a
repository carries no author, no licence and no origin, and six months later nobody can supply any
of the three — at which point the only safe move is to delete it and everything measured against it.

## Decision

**`core/test/data/panoramas/small_hangar_01_1k.jpg`**, CC0, copied byte for byte from
`google/model-viewer` at a recorded commit, with `sources.json` beside it naming the work, the
author, the licence, the upstream path and the digest of the bytes.

**Kept as received rather than transcoded.** A digest is only checkable against upstream if the
bytes are upstream's, so converting to a format the standard library reads — P6, which is what this
repository's datasets are written in — would have destroyed the one thing that makes the record
verifiable. That decides the dependency: **Pillow, in the `datasets` group**, never in the checkers,
which stay standard-library only (ADR 0048, ADR 0050).

**`tools/asset_provenance.py` keeps the record honest.** A directory holding a `sources.json` is an
asset directory, and the build fails if a file there has no entry, an entry is missing a field or
holds a blank one, two entries claim one file, or the bytes no longer hash to what is recorded. The
last is the one that matters most: without it a swapped file inherits the clearance of the one it
replaced, keeping its name, its licence and its URL.

**Both worlds stay, named.** `Rendered` takes a `World`, and `Acceptance` still renders a
checkerboard — because across all 165 combinations of the ring's eleven pairs, three detectors and
priors perturbed from one to six degrees, the hangar's *lowest* inlier fraction is 0.652. Nothing in
it can produce the answered-but-not-accepted outcome that test exists to catch, and ADR 0056's
figures are the checkerboard's and are untouched.

## Consequences

- **The numbers moved, and in the direction the reasoning predicted.** Against the hangar all three
  detectors register all eleven pairs, where ORB registered eight of eleven against the
  checkerboard: medians 0.024° (SIFT), 0.061° (AKAZE) and 0.101° (ORB). The photograph is an
  *easier* world than the checkerboard for ORB, which was declining pairs it could gather no
  consensus on. `docs/06-roadmap.md` carries the table.
- **The frames are soft.** 1024 by 512 is 2.84 pixels per degree and a 640 by 480 frame at 66
  degrees is 9.7, so every rendered frame is upsampled about three and a half times. That cost is
  paid and measured rather than argued about; a sharper world is a bigger file, and see the rejected
  alternatives.
- **A dataset's exact bytes now depend on a JPEG decoder** when `--panorama` is given, so on a
  Pillow version rather than on this repository alone. The committed format fixture is unaffected:
  `core/test/data/synthetic-ring-4` is rendered from the checkerboard, and the test that compares it
  byte for byte against a fresh render never reads an image file (ADR 0053).
- **Every asset added from here needs a record**, which is the cost and also the point.

## Rejected alternatives

**Transcode to P6 and add no dependency.** Tempting, because this repository already reads and
writes P6 in a dozen lines and the checkers would have stayed standard-library only. Rejected: the
committed file would then be a derivative whose fidelity nobody could check against anything, the
digest would attest to our conversion rather than to the published work, and 1024 by 512 as P6 is
1.5 MB against 179 KB.

**A larger panorama.** Four repositories were read for one whose licence is stated where the bytes
are — `mrdoob/three.js`, `KhronosGroup/glTF-Sample-Environments`, `BabylonJS/Assets` and
`google/model-viewer` — and every candidate that passed was 1k. three.js carries a 4k panorama and
attributes none of them; Khronos names its sources but not their licences, and those sources are
research-use and non-commercial archives; Babylon states CC0 per-directory for one HDRI that is
26 MB in Radiance format.

**A better-looking 1k from the same repository.** `whipple_creek_regional_park_1k_HDR.jpg` is a
forest — 7% of it is flat against the hangar's 22%, and it has texture in every band. Its
attribution names only the `.hdr` it was derived from. Both readings of that file are permissive and
it would almost certainly have been fine, which is exactly the sentence that should not appear in a
provenance record.

**Shooting our own.** The maintainer offered to capture spheres with a Pixel, and that is the better
answer for fidelity — a real lens, real noise, real exposure variation, and a licence with no
question in it at all. It is not this change: this one needed a photograph today, and the checker
and the reader built here are what any captured sphere will arrive through.
