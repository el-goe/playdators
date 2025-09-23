# Playdate 3D rasterizer testbed

### Current Status

TODO

Most of the code layout and structure is taken from [Everybody Wants to Crank the World](https://github.com/aras-p/demo-pd-cranktheworld)
demo I made in 2024. As such, it compiles on Playdate, and on PC as well (via Sokol).

### Application Controls

- Crank (playdate) / Mouse scroll wheel (PC) orbits the camera,
- Up/Down moves camera closer / further,
- A toggles wireframe (default off)
- Left/Right toggles between different rendering techniques, indicated by `a:` number in the upper timings line:
  - 0: scanline rasterizer with simple dither pattern,
  - 1: scanline rasterizer with blue noise dither pattern,
  - 2: scanline rasterizer with checkerboard based on UVs,
  - 3: halfspace rasterizer with checkerboard based on UVs,

### License

Everything I wrote myself is Unlicense / Public Domain. However some 3rd party libraries are used too:
- Scanline rasterizer is based on Chris Hecker's ["Perspective Texture Mapping" series](https://chrishecker.com/Miscellaneous_Technical_Articles) (1995-1996),
  specifically `SUBAFXFL.CPP`.
- Halfspace rasterizer is based on Fabien Giesen's "[Simple watertight triangle rasterizer](https://gist.github.com/rygorous/9b793cd21d876da928bf4c7f3e625908)"
  and [blog posts](https://fgiesen.wordpress.com/2013/02/10/optimizing-the-basic-rasterizer/).
- Parts were based on `mini3d` from Playdate SDK examples (Created by Dave Hayden on 10/20/15, Copyright © 2015 Panic, Inc.)
  and on [mini3d-plus](https://github.com/nstbayless/mini3d-plus) (MIT license), but by now I have replaced them.
- (only on PC): [Sokol](https://github.com/floooh/sokol): sokol_app, sokol_gfx, sokol_time. zlib/libpng license.
