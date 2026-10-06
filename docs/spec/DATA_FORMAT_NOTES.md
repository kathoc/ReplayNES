# DATA FORMAT NOTES
InputRecord proposal:
- frame delta (varint)
- P1 bitfield u8
- P2 bitfield u8
- event flags
Fix the bit order and manage it with a version.

Checkpoint metadata:
stateID, frameIndex, parentSegmentID, coreCompatibilityID, stateFormatVersion, checksum.

Timeline:
immutable segment = startFrame + parent + input records + optional end checkpoint.
A branch means multiple children of the same parent segment. activeHeadID selects the completed take.

Verification hashes are not the sole basis for product file compatibility; they are also used to detect replay divergence.
