# Vulkan Hybrid Rasterizer
Basic Vulkan raster engine, using nvpro_core2 library and template as the starting point.
Build with cmake -S . -B build

The end goal of this engine is to study and experiment with hybrid rendeirng techniques. In many 3D softwares, deffered rendering pipelines are combined with path tracing techniques to achieve realistic transparency, caustics, and global illumination. This project means to study how these techniques are combined, as well as to learn Vulkan at the same time.

## Goals
- [x] Setup
- [x] GLTF model loading
- [ ] Basic Rasterization framework
- [ ] PBR materials
- [ ] Transparency Pass
- [ ] Post processing (DOF, Tonemapping pass)

