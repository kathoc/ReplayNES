# DATA FORMAT NOTES
InputRecord案:
- frame delta (varint)
- P1 bitfield u8
- P2 bitfield u8
- event flags
bit orderを固定しversion管理する。

Checkpoint metadata:
stateID, frameIndex, parentSegmentID, coreCompatibilityID, stateFormatVersion, checksum.

Timeline:
immutable segment = startFrame + parent + input records + optional end checkpoint.
branchは同じparent segmentから複数childを持つ。activeHeadIDで完成takeを選ぶ。

検証用hashは製品ファイル互換性の唯一の根拠にせず、再生divergence検出にも使う。
