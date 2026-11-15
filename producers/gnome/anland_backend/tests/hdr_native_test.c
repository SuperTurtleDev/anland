/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backends/anland/meta-anland-color.h"
#include "clutter/clutter-stage-view-private.h"
#include "clutter/clutter-color-pipeline-shader.h"
#include "cogl/cogl-renderer-egl.h"
#include "cogl/cogl-display-egl.h"
#include "cogl/cogl-context-egl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include <stdio.h>

/* Exercise the actual native StageView allocation/color pass without a desktop
 * frame clock or a monitor. Its GObject properties and GL code are unmodified. */
typedef struct { ClutterStageView parent; } HdrTestView;
typedef struct { ClutterStageViewClass parent; } HdrTestViewClass;
GType hdr_test_view_get_type (void);
G_DEFINE_TYPE (HdrTestView, hdr_test_view, CLUTTER_TYPE_STAGE_VIEW)
static void hdr_test_constructed (GObject *object) { (void) object; }
static void hdr_test_view_class_init (HdrTestViewClass *klass)
{ G_OBJECT_CLASS (klass)->constructed = hdr_test_constructed; }
static void hdr_test_view_init (HdrTestView *self) { (void) self; }

typedef struct { CoglDisplayEGL parent; } HdrTestDisplay;
typedef struct { CoglDisplayEGLClass parent; } HdrTestDisplayClass;
GType hdr_test_display_get_type (void);
G_DEFINE_TYPE (HdrTestDisplay, hdr_test_display, COGL_TYPE_DISPLAY_EGL)
static int test_attributes (CoglDisplayEGL *display, EGLint *attrs)
{ (void) display; (void) attrs; return 0; }
static gboolean test_context_created (CoglDisplayEGL *display, GError **error)
{
  CoglRendererEGL *renderer = COGL_RENDERER_EGL(cogl_display_get_renderer(COGL_DISPLAY(display)));
  (void) error;
  return eglMakeCurrent(cogl_renderer_egl_get_edisplay(renderer),EGL_NO_SURFACE,EGL_NO_SURFACE,cogl_display_egl_get_egl_context(display));
}
static void hdr_test_display_class_init (HdrTestDisplayClass *klass)
{
  COGL_DISPLAY_EGL_CLASS(klass)->add_config_attributes=test_attributes;
  COGL_DISPLAY_EGL_CLASS(klass)->context_created=test_context_created;
}
static void hdr_test_display_init (HdrTestDisplay *self) { (void) self; }

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#expr); failures++; } } while (0)
static double pq (double nits)
{
  double p = pow (fmax (nits, 0) / 10000.0, 2610.0 / 16384.0);
  return pow ((3424.0/4096.0 + (2413.0/128.0)*p) /
              (1 + (2392.0/128.0)*p), 2523.0/32.0);
}
static MtkRegion *debts[3];
static unsigned int current;
static const MtkRegion *forced_copy;
static MtkRegion *copy_region (ClutterStageView *view, const MtkRegion *damage, gpointer data)
{
  (void) view; (void) data;
  MtkRegion *region = meta_anland_accumulate_copy_damage (debts, 3, current, damage);
  if (forced_copy) { mtk_region_unref(region); return mtk_region_copy(forced_copy); }
  return region;
}
static CoglFramebuffer *new_fbo (CoglContext *ctx, int width, int height, CoglPixelFormat format)
{
  GError *error = NULL;
  CoglTexture *texture = cogl_texture_2d_new_with_format (ctx,width,height,format);
  CHECK (cogl_texture_allocate (texture,&error));
  if (error) g_error ("texture: %s",error->message);
  CoglFramebuffer *fbo = COGL_FRAMEBUFFER (cogl_offscreen_new_with_texture (texture));
  g_object_unref (texture);
  CHECK (cogl_framebuffer_allocate (fbo,&error));
  if (error) g_error ("fbo: %s",error->message);
  return fbo;
}
static void read_pixel (CoglFramebuffer *fbo, int x, int y, float out[4])
{
  CoglContext *ctx = cogl_framebuffer_get_context (fbo);
  uint32_t packed = 0;
  gboolean packed10 = cogl_framebuffer_get_red_bits(fbo)==10;
  CoglBitmap *bitmap = cogl_bitmap_new_for_data (ctx,1,1,
    packed10 ? COGL_PIXEL_FORMAT_ABGR_2101010 : COGL_PIXEL_FORMAT_RGBA_FP_32323232,
    packed10 ? 4 : 16, packed10 ? (uint8_t *)&packed : (uint8_t *)out);
  CHECK (cogl_framebuffer_read_pixels_into_bitmap (fbo,x,y,COGL_READ_PIXELS_COLOR_BUFFER,bitmap));
  g_object_unref (bitmap);
  if(packed10) { out[0]=(packed&1023)/1023.0f; out[1]=((packed>>10)&1023)/1023.0f; out[2]=((packed>>20)&1023)/1023.0f; out[3]=(packed>>30)/3.0f; }
}
static void test_policy (void)
{
  const char *off[] = {"0"," false ","NO","oFf"};
  for (unsigned int i=0;i<G_N_ELEMENTS(off);i++) CHECK(anland_hdr_environment_disabled(off[i]));
  CHECK(!anland_hdr_environment_disabled(NULL)); CHECK(!anland_hdr_environment_disabled("1"));
  CHECK(anland_hdr_mode_for_hal(43,true)==ANLAND_HDR_HDR10);
  CHECK(anland_hdr_mode_for_hal(43,false)==ANLAND_HDR_REJECTED);
  CHECK(anland_hdr_mode_for_hal(99,true)==ANLAND_HDR_REJECTED);
  for(unsigned int f=1;f<=5;f++) if(f==1||f==2||f==5)
    CHECK(anland_hdr_mode_for_hal(f,true)==ANLAND_HDR_SDR);
}
static void test_transforms (CoglContext *ctx, ClutterColorState *scene_color, ClutterColorState *wire_color)
{
  for(int transform=0;transform<8;transform++) {
    gboolean rotated=mtk_monitor_transform_is_rotated(transform);
    MtkRectangle layout={.width=rotated?11:17,.height=rotated?17:11};
    MtkRectangle patch={.x=3,.y=2,.width=5,.height=4};
    MtkRegion *full=mtk_region_create_rectangle(&layout), *small=mtk_region_create_rectangle(&patch), *empty=mtk_region_create();
    CoglFramebuffer *out[3];
    for(int i=0;i<3;i++) { if(debts[i]) mtk_region_unref(debts[i]); debts[i]=mtk_region_copy(full); out[i]=new_fbo(ctx,17,11,COGL_PIXEL_FORMAT_ABGR_2101010); }
    ClutterStageView *view=g_object_new(hdr_test_view_get_type(),"name","transform-test","layout",&layout,"framebuffer",out[0],"color-state",scene_color,"output-color-state",wire_color,"transform",transform,NULL);
    clutter_stage_view_set_anland_hdr_copy_func(view,copy_region,NULL);
    while(g_main_context_iteration(NULL,FALSE));
    CoglFramebuffer *scene=clutter_stage_view_get_framebuffer(view);
    CHECK(cogl_framebuffer_get_red_bits(scene)>=16);
    cogl_framebuffer_clear4f(scene,COGL_BUFFER_BIT_COLOR,1,1,1,1);
    current=0; clutter_stage_view_after_paint(view,full);
    current=1; clutter_stage_view_replace_framebuffer(view,out[1]); clutter_stage_view_after_paint(view,empty);
    cogl_framebuffer_orthographic(scene,0,0,layout.width,layout.height,-1,1);
    cogl_framebuffer_push_rectangle_clip(scene,patch.x,patch.y,patch.width,patch.height);
    cogl_framebuffer_clear4f(scene,COGL_BUFFER_BIT_COLOR,0.203f,0.010f,0.001f,1);
    cogl_framebuffer_pop_clip(scene);
    current=0; clutter_stage_view_replace_framebuffer(view,out[0]); clutter_stage_view_after_paint(view,small);
    current=1; clutter_stage_view_replace_framebuffer(view,out[1]); clutter_stage_view_after_paint(view,empty);
    current=0; clutter_stage_view_replace_framebuffer(view,out[0]); forced_copy=full; clutter_stage_view_after_paint(view,empty); forced_copy=NULL;
    int differences=0;
    for(int y=0;y<11;y++) for(int x=0;x<17;x++) { float a[4],b[4]; read_pixel(out[0],x,y,a);read_pixel(out[1],x,y,b);for(int c=0;c<3;c++) if(fabs(a[c]-b[c])*1023>0.1) differences++; }
    CHECK(differences==0); printf("transform %d partial/full differences=%d\n",transform,differences);
    mtk_region_unref(full);mtk_region_unref(small);mtk_region_unref(empty);
    g_object_unref(view); for(int i=0;i<3;i++)g_object_unref(out[i]);
  }
}
int main (void)
{
  GError *error = NULL;
  test_policy();
  EGLDisplay edpy = eglGetPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
  CoglRendererEGL *renderer = cogl_renderer_egl_new();
  cogl_renderer_egl_set_edisplay(renderer,edpy);
  if(!cogl_renderer_connect(COGL_RENDERER(renderer),&error)) g_error("renderer: %s",error->message);
  CoglDisplayEGL *display = g_object_new(hdr_test_display_get_type(),"renderer",renderer,NULL);
  if(!cogl_display_setup(COGL_DISPLAY(display),&error)) g_error("display: %s",error->message);
  CoglContext *ctx = cogl_context_egl_new(COGL_DISPLAY(display),&error);
  if(!ctx) g_error("context: %s",error->message);
  ClutterContext *cc = g_object_new(CLUTTER_TYPE_CONTEXT,NULL);
  ClutterColorState *scene_color = meta_anland_color_state_new(cc,TRUE,FALSE);
  ClutterColorState *wire_color = meta_anland_color_state_new(cc,TRUE,TRUE);
  CHECK(clutter_color_state_required_format(scene_color)==CLUTTER_ENCODING_REQUIRED_FORMAT_FP16);
  const ClutterLuminance *lum = clutter_color_state_params_get_luminance(CLUTTER_COLOR_STATE_PARAMS(wire_color));
  CHECK(lum->min==0 && lum->max==10000 && lum->ref==203);
  CHECK(lum->mastering_max==1000);
  MtkRectangle layout={.x=0,.y=0,.width=17,.height=11};
  MtkRegion *full=mtk_region_create_rectangle(&layout);
  MtkRegion *empty=mtk_region_create();
  CoglFramebuffer *out[3];
  for(int i=0;i<3;i++) { out[i]=new_fbo(ctx,17,11,COGL_PIXEL_FORMAT_ABGR_2101010); debts[i]=mtk_region_copy(full); }
  ClutterStageView *view=g_object_new(hdr_test_view_get_type(),"name","anland-hdr-test","layout",&layout,"framebuffer",out[0],"color-state",scene_color,"output-color-state",wire_color,NULL);
  clutter_stage_view_set_anland_hdr_copy_func(view,copy_region,NULL);
  while(g_main_context_iteration(NULL,FALSE));
  CoglFramebuffer *scene=clutter_stage_view_get_framebuffer(view);
  CHECK(scene!=out[0]); CHECK(cogl_framebuffer_get_red_bits(scene)>=16);
  cogl_framebuffer_clear4f(out[0],COGL_BUFFER_BIT_COLOR,1,0.5,0.25,1);
  float trial[4]; read_pixel(out[0],8,5,trial); CHECK(trial[0]>0.999f && fabs(trial[1]-0.5f)<0.001f);
  const double levels[]={0,0.001,0.005,1,203,1000,2000};
  float pixels[4];
  for(unsigned int l=0;l<G_N_ELEMENTS(levels);l++) {
    cogl_framebuffer_clear4f(scene,COGL_BUFFER_BIT_COLOR,(float)(levels[l]/1000),(float)(levels[l]/1000),(float)(levels[l]/1000),1);

    current=0; clutter_stage_view_after_paint(view,full);
    cogl_framebuffer_finish(out[0]); read_pixel(out[0],8,5,pixels);

    double expected=pq(fmin(levels[l],1000));
    CHECK(fabs(pixels[0]-expected)*1023<1.1);
    printf("level %.3f nit: %.7f expected %.7f\n",levels[l],pixels[0],expected);
  }
  /* Rotating slots catches up from one persistent scene, including no-damage
   * frames. Final conversion must touch only the selected slot's debt. */
  current=1; clutter_stage_view_replace_framebuffer(view,out[1]); clutter_stage_view_after_paint(view,empty);
  current=2; clutter_stage_view_replace_framebuffer(view,out[2]); clutter_stage_view_after_paint(view,empty);
  CHECK(mtk_region_is_empty(debts[0])&&mtk_region_is_empty(debts[1])&&mtk_region_is_empty(debts[2]));
  MtkRectangle patch={.x=3,.y=2,.width=5,.height=4};
  MtkRegion *small=mtk_region_create_rectangle(&patch);
  cogl_framebuffer_orthographic(scene,0,0,17,11,-1,1);
  cogl_framebuffer_push_rectangle_clip(scene,patch.x,patch.y,patch.width,patch.height);
  cogl_framebuffer_clear4f(scene,COGL_BUFFER_BIT_COLOR,0.203f,0.010f,0.001f,1);
  cogl_framebuffer_pop_clip(scene);
  current=2; clutter_stage_view_after_paint(view,small);
  for(int i=0;i<3;i++) {
    current=i; clutter_stage_view_replace_framebuffer(view,out[i]); clutter_stage_view_after_paint(view,empty);
    read_pixel(out[i],4,3,pixels);
    CHECK(fabs(pixels[0]-pq(203))*1023<1.1); CHECK(fabs(pixels[1]-pq(10))*1023<1.1); CHECK(fabs(pixels[2]-pq(1))*1023<1.1);
    read_pixel(out[i],12,8,pixels); CHECK(fabs(pixels[0]-pq(1000))*1023<1.1);
  }
  CHECK(mtk_region_is_empty(debts[0])&&mtk_region_is_empty(debts[1])&&mtk_region_is_empty(debts[2]));
  /* Queueing copy-only work records an empty repaint, preserving scene age. */
  MtkRegion *pending=NULL; if(clutter_stage_view_has_redraw_clip(view)) { pending=clutter_stage_view_take_accumulated_redraw_clip(view); if(pending) mtk_region_unref(pending); }
  for (int i=0;i<3;i++) {
    /* Even a current slot needs a frame/present; empty debt cannot suppress
     * the copy-only redraw marker that drives Mutter's present path. */
    current=i; clutter_stage_view_replace_framebuffer(view,out[i]);
    clutter_stage_view_queue_anland_hdr_copy(view);
    CHECK(clutter_stage_view_has_redraw_clip(view));
    pending=clutter_stage_view_take_accumulated_redraw_clip(view); CHECK(pending&&mtk_region_is_empty(pending));
    clutter_stage_view_after_paint(view,pending); mtk_region_unref(pending);
    read_pixel(out[i],4,3,pixels); CHECK(fabs(pixels[0]-pq(203))*1023<1.1);
  }
  ClutterColorState *sdr=meta_anland_color_state_new(cc,FALSE,FALSE);
  CoglPipeline *sdr_pipeline=cogl_pipeline_new(ctx);
  clutter_color_pipeline_shader_set_color_state(sdr_pipeline,sdr,scene_color,CLUTTER_COLOR_STATE_TRANSFORM_OPAQUE);
  cogl_framebuffer_clear4f(scene,COGL_BUFFER_BIT_COLOR,0,0,0,1);
  cogl_framebuffer_draw_rectangle(scene,sdr_pipeline,0,0,17,11);
  current=0; clutter_stage_view_replace_framebuffer(view,out[0]); clutter_stage_view_after_paint(view,full);
  read_pixel(out[0],8,5,pixels); CHECK(fabs(pixels[0]-pq(203))*1023<1.1);
  printf("SDR white PQ %.7f expected %.7f\n",pixels[0],pq(203));
  /* Native HDR client transform must retain highlights above SDR white. */
  CoglPipeline *hdr_pipeline=cogl_pipeline_new(ctx);
  clutter_color_pipeline_shader_set_color_state(hdr_pipeline,wire_color,scene_color,CLUTTER_COLOR_STATE_TRANSFORM_OPAQUE);
  CoglFramebuffer *hdr_client=new_fbo(ctx,17,11,COGL_PIXEL_FORMAT_RGBA_FP_32323232);
  cogl_framebuffer_clear4f(hdr_client,COGL_BUFFER_BIT_COLOR,(float)pq(1000),(float)pq(1000),(float)pq(1000),1);
  cogl_pipeline_set_layer_texture(hdr_pipeline,0,cogl_offscreen_get_texture(COGL_OFFSCREEN(hdr_client)));
  cogl_framebuffer_draw_textured_rectangle(scene,hdr_pipeline,0,0,17,11,0,0,1,1);
  read_pixel(scene,8,5,pixels); CHECK(fabs(pixels[0]-1.0)<0.002);
  current=0; clutter_stage_view_after_paint(view,full);
  read_pixel(out[0],8,5,pixels); CHECK(fabs(pixels[0]-pq(1000))*1023<1.1);
  printf("HDR client 1000-nit PQ %.7f expected %.7f\n",pixels[0],pq(1000));
  g_object_unref(hdr_client); g_object_unref(hdr_pipeline); g_object_unref(sdr_pipeline);
  g_object_unref(view);
  test_transforms(ctx,scene_color,wire_color);
  printf("RESULT %s failures=%d\n",failures?"FAIL":"PASS",failures);
  return failures?1:0;
}
