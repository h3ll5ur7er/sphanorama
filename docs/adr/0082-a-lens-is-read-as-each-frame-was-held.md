# 0082 — A lens is read as each frame was held

**Status:** accepted

## Context

`Begin` takes the field-of-view pair from the camera as the phone is held when the capture starts,
and the page names the wide angle onto whichever screen axis is long at that moment. The spec keeps
that pair for the life of the capture, and across every resume since ADR 0077. The frames follow the
phone: a browser hands the camera's picture back upright on the screen, so a phone turned a quarter
returns its frames the other way round.

The build paired the stored angles with the frames' own size, one lens for every frame. So a capture
with bursts taken both ways round was refused by `PanoramaBuildManager::Start` as "not all one
size", after it had been written down as complete. A capture taken wholly the other way round
from how it began built with pixels about 1.9 times non-square, which the single focal scale
`Refine` fits (ADR 0066) cannot correct.

Three answers were put to the maintainer: lock the screen's orientation for a capture; store the
angles against the sensor's long and short edges; or swap the pair per frame. The second was chosen.

## Decision

- **A lens is the glass, and a frame's shape says how it was held.**
  - `LensForFrame(lens, width, height)` in `utilities/camera_model` answers the lens itself for a
    frame of its shape.
  - For a frame of its shape turned, it answers the lens turned a quarter: focal lengths, centre
    and size swapped.
  - For anything else it answers a lens `IsUsableLens` refuses.
  - The shape cannot say which way the phone turned, so only a lens that reads the same either way
    turns: its centre exactly in the middle of its frame, no tangential distortion and no rolling
    shutter. Every lens the app makes is one: `LensFromFieldOfView`, and `Refine`'s fit of it,
    which scales only the focal lengths.
- **The build reads the stored pair against the sensor's edges.** The wider angle goes on the
  longer edge of the first frame, whichever axis the capture began with it on. Every frame must be
  that shape or that shape turned, or `Start` refuses.
- **Every engine reads each frame through the lens turned to it.**
  - `FeatureSet` gains the shape of the frame its keypoints are pixels of, filled by
    `ExtractFeatures`, and `EstimatePairwise` reads each set through its own lens.
  - `FramePrior` gains its frame's shape, and `Refine` reads each frame's matches by it. The shape
    is carried on the prior because the priors are the frame set: one per frame, where a pair's
    would be two copies to disagree.
  - `RenderPreview` reads the shape of each `FrameRef` it is handed.
  - A frame whose shape the lens cannot be read for is `InvalidArgument` in all three, a shape of
    zero included.
- **The stored spec does not change.** Its pair still names the axes the plan was laid on, which
  the planner and the page's overlay read. The build reads the same two numbers as a long edge and
  a short one, so no document format moves.

## Consequences

- **A turned capture builds.** On the photograph ring with its last six frames turned, each with
  its pose turned with it, the preview is within the following mean of the photograph:

  | Detector | Turned capture | Unturned ring |
  |----------|---------------:|--------------:|
  | ORB      | 2.13           | 1.56          |
  | AKAZE    | 1.78           | 1.37          |
  | SIFT     | 1.35           | 1.23          |

  Before this change, `Start` refused that capture.
- **A turned ring registers, worse than the unturned one, and the cause is the frames.** With every
  other frame turned, the median errors are 0.107 (ORB), 0.068 (AKAZE) and 0.030 (SIFT) degrees,
  against the unturned ring's 0.046, 0.029 and 0.014, and the focal length is still fitted to
  within 0.08%. Read through one unturned lens, the medians were about seven degrees.
  - A turned 640×480 frame is 46.8 degrees across where it was 60, so neighbours thirty degrees
    apart share 16.8 degrees of the world rather than 30.
  - Every frame turned measures 0.060, 0.053 and 0.035. A ring rendered upright at 480×640 with
    nothing turned measures 0.060 (AKAZE) and 0.036 (SIFT), so the turn itself adds nothing.
  - A turned frame composes byte for byte as it did unturned.
- **The pose is assumed to turn with the frame.**
  - The page reports the viewfinder's attitude, the screen's rotation applied (ADR 0017), and that
    is what makes a turned frame's pose the turned one.
  - The tests turn frame and pose together.
  - Nothing has measured a phone turned partway through a capture.
- **The plan is still laid for the phone as held at `Begin`, and that stays open.** A frame taken
  turned covers its cell narrower one way and wider the other. So the overlap planned between
  cells thins along one axis, and the overlay draws each cell's footprint with the axes the
  capture began with.
- **Some lenses cannot read a turned frame**, and `Refine`, `EstimatePairwise` and `RenderPreview`
  refuse it under them: a lens whose centre is off the middle, or that has tangential distortion or
  a rolling shutter. None is made today. A lens with fitted decentring or tangential distortion
  would need the direction of the turn, which the pose's roll carries and the shape does not.
- **Every caller now says each frame's shape.** A `FramePrior` or `FeatureSet` left at zero is
  refused rather than read as the lens's shape. Both are mirrored into the TypeScript contract and
  neither crosses the boundary.

## Rejected alternatives

**Lock the screen's orientation for a capture.** It is the smallest change. But the Screen
Orientation API locks only in full screen on most browsers and not at all on some, so the case it
exists for would still reach the build. And a user turned mid-capture by the lock would see the
viewfinder sideways.

**Turn the pixels at capture so every frame has the first frame's shape.** The engines would then
need nothing. But it is pixel work on every burst frame on the phone, in the page or a manager, both
of which are the wrong place for it. It also needs the direction of the turn, which the frame's
shape cannot give, so the pose's roll would have to choose it. And it hides how each frame was
taken from everything after it.

**Store the long and short angles in the spec, with an orientation.** A contract and a document
format change, for a fact the frames already carry, while the planner still wants the axes it laid
the plan on. Reading the stored pair as long and short gives the build the same thing.

**Carry each frame's shape on its pair.** `PairwiseResult` would hold two shapes per pair, and a
frame in four pairs would have four copies to disagree with each other.
