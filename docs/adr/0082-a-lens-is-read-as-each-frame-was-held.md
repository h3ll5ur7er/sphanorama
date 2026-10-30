# 0082 — A lens is read as each frame was held

**Status:** accepted; amends [0070](0070-a-build-reads-the-capture-and-is-paced-by-poll.md), whose
frames had to be one size, and [0067](0067-a-device-keeps-its-lens-and-a-fit-must-beat-it.md),
whose kept lens is keyed by the frames' orientation

## Context

`Begin` takes the field-of-view pair from the camera as the phone is held when the capture starts,
and the page names the wide angle onto whichever screen axis is long at that moment. The spec keeps
that pair for the life of the capture, and across every resume since ADR 0077. The frames follow the
phone: a browser hands the camera's picture back upright on the screen, so a phone turned a quarter
returns its frames the other way round.

The build paired the stored angles with the frames' own size, one lens for every frame. So a capture
with bursts taken both ways round was refused by `PanoramaBuildManager::Start` as "not all one
size", after it had been written down as complete. A capture taken wholly the other way round
from how it began built with pixels non-square by the square of the frame's aspect, 1.78 for 4:3
and 3.16 for 16:9, since the page's lens has square pixels. The single focal scale `Refine` fits
(ADR 0066) cannot correct that.

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
    turns: its centre in the middle of its frame to a millionth of a pixel, no tangential
    distortion and no rolling shutter. Every lens the app makes is one: `LensFromFieldOfView`,
    `Refine`'s fit of it, which scales only the focal lengths, and `KeptLensFor`'s rescaling of a
    kept one, whose centre can land a rounding off the middle (1288 by 966 from 1280 by 960 gives
    483.00000000000006), which is why the middle is not asked for exactly.
- **The build reads the stored pair against the sensor's edges.** The wider angle goes on the
  longer edge, whichever axis the capture began with it on, and the capture's lens is long edge
  across whichever way its first frame was held, so `Refine` fits one lens for every capture of a
  device. Every frame must be the first frame's shape or that shape turned, or `Start` refuses.
- **Every engine reads each frame through the lens turned to it.**
  - `FeatureSet` gains the shape of the frame its keypoints are pixels of, filled by
    `ExtractFeatures`, and `EstimatePairwise` reads each set through its own lens.
  - `FramePrior` gains its frame's shape, and `Refine` reads each frame's matches by it. The shape
    is carried on the prior because the priors are the frame set: one per frame, where a pair's
    would be two copies to disagree.
  - `RenderPreview` reads the shape of each `FrameRef` it is handed.
  - A frame whose shape the lens cannot be read for is `InvalidArgument` in all three, a shape of
    zero included. So is a keypoint or a match outside the frame its set or its prior names: a
    shape is a second copy of a fact the pixels also carry, and a shape the lens reads that is not
    the frame's would read every pixel the wrong way round. A ring with every other frame turned,
    declared unturned, solved six degrees out; an unturned frame declared turned put the worst frame
    eleven out. Both called the fit precise.
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
- **A turned ring registers, worse than the unturned one.** The ring is rendered through a lens 66
  by 50 degrees, so a turned frame is 50 degrees across where it was 66.

  | Ring | Overlap of neighbours | ORB | AKAZE | SIFT |
  |------|----------------------:|----:|------:|-----:|
  | unturned | 36° | 0.046 | 0.029 | 0.014 |
  | every other frame turned | 28° | 0.107 | 0.068 | 0.030 |
  | every frame turned | 20° | 0.060 | 0.053 | 0.035 |

  Median errors in degrees, each row a test in `registration_accuracy_test.cpp`. With every other
  frame turned the focal length is still fitted to within 0.08%; read through one unturned lens, the
  medians were about seven degrees.
  - **Registering across a quarter turn costs ORB and AKAZE**, and the overlap is not what does it.
    With every other frame turned every pair is between frames held a quarter turn apart, and those
    rings share more of the world than every frame turned yet register worse for both, while SIFT
    does not. A capture turned partway through has such pairs only where it turned, and a turned
    frame shares less of the world with each neighbour wherever it is.
  - **For AKAZE the cost is the half-pixel convention ADR 0061 measured and kept.** Keypoints are
    read half a pixel up and to the left of where the lens's convention puts them. Within one way
    round that offset is the same in every frame and the optical centre absorbs it; between frames
    held differently it points different ways in the lens's axes, and nothing absorbs it. With the
    correction applied to `ReadBearings`, measured and not applied here, AKAZE's ring with every
    other frame turned reads 0.036 against the unturned ring's 0.035. ORB keeps a cost, 0.121
    against 0.040, and SIFT reads 0.034 against 0.030. The correction moves every published figure,
    which is ADR 0061's reason for keeping it, and this one more than the rest; the turned rings are
    entries in the list of what it moves.
  - The reading itself is exact: a turned frame composes byte for byte as it did unturned, and
    exact matches between frames held either way solve to within a hundredth of a degree.
- **The pose is assumed to turn with the frame.**
  - The page reports the viewfinder's attitude, the screen's rotation applied (ADR 0017), and that
    is what makes a turned frame's pose the turned one.
  - The tests turn frame and pose together.
  - Nothing has measured a phone turned partway through a capture.
- **The plan is still laid for the phone as held at `Begin`, and that stays open.** A frame taken
  turned covers its cell narrower one way and wider the other. So the overlap planned between
  cells thins along one axis. And the page's sighting reads the stored pair onto the axes the
  capture began with, for each cell's footprint and for where its marker is drawn and whether it is
  on screen: on a 4:3 phone, turned, a cell at the screen's right edge is drawn 0.875 of the way
  across.
- **Some lenses cannot read a turned frame**, and `Refine`, `EstimatePairwise` and `RenderPreview`
  refuse it under them: a lens whose centre is off the middle, or that has tangential distortion or
  a rolling shutter. None is made today. A lens with fitted decentring or tangential distortion
  would need the direction of the turn, which the pose's roll carries and the shape does not.
- **A kept lens is one lens either way round.** ADR 0067 keyed a device's kept lens by the grabbed
  frames' shape with their orientation kept, so a phone turned between captures kept two, and
  refused a frame of another shape as a crop. Both are the same glass under this decision: the key
  is the shape's long edge to its short, and a frame of the kept shape turned is read through
  `LensForFrame` rather than refused. Nothing writes a kept lens yet, so this is decided for the
  writer, which reads the kept lens at the size of the build's lens, long edge across, never
  turned. Until then `AmendKeptLens` refuses a turned shape, and `KeptLensFor` answers one with
  `FailedPrecondition`, the code ADR 0067 discards the kept document on; a writer asking for it
  turned would lose a good lens.
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
