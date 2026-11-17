/* Runs on an Android device without installing or replacing the app.
 * Include the frontend to exercise its JNI input and real presentation paths. */
#include "../../android/app/src/main/cpp/frontend.c"
#include <assert.h>
#include "../../src/render/render_backend.h"

extern int OPTION_widescreen;
extern int gladLoadGLLoader(retro_proc_address_t (*loader)(const char *));
extern void (*glad_glInvalidateFramebuffer)(GLenum, GLsizei, const GLenum *);

static void test_input(void) {
  for (int p = 0; p < MAX_PORTS; p++) {
    JNI(setInput)(NULL, NULL, p, 1 << RETRO_DEVICE_ID_JOYPAD_A,
                  -12345, 23456, 8192, 24575);
    assert(input_state(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                       RETRO_DEVICE_ID_JOYPAD_L2) == 8192);
    assert(input_state(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                       RETRO_DEVICE_ID_JOYPAD_R2) == 24575);
    assert(input_state(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                       RETRO_DEVICE_ID_ANALOG_X) == -12345);
    assert(input_state(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1);
    assert(input_state(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) == 0);
    JNI(setInput)(NULL, NULL, p, 1 << RETRO_DEVICE_ID_JOYPAD_L2, 0, 0, 8192, 0);
    assert(input_state(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                       RETRO_DEVICE_ID_JOYPAD_L2) == INT16_MAX);
    JNI(setInput)(NULL, NULL, p, 0, 0, 0, -1, 65535);
    assert(pads[p].lt == 0 && pads[p].rt == INT16_MAX);
    assert(set_rumble_state(p, RETRO_RUMBLE_STRONG, 12345));
    assert(set_rumble_state(p, RETRO_RUMBLE_WEAK, 23456));
    assert(JNI(rumble)(NULL, NULL, p) == 23456);
    set_rumble_state(p, RETRO_RUMBLE_STRONG, 0);
    set_rumble_state(p, RETRO_RUMBLE_WEAK, 0);
    assert(JNI(rumble)(NULL, NULL, p) == 0);
  }
  assert(input_state(MAX_PORTS, RETRO_DEVICE_JOYPAD, 0, 0) == 0);
  assert(!set_rumble_state(MAX_PORTS, RETRO_RUMBLE_STRONG, 1));
  puts("PASS: four-port analog pressure, digital input, clamps, rumble and stop");
}

static void paint(void) {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glEnable(GL_SCISSOR_TEST);
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      glScissor(x * video_width / 4, y * video_height / 4,
                video_width / 4, video_height / 4);
      glClearColor(x / 3.f, y / 3.f, (x + y) / 6.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
    }
  }
  glDisable(GL_SCISSOR_TEST);
}

static void compare(const char *aspect, int wide, int portrait) {
  option_set("nodosumi_aspect", aspect, 1);
  option_set("nodosumi_widescreen", wide ? "enabled" : "disabled", 1);
  surface_width = portrait ? 1080 : 1920;
  surface_height = portrait ? 1920 : 1080;
  size_t bytes = surface_width * surface_height * 4;
  unsigned char *reference = malloc(bytes), *actual = malloc(bytes);
  // Independent reference: always clear and blit the expected rectangle.
  const int dw = portrait ? 1080 : 1920;
  const int dh = portrait ? 1920 : 1080;
  const int stretch = !strcmp(aspect, "stretch") && !wide;
  const int is_wide = wide || !strcmp(aspect, "16:9");
  int h = stretch ? dh : dw * (is_wide ? 9 : 3) / (is_wide ? 16 : 4);
  int w = dw;
  if (h > dh) { h = dh; w = dh * (is_wide ? 16 : 4) / (is_wide ? 9 : 3); }
  int x = (dw - w) / 2;
  int y = portrait && !stretch ? dh - h : (dh - h) / 2;
  int sh = is_wide ? video_width * 9 / 16 : video_height;
  int sy = (video_height - sh) / 2;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glBlitFramebuffer(0, sy, video_width, sy + sh, x, y, x + w, y + h,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadPixels(0, 0, surface_width, surface_height, GL_RGBA, GL_UNSIGNED_BYTE, reference);
  glClearColor(1, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  present();
  glReadPixels(0, 0, surface_width, surface_height, GL_RGBA, GL_UNSIGNED_BYTE, actual);
  int max_error = 0;
  for (size_t i = 0; i < bytes; i++) {
    int error = abs((int)reference[i] - actual[i]);
    if (error > max_error) max_error = error;
  }
  printf("%dx%d %s wide=%d portrait=%d: maximum channel difference %d\n",
         video_width, video_height, aspect, wide, portrait, max_error);
  if (max_error > 2) {
    for (size_t i = 0; i < bytes; i += 4) {
      if (abs((int)reference[i] - actual[i]) > 2 || abs((int)reference[i+1] - actual[i+1]) > 2) {
        printf("Mismatch at %zu,%zu: expected %u %u %u %u actual %u %u %u %u GL=%x\n",
               (i/4)%surface_width, (i/4)/surface_width,
               reference[i],reference[i+1],reference[i+2],reference[i+3],
               actual[i],actual[i+1],actual[i+2],actual[i+3],glGetError());
        break;
      }
    }
  }
  assert(max_error <= 2);
  assert(glGetError() == GL_NO_ERROR);
  free(reference); free(actual);
}

static void test_projection(void) {
  gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
  assert(gladLoadGLLoader(get_proc_address));
  glad_glInvalidateFramebuffer = glInvalidateFramebuffer;
  struct render_backend *r = r_create(640, 360, NULL);
  const struct ta_vertex vertices[] = {
      {{0, 0, 1}, {0, 0}, UINT32_MAX, 0},
      {{640, 0, 1}, {0, 0}, UINT32_MAX, 0},
      {{0, 480, 1}, {0, 0}, UINT32_MAX, 0},
      {{640, 480, 1}, {0, 0}, UINT32_MAX, 0}};
  const uint16_t indices[] = {0, 1, 2, 2, 1, 3};
  struct ta_surface surf = {0};
  surf.params.depth_func = DEPTH_ALWAYS;
  surf.params.shade = SHADE_MODULATE;
  surf.num_verts = 6; surf.max_vert = 3;
  video_width = 640; video_height = 360;
  fbo_create();
  for (int mode = 0; mode < 3; mode++) {
    OPTION_widescreen = mode != 0;
    texture_handle_t target = 0;
    if (mode == 2) {
      target = r_create_texture(r, PXL_RGBA, FILTER_BILINEAR,
                                WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE,
                                0, 640, 360, NULL);
      r_begin_target(r, target, 640, 360);
    } else {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      r_viewport(r, 0, 0, 640, 360);
      r_clear(r);
    }
    r_begin_ta_surfaces(r, 640, 480, vertices, 4, indices, 6, 0);
    r_draw_ta_surface(r, &surf);
    r_end_ta_surfaces(r);
    unsigned char edge[4], center[4];
    glReadPixels(10, 180, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, edge);
    glReadPixels(320, 180, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    assert(center[0] == 255);
    assert(edge[0] == (mode == 1 ? 0 : 255));
    if (target) { r_end_target(r); r_destroy_texture(r, target); }
    assert(glGetError() == GL_NO_ERROR);
  }
  OPTION_widescreen = 0;
  r_destroy(r);
  fbo_destroy();
  puts("PASS: real TA widescreen projection and unmodified RTT projection/discard");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  test_input();
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  assert(eglInitialize(display, NULL, NULL));
  EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                   EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                   EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config;
  EGLint count;
  assert(eglChooseConfig(display, attrs, &config, 1, &count) && count);
  EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
  EGLint surface_attrs[] = {EGL_WIDTH, 1920, EGL_HEIGHT, 1920, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
  assert(eglMakeCurrent(display, surface, surface, context));
  printf("GPU: %s\n", glGetString(GL_RENDERER));
  test_projection();
  const int sizes[][2] = {{640,480}, {1280,960}, {2560,1920}, {2560,1440}};
  for (int i = 0; i < 4; i++) {
    video_width = sizes[i][0]; video_height = sizes[i][1];
    fbo_create();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    paint();
    compare("4:3", 0, 0);
    compare("16:9", 0, 0);
    compare("stretch", 0, 0);
    compare("4:3", 1, 0);
    compare("4:3", 1, 1);
    surface_width = 1920; surface_height = 1080;
    fbo_destroy();
  }
  puts("PASS: presentation parity, resizing, portrait and GL errors");
  return 0;
}
