/***********************************************************************/
/* Open Visualization Data Explorer                                    */
/* The scene file the executive writes for the new UI's GPU renderer.  */
/***********************************************************************/

/*
 * ADR 0003: the executive flattens the object an Image node renders into
 * drawable meshes (module DxuiScene); the UI draws them with OpenGL and
 * handles the camera itself. This header is the contract between the two:
 * the module writes it (C), the UI reads it (C++).
 *
 * Layout, little-endian, every field 4 bytes, no padding:
 *
 *   char     magic[8]          "DXSCENE1"
 *   uint32   chunkCount
 *   chunks, each:
 *     uint32 type              DXS_CHUNK_*
 *     uint32 bytes             payload length, a multiple of 4
 *     payload
 *
 *   DXS_CHUNK_MESH payload:
 *     uint32  primitive        DXS_POINTS, DXS_LINES (pairs of vertices),
 *                              DXS_TRIANGLES (triples)
 *     uint32  vertexCount
 *     uint32  indexCount       0: vertices are used in order
 *     uint32  flags            DXS_HAS_* below
 *     float32 matrix[16]       object to world, column-major (OpenGL order)
 *     float32 ambient, diffuse, specular, shininess   (DX "shading" values)
 *     float32 positions[3 * vertexCount]
 *     float32 normals[3 * vertexCount]        if DXS_HAS_NORMALS
 *     float32 colors[3 * vertexCount]         if DXS_HAS_COLORS (front colours)
 *     float32 opacities[vertexCount]          if DXS_HAS_OPACITIES
 *     uint32  indices[indexCount]
 *   A mesh without colours is drawn in DX's default colour (grey 0.7).
 *   Quads, polylines, faces and the like arrive already split into triangles
 *   and segments; colours attached to elements arrive per vertex.
 *
 *   DXS_CHUNK_LIGHT payload:
 *     uint32  kind             DXS_LIGHT_AMBIENT, DXS_LIGHT_DISTANT
 *     uint32  cameraRelative   1: direction is in camera space (DX "camera"
 *                              lights, and the default light)
 *     float32 color[3]
 *     float32 direction[3]     towards the light; unused for ambient
 *   The lights are the ones DX would render with, defaults included.
 *
 *   DXS_CHUNK_SCREEN payload: a mesh drawn in image pixels, as DX draws
 *   screen objects (captions, colour bars; libdx lbshade.Z _dxfScreen_Shade):
 *     uint32  position         DXS_SCREEN_* below: what `origin` means
 *     int32   z                +1 in front of everything, -1 behind, 0 in place
 *     float32 origin[3]        DXS_SCREEN_VIEWPORT: fraction of the image,
 *                              (0,0) lower left, (1,1) upper right;
 *                              DXS_SCREEN_PIXEL: pixels from the lower left;
 *                              DXS_SCREEN_WORLD: a world point, placed where
 *                              the camera sees it;
 *                              DXS_SCREEN_STATIONARY: ignored (image centre)
 *     then the DXS_CHUNK_MESH payload, its positions (after `matrix`) in
 *     pixels from the origin, x right, y up, at the executive's image size
 *   Readers that do not know a chunk type skip it, so this addition keeps
 *   the DXSCENE1 magic.
 *
 *   DXS_CHUNK_INFO payload:
 *     uint32  skipped[DXS_SKIPPED_COUNT]  what was not exported, by kind
 *     float32 bounds[6]        xmin ymin zmin xmax ymax zmax, world space
 */

#ifndef DXUISCENE_H
#define DXUISCENE_H

#define DXS_MAGIC "DXSCENE1"

#define DXS_CHUNK_MESH  1
#define DXS_CHUNK_LIGHT 2
#define DXS_CHUNK_INFO  3
#define DXS_CHUNK_SCREEN 4

/* DXS_CHUNK_SCREEN positions; the same numbers as DX's SCREEN_* (dx/screen.h). */
#define DXS_SCREEN_WORLD      0
#define DXS_SCREEN_VIEWPORT   1
#define DXS_SCREEN_PIXEL      2
#define DXS_SCREEN_STATIONARY 3

#define DXS_POINTS    1
#define DXS_LINES     2
#define DXS_TRIANGLES 3

#define DXS_HAS_NORMALS   1
#define DXS_HAS_COLORS    2
#define DXS_HAS_OPACITIES 4

#define DXS_LIGHT_AMBIENT 1
#define DXS_LIGHT_DISTANT 2

/* Indices into DXS_CHUNK_INFO's skipped[]. */
#define DXS_SKIPPED_SCREEN   0   /* screen objects not exported (none at present) */
#define DXS_SKIPPED_VOLUME   1   /* cubes, tetrahedra: volume rendering */
#define DXS_SKIPPED_TEXTURE  2   /* textured fields (drawn untextured) */
#define DXS_SKIPPED_CLIPPED  3   /* clipped objects (drawn unclipped) */
#define DXS_SKIPPED_OTHER    4
#define DXS_SKIPPED_COUNT    5

#endif /* DXUISCENE_H */
