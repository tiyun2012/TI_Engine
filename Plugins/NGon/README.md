# N-Gon for Unreal Engine 5.8

N-Gon is a project-local C++ editor plugin for picking authored polygons, edges and logical vertices. It reads raw FBX polygon loops and previews them over a triangulated surface.

## Install

1. Close Unreal Editor and extract the ZIP into your C++ project's `Plugins` directory. The descriptor goes at `Plugins/NGon/NGon.uplugin`. If you installed the earlier `NGonPreview` plugin, disable it when switching to this build.
2. Put your raw default mesh at **`Plugins/NGon/Meshes/Default.fbx`**. If the folder contains exactly one FBX, it can have any filename. The folder is alongside `NGon.uplugin`.
3. Build your project's **Editor** target with your UE 5.8 source build. On Windows, replace the sample paths and target name with yours.

   ```bat
   "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" MyProjectEditor Win64 Development -Project="D:\Projects\MyProject\MyProject.uproject" -WaitMutex
   ```

4. Open the project and choose **Window → N-Gon**. The default FBX loads when the tab opens. If it is missing, ambiguous, or unreadable, the tab opens the built-in demo and shows the reason.

## Try it

| Control | What to check |
| --- | --- |
| **Reload Default** | Replace the FBX on disk and click to reload it. A failed reload keeps the current preview. |
| **Open FBX** | Opens a browser in `Meshes` when that folder exists. Choose another raw FBX to preview. |
| **Static Mesh** | Choose an Unreal asset to inspect its editable LOD 0. Use the raw FBX for polygons that the Unreal import already triangulated. |
| **Face / Edge / Vertex** | Click to select. Shift-click adds to the active selection. A face highlights all its underlying triangles. |
| **Heatmap / Heat Bias** | Scrub the heat value and watch the **Colors** counter. FBX red values of zero stay zero at neutral bias. |
| **Bend** | Scrub the position test and check that surface picking and visible overlays follow it. The **Positions** counter advances. |
| **Load Demo** | Check for 3 faces, 8 triangles, 12 edges and 10 logical vertices. Click both sides of the hidden diagonal in a face, then click inside the concave notch to test a miss. |

Use Alt + left drag to orbit, scroll to zoom, and **Frame Mesh** to reset the camera.

## FBX export and preview

- In Maya or your DCC, export the original polygon geometry with **Triangulate** disabled. Each polygon needs a simple perimeter. The loader reports when the file contains triangle faces only.
- The loader applies FBX node and geometric transforms and converts the result to Z-up centimeters. The viewport combines the polygon mesh nodes in the file into a single preview.
- The first vertex color layer's red channel drives the scalar heatmap. Direct and indexed colors by control point or polygon corner are supported. Where corners share a vertex, the plugin averages their valid red samples. Missing samples use a gradient along the mesh's longest axis.
- FBX previews use the editor's preview material. Skin deformation, animation, source materials and source UV display are outside this build.

## Why the update uses these APIs

`NGonFbx` reads original polygon loops from the SDK. `NGonTopology` builds polygons in `FMeshDescription` and maps the resulting triangles back to source face IDs. `NGonPicking` uses an AABB tree for face hits and screen-space snapping for edges and vertices.

`NGonPreviewScene` builds the color overlay indices when the topology loads, then edits values in place for heatmap and face selection changes. Bend uses the dynamic mesh position update path, rebuilds the picking tree, and updates the existing preview line and point geometry in place. The counters show plugin update requests.

[Epic's mesh description polygon API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/MeshDescription/FMeshDescription/CreatePolygon) · [Epic's attribute update constraints](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/GeometryFramework/UDynamicMeshComponent/NotifyMeshVertex-) · [Autodesk's FBX mesh API](https://help.autodesk.com/cloudhelp/2020/ENU/FBX-API-Reference/cpp_ref/class_fbx_mesh.html)

## Run the tests

Run `Automation RunTests NGon` in the Unreal Editor console. The tests cover demo topology and picking, default-file precedence and a generated FBX with concavity, transform offsets and both imported color mappings.

```bat
"D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\Projects\MyProject\MyProject.uproject" -ExecCmds="Automation RunTests NGon" -unattended -nopause -nullrhi -log -TestExit="Automation Test Queue Empty"
```

This archive contains source. Run the native build and use the checklist in your UE 5.8 editor to verify the viewport.
