# N-Gon 0.2.3 for Unreal Engine 5.8

N-Gon is a project-local C++ editor plugin for picking authored polygons, edges and logical vertices. It reads raw FBX polygon loops and previews them over a triangulated surface.

## Install

1. Close Unreal Editor and extract the ZIP into your C++ project's `Plugins` directory. The descriptor goes at `Plugins/NGon/NGon.uplugin`. If you installed the earlier `NGonPreview` plugin, disable it when switching to this build.
2. Put your raw default mesh at **`Plugins/NGon/Meshes/Default.fbx`**. If the folder contains exactly one FBX, it can have any filename. The folder is alongside `NGon.uplugin`.
3. Build your project's **Editor** target with your UE 5.8 source build. On Windows, replace the sample paths and target name with yours.

   ```bat
   "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" MyProjectEditor Win64 Development -Project="D:\Projects\MyProject\MyProject.uproject" -WaitMutex
   ```

4. Open the project and choose **Window → N-Gon**. The default FBX loads when the tab opens. If it is missing, ambiguous, or unreadable, the tab opens the built-in demo and shows the reason.

When updating an existing installation, keep your `Plugins/NGon/Meshes` folder and rebuild the Editor target with Unreal closed.

## Try it

| Control | What to check |
| --- | --- |
| **Reload Default** | Replace the FBX on disk and click to reload it. A failed reload keeps the current preview. |
| **Open FBX** | Opens a browser in `Meshes` when that folder exists. Choose another raw FBX to preview. |
| **Static Mesh** | Choose an Unreal asset to inspect its editable LOD 0. Use the raw FBX for polygons that the Unreal import already triangulated. |
| **Face / Edge / Vertex** | Click to select. Shift-click adds to the active selection. A face highlights all its underlying triangles. |
| **Gizmo frame** | The gizmo follows the selected component like Maya/TiGUI: faces use centroid + normal + perimeter tangent, edges use midpoint + edge direction + adjacent normal, and vertices use incident-face normals. Multiple components use weighted averages. Enable **Active Frame** to pivot/orient from the active component. |
| **Transform tool** | Click the viewport and select a face, edge, vertex, or joint. Press **W** for Translate, **E** for Rotate, **R** for Scale, or **Q** to hide the gizmo while selecting. The handles draw above the surface, including on joints inside the mesh. Drag a handle to transform the selection and press **Esc** during a drag to restore the starting pose. The toolbar also provides Combined mode. |
| **Joints and components** | Press **4** or choose a joint in the Skeleton Tree to target bones. Press **1**, **2**, or **3** to pick mesh vertices, edges, or faces. |
| **Shading** | Choose **Smooth**, **Faceted**, or **Wireframe** from the viewport toolbar. Unreal mesh assets display their assigned material slots under preview lighting. The demo and raw FBX use a neutral material. Enable **Vertices** in the toolbar to see every logical vertex on an imported mesh. |
| **Delete Faces** | In Face mode, select one or more faces and click **Delete Faces** or press **Delete/Backspace**. The preview rebuilds the remaining authored polygons, refreshes picking/overlays/gizmo state, and refuses to delete the final face. |
| **Soft Heatmap** | Enable Soft Selection, select a face, edge, or vertex, then toggle Soft Heatmap. The surface uses the same blue-to-green-to-orange influence ramp as the soft-selection markers; changing radius, metric, connectivity, or gizmo deformation updates it. |
| **Bend** | Scrub the position test and check that surface picking and visible overlays follow it. The **Positions** counter advances. |
| **Load Demo** | Check for 3 faces, 8 triangles, 12 edges and 10 logical vertices. Click both sides of the hidden diagonal in a face, then click inside the concave notch to test a miss. |

Choose **Window → N-Gon UV Editor** to open the native Slate UV editor. It reads the primary UV overlay from the same raw FBX/default mesh and keeps overlay element IDs intact, so authored seams remain separate UV corners. Corner, edge, face and island modes support Shift-add and Ctrl marquee selection; left-drag moves the selected UVs, middle/right drag pans, and the wheel zooms around the cursor. The toolbar provides Fit, 90-degree rotation, Flip U/V, Normalize and Reload. The grid, UV triangle outlines, corner markers, selected highlighting and status line provide the same inspection feedback as the TiGUI UV canvas.

Use Alt + left drag to orbit, scroll to zoom, and **F** to frame the current selection or the full mesh. The gizmo keeps its starting frame through each drag so soft selection stays stable.

For a skeletal viewport check, load your mesh and select **spine_01** or a comparable joint in the Skeleton Tree. Press **W** and drag an arrow over the character. Press **E** and drag a ring, then repeat from another orbit angle. Press **Q** to hide the handles. Press **3** and click a face to check component selection.

## FBX export and preview

- In Maya or your DCC, export the original polygon geometry with **Triangulate** disabled. Each polygon needs a simple perimeter. The loader reports when the file contains triangle faces only.
- The loader applies FBX node and geometric transforms and converts the result to Z-up centimeters. The viewport combines the polygon mesh nodes in the file into a single preview.
- The first vertex color layer's red channel is imported as source metadata for compatibility. The viewport heatmap is driven by active soft-selection influence, so it follows the selected seeds and the current radius, metric, connectivity, and transform mode. Missing source samples still use a longest-axis fallback for inspection data.
- FBX previews use the editor's preview material. Skin deformation, animation and source materials are outside this build. UV edits are held in the editor document for the active UV tab; use Reload to restore the source mesh.

## Why the update uses these APIs

`NGonFbx` reads original polygon loops from the SDK. `NGonTopology` builds polygons in `FMeshDescription` and maps the resulting triangles back to source face IDs. `NGonPicking` uses an AABB tree for face hits and screen-space snapping for edges and vertices.

`NGonPreviewScene` keeps the asset material IDs and vertex colors. Face selection uses a secondary render pass, and heatmap colors use a temporary render override. The viewport projects transform handle geometry once per frame for drawing and projects the same geometry on mouse-down for picking. Bend updates dynamic mesh positions and the picking tree in place.

[Epic's mesh description polygon API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/MeshDescription/FMeshDescription/CreatePolygon) · [Epic's attribute update constraints](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/GeometryFramework/UDynamicMeshComponent/NotifyMeshVertex-) · [Autodesk's FBX mesh API](https://help.autodesk.com/cloudhelp/2020/ENU/FBX-API-Reference/cpp_ref/class_fbx_mesh.html)

## Run the tests

Run `Automation RunTests NGon` in the Unreal Editor console. The suite covers demo topology and picking, edge loops, face deletion, default FBX precedence, FBX import, projected gizmo handles, and restoration of asset materials and vertex colors after paint or heatmap use.

```bat
"D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\Projects\MyProject\MyProject.uproject" -ExecCmds="Automation RunTests NGon" -unattended -nopause -nullrhi -log -TestExit="Automation Test Queue Empty"
```

This archive contains source. Run the native build and use the checklist in your UE 5.8 editor to verify the viewport.
