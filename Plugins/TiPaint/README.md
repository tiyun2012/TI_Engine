# Ti Paint for Unreal Engine 5.8

Ti Paint is a native Unreal editor plugin port of the portable TiGUI painter foundation. Open it from **Window -> Ti Paint**.

The current port provides:

- A Slate paint canvas with pan, zoom, framing and a visible painted image preview.
- Brush and erase modes with radius, hardness and opacity controls.
- Surface-safe normalized UV document coordinates, suitable for the next 3D mesh painting milestone.
- Layered RGBA documents with active-layer selection, add/remove, visibility and lock controls.
- Copy-on-write-style stroke transactions at the document boundary, including cancel, undo and redo.
- PNG export through Unreal's ImageWrapper module.

The document and brush API is intentionally renderer-independent, following TiGUI's portable paint document boundary. This keeps layer history and paint rules separate from Slate and leaves room for the TiGUI surface projection, stencil, editable-region and material-channel features to be added as subsequent adapters. The current version does not silently modify an Unreal mesh or material asset.

## Verification

Build the editor target, then run the TiPaint automation suite with UnrealEditor-Cmd and `Automation RunTests TiPaint`.

`TiPaint.Document.BrushLayersUndo` verifies brush painting, erase-safe layer locking, layer creation, composite changes and exact undo/redo boundaries.
