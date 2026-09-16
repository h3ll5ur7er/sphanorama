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

**And committing one at all needs saying, because ADR 0053 drew a line here**: *a format fixture may
be committed, a measurement dataset may not*, for two reasons — a dataset big enough to measure a
detector on is megabytes, and a committed one is a snapshot of a generator that has since moved. A
source panorama is a third thing that line does not name, and neither reason reaches it. 179 KB is
not megabytes, and a photograph is not a snapshot of our generator: it is the input the generator
reads, and it is the one thing in the loop that is *supposed* to stay fixed while everything around
it moves. It is committed for the same reason the fixture is — a number nobody can reproduce is not
a measurement.

## Decision

**`core/test/data/panoramas/small_hangar_01_1k.jpg`**, CC0, copied byte for byte from
`google/model-viewer` at a recorded commit, with `sources.json` beside it naming the work, the
author, the licence, the upstream path and the digest of the bytes.

**Kept as received rather than transcoded.** A digest is only checkable against upstream if the
bytes are upstream's, so converting to a format the standard library reads — P6, which is what this
repository's datasets are written in — would have destroyed the one thing that makes the record
verifiable. That decides the dependency: **Pillow, in the `datasets` group**, never in the checkers,
which stay standard-library only (ADR 0048, ADR 0050).

**`tools/asset_provenance.py` keeps the record honest.** Every tracked asset must have an entry in
the nearest `sources.json` above it, and the build fails if one has none, if an entry is missing a
field or holds a blank one, if two entries claim one file, or if the bytes no longer hash to what is
recorded. The last matters most: without it a swapped file inherits the clearance of the one it
replaced, keeping its name, its licence and its URL.

**What counts as an asset is two rules, because neither alone is right.** Bytes that are not valid
UTF-8 cannot be source here. And a name whose extension is a media format is somebody's work even
when it decodes — which is not hypothetical: adding that rule immediately found `shell/public/icon.svg`,
unrecorded in this repository since the PWA shell landed.

**Our own work is recorded too, under `ours`**, with an author, a licence and a digest but no
upstream trail. The first shape of that asked only for a command, which made it an escape hatch: a
third-party file was one sentence — "we made this ourselves" — away from cleared, with the licence
question skipped entirely. No checker can refuse a false licence, and that is a lie rather than a
hole; what a checker can refuse is the question going unasked. Where an entry carries a
`produced_by` command, `tools/test_synth_dataset.py` **runs it** and compares the bytes, so a
recorded command that stops reproducing its output fails the build instead of ageing into fiction.

**Both worlds stay, named.** `Rendered` takes a `World`, and `Acceptance` still renders a
checkerboard — because across all 165 combinations of the ring's eleven pairs, three detectors and
five perturbations of the prior (1, 2, 3, 4 and 6 degrees about x), the hangar's *lowest* inlier
fraction is 0.652. Nothing in
it can produce the answered-but-not-accepted outcome that test exists to catch, and ADR 0056's
figures are the checkerboard's and are untouched.

**Six rules the checker gained after this decision was first written, recorded here because this
section reads as the complete list of what a record must say.** A raster entry — an extension in
`asset_provenance.SHAPED` — must carry `width` and `height`, which the checker cannot verify itself
and `tools/test_synth_dataset.py` does where Pillow is present; leaving it optional meant the one
fact needing a decoder could be deleted from a record with nothing going red. And an entry recording
a `projection` must use a token from `asset_provenance.PROJECTIONS`, because a test branches on that
field: while the panorama's record spelled it as a description, the 2:1 assertion keyed on it was
dead for every record in the tree.

And four more, which arrived in review rather than in this decision and are the reason this
paragraph is a running list rather than a closed one:

- **A licence for our own work is checkable or it is not a licence.** An `ours` entry says exactly
  `"same as this repository"` — which must resolve to a non-empty licence file *in the index* — or
  names a licence with a `licence_url` beside it, the bar `REQUIRED` already sets for `assets`.
  Four rounds tried instead to recognise a deferral loosely and each was defeated by a spelling the
  last had not thought of; the rule that ended it asks what a record has behind it rather than what
  its prose resembles.
- **A record is a claim about the committed tree.** A `sources.json` that is not in the index
  accounts for nothing in anybody's checkout, and said so only after a reviewer found it clearing a
  tracked asset here and failing in a fresh clone of the same commit.
- **A field a program reads is a single string**, by type and before the prose rule. A `produced_by`
  spelled as a list cleared the checker and died in its consumer; the same was then true of
  `source_blob` and `licence` inside the commit that fixed it, which is why `READ_BY_A_PROGRAM` is a
  tuple and the suite's cases are generated from it.
- **A `file` names a file in the record's own directory.** Absolute paths, `..`, symlinks and
  anything resolving outside are refused, because a digest satisfied by a path outside the
  repository pins nothing.

## Consequences

- **The numbers moved, and in the direction the reasoning predicted.** Against the hangar all three
  detectors register all eleven pairs, where ORB registered eight of eleven against the
  checkerboard: medians 0.024° (SIFT), 0.061° (AKAZE) and 0.101° (ORB). The photograph is an
  *easier* world than the checkerboard for ORB, which was declining pairs it could gather no
  consensus on. `docs/06-roadmap.md` carries the table.
- **The frames are soft.** 1024 by 512 is 2.84 pixels per degree and a 640 by 480 frame at 66
  degrees is 9.7, so every rendered frame is upsampled about 3.4 times. That cost is
  paid and measured rather than argued about; a sharper world is a bigger file, and see the rejected
  alternatives.
- **A dataset's exact bytes now depend on a JPEG decoder** when `--panorama` is given, so on a
  Pillow version rather than on this repository alone. The committed format fixture is unaffected:
  `core/test/data/synthetic-ring-4` is rendered from the checkerboard, and the test that compares it
  byte for byte against a fresh render never reads an image file (ADR 0053).
- **The figures this replaces are recorded here rather than deleted.** They were the live Phase 2
  table, and `docs/00-principles.md` makes withdrawing a published measurement an ADR trigger. This
  is not a withdrawal in ADR 0057's sense — nothing about them was wrong, and they are still true of
  the world they were taken in — but a reader who remembers them deserves better than their absence.
  Measured against the checkerboard, twelve-frame ring, prior three degrees out:

  | detector | pairs registered | median | mean | max |
  | -------- | ---------------- | ------ | ---- | --- |
  | AKAZE | 11 of 11 | 0.063° | 0.085° | 0.204° |
  | SIFT | 11 of 11 | 0.097° | 0.092° | 0.147° |
  | ORB | 8 of 11 | 0.068° | 0.095° | 0.219° |

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
research-use and non-commercial archives; Babylon states CC0 per-directory for one HDRI, which is 26 MB
because it is the 4k Radiance original.

**A better-looking 1k from the same repository.** `whipple_creek_regional_park_1k_HDR.jpg` is a
forest with texture in every band, where the hangar's floor is bare concrete. Quantitatively, and
naming the metric because the pair is meaningless without it: taking *flat* to mean a pixel whose
gradient magnitude is under 0.01 on a luma plane in the renderer's own [-1, 1] colour convention,
7% of whipple is flat against the hangar's 22%. A reviewer swept fourteen other definitions of flat
and could not land on that pair under any of them, which is the honest state of it — every one of
the fourteen agreed on the direction, by a factor of three to twenty, and none agreed on the
numbers. Its attribution names only the `.hdr` it was derived from. Both readings of that file are permissive and
it would almost certainly have been fine, which is exactly the sentence that should not appear in a
provenance record.

**Shooting our own.** The maintainer offered to capture spheres with a Pixel, and that is the better
answer for fidelity — a real lens, real noise, real exposure variation, and a licence with no
question in it at all. It is not this change: this one needed a photograph today, and the checker
and the reader built here are what any captured sphere will arrive through.
