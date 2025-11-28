## Masaryk University

## Faculty of Informatics

# Real-time Snow Deformation

### Master’s Thesis

## Daniel Hanák

### Brno, Spring 2021



_This is where a copy of the official signed thesis assignment and a copy of the
Statement of an Author is located in the printed version of the document._



## Declaration

Hereby I declare that this paper is my original authorial work, which
I have worked out on my own. All sources, references, and literature
used or excerpted during elaboration of this work are properly cited
and listed in complete reference to the due source.

```
Daniel Hanák
```
**Advisor:** Mgr. Jiří Chmelík, Ph.D.

```
i
```


## Acknowledgements

```
I would like to express my gratitude to my advisor Jiří Chmelík, assis-
tant professor at the Department of Visual Computing, for his guid-
ance, helpful advice, and remarks. Furthermore, I would also like to
thank my friends and colleagues for their reviews of my work. Lastly,
I would like to thank my family for their encouragement and support
during my studies.
```
```
iii
```

## Abstract

Procedural terrain deformation has remained an open problem in com-
puter games. Having dynamic objects such as characters and animals
interact with the environment makes the scene more immersive and
alive. This thesis presents a scalable real-time snow rendering tech-
nique implemented on the GPU using compute shaders and hardware
tessellation. The snow can be deformed by any dynamic object and
has a sliding window that moves with the player, which allows this
technique to be used in open-world games. Furthermore, it supports el-
evation along the edges of the deformation and filling generated trails
over time. Presented are also performance results of the proposed
technique evaluated on multiple GPUs.

```
iv
```

## Keywords

computer graphics, game development, rendering, shader, compute,
tessellation, hlsl, deformation, snow

```
v
```


## Contents


**A Electronic Attachments 41**

viii


## List of Tables

```
6.1 Measured GPU performance of the compute shaders. 32
```
```
ix
```


## List of Figures

- 1 Introduction
- 2 Related Work
   - 2.1 Assassin’s Creed III
   - 2.2 Batman: Arkham Origins
   - 2.3 Rise of the Tomb Raider
   - 2.4 Horizon Zero Dawn: The Frozen Wilds
   - 2.5 The Last of Us Part II
- 3 The Graphics Rendering Pipeline
   - 3.1 Vertex Processing
   - 3.2 The Tessellation Stage
   - 3.3 The Geometry Shader
   - 3.4 Pixel Processing
   - 3.5 The Compute Shader
- 4 Implementation
   - 4.1 Rendering Orthographic Height
   - 4.2 Deformation Heightmap
   - 4.3 Sliding Window
   - 4.4 The Vertex Shader
   - 4.5 The Tessellation Stage
   - 4.6 The Pixel Shader
   - 4.7 Creating Textures
- 5 Example Scene
   - 5.1 Snow Renderer
   - 5.2 Level Design
- 6 Results
   - 6.1 Memory Requirements
   - 6.2 Performance
   - 6.3 Visual Quality
- 7 Conclusion
- Bibliography
- 2.1 Deformable snow in Rise of the Tomb Raider
- 2.2 Horizon Zero Dawn: The Frozen Wilds environment
- 2.3 The Last of Us Part II snowy forest
- 4.1 Debug view of computed normals.
- 4.2 Undeformed snow material
- 5.1 Chomper rendered using PBR
- 5.2 A shot from the testing environment
- 6.1 Average frame rate measured on multiple GPUs
- 6.2 A shot from the winter forest
- 6.3 The deformable snow material with different parameters



## 1 Introduction

Nowadays, many AAA^1 video games push the boundaries of 3D com-
puter graphics with groundbreaking open-world environments. Game
studios employ various sophisticated algorithms to solve problems re-
lated to rendering interactive virtual worlds, such as snow simulation,
which has always been a challenging task. Having dynamic objects
such as characters, animals, and debris from explosions interact with
the snow makes the environment more immersive and alive. A straight-
forward way how to achieve a snow surface is to store white color in
the object’s diffuse texture. Character footsteps are then rendered as
decals projected on the snow geometry [1]. Decals are often used in
games for effects such as bullet holes and tire marks. They are usually
rendered as textures that are orthographically projected through a
convex volume. Unfortunately, this common technique lacks scalabil-
ity and persistence. The footsteps in snow usually have an upper limit
to the number of active decals at the same time.
This thesis aims to develop a scalable real-time snow deformation
technique that can be used in video games using implementation on
modern GPUs to achieve real-time performance. More specifically,
the proposed snow deformation technique has to work in a massive
open-world game on top of static objects. It also has to work for any
dynamic object with skeletal animation, and it has to run fast on the
GPU.
The following chapter describes various approaches of real-time
snow deformation used in video games. Chapter 3 presents the real-
time graphics rendering pipeline that is used in computer games to
render 2D images on a screen. The in-depth overview and implemen-
tation details of the proposed real-time snow deformation technique
are shown in Chapter 4. Implementation details of the testing environ-
ment created in Unity Engine are presented in Chapter 5. Performance
results of the implementation evaluated on multiple GPUs are pre-
sented in Chapter 6. Finally, the thesis concludes in Chapter 7 with a
summary and discussion of future work.

1. A triple-A video game (AAA) is generally a title developed by a large studio
with a higher development budget.



## 2 Related Work

This chapter presents several different approaches to real-time snow
deformation rendering. The following games with presentations and
articles on the subject achieved notable results with their advantages
and disadvantages.

### 2.1 Assassin’s Creed III

One of the first deformable snow implementations was proposed by
Ubisoft Entertainment in 2012 for the game _Assassin’s Creed III_ [2].
Deformable snow in the winter environment is one of the leading
graphics features of the game. The snow affects both visual and game-
play aspects of the game because it is deformed by all characters and
animals, which allows the player to track them.
The game takes place in a vast and varied world, which supports
both summer and winter conditions. The snow mesh for the winter
environment is generated as a displaced copy of the base ground
mesh. When a dynamic object steps into the snow mesh, the colliding
triangles are removed from the index buffer. A new set of tessellated
and displaced triangles is generated based on the removed triangles
and displacement texture.
To accomplish this, they use a technique known as _render to ver-
tex buffer_ (R2VB) [3], which allows runtime GPU tessellation^1. For
the tessellation, they use barycentric coordinates of pre-tessellated
triangles, with the limitation that all created triangles have the same
tessellation factor. The displacement is rendered into a render target as
a geometrical approximation of the character’s movement and used as
a vertex buffer in a second pass to push down and tessellate triangles.
This technique has a significant drawback. The outer vertices of
a tessellated polygon may not lie within the adjacent polygon’s edge.
These vertices are known as T-junctions [5]. It also happens when two
triangles have edges that fall within the same line in space but do not

1. As a Playstation 3 and Xbox 360 title, the game was bounded toShader Model 3.
GPUs. Compute shaders and hardware tessellation became available on next-
generation consoles [4].


2. Related Work

```
share the same endpoint vertices. These vertices cause visible seams
in the snow mesh.
```
### 2.2 Batman: Arkham Origins

Back in 2013, WB Games Montreal in collaboration with NVIDIA pro-
posed a technique for the rendering of deformable snow featured in
_Batman: Arkham Origins_ [6]. The game takes place in Gotham City,
which is a fictional city set within the United States. Therefore, the en-
vironment consists of streets and rooftops. For each rooftop and street,
they dynamically allocate and deallocate displacement heightmaps at
runtime. These heightmaps are created and released based on player
visibility, object size, and occupancy.
To generate a heightmap, they render snow-affecting objects from
under the surface using an orthogonal frustum (to avoid any perspec-
tive distortion) into an offscreen buffer. The frustum height equals
the height of the snow. The rendered depth buffer is then filtered
and accumulated with a custom post-process chain using _ping-pong_
buffers [7]. It uses two offscreen buffers, first for reading and second
for writing; after each frame of the depth rendering, the roles of these
buffers switch. The depth buffer is filtered with a 4-tap bilinear Poisson
disk which adds gradience to the overall result. They also subtract a
constant value from the heightmap to replenish the deformed snow
during the merging stage.
The deformable snow surface has two states: non-deformed snow
and completely flattened snow, both with snow materials that simulate
the snow state. They blend these two materials in a pixel shader based
on the heightmap. Diffuse and specular textures are blended using
linear interpolation, but for normal textures, they use _reoriented normal
mapping_ [8], which is a quaternion-based technique that blends two
normal maps correctly.
This snow deformation technique is used with _relief mapping_ [9]
on consoles (Playstation 3 and Xbox 360); the PC version is enhanced
with GPU tessellation on DirectX 11, where the heightmap is used to
displace the geometry of the snow mesh.
The technique allows for visually powerful and interactive de-
formable snow, which works with dynamic shadows and dynamic


2. Related Work

```
ambient occlusion that fills trails in the snow. On the other hand, the
disadvantage of this technique is that it works only on flat rectangular
surfaces.
```
### 2.3 Rise of the Tomb Raider

```
Deferred snow deformation used in Rise of the Tomb Raider is a tech-
nique that was proposed by Crystal Dynamics in 2015 [10]. It is an
essential graphical feature and crucial gameplay element that allows
the player to track characters and animals while remaining hidden
from sight. The result of this technique is shown in Figure 2.1.
```
```
Figure 2.1: Deformable snow in Rise of the Tomb Raider (Image courtesy
of Square Enix. Game developed by Crystal Dynamics.)
```
This technique uses a deformation compute shader for generating a
deformation heightmap. The heightmap is a 32-bit 1024 × 1024 texture
with a resolution of 4 cm per texel. All snow-affecting objects are
approximated using points in space that are stored in a global buffer.
The deformation shader is dispatched with one thread group for each
point in the global buffer. Each group computes a 32 × 32 texel area


2. Related Work

around the point as a squared distance between the current sample
and the point. The computed trail closely resembles a quadratic curve.
This technique also supports an elevation along the edges of the trail,
which improves the overall look.
The computed area is written using atomic minimum because two
different points can affect the same texel in the heightmap. This is
done using an _unordered access view_ (UAV) that allows write access
from multiple threads to any location without conflicts using atomic
operations [4].
The deformation heightmap works as a sliding window to keep the
snow deformation centered on the player. When the player moves, the
texels of the heightmap that are out of range become the new texels in
the heightmap. A straightforward way to erase texels along the edge
of the sliding window is to set these texels to the maximum value. This
creates a sharp transition in the snow heightmap along the edge of the
sliding window, which is seen when the player goes back. A proposed
solution is to update more rows and columns along the edge of the
sliding window with an exponential function that increases the snow
level for these texels.
This technique also uses another compute shader to fill created
trails in the snow over time to simulate blizzard conditions. The shader
is dispatched on the entire deformation heightmap and increases the
texel values by a global fill rate.
The deferred snow deformation technique computes the defor-
mation based on points that approximate the snow-affecting geom-
etry with compute shaders. It also supports snow elevation that is
not present in the previous techniques. The major drawback of this
technique is that each snow-affecting geometry needs a second repre-
sentation defined using points that approximate the object.

### 2.4 Horizon Zero Dawn: The Frozen Wilds

```
In 2017, Guerrilla Games released Horizon Zero Dawn: The Frozen Wilds
expansion, which takes place in a snowy mountain region. For this
environment, they proposed a snow deformation technique that works
in a massive open world with any dynamic object [11]. A screenshot
from the game is shown in Figure 2.2.
```

2. Related Work

```
This technique consists of two passes. The first pass does a scene
query of snow-affecting objects within a snow deformation area de-
fined by an orthographic frustum centered on the player. Objects
inside the frustum are rendered from below the minimum terrain
height into a 16-bit linear depth buffer. The depth is rendered using
nonuniform axes in normalized device coordinates (NDC) to ensure that
small objects have enough samples. Since the snow-affecting objects
are rendered into a depth buffer, it is more effective to use a lower
level of detail (LOD) to improve overall performance.
```
Figure 2.2: _Horizon Zero Dawn: The Frozen Wilds_ has a massive open
world with a snowy environment ruled by robotic creatures. (Im-
age courtesy of Sony Interactive Entertainment. Game developed by
Guerrilla Games.)

```
The second pass is done using a compute shader. Each thread
group reads persistent deformation data from the previous frame
stored in ping-pong buffers and puts them in local data share (LDS) [12].
Stored texels are filtered using a time-dependent temporal min-average
3 × 3 filter which converges towards a defined slope gradient. Then
the depth buffer rendered in the first pass is sampled. If the sampled
depth is below the current snow height and above the terrain, the
```

2. Related Work

snow height is replaced. The result is written to the persistent buffer
for the next frame.
This technique uses a sliding window that moves with the player;
the persistent deformation is sampled using an offset, depending on
how the player moved. It allows using the deformation system in
a massive open world. The deformation system outputs the result
in world space, allowing for different use cases such as interactive
snow slush on lakes. The drawback of this technique is that it does
not support the trail elevation present in _Rise of the Tomb Raider_.

### 2.5 The Last of Us Part II

Recently, Naughty Dog proposed a snow deformation technique used
in their game _The Last of Us Part II_ that was released in 2020 [13]. They
used this technique for various effects such as blood melting snow,
deformation revealing mud beneath the snow, and tracks on various
deformable materials. The result of this technique is shown in Figure
2.3.

Figure 2.3: _The Last of Us Part II_ takes place in post-apocalyptic environ-
ments within the United States. (Image courtesy of Sony Interactive
Entertainment. Game developed by Naughty Dog.)


2. Related Work

Each mesh and terrain region that is deformed by snow-affecting
objects has assigned a particle render target. In each frame, characters
that affect the deformable snow do a ray cast from their knees to their
feet. If the ray cast hits a snow surface, a defined footprint attached to
the character is drawn into the particle render target assigned to the
surface.
The snow surface is rendered using _parallax occlusion mapping_
(POM) [14, 15], which takes the particle render target as a heightmap.
The shader also computes a screen space depth to get the correct in-
tersection shape. The depth is calculated as world space positions
defined by the deformation heightmap, converted to screen space. The
depth is written into the depth buffer by a custom pixel shader before
the G-buffer pass.
The particles write to the render target with a cone shape which is
then remapped using a quadratic function to get smooth raised edges.
Undeformed snow is depressed to extend the raised edges around
the deformation. The shader also adds a detail normal map using the
particle render target as a mask.
Since this technique is using the POM, there is no need to use run-
time GPU tessellation. This technique is not rendering the geometry of
snow-affecting objects into depth render target as some of the previous
techniques do. Instead, render target positions for particle rendering
are determined by ray casts.



## 3 The Graphics Rendering Pipeline

This chapter presents the real-time graphics rendering pipeline that
is used in computer games to render 2D images on a screen. It is an
ordered chain of computational stages, each processing a stream of
input data using a massive parallel processor and producing a stream
of output data.

### 3.1 Vertex Processing

To render an object on a screen, the application issues a _draw call_ that in-
vokes the graphics API such as DirectX, OpenGL, and Vulkan to draw
the object, which causes the graphics pipeline to execute its stages.
Before a draw call is issued into a command buffer, the application
needs to bind all resources to the rendering pipeline referenced in the
draw call. Data used for rendering an object are provided using vertex
buffers, also known as vertex buffer objects (VBOs) in OpenGL. It is
an array of vertex data in a given layout stored in a contiguous chunk
of memory. The layout defines vertex attributes passed to the vertex
shader, such as position, color, normal, and UV coordinate. The size
of a vertex measured in bytes is called a stride. The vertex buffers are
attached to input streams; data from input streams are gathered by
an input assembler and converted into canonical form [16]. The input
assembler specifies primitives, such as points, lines, and triangles,
formed from provided data [4].
The vertex buffer is often referenced by an index buffer that stores
indices of vertices. Indices are stored as 16-bit or 32-bit unsigned
integers based on the total number of vertices present in the vertex
buffer. Indexing allows the vertex processor to store computed post-
transform results into cache determined by index value; therefore,
recomputation of the already computed vertex data can be avoided.
The order in which the index buffer accesses vertices should match
the order in the vertex buffer to minimize cache miss [4].
Individual vertices are passed into a vertex shader. The vertex
shader is a fully programmable stage that is responsible for shading
and transformation of passed vertices. It usually transforms vertices
from local space into homogeneous clip space. However, it is also


3. The Graphics Rendering Pipeline

used for various effects such as procedural animation [17], displace-
ment based on height maps using texture fetch [18], and animating
characters using skinning or morphing techniques [4]. The output of
this stage is a fully transformed vertex with per-vertex data that are
interpolated across a triangle or line based on the input assembler.
The data can also be stored in memory or be sent to the tessellation
stage or the geometry shader on modern GPUs.

### 3.2 The Tessellation Stage

The tessellation stage is an optional GPU feature in the rendering
pipeline used for subdividing input geometry. It became available
with Shader Model 5.0 in DirectX 11 and OpenGL 4.0.
There are several advantages to increase the mesh triangle count
using tessellation. Since the GPU tessellates the object geometry on the
fly, the mesh can be stored using a lower triangle count, which reduces
the memory footprint. Beyond memory savings, the tessellation stage
keeps the bus between GPU and CPU from becoming the bottleneck.
The mesh can be rendered efficiently by having an appropriate number
of triangles created based on the camera view [4]. For example, if a
mesh is far away from the camera, only a few triangles are needed.
As the mesh gets closer to the camera, the tessellation increases the
mesh’s detail, which contributes to the final image. This dynamic GPU
level of detail is also used to control the application’s performance to
maintain frame rate.
The tessellation stage consists of three sub-stages: _hull shader_ , _tessel-
lator_ , and _domain shader_. First, the hull shader needs a patch primitive
definition with a number of control points. These control points can
define an arbitrary subdivision surface, a Bézier patch, or a simple
triangle. For each input patch, the hull shader outputs tessellation fac-
tors for the tessellator. They are computed using various metrics such
as screen area coverage, orientation, and distance from the camera.
The tessellation factors define how many triangles should be gener-
ated for a given patch. The hull shader also performs processing on
individual control points. It can change the surface representation
using removing or adding control points as desired. The hull shader
can be used to augment standard triangles into higher-order cubic


3. The Graphics Rendering Pipeline

Bézier patches. This technique is known as the _PN triangle_ scheme
[19]. This scheme improves existing triangle meshes without the need
to modify the existing asset creation pipeline.
The hull shader sends tessellation factors and surface type into the
tessellator, which is a fixed-function stage done by the hardware. It
tessellates the patches based on the tessellation factors and generates
barycentric coordinates for the new vertices. These with the patch
control points are then sent to the domain shader. The domain shader
processes each set of barycentric coordinates and generates per-vertex
data using interpolation. The formed triangles are then passed on
down to the rendering pipeline.

### 3.3 The Geometry Shader

The geometry shader is an optional and fully programmable stage in
the rendering pipeline introduced with Shader Model 4.0 in DirectX 10
and OpenGL 3.2. It is designed for transforming input data or making
a limited number of copies. It operates on vertices of entire primitives
such as points, lines, and triangles and outputs vertices of zero or more
primitives of the same type as the input. It can also effectively delete
an input primitive by not producing an output [16]. The geometry
shader is used for various effects, such as efficient cascaded shadow
maps for high-quality shadow generation, creating particle quads
from point data, rendering six faces of a cube map, and extruding fins
along silhouettes for fur rendering.

### 3.4 Pixel Processing

After the vertex processing stages perform their operations, each prim-
itive is clipped against a view volume, transformed into normalized
device coordinates using perspective division, and mapped to screen.
Then, the primitives are passed into a rasterization stage, which gener-
ates fragments that are inside the primitives. It is a fixed-function stage
done by the hardware, but it can be somehow configured [4]. The
rasterizer interpolates the fragment’s depth and other vertex attributes
defined in previous stages based on the primitive type.


3. The Graphics Rendering Pipeline

All generated fragments are then passed to the _pixel shader_ (also
known as the _fragment shader_ in OpenGL). The pixel shader is a fully
programmable stage that computes any per-pixel data using interpo-
lated shading data as input. It usually samples one or more texture
maps using UV coordinates and runs per-pixel lighting calculations
to determine the pixel color that is written into a render target. The
render target is usually associated with a _z-buffer_ (also called _depth
buffer_ ) and a _stencil buffer_. The z-buffer is usually a 24-bit float buffer
that stores scene depth. It is used for testing the depth of the pro-
cessed fragment against the content stored in the z-buffer based on
a defined function. If the fragment passes the test, it is written into
the color buffer, and the z-buffer is updated with the processed frag-
ment’s depth. Otherwise, the fragment is discarded. The stencil buffer
is an offscreen buffer used to store locations of the rendered objects.
It is usually an 8-bit unsigned integer buffer used for optimizations
and various effects such as real-time reflections and efficient shadow
volume rendering [20].
Nowadays, many games use the _deferred shading_ [21, 22] to render
opaque objects in complex scenes that require multiple render passes
to compute the final pixel color. It differs from the forward rendering
technique by separating lighting from the actual rendering of objects.
It queries all opaque objects only once and stores whatever is needed
to compute the required subsequent lighting computations, such as
position, albedo, normal, and roughness. These surface properties are
stored in separate render targets known as _G-buffers_. These buffers are
accessed as textures in subsequent passes to compute the illumination
based on light volumes present in the view frustum.

### 3.5 The Compute Shader

```
Non-graphical applications that do not use the graphics render pipeline
also benefit from a GPU that provides a massive amount of compu-
tational power. The use of the GPU for non-graphical applications is
known as general-purpose GPU programming (GPGPU). Platforms such
as OpenCL and CUDA allow using the parallel architecture of the
GPU for arbitrary computation without the graphics-specific function-
ality. For GPGPU programming, the application generally demands
```

3. The Graphics Rendering Pipeline

to access the results back on the CPU, which requires copying data
from video memory located on the GPU to the system memory. For
computer games, the computational result is usually used as an in-
put to the graphics rendering pipeline without the need to copy the
data between GPU and CPU. These data are usually computed using
compute shaders.
The compute shader is a programmable shader that became avail-
able with Shader Model 5.0 in DirectX 11 and OpenGL 4.3. It is not
directly part of the graphics rendering pipeline, but it is also invoked
by the graphics API. The number of threads desired for the compute
shader execution is split into a grid of thread groups. The number of
thread groups is passed into the dispatch command used to execute the
compute shader. These thread groups are defined in three-dimensional
space using x-, y-, and z-coordinates suitable for processing 2D or 3D
data. Each thread group has its shared memory known as _local data
share_ (LDS), which is a high-bandwidth and low-latency explicitly
addressed memory that can be accessed by any thread in the group.
The GPU hardware divides threads up into _warps_ (also known as
_wavefronts_ on ATI GPUs) processed by _single instruction, multiple data_
(SIMD) units. In order to use all threads in the warp, the group size
should be multiple of the warp size, which is defined by the GPU [23].
If a warp becomes stalled, for example, to fetch texture data, the GPU
processor switches and executes instructions for another warp which
hides latency [4].
Nowadays, compute shaders are used for post-processing effects
commonly performed by rendering screen-filling quadrilateral with a
rendered image treated as a texture. The rendered image can be di-
rectly passed as input into the compute shader, which then outputs the
modified image based on the desired computation. Compute shaders
are also used for various effects and rendering pipeline optimizations
such as procedural vertex animation, bounding volume generation,
particle systems, and visibility determination [12].



## 4 Implementation

The proposed snow deformation technique is based on the approach
used in _Horizon Zero Dawn: The Frozen Wilds_. It extends the solution
presented by Guerrilla Games using snow elevation along the edges of
the deformation. The presented algorithm consists of custom render
passes that are implemented on the GPU. This chapter presents an
in-depth overview of the individual render passes and their imple-
mentations.

### 4.1 Rendering Orthographic Height

The first pass of the algorithm does a scene query of snow-affecting ob-
jects, such as characters, animals, and debris from explosions, within
an orthographic frustum centered on the player. The size of the or-
thographic frustum defines the region around the player that can be
deformed. The snow-affecting objects are rendered from below into
a 16-bit linear depth buffer. Since these objects are rendered into the
depth buffer, and most dynamic objects have a high vertex count, the
algorithm should query a lower LOD to reduce the vertex process-
ing. Therefore, Guerrilla Games used low-resolution dynamic shadow
casting LODs for the depth rendering pass [11].

### 4.2 Deformation Heightmap

The deformation heightmap is computed using a custom chain of
compute shaders based on the terrain heightmap used to generate
the terrain geometry and the orthographic height computed during
the previous pass. If the level contains custom geometry that is not
encoded in the terrain heightmap, the algorithm also renders bound-
ing volumes used for collision detection into the depth buffer without
backface culling.
The algorithm updates the snow height if the sampled height of
snow-affecting objects is above the terrain height and below the snow
height. It also adds a global fill rate to the snow height, filling the snow
trails over time. The fill rate can be adjusted to emulate the weather


4. Implementation

conditions in the game. The computed deformation is accumulated
in a persistent 8-bit UNORM^1 ping-pong buffer used to sample the
persistent snow deformation in the next frame.
Next, the computed snow deformation is remapped using a pro-
posed cubic function:

```
f(x) =−3.4766x^3 +6.508x^2 −2.231x+0.215. (4.1)
```
The cubic function depresses the undeformed snow, which generates
the desired snow elevation along the edges of the deformation. The
coefficients of the cubic function were computed using polynomial
regression. The remapped deformation is stored into an intermediate
buffer that is sampled in the following compute shader. However,
reading from a resource that the GPU still uses to write data is known
as a resource hazard. Modern graphics APIs, such as DirectX 12 and
Vulkan, associate a state to each resource to prevent resource hazards.
These states are transitioned using resource barriers that are added to
the command queue to instruct the GPU to change the resource state
before executing subsequent commands [23].
The next compute shader filters the heightmap with a Gaussian
blur filter which creates a smooth transition between deformed and
undeformed snow. Image blur filters are usually used in computer
games for post-processing effects such as bloom and depth of field.
The Gaussian blur is a separable filter that can be computed in two
passes, horizontal and vertical, which provides a significant perfor-
mance increase while providing the same result. The algorithm first
reads all texels that are needed for the computation from the interme-
diate buffer and stores them into LDS. Storing the required texels into
LDS and replacing all subsequent memory loads with LDS loads re-
duces the number of accesses to global memory. It reduces redundant
loads, which significantly improves overall performance. Then, for
each texel, the algorithm computes the weighted sum of neighboring
texels. The computed result is then passed into the vertical pass, which
is computed in the same manner.
Next, the algorithm computes the normal vectors based on the
filtered heightmap using the finite difference method. The normals

1. UNORM is an unsigned normalized texture format with components mapped
    to the range[0, 1].


4. Implementation

```
are defined at mesh vertices; they are used to represent the surface
orientation, which is needed for lighting computations. They are usu-
ally generated in 3D modeling packages, such as 3ds Max, Maya, and
ZBrush, using smoothing techniques. Since the normal directions
change based on the deformation heightmap, the algorithm computes
the normals on the fly. The normal of an implicit surface is computed
using partial derivatives [24], called the gradient:
```
```
∇f(x,y,z) = (
```
```
∂ f
∂ x
```
#### ,

```
∂ f
∂ y
```
#### ,

```
∂ f
∂ z
```
#### ). (4.2)

```
The algorithm uses the central finite difference to approximate the
derivatives needed for computing the normals:
```
```
∂ f
∂ x
```
```
≈f(x+∆h)−f(x−∆h). (4.3)
```
First, the deformation heightmap is fetched into LDS using gather
instructions which gets the four texels that would be used for hard-
ware bilinear interpolation. Next, the algorithm computes the normals

```
Figure 4.1: Debug view of the deformable snow with normals com-
puted using central finite difference.
```

4. Implementation

using one texel offset stored in LDS. The normals computed using
central finite difference are shown in Figure 4.1. Finally, the normal
transformed into the range[0, 1]and corresponding height value are
packed into a 32-bit UNORM buffer that can be accessed in the subse-
quent rendering pipeline to deform the snow geometry.

### 4.3 Sliding Window

Since the snow deformation technique is needed to work in an open-
world game, the deformation heightmap moves with the player. The
sliding is done by reading texels from the persistent buffer using a
movement offset scaled from world space units to match the heightmap
resolution. The movement offset is computed based on the player po-
sition and passed into the compute shader as a uniform variable. The
algorithm also resets the values of the deformation heightmap on the
edges of the sliding window to prevent unwanted texture tiling. Since
the algorithm filters the heightmap using the Gaussian blur, there is
no need to exponentially increase the snow fill rate along the border
of the sliding window, which is done in _Rise of the Tomb Raider_.

### 4.4 The Vertex Shader

The vertex shader for the deformable snow uses a material heightmap,
and the deformation heightmap generated using custom render passes.
The material heightmap is a linear grayscale texture used to displace
vertices along the normal vectors. In order to sample the deformation
heightmap, the algorithm computes world space vertex positions that
are transformed into texture space UV coordinates using the position
of the sliding window and the size of the view frustum. Next, the algo-
rithm displaces the vertices based on the deformation and transforms
them to clip space. Then, it unpacks the normals from the persistent
buffer and multiplies them with the transpose of the matrix’s adjoint
to correctly get the world space normal vectors [24]. The adjoint is
guaranteed to exist, but the transformed normals are not guaranteed
to be of unit length. Therefore, they need to be normalized.


4. Implementation

### 4.5 The Tessellation Stage

The proposed technique uses the tessellation stage to efficiently ren-
der the snow geometry by having an appropriate number of triangles
created based on the camera view. If the snow geometry is far away
from the camera, only a few triangles are needed. As the snow geome-
try gets closer to the camera, the algorithm subdivides the geometry,
contributing to the final image.
The algorithm uses an input assembler that gathers the input
stream data as patches^2 with three control points converted to canoni-
cal form. Therefore, the algorithm needs to set the input assembler to
use patch topology in the graphics pipeline state descriptor. The ver-
tex shader is then used to transform the vertex data, such as position,
normal, tangent, and UV coordinate, to the patch control point. These
control points that form the patch are sent into the hull shader, which
outputs the tessellation factors for the tessellator. Since the algorithm
tessellates triangle patches, the hull shader has to output three edge
factors and one center factor. These tessellation factors are computed
in the constant function based on the distance from the camera and
view frustum.
One common technique used in video games to speed up the ren-
dering process is to compare the bounding volume, such as _sphere_ ,
_axis-aligned bounding box_ (AABB), and _oriented bounding box_ (OBB), of
each rendered object to the view frustum. If the bounding volume is
inside or intersects the view frustum, then the object may be visible.
Therefore, a draw command is added to the command queue to in-
struct the GPU to draw the geometry. If instead the bounding volume
is outside the view frustum, then the object is omitted. This technique
is known as _view frustum culling_ [25].
The algorithm does the view frustum culling in the hull shader to
minimize unnecessary computation. The view frustum is a pyramid
that is truncated by a near and a far plane. Therefore, the algorithm
compares each control point of a patch to six planes that define the
view frustum. The plane is commonly defined using a normal vector
and distance from the origin. The algorithm test if the control point is

2. For example, in DirectX 12, the patch with three control points is defined using
D3D_PRIMITIVE_3_CONTROL_POINT_PATCH.


4. Implementation

in the positive or negative half-space using signed distance, which is
computed using the dot product. Assume that the positive half-space
is outside of the view frustum. The algorithm then goes through the
six planes, and for each plane determines if the control point is in
the positive half-space. If it is true for all control points of the given
patch, the algorithm discards the patch and sets the tessellation factors
to zero. Otherwise, the given patch intersects or is inside the view
frustum.
For each patch that passed the view frustum culling, the algo-
rithm computes the tessellation factors based on the distance from the
camera. Then, the patch control points with the tessellation factors
are passed into the tessellator, which is a fixed-function done by the
hardware. It subdivides the patches based on the tessellation factors
and generates barycentric coordinates for new vertices. These with
the patch control point are sent into the domain shader. The domain
shader interpolates the data for the new vertices provided in the patch
control points based on the barycentric coordinates. For each gen-
erated vertex, the algorithm invokes the vertex shader described in
Section 4.4. Then, the vertices are passed on down to the rasterization
stage.

### 4.6 The Pixel Shader

Nowadays, many games use _physically based rendering_ (PBR), which
provides an accurate representation of how light interacts with sur-
faces. The snow deformation technique is not limited to PBR; it can
be used with any shading model to achieve the desired visual quality.
However, the PBR is also used in stylized games such as _Spyro Reignited
Trilogy_ and _Ratchet & Clank: Rift Apart_. Therefore, the following section
presents technical details that are needed to develop a standard PBR
material.

```
4.6.1 Physically Based Rendering
```
When a light ray hits a surface, it can be reflected off the surface, ab-
sorbed, and scattered internally. Reflectance properties of the surface
are quantified using the _bidirectional reflectance distribution function_


4. Implementation

(BRDF), denoted asf(l,v). The BRDF gives the distribution of outgo-
ing light in all directions based on the direction of the incoming light.
It is present in the rendering equation used to compute the outgoing
radianceLo(p,v)from the surface pointpin the given directionv:

```
Lo(p,v) =Le(p,v) +
```
```
∫
```
```
Ω
```
```
f(l,v)Li(p,l)(n·l)dl, (4.4)
```
whereLe(p,v)describes the emitted radiance from the surface, and
Li(p,l)is the incoming radiance into the point. The integration is
performed overlvectors that lie inside the unit hemisphere above the
surface [4].
The standard material model used in most game engines is based
on the Cook-Torrance BRDF [26]. The specular term uses the micro-
facet model:

```
fs(l,v) =
```
```
F(l,h)G(l,v,h)D(h)
4 (n·l)(n·v)
```
#### . (4.5)

```
The amount of light reflected rather than refracted is described by
the Fresnel reflectanceF(l,h), which is commonly implemented using
Schlick’s approximation:
F(l,h)≈F 0 + ( 1 −F 0 )( 1 −(l·h))^5 , (4.6)
```
which is an RGB interpolation between F 0 (characteristic specular
color of the substance at normal incidence) and white.
The surface’s _normal distribution function_ (NDF), denoted asD(h),
is the statistical distribution of microfacet surface normals over the
microgeometry surface area. It describes how many of the microfacets
are pointing in the direction of the half vector. The most often used
NDF in both film [27] and game [28, 29] industries is the _Trowbridge-
Reitz distribution_ , also known as the _GGX distribution_ :

```
D(h) =
```
```
χ +(n·h) α^2
π ( 1 + (n·h)^2 ( α^2 − 1 ))^2
```
#### , (4.7)

where the surface roughness is provided by the _α_ parameter, and
_χ_ +(x)is the positive characteristic function:

```
χ +(x) =
```
#### 

#### 

#### 

```
1, ifx>0,
0, otherwise.
```
#### (4.8)


4. Implementation

The geometric attenuation factorG(l,v,h)describes how many
microfacets of the given orientation are masked or shadowed. The
geometric term is usually based on the Smith’s visibility function; for
example, Guerrilla Games implemented Schlick’s approximation [30]
into Decima Engine that was used for games such as _Killzone: Shadow
Fall_ and _Horizon: Zero Dawn_. Another example, Frostbite Engine uses
_Smith height-correlated masking-shadowing function_ [29]:

```
G(l,v,h) =
```
```
χ +(v·h) χ +(l·h)
1 +Λ(v) +Λ(l)
```
#### , (4.9)

```
Λ(m) =
```
#### − 1 +

#### √

```
1 + α^2 tan^2 ( θ m)
2
```
#### . (4.10)

```
The most widely used diffuse term of the BRDF that represents
local subsurface scattering is the Lambertian shading model, which
has a constant value:
fd(l,v) =
```
```
ρ
π
```
#### . (4.11)

The constant reflectance _ρ_ of the Lambertian model is typically referred
to as the diffuse color, also known as the albedo.

```
4.6.2 Blending Materials
```
The proposed technique also uses the pixel shader to combine the
BRDF parameters of deformed and undeformed snow. The color pa-
rameters are blended using linear interpolation based on the defor-
mation heightmap. The normal textures are blended using reoriented
normal mapping, which is a quaternion-based technique that blends
two normal maps correctly [8].

### 4.7 Creating Textures

Textures for video games are commonly created using photogramme-
try or procedural generation using a node-based approach where each
node represents a specific function with inputs and outputs. These
nodes are connected into a network which then outputs the desired
result. The popular material authoring tool used for game develop-
ment is Substance Designer, which was also used for creating the snow


4. Implementation

textures. See Figure 4.2. However, the procedural generation is not
limited to textures; it is also used for procedural modeling, terrain
generation, and animations. For example, Ubisoft Entertainment used
SideFX Houdini for procedural world generation in _Far Cry 5_ [31].

```
Figure 4.2: Undeformed snow material rendered using PBR.
```
Another popular approach used in AAA games is photogramme-
try. It is a process of authoring digital assets such as textures and
geometry from multiple images. The usual process of creating game-
ready assets using photogrammetry is divided into the following
stages: taking series of photographs of the object from various angles,
aligning photographs in a photogrammetry software that is used for
the reconstruction, generating a high-resolution mesh with color data,
mesh simplification, and mesh retopology. Textures are then baked
from the high-resolution mesh on the low-resolution version. Many
games contain assets created using photogrammetry. For example,
developers from DICE visited locations such as California’s national
redwood forests and Iceland to capture complete asset libraries for
_Star Wars: Battlefront_ [32].



## 5 Example Scene

```
The snow deformation technique presented in Chapter 4 was imple-
mented into Unity Engine 2020.3.4f1 LTS. The implementation uses
assets from the official Unity 3D Game Kit, such as characters, anima-
tions, and particle systems, in order to create a testing environment
for the deformable snow. The implementation details of the testing
environment are presented in this chapter.
```
### 5.1 Snow Renderer

The deformable snow renderer has a custom C# script that prepares
all render targets needed for the snow deformation based on the given
parameters. It also gets the shader uniform locations and binds re-
sources that do not change during the execution before the game starts
to minimize changes in the graphics rendering pipeline. The renderer
swaps the persistent ping-pong buffer to correctly accumulate the
deformed snow and updates the player offset provided by the sliding
window. The custom chain of compute shaders is dispatched each
frame before executing the standard rendering pipeline to minimize
switches between the rendering pipeline and compute shaders, which
significantly improves performance [33].
The deformable snow shader is implemented using PBR. It utilizes
the same BRDF function as the Unity standard PBR shader. The dif-
fuse term of indirect light is approximated using spherical harmonics;
the specular term is sampled from the nearest HDR cube map. The
cube map can be generated using a reflection probe, which captures a
spherical view of its surroundings. The reflection probes can be evalu-
ated in real-time and therefore support also dynamic objects. Cube
maps used for lighting computations are utilized in many games. For
example, Guerrilla Games implemented a voxel structure for _Killzone:
Shadow Fall_ to place the light probes effectively [34]. The light probes
are placed in empty voxels which have at least one neighbor occupied
by static geometry. They are then interpolated using a tetrahedral
mesh.
The deformable snow shader has a custom shadow caster pass
in order to support self-shadowing. The shadow pass uses the same


5. Example Scene

vertex shader as the standard render pass, but it uses an empty pixel
shader to minimize unnecessary computation. Unity renders real-time
shadows using a technique called _shadow mapping_. Each object that
casts shadows is rendered using the defined shadow caster pass into
the z-buffer from the light source position. The generated z-buffer is
called the _shadow map_ , which contains the depth of the closest object
to the light source. The shadow map is then sampled in the pixel
shader during the regular render pass. If the processed fragment is
farther away from the light source than the sampled value from the
shadow map, the fragment is in shadow. For distant directional light,
it is sufficient to generate a single shadow map using an orthographic
projection. However, local light sources surrounded by shadow casters
are usually rendered into a cube map, known as the _omnidirectional
shadow map_ [4]. The disadvantage of the shadow mapping technique
is that the visual quality of the rendered shadows depends on the
precision of the z-buffer and the resolution of the shadow map.

### 5.2 Level Design

In order to test the implemented deformable snow, the testing envi-
ronment has a third-person camera that follows the player. The scene
contains a terrain created in 3Ds Max with simplified geometry used
for collision detection and standard geometry for rendering the de-
formable snow. The simplified terrain is also utilized for generating
navigation meshes used by the enemies for pathfinding. There are
three types of enemies (Chomper, Spitter, and Grenadier) that attack
the player. Each enemy is implemented using a state machine and
uses ray casts to determine if the player is visible or not. Chomper is a
simple close-range melee enemy that chases the player. Spitter fires
acid projectiles that explode when they collide with a different object.
Finally, Grenadier is a boss enemy with complex behavior. All these
enemies with different sets of animations and particle systems deform
the snow geometry. In addition, the scene contains custom particle
systems that simulate blizzard conditions. Hence, filling the created
trails over time looks more realistic.
The created testing environment utilizes the deferred rendering
path and uses a global post-processing volume which adds bloom,


5. Example Scene

Figure 5.1: Chomper is one of the tested enemies. On the left is the
geometry rendered using PBR. On the right is the wireframe. The
mesh contains 14 702 triangles.

ambient occlusion, and color grading. The bloom effect is caused by
scattering in the lens, which creates a glow around the light source.
The standard implementations used in games extract bright texels
of the rendered image into a temporary render target, which is then
downscaled into a mipmap chain. Each generated mipmap is then
filtered and added to the original image. Computing the bloom effect
at lower resolution increases the size of the bloom and improves overall
performance [35].
Many games use precomputed ambient occlusion stored in tex-
tures. However, for dynamic scenes with moving objects, better results
can be achieved by computing the ambient occlusion on the fly. The
real-time ambient occlusion is commonly estimated using _screen-space
ambient occlusion_ (SSAO), which is computed using the z-buffer in a
full-screen pass. For each texel of the rendered image, the algorithm
compares a set of samples distributed in a sphere around the texel’s
position against the z-buffer [36].
Color grading in video games is commonly performed by inter-
actively manipulating the colors of the rendered scene in external


5. Example Scene

software used for composition until the desired result is achieved. The
color grading is then baked into a three-dimensional color lookup
table (LUT), which is then used to remap the colors of the rendered
image [37].
A screenshot from the testing environment with the implemented
deformable snow is shown in Figure 5.2.

Figure 5.2: A shot from the testing environment with the implemented
deformable snow.


## 6 Results

```
This chapter presents memory requirements and execution times of the
individual render passes evaluated on multiple GPUs. Furthermore,
this chapter shows the visual quality achieved with the deformable
snow in a winter forest.
```
### 6.1 Memory Requirements

```
Similarly, as in Horizon Zero Dawn: The Frozen Wilds , the desired visual
quality with a 64 × 64 m deformable region around the player was
achieved using 1024 × 1024 buffers with a resolution of6.25cm per
texel, which adds up to 9 MB of VRAM. In addition, if the level contains
custom geometry that is not encoded in the terrain heightmap, the
algorithm also renders bounding volumes used for collision detection
into another 16-bit depth buffer which requires additional 2 MB of
VRAM.
```
### 6.2 Performance

A series of performance tests were conducted to analyze the GPU cost
of the implemented snow deformation technique. The measurements
were done using NVIDIA Nsight Graphics, a standalone tool used
for profiling and debugging on NVIDIA GPUs. It allows capturing
individual frames and performing a review of all rendering calls,
including the GPU pipeline state, visualization of geometry, textures,
and unordered access views. All tests were performed on a computer
with an Intel Core i7-8700 CPU (3.2 GHz), 32 GB DDR4-2666 RAM,
and different NVIDIA GPUs.
The GPU cost of the snow deformation technique is broken into
four parts: rendering the snow-affecting objects using the orthographic
frustum from below into the depth buffer, computing the snow defor-
mation, filtering the heightmap using Gaussian blur, and computing
the normals using the central finite difference method. The cost of
rendering the snow-affecting objects depends on the vertex complexity
and the number of objects present in the orthographic view frustum,


6. Results

```
Table 6.1: Measured GPU performance of the compute shaders.
```
```
GPU Deformation Gaussian Blur Normals
GTX 1060 0.07 ms 0.13 ms 0.14 ms
GTX 1070 0.05 ms 0.08 ms 0.10 ms
RTX 2070 0.04 ms 0.06 ms 0.04 ms
RTX 2070S 0.03 ms 0.05 ms 0.03 ms
```
but in general, it takes0.01ms per object. The measured performance
of the compute shaders used for the other render passes is shown in
Table 6.1.
The measured times of the compute shaders are expected due to
the performance of the GPUs. Computing the normals using central
finite difference requires neighbor texels. These texels are fetched into
LDS using gather instructions, which loads the four texels that would
be used for hardware bilinear interpolation. The required time for
computing the normals using LDS improved significantly on GPUs
powered by NVIDIA Turing architecture.
In order to create a smooth transition between deformed and unde-
formed snow, the deformation heightmap is filtered using a Gaussian
blur with a 5 × 5 kernel. Unfortunately, the filter is implemented using
two separate passes, horizontal and vertical, that require multiple
state transitions using resource barriers. In addition, the vertical pass
needs more time than the horizontal pass due to cache misses. For
example, the horizontal pass takes only 0.03 ms on GTX 1070, but the
vertical pass requires 0.05 ms.
Another series of performance tests were done to analyze the GPU
cost of the game environment presented in Chapter 5. The tests uti-
lized the deferred renderer with the target screen resolution set to
1920 × 1200. The testing environment contains real-time directional
light and baked reflection probes used for indirect lighting. Both static
and dynamic objects receive soft shadows with four cascades set to
very high resolution. The renderer accumulates frames over time
which are used for temporal antialiasing. The environment also con-
tains a global post-processing volume used to add bloom, ambient


6. Results

occlusion, and color grading. The average frame rates of the testing
environment measured on multiple GPUs are shown in Figure 6.1.

```
0 50 100 150 200 250
frames per second
```
```
GTX 1060
```
```
GTX 1070
```
```
RTX 2070
```
```
RTX 2070S
```
```
130
```
```
176
```
```
206
```
```
251
```
```
Figure 6.1: Average frame rate measured on multiple GPUs.
```
### 6.3 Visual Quality

Game studios employ various sophisticated algorithms to push the
boundaries of 3D computer graphics with realistic open-world envi-
ronments. The visual quality of the proposed technique depends on
the resolution of the deformation heightmap, tessellation level, and
provided textures. Therefore, the technique was also implemented in
a realistic forest environment. The environment consists of various
assets such as trees, rocks, and grasses. Similarly to the testing envi-
ronment, it utilizes a deferred renderer with a custom post-processing
layer that adds bloom, ambient occlusion, and color grading. The
achieved visual quality of the deformable snow in the winter forest is
shown in Figure 6.2 and 6.3.


6. Results

Figure 6.2: A shot from the winter forest with the implemented de-
formable snow.

Figure 6.3: The deformable snow material with different parameters.


## 7 Conclusion

This thesis presented a scalable real-time snow deformation technique
implemented on the GPU using compute shaders and hardware tes-
sellation. It is based on the approach used in _Horizon Zero Dawn: The
Frozen Wilds_. The method proposed by Guerrilla Games was extended
using the snow elevation along the edges of the deformation. Using
a sliding window that moves the deformation heightmap with the
player anywhere in the world allows this technique to be used in a
massive open-world game. It is also not limited to snow deformation;
it has various uses, such as mud rendering in an offroad simulator
or collision detection between dynamic objects and vegetation. The
vegetation model can move the vertices along normals sampled from
the deformation buffer.
The algorithm renders the snow-affecting objects using an ortho-
graphic frustum centered on the player from below into a depth buffer.
The rendered depth is then used to determine if the deformation has to
be applied. The deformation heightmap is accumulated in a persistent
ping-pong buffer. The algorithm remaps the heightmap using a cubic
function and filters the height using a separable Gaussian blur filter.
Finally, the normals are computed using the finite difference method
based on the deformation heightmap. The height and normals are
then stored in a buffer that can be accessed by any shader.
The implementation using compute shaders allowed the use of
LDS, which is not accessible in the graphics rendering pipeline. It
is a high-bandwidth, and low-latency explicitly addressed memory
used to prefetch data from the video memory. Using LDS, the algo-
rithm reduced redundant loads, which significantly improved overall
performance.
Future research should investigate a more extensive persistent
solution that is not limited to the size of the view frustum but can
stream the generated deformation to the memory, which can be used
later to stream the deformation back if the player returns.



## Bibliography

1. PERSSON, Emil. Volume Decals. In: ENGEL, Wolfgang (ed.).
    _GPU Pro_^2 _: Advanced Rendering Techniques_. 1st. CRC Press, 2011.
2. ST-AMOUR, Jean-Francois. Rendering _Assassin’s Creed III_. _Game_
    _Developers Conference (GDC)_. 2013. Available also from:www.
       gdcvault.com/play/1017710/Rendering-Assassin-s-Creed.
3. SCHEUERMANN, Thorsten. Render to Vertex Buffer with D3D9.
    _SIGGRAPH GPU Shading and Rendering Course_. 2006.
4. AKENINE-MÖLLER, Tomas; HAINES, Eric; HOFFMAN, Naty;
    PESCE, Angelo; HILLAIRE, Sebastien; IWANICKI, Michał. _Real-
Time Rendering, Fourth Edition_. 4th. CRC Press, 2018.
5. LENGYEL, Eric. T-Junction Elimination and Retriangulation. In:
    TREGLIA, Dante (ed.). _Game Programming GEMS 3_. 1st. Charles
    River Media, 2002.
6. BARRE-BRISEBOIS, Colin. Deformable Snow Rendering in _Bat-_
    _man: Arkham Origins_. _Game Developers Conference (GDC)_. 2014.
    Available also from:www.gdcvault.com/play/1020379/Deformable-
       Snow-Rendering-in-Batman.
7. OAT, Chris. A Steerable Streak Filter. In: ENGEL, Wolfgang (ed.).
    _Shader_ X^3 _Advanced Rendering with DirectX and OpenGL_. 1st. Charles
    River Media, 2004.
8. BARRÉ-BRISEBOIS, Colin; HILL, Stephen. _Blending in Detail with_
    _Reoriented Normal Mapping_ [online] [visited on 2021-04-01]. Avail-
    able from:https : / / blog. selfshadow. com / publications /
    blending-in-detail/.
9. POLICARPO, Fábio; OLIVEIRA, Manuel M.; COMBA, João L. D.
    Real-Time Relief Mapping on Arbitrary Polygonal Surfaces. In:
    _Proceedings of the 2005 Symposium on Interactive 3D Graphics and_
    _Games_. Association for Computing Machinery, 2005, pp. 155–162.
10. MICHELS, Anton Kai; SIKACHEV, Peter. Deferred Snow Defor-
mation in _Rise of the Tomb Raider_. In: ENGEL, Wolfgang (ed.).
    _GPU Pro_^7 _: Advanced Rendering Techniques_. 1st. CRC Press, 2016.


#### BIBLIOGRAPHY

11. ÖRTEGREN, Kevin. Real-time Snow Deformation in _Horizon Zero_
    _Dawn: The Frozen Wilds_. In: ENGEL, Wolfgang (ed.). _GPU Zen 2:_
    _Advanced Rendering Techniques_. 1st. Black Cat Publishing, 2019.
12. WIHLIDAL, Graham. Optimizing the Graphics Pipeline with
    Compute. In: ENGEL, Wolfgang (ed.). _GPU Zen: Advanced Ren-_
    _dering Techniques_. 1st. Black Cat Publishing, 2017.
13. BRINCK, Waylon; TANG, Qingzhou. The Technical Art of _The_
    _Last of Us Part II_. _SIGGRAPH Advances in Real-Time Rendering in_
       _3D Graphics and Games course_. 2020.
14. BRAWLEY, Zoe; TATARCHUK, Natalya. Parallax Occlusion Map-
    ping: Self-Shadowing, Perspective-Correct Bump Mapping Us-
       ing Reverse Height Map Tracing. In: ENGEL, Wolfgang (ed.).
    _Shader_ X^3 _Advanced Rendering with DirectX and OpenGL_. 1st. Charles
       River Media, 2004.
15. TATARCHUK, Natalya. Practical Parallax Occlusion Mapping
    with Approximate Soft Shadows for Detailed Surface Render-
       ing. _SIGGRAPH Advanced Real-Time Rendering in 3D Graphics and_
    _Games course_. 2006.
16. BLYTHE, David. The Direct3D 10 System. _ACM Transactions on_
    _Graphics_. 2006, vol. 25, no. 3, pp. 724–734.
17. SOUSA, Tiago. Vegetation Procedural Animation and Shading
    in _Crysis_. In: NGUYEN, Hubert (ed.). _GPU Gems 3_. 1st. Addison-
    Wesley, 2007.
18. ANDERSSON, Johan. Terrain Rendering in Frostbite Using Proce-
    dural Shader Splatting. _SIGGRAPH Advanced Real-Time Rendering_
    _in 3D Graphics and Games course_. 2007.
19. VLACHOS, Alex; PETERS, Jörg; BOY, Chas; MITCHEL, Jason L.
    Curved PN Triangles. In: _Proceedings of the 2001 Symposium on_
    _Interactive 3D Graphics_. 2001, pp. 159–166.
20. MCGUIRE, Morgan. Efficient Shadow Volume Rendering. In:
    FERNANDO, Randima (ed.). _GPU Gems: Programming Techniques,_
    _Tips and Tricks for Real-Time Graphics_. 1st. Addison-Wesley, 2004.
21. THIBIEROZ, Nicolas. Deferred Shading with Multiple Render
    Targets. In: ENGEL, Wolfgang (ed.). _Shader_ X^2 _: Shader Program-_
       _ming Tips & Tricks with DirectX 9_. 1st. Wordware, 2004.


#### BIBLIOGRAPHY

22. VALIENT, Michal. The Rendering Technology of _Killzone 2_. _Game_
    _Developers Conference (GDC)_. 2009. Available also from:www.
       gdcvault.com/play/1330/The- Rendering- Technology- of-
       KILLZONE.
23. LUNA, Frank. _Introduction to 3D Game Programming with DirectX_
    _12_. 1st. Mercury Learning & Information, 2016.
24. LENGYEL, Eric. _Mathematics for 3D Game Programming and Com-_
    _puter Graphics_. 3rd. Cengage Learning PTR, 2011.
25. COLLIN, Daniel. Culling the Battlefield. _Game Developers Confer-_
    _ence (GDC)_. 2011. Available also from:www.gdcvault.com/play/
    1014491/Culling-the-Battlefield-Data-Oriented.
26. COOK, Robert L.; TORRANCE, Kenneth E. A Reflectance Model
    for Computer Graphics. _ACM Transactions on Graphics_. 1982, vol. 1,
    no. 1, pp. 7–24.
27. BURLEY, Brent. Physically Based Shading at Disney. _SIGGRAPH_
    _Practical Physically Based Shading in Film and Game Production_
    _course_. 2012.
28. KARIS, Brian. Real Shading in Unreal Engine 4. _SIGGRAPH Phys-_
    _ically Based Shading in Theory and Practice course_. 2013.
29. LAGARDE, Sébastien; ROUSIERS, Charles. Moving Frostbite to
    Physically Based Rendering. _SIGGRAPH Physically Based Shading_
    _in Theory and Practice course_. 2014.
30. DROBOT, Michal. Lighting of _Killzone: Shadow Fall_. _Digital Drag-_
    _ons conference_. 2013.
31. MCAULEY, Stephen. The Challenges of Rendering an Open
    World in _Far Cry 5_. _SIGGRAPH Advances in Real-Time Rendering_
       _in 3D Graphics and Games course_. 2018.
32. BROWN, Kenneth; HAMILTON, Andrew. Photogrammetry and
    _Star Wars Battlefront_. _Game Developers Conference (GDC)_. 2016.
33. ZINK, Jason; PETTINEO, Matt; HOXLEY, Jack. _Practical Rendering_
    _& Computation with Direct3D 11_. 1st. CRC Press, 2011.


#### BIBLIOGRAPHY

34. VALIENT, Michal. Taking _Killzone: Shadow Fall_ Image Quality into
    the Next Generation. _Game Developers Conference (GDC)_. 2014.
Available also from:www.gdcvault.com/play/1020770/Taking-
Killzone-Shadow-Fall-Image.
35. JIMENEZ, Jorge. Next Generation Post Processing in _Call of Duty_
    _Advanced Warfare_. _SIGGRAPH Advances in Real-Time Rendering in_
       _3D Graphics and Games course_. 2014.
36. MITTRING, Martin. Finding Next Gen – CryEngine 2. _SIGGRAPH_
    _Advanced Real-Time Rendering in 3D Graphics and Games Course_.
       2007.
37. BRINCK, Waylon; MAXIMOV, Andrew. The Technical Art of
    _Uncharted 4_. _SIGGRAPH production session_. 2016.


## A Electronic Attachments

```
The following items are attached to this thesis:
```
- The build.zip archive containing the standalone application with
    the testing environment and implemented deformable snow.
- The source.zip archive containing the source files.


