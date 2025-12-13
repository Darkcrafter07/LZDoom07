#ifndef GL_BSP_HPP_
#define GL_BSP_HPP_

#include "gl/scene/gl_scene.h"
#include "p_local.h"

class GLRenderer;
class GLWall;
class GLFlat;
class GLSprite;

class GLSceneDrawer {
public:
    GLSceneDrawer(GLRenderer *renderer);
    ~GLSceneDrawer();
    
    void UnclipSubsector(subsector_t *sub);
    void AddLine(seg_t *seg, bool portalclip);
    void PolySubsector(subsector_t *sub);
    void RenderPolyBSPNode(void *node);
    void AddPolyobjs(subsector_t *sub);
    void AddLines(subsector_t *sub, sector_t *sector);
    void AddSpecialPortalLines(subsector_t *sub, sector_t *sector, line_t *line);
    void RenderThings(subsector_t *sub, sector_t *sector);
    void DoSubsector(subsector_t *sub);
    void RenderBSPNode(void *node);
    
private:
    GLRenderer *GLRenderer;
    sector_t *currentsector;
    subsector_t *currentsubsector;
    angle_t viewangle;
    
    FClipper clipper;
    int validcount;
    area_t in_area;
    int rendered_lines;
    
    // Clock variables for performance timing
    FPerformanceTimer SetupWall;
    FPerformanceTimer SetupFlat;
    FPerformanceTimer SetupSprite;
    FPerformanceTimer ClipWall;

    TArray<seg_t*> checksegs;
};

// Console variables (CVARs) from gl_bsp.cpp
EXTERN_CVAR(Bool, gl_render_segs)
EXTERN_CVAR(Bool, gl_render_things)
EXTERN_CVAR(Bool, gl_render_walls)
EXTERN_CVAR(Bool, gl_render_flats)
EXTERN_CVAR(Float, gl_line_distance_cull)

// Helper functions
inline bool IsDistanceCulled(AActor* thing);
inline bool IsDistanceCulled(seg_t *line);
inline bool PointOnLine(const DVector2 &pos, const line_t *line);

#endif // GL_BSP_HPP_