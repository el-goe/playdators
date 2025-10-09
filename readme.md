# PlayDators - toy 3D engine for Playdate

### Graphics

`src/render.h` contains a tiny 3D rasterizer.

* Meshes are either created from code procedurally (`mesh_create`) or loaded from a custom binary format (`mesh_load_from_file`). Meshes contain:
	* Vertex positions,
		* A single mesh can have multiple "frames" of animation, where vertex positions can vary for each frame.
	* Triangle index buffer (3 16-bit vertex indices per triangle),
	* Optional triangle colors (1 byte per triangle),
	* Optional edge flags, to indicate which edges of triangle should be drawn when drawing wireframe.
	* Optional texture coordinates (though they are fairly useless currently).
* A "3D scene" is composed of `MeshInstance` objects, which point to the mesh as well as per-instance parameters:
	* 3D transform matrix,
	* Color (single float),
	* Animation frame index,
	* Drawing flags:
		* Solid: uses per-instance color, combined with mesh per-face color if present,
		* Solit and lit: additionally modulates the color by a wrapped diffuse term between view-space lighting direction and view-space
		  triangle normal,
		* Wireframe: draw triangle edges. Per-mesh data can be present to indicate which edges to draw.
		* Silhouette wireframe: draw edges of "silhouette" faces. Mesh face adjacency is computed on first use, then
		  edges are drawn between neighboring faces where one face is visible and the other is not.
	* Sorting bias: additional value to to view space distance while sorting mesh instances for painter's algorithm.
* Objects in the scene are sorted back-to-front and drawn in that order (with optional per-instance sorting fudge), and then triangles
  each mesh are sorted back-to-front and drawn in that order too. _There is no depth buffer_, so sorting artifacts are entirely possible!
* Everything is rendered using either fixed dithering patterns, or dithering through a premade blue noise texture.
* Coordinate conventions are like in Unity game engine (left handed, Y up): +X to the right, +Y up, +Z goes forward into the screen.
* A **Blender export add-on** is in `blender/addons` folder. It can be used to export meshes into a file format understood by
  `mesh_load_from_file`.

### Platform abstaction

In order for the code to work on both the Playdate as well as PC, there is platform abstaction under `src/platform.h`:

* `plat_file_` functions for working with files, similar to C stdio.
* `plat_time_` functions for querying time.
* `plat_sound_` functions for loading IMA ADPCM compressed WAV files and playing them back, including looping and volume control.
  Multiple sounds can be played at once.
* `plat_input_` functions for querying buttons and crank (on PC maps to mouse scroll wheel) input.
* `plat_malloc` and related functions for memory allocation.

### Math

`src/mathlib.h` contains a tiny "math library" for what was needed in this code. Major parts:
* `float3` struct and `v3_` functions for working with 3D vectors,
* `xform` struct `xform_` functions for working with 3D transforms (3x3 rotation matrix expressed as three coordinate axes, plus translation vector),
* `XorShift32` based random number generator,
* Various math functions like `min`, `max`, `clamp`, `saturate`, `lerp`, `smoothstep` and so on.
* Float to half precision (FP16) conversion: `float_to_half`, `float_to_half_topbits` that map to built-in Playdate CPU instruction.

### CMake build system

The build is done with CMake, and I only ever tested it on Windows. There, just open the folder in Visual Studio (that's the only IDE I tested; and I only tested VS2022 version).
There will be several platforms in the platform dropdown: regular `x64` is for PC build, platforms with `(Playdate)` are for Playdate device build, and `(Playdate Sim)` is for
Playdate simulator build.

`Source` folder is expected to have data files; some of these will be automatically processed by Playdate SDK tools while doing a build (e.g. PNG files will get converted to `.pdi`; WAV files
to `.pda` and so on).

Most of the code layout and structure is taken from [Everybody Wants to Crank the World](https://github.com/aras-p/demo-pd-cranktheworld)
demo (2024) and [Surface-Stable Fractal Dithering on Playdate](https://github.com/aras-p/playdate-dither3d) (2025).

### Sample application / "game"

`src/main.c` contains a simple "3D game", where Chocomel the dog (from Blender Studio DOG WALK game - [Steam](https://store.steampowered.com/app/3775050/DOGWALK/),
[project page](https://studio.blender.org/projects/dogwalk/)) has to run along a 3D curve in space, jumping over or digging through obstacles.

| | | | | |
|---|---|---|---|---|
| ![](/images/playdators_choco_0.png?raw=true "") | ![](/images/playdators_choco_1.png?raw=true "") | ![](/images/playdators_choco_2.png?raw=true "") | ![](/images/playdators_choco_3.png?raw=true "") | ![](/images/playdators_choco_4.png?raw=true "") |

All the meshes there are from the [Dog Walk](https://studio.blender.org/projects/dogwalk/) project, and as such they are not particularly optimized for either Playdate, nor
for a graphics engine that has no textures. I applied Blender's Decimate modifier on them, exported, and that's it.

The music and sounds are from the Dog Walk project as well, converted to IMA ADPCM in Audacity.

The game tends to render 500-1000 visible triangles as well as a bunch of triangle edges, at runs at 45-50 frames per second on the Playdate device.

Controls:
- Right: move Chocomel,
- Up: jump (can only do that while running),
- B: dig through an obstacle,
- Crank / mouse wheel: orbit the camera a bit.
- A: toggle rendering between simple dithering and blue-noise dithering.


### License

Everything I wrote myself is Unlicense / Public Domain. However some 3rd party libraries are used too:
- Scanline rasterizer is based on Chris Hecker's ["Perspective Texture Mapping" series](https://chrishecker.com/Miscellaneous_Technical_Articles) (1995-1996),
  specifically `SUBAFXFL.CPP`.
- Modified version of Cameron Hart's [radixsort](https://github.com/bitshifter/radixsort), zlib license.
- Mesh adjacency calculation based on Arseny Kapoulkine's [meshoptimizer](https://github.com/zeux/meshoptimizer), MIT license.
- Parts were based on `mini3d` from Playdate SDK examples (Created by Dave Hayden on 10/20/15, Copyright © 2015 Panic, Inc.)
  and on [mini3d-plus](https://github.com/nstbayless/mini3d-plus) (MIT license), but by now I have replaced them.
- (only on PC): [stb_image.h](https://github.com/nothings/stb): Public Domain / MIT.
- (only on PC): [Sokol](https://github.com/floooh/sokol): sokol_app, sokol_audio, sokol_gfx, sokol_time. zlib/libpng license.
