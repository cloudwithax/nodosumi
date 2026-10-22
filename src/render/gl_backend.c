#include <glad/glad.h>
#include "core/core.h"
#include "host/host.h"
#include "render/render_backend.h"
#include "core/thread.h"
#if !PLATFORM_WINDOWS
#include <dlfcn.h>
#endif

enum texture_map {
  MAP_DIFFUSE,
};

enum uniform_attr {
  UNIFORM_PROJ,
  UNIFORM_DIFFUSE,
  UNIFORM_VIDEO_SCALE,
  UNIFORM_ALPHA_REF,
  UNIFORM_ATTRS,
  UNIFORM_NUM_UNIFORMS,
};

static const char *uniform_names[] = {
    "u_proj", "u_diffuse", "u_video_scale", "u_alpha_ref", "u_attrs",
};

enum shader_attr {
  /* shade attributes are mutually exclusive, so they don't use unique bits */
  ATTR_SHADE_DECAL = 0x0,
  ATTR_SHADE_MODULATE = 0x1,
  ATTR_SHADE_DECAL_ALPHA = 0x2,
  ATTR_SHADE_MODULATE_ALPHA = 0x3,
  ATTR_SHADE_MASK = 0x3,
  /* remaining attributes can all be combined together */
  ATTR_TEXTURE = 0x4,
  ATTR_IGNORE_ALPHA = 0x8,
  ATTR_IGNORE_TEXTURE_ALPHA = 0x10,
  ATTR_OFFSET_COLOR = 0x20,
  ATTR_ALPHA_TEST = 0x40,
  ATTR_DEBUG_DEPTH_BUFFER = 0x80,
  ATTR_COUNT = 0x100
};

struct shader_prog {
  GLuint prog;
  GLuint vertex_shader;
  GLuint fragment_shader;
  GLint loc[UNIFORM_NUM_UNIFORMS];

  /* the last global uniforms bound to this program */
  uint64_t uniform_token;

  /* the last alpha_ref bound to this program, -1 if none */
  int alpha_ref;

  /* last attrs bound to the uber program, -1 if none */
  int attrs;
};

struct texture {
  GLuint texture;
  /* min filter for after r_make_mipmaps, 0 if none pending */
  GLenum mip_filter;
};

struct viewport {
  int x, y, w, h;
};

#define TA_NUM_VBOS 3

struct render_backend {
  struct host *host;
  int width, height;

  /* current viewport */
  struct viewport viewport;

  /* default assets created during intitialization */
  GLuint white_texture;
  struct shader_prog ta_progs[ATTR_COUNT];
  struct shader_prog ui_program;

  /* driver binaries in one file per game, keyed by driver + shader source,
     so a variant compiles once instead of every boot. written by the main
     thread at boot, then by the compile thread */
  FILE *prog_file;

  /* missing variants compile on a thread with its own context (shared with
     the frontend's) so the main thread never waits. until one is ready its
     surfaces use the uber program, reading attrs from a uniform */
  struct shader_prog uber;
  void *egl_lib;
  void *egl_display;
  void *egl_context;
  void *egl_surface;
  thread_t cthread;
  mutex_t cmutex;
  cond_t ccond;
  int quit;
  int cqueue[ATTR_COUNT];
  int qhead;
  int qtail;
  /* compile thread marks these under cmutex, main thread takes the
     program out of compiled */
  uint8_t done[ATTR_COUNT];
  struct shader_prog compiled[ATTR_COUNT];
  /* main thread only */
  uint8_t queued[ATTR_COUNT];

  /* offscreen framebuffer for blitting raw pixels */
  GLuint pixel_fbo;
  GLuint pixel_texture;

  /* framebuffer for drawing to a texture, and the one bound before it was */
  GLuint target_fbo;
  GLuint target_depth;
  int target_width, target_height;
  GLint target_prev_fbo;
  int target;

  /* texture cache */
  struct texture textures[MAX_TEXTURES];
  /* destroyed during a convert, but an earlier context may still draw them.
     only r_delete_dead_textures actually deletes */
  texture_handle_t dead_textures[MAX_TEXTURES];
  int num_dead_textures;
  /* waiting on mipmaps, oldest first. the driver builds them on the cpu and a
     scene can bring dozens at once, so do a few each frame. until then the top
     level is used */
  texture_handle_t mip_textures[MAX_TEXTURES];
  int num_mip_textures;

  /* surface render state */
  GLuint ta_vao;
  /* tr_convert_context writes into the next one. the last converted frame is
     only drawn after the next convert, so these rotate, not orphan */
  GLuint ta_vbos[TA_NUM_VBOS];
  int ta_vbo_sizes[TA_NUM_VBOS];
  int ta_next_vbo;
  GLuint ta_ibo;
  /* indices append to ta_ibo unsynchronized, the gpu only reads behind the
     head. on overflow, orphan the storage and start at 0 */
  int ta_ibo_size, ta_ibo_head;
  int ta_ibo_offset;
  GLuint ui_vao;
  GLuint ui_vbo;
  GLuint ui_ibo;
  int ui_use_ibo;

  /* global uniforms that are constant for every surface rendered between a call
     to begin_surfaces and end_surfaces */
  uint64_t uniform_token;
  float uniform_video_scale[4];

  /* gl state last set by r_draw_ta_surface, -1 when unknown. each gl call costs
     the mali driver a revalidation, so redundant ones are skipped. reset by
     r_begin_ta_surfaces, as other paths change the same state */
  int ta_depth_write;
  int ta_depth_func;
  int ta_cull;
  int ta_blend;
  GLuint ta_prog;
  GLuint ta_texture;
  /* set while the surfaces mark the stencil with whether they're shadowed, and
     which they last marked it with */
  int ta_stencil;
  int ta_shadow;
};

#include "render/ta.glsl"
#include "render/ui.glsl"

static GLenum filter_funcs[] = {
    GL_NEAREST,               /* FILTER_NEAREST */
    GL_LINEAR,                /* FILTER_BILINEAR */
    GL_NEAREST_MIPMAP_LINEAR, /* FILTER_NEAREST + mipmaps */
    GL_LINEAR_MIPMAP_LINEAR   /* FILTER_BILINEAR + mipmaps */
};

static GLenum wrap_modes[] = {
    GL_REPEAT,         /* WRAP_REPEAT */
    GL_CLAMP_TO_EDGE,  /* WRAP_CLAMP_TO_EDGE */
    GL_MIRRORED_REPEAT /* WRAP_MIRRORED_REPEAT */
};

static GLenum depth_funcs[] = {
    GL_NONE,     /* DEPTH_NONE */
    GL_NEVER,    /* DEPTH_NEVER */
    GL_LESS,     /* DEPTH_LESS */
    GL_EQUAL,    /* DEPTH_EQUAL */
    GL_LEQUAL,   /* DEPTH_LEQUAL */
    GL_GREATER,  /* DEPTH_GREATER */
    GL_NOTEQUAL, /* DEPTH_NEQUAL */
    GL_GEQUAL,   /* DEPTH_GEQUAL */
    GL_ALWAYS    /* DEPTH_ALWAYS */
};

static GLenum cull_face[] = {
    GL_NONE,  /* CULL_NONE */
    GL_FRONT, /* CULL_FRONT */
    GL_BACK   /* CULL_BACK */
};

static GLenum blend_funcs[] = {GL_NONE,
                               GL_ZERO,
                               GL_ONE,
                               GL_SRC_COLOR,
                               GL_ONE_MINUS_SRC_COLOR,
                               GL_SRC_ALPHA,
                               GL_ONE_MINUS_SRC_ALPHA,
                               GL_DST_ALPHA,
                               GL_ONE_MINUS_DST_ALPHA,
                               GL_DST_COLOR,
                               GL_ONE_MINUS_DST_COLOR};

static GLenum prim_types[] = {
    GL_TRIANGLES, /* PRIM_TRIANGLES */
    GL_LINES,     /* PRIM_LINES */
};

static GLuint internal_formats[] = {
    GL_RGB,  /* PXL_RGB */
    GL_RGBA, /* PXL_RGBA */
    GL_RGBA, /* PXL_RGBA5551 */
    GL_RGB,  /* PXL_RGB565 */
    GL_RGBA, /* PXL_RGBA4444 */
};

static GLuint pixel_formats[] = {
    GL_UNSIGNED_BYTE,          /* PXL_RGB */
    GL_UNSIGNED_BYTE,          /* PXL_RGBA */
    GL_UNSIGNED_SHORT_5_5_5_1, /* PXL_RGBA5551 */
    GL_UNSIGNED_SHORT_5_6_5,   /* PXL_RGB565 */
    GL_UNSIGNED_SHORT_4_4_4_4, /* PXL_RGBA4444 */
};

static inline void r_bind_texture(struct render_backend *r,
                                  enum texture_map map, GLuint tex) {
  glActiveTexture(GL_TEXTURE0 + map);
  glBindTexture(GL_TEXTURE_2D, tex);
}

static void r_print_shader_log(GLuint shader) {
  int max_length, length;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_length);

  char *info_log = malloc(max_length);
  glGetShaderInfoLog(shader, max_length, &length, info_log);
  LOG_INFO(info_log);
  free(info_log);
}

static int r_compile_shader(const char *source, GLenum shader_type,
                            GLuint *shader) {
  size_t sourceLength = strlen(source);

  *shader = glCreateShader(shader_type);
  glShaderSource(*shader, 1, (const GLchar **)&source,
                 (const GLint *)&sourceLength);
  glCompileShader(*shader);

  GLint compiled;
  glGetShaderiv(*shader, GL_COMPILE_STATUS, &compiled);

  if (!compiled) {
    r_print_shader_log(*shader);
    glDeleteShader(*shader);
    return 0;
  }

  return 1;
}

static void r_destroy_program(struct shader_prog *program) {
  if (program->vertex_shader) {
    glDeleteShader(program->vertex_shader);
  }

  if (program->fragment_shader) {
    glDeleteShader(program->fragment_shader);
  }

  if (program->prog) {
    glDeleteProgram(program->prog);
  }

  memset(program, 0, sizeof(*program));
}

static void r_setup_program(struct shader_prog *program);

/* gles 3.0 on every platform */
#define GLSL_VERSION "300 es"

static int r_compile_program(struct render_backend *r,
                             struct shader_prog *program, const char *header,
                             const char *vertex_source,
                             const char *fragment_source) {
  char buffer[16384] = {0};

  memset(program, 0, sizeof(*program));
  program->alpha_ref = -1;
  program->attrs = -1;
  program->prog = glCreateProgram();

  if (vertex_source) {
    snprintf(buffer, sizeof(buffer) - 1, "#version " GLSL_VERSION
                                         "\n"
                                         "%s%s",
             header ? header : "", vertex_source);
    buffer[sizeof(buffer) - 1] = 0;

    if (!r_compile_shader(buffer, GL_VERTEX_SHADER, &program->vertex_shader)) {
      r_destroy_program(program);
      return 0;
    }

    glAttachShader(program->prog, program->vertex_shader);
  }

  if (fragment_source) {
    snprintf(buffer, sizeof(buffer) - 1, "#version " GLSL_VERSION
                                         "\n"
                                         "%s%s",
             header ? header : "", fragment_source);
    buffer[sizeof(buffer) - 1] = 0;

    if (!r_compile_shader(buffer, GL_FRAGMENT_SHADER,
                          &program->fragment_shader)) {
      r_destroy_program(program);
      return 0;
    }

    glAttachShader(program->prog, program->fragment_shader);
  }

  if (glProgramParameteri) {
    glProgramParameteri(program->prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT,
                        GL_TRUE);
  }
  glLinkProgram(program->prog);

  GLint linked;
  glGetProgramiv(program->prog, GL_LINK_STATUS, &linked);

  if (!linked) {
    r_destroy_program(program);
    return 0;
  }

  r_setup_program(program);

  return 1;
}

static void r_setup_program(struct shader_prog *program) {
  for (int i = 0; i < UNIFORM_NUM_UNIFORMS; i++) {
    program->loc[i] = glGetUniformLocation(program->prog, uniform_names[i]);
  }

  /* bind diffuse sampler once after compile, this currently never changes */
  glUseProgram(program->prog);
  glUniform1i(program->loc[UNIFORM_DIFFUSE], MAP_DIFFUSE);
  glUseProgram(0);
}

static void r_destroy_compile_thread(struct render_backend *r);

static void r_destroy_shaders(struct render_backend *r) {
  r_destroy_compile_thread(r);

  if (r->prog_file) {
    fclose(r->prog_file);
    r->prog_file = NULL;
  }

  for (int i = 0; i < ATTR_COUNT; i++) {
    r_destroy_program(&r->ta_progs[i]);
  }

  r_destroy_program(&r->uber);
  r_destroy_program(&r->ui_program);
}

static int r_queue_prog(struct render_backend *r, int idx,
                                int required);
static void r_open_cache(struct render_backend *r, const char *path);
static void r_create_compile_thread(struct render_backend *r);

static void r_create_shaders(struct render_backend *r,
                             const char *prog_file) {
  if (!r_compile_program(r, &r->ui_program, NULL, ui_vp, ui_fp)) {
    LOG_FATAL("failed to compile ui shader");
  }

  /* load cached variants */
  if (prog_file && glGetProgramBinary && glProgramBinary) {
    r_open_cache(r, prog_file);
  }

  /* precompile common variants so first use doesn't hitch for frames. the
     rest compile lazily in r_get_ta_program */
  for (int shade = 0; shade < 4; shade++) {
    for (int texture = 0; texture < 2; texture++) {
      for (int alpha_test = 0; alpha_test < 2; alpha_test++) {
        for (int offset_color = 0; offset_color < 2; offset_color++) {
          int idx = shade;
          if (texture) {
            idx |= ATTR_TEXTURE;
          }
          if (alpha_test) {
            idx |= ATTR_ALPHA_TEST;
          }
          if (offset_color) {
            idx |= ATTR_OFFSET_COLOR;
          }
          r_queue_prog(r, idx, 0);
        }
      }
    }
  }

  r_queue_prog(r, ATTR_COUNT, 0);
  r_create_compile_thread(r);
}

static void r_destroy_textures(struct render_backend *r) {
  glDeleteTextures(1, &r->white_texture);

  glDeleteFramebuffers(1, &r->pixel_fbo);
  glDeleteTextures(1, &r->pixel_texture);

  glDeleteFramebuffers(1, &r->target_fbo);
  glDeleteRenderbuffers(1, &r->target_depth);

  for (int i = 0; i < MAX_TEXTURES; i++) {
    struct texture *tex = &r->textures[i];

    if (!tex->texture) {
      continue;
    }

    glDeleteTextures(1, &tex->texture);
  }
}

static void r_create_textures(struct render_backend *r) {
  /* create default all white texture */
  uint8_t pixels[64 * 64 * 4];
  memset(pixels, 0xff, sizeof(pixels));

  glGenTextures(1, &r->white_texture);
  glBindTexture(GL_TEXTURE_2D, r->white_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels);
  glBindTexture(GL_TEXTURE_2D, 0);

  /* create fbo for blitting raw framebuffers to */
  glGenFramebuffers(1, &r->pixel_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, r->pixel_fbo);

  glGenTextures(1, &r->pixel_texture);
  glBindTexture(GL_TEXTURE_2D, r->pixel_texture);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
               GL_UNSIGNED_SHORT_5_6_5, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GLenum buffers[] = {GL_COLOR_ATTACHMENT0};
  glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, r->pixel_texture,
                       0);
  glDrawBuffers(ARRAY_SIZE(buffers), buffers);

  GLenum res = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  CHECK_EQ(res, GL_FRAMEBUFFER_COMPLETE);

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void r_destroy_vertex_arrays(struct render_backend *r) {
  glDeleteBuffers(1, &r->ui_ibo);
  glDeleteBuffers(1, &r->ui_vbo);
  glDeleteVertexArrays(1, &r->ui_vao);

  glDeleteBuffers(1, &r->ta_ibo);
  glDeleteBuffers(TA_NUM_VBOS, r->ta_vbos);
  glDeleteVertexArrays(1, &r->ta_vao);
}

/* about 15 frames of a busy scene */
#define TA_IBO_SIZE (1024 * 1024)

/* append len bytes to the stream buffer bound to target, returning the offset
   they were written at */
static int r_stream_data(GLenum target, int *size, int *head, const void *data,
                         int len) {
  if (*head + len > *size) {
    *size = MAX(*size, len);
    glBufferData(target, *size, NULL, GL_STREAM_DRAW);
    *head = 0;
  }

  void *ptr = glMapBufferRange(target, *head, len,
                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                   GL_MAP_UNSYNCHRONIZED_BIT);
  memcpy(ptr, data, len);
  glUnmapBuffer(target);

  int offset = *head;
  *head = (*head + len + 63) & ~63;
  return offset;
}

static void r_create_vertex_arrays(struct render_backend *r) {
  /* ui vao */
  {
    glGenVertexArrays(1, &r->ui_vao);
    glBindVertexArray(r->ui_vao);

    glGenBuffers(1, &r->ui_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->ui_vbo);

    glGenBuffers(1, &r->ui_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ui_ibo);

    /* xy */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct ui_vertex),
                          (void *)offsetof(struct ui_vertex, xy));

    /* texcoord */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct ui_vertex),
                          (void *)offsetof(struct ui_vertex, uv));

    /* color */
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(struct ui_vertex),
                          (void *)offsetof(struct ui_vertex, color));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  /* ta vao */
  {
    glGenVertexArrays(1, &r->ta_vao);
    glBindVertexArray(r->ta_vao);

    glGenBuffers(TA_NUM_VBOS, r->ta_vbos);

    glGenBuffers(1, &r->ta_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ta_ibo);
    r->ta_ibo_size = TA_IBO_SIZE;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, r->ta_ibo_size, NULL, GL_STREAM_DRAW);

    /* xyz, texcoord, color and offset color, pointed at each frame's
       vertices by r_begin_ta_surfaces */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
}

static void r_set_initial_state(struct render_backend *r) {
  glDepthMask(1);
  glDisable(GL_DEPTH_TEST);

  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);

  glDisable(GL_BLEND);
}

/* compile variant idx, or the uber program when idx is ATTR_COUNT. this leaves
   no gl state behind, so the compile thread can call it too */
static int r_build_prog(struct render_backend *r,
                                struct shader_prog *program, int idx) {
  char header[1024];

  if (idx == ATTR_COUNT) {
    snprintf(header, sizeof(header), "#define UBER\n");
  } else {
    snprintf(header, sizeof(header),
             "#define SHADE %d\n"
             "#define TEXTURE %s\n"
             "#define IGNORE_ALPHA %s\n"
             "#define IGNORE_TEXTURE_ALPHA %s\n"
             "#define OFFSET_COLOR %s\n"
             "#define ALPHA_TEST %s\n"
             "%s",
             idx & ATTR_SHADE_MASK, (idx & ATTR_TEXTURE) ? "true" : "false",
             (idx & ATTR_IGNORE_ALPHA) ? "true" : "false",
             (idx & ATTR_IGNORE_TEXTURE_ALPHA) ? "true" : "false",
             (idx & ATTR_OFFSET_COLOR) ? "true" : "false",
             (idx & ATTR_ALPHA_TEST) ? "true" : "false",
             (idx & ATTR_DEBUG_DEPTH_BUFFER) ? "#define DEBUG_DEPTH_BUFFER\n"
                                             : "");
  }

  return r_compile_program(r, program, header, ta_vp, ta_fp);
}

/* the driver and the shader source both decide what a binary holds */
static void r_cache_key(char *key, size_t size) {
  const char *sources[] = {ta_vp, ta_fp};
  uint32_t hash = 2166136261u;
  int i;

  for (i = 0; i < ARRAY_SIZE(sources); i++) {
    for (const char *c = sources[i]; *c; c++) {
      hash = (hash ^ (uint8_t)*c) * 16777619u;
    }
  }

  snprintf(key, size, "nodosumi programs 1|%s|%s|%08x",
           (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION), hash);
}

static void r_save_prog(struct render_backend *r, GLuint prog, int idx) {
  if (!r->prog_file) {
    return;
  }

  GLint size = 0;
  glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &size);
  if (size <= 0) {
    return;
  }

  void *data = malloc(size);
  GLsizei length = 0;
  GLenum format = 0;
  glGetProgramBinary(prog, size, &length, &format, data);

  if (length > 0) {
    uint32_t entry[3] = {(uint32_t)idx, (uint32_t)format, (uint32_t)length};
    fwrite(entry, sizeof(entry), 1, r->prog_file);
    fwrite(data, 1, length, r->prog_file);
    fflush(r->prog_file);
  }

  free(data);
}

static int r_load_prog(struct shader_prog *program, GLenum format,
                          const void *data, GLsizei length) {
  memset(program, 0, sizeof(*program));
  program->alpha_ref = -1;
  program->attrs = -1;
  program->prog = glCreateProgram();
  glProgramBinary(program->prog, format, data, length);

  GLint linked;
  glGetProgramiv(program->prog, GL_LINK_STATUS, &linked);

  if (!linked) {
    r_destroy_program(program);
    return 0;
  }

  r_setup_program(program);

  return 1;
}

/* the file starts with the key, then idx/format/length and the binary for each
   program. a mismatched or short file is started over */
/* TODO prune entries, the file only grows */
static void r_open_cache(struct render_backend *r, const char *path) {
  char key[512];
  r_cache_key(key, sizeof(key));
  uint32_t key_len = (uint32_t)strlen(key);
  int valid = 0;
  int loaded = 0;

  FILE *f = fopen(path, "rb");
  if (f) {
    char stored[512];
    uint32_t stored_len = 0;
    valid = fread(&stored_len, sizeof(stored_len), 1, f) == 1 &&
            stored_len == key_len &&
            fread(stored, 1, key_len, f) == key_len &&
            !memcmp(stored, key, key_len);

    while (valid) {
      uint32_t entry[3];
      size_t got = fread(entry, 1, sizeof(entry), f);
      if (!got) {
        break;
      }
      if (got != sizeof(entry) || entry[0] > ATTR_COUNT ||
          entry[2] > (1 << 22)) {
        valid = 0;
        break;
      }

      void *data = malloc(entry[2]);
      if (fread(data, 1, entry[2], f) != entry[2]) {
        valid = 0;
      } else {
        int idx = (int)entry[0];
        struct shader_prog *program =
            idx == ATTR_COUNT ? &r->uber : &r->ta_progs[idx];
        if (!program->prog &&
            r_load_prog(program, entry[1], data, entry[2])) {
          loaded++;
        }
      }
      free(data);
    }

    fclose(f);
  }

  LOG_INFO("r_open_cache %s, %d programs loaded%s", path, loaded,
           valid ? "" : ", starting over");

  r->prog_file = fopen(path, valid ? "ab" : "wb");
  if (r->prog_file && !valid) {
    fwrite(&key_len, sizeof(key_len), 1, r->prog_file);
    fwrite(key, 1, key_len, r->prog_file);
    fflush(r->prog_file);
  }
}

static int r_queue_prog(struct render_backend *r, int idx,
                                int required) {
  struct shader_prog *program =
      idx == ATTR_COUNT ? &r->uber : &r->ta_progs[idx];

  if (program->prog) {
    return 1;
  }

  int res = r_build_prog(r, program, idx);
  if (!res) {
    if (required) {
      CHECK(res, "failed to compile ta shader");
    } else {
      LOG_WARNING("skipping ta shader variant 0x%x", idx);
    }
  } else if (!r->cthread) {
    /* once there's a compile thread, the cache is its to write */
    r_save_prog(r, program->prog, idx);
  }
  return res;
}

#if !PLATFORM_WINDOWS
#define EGL_CONFIG_ID 0x3028
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

static unsigned (*egl_make_current)(void *, void *, void *, void *);
static unsigned (*egl_destroy_context)(void *, void *);
static unsigned (*egl_destroy_surface)(void *, void *);

/* a context shared with the current one, with a surface to make it current on.
   the frontend's context isn't ours to share, so it's found through egl */
static int r_create_compile_context(struct render_backend *r) {
  void *(*get_current_display)(void);
  void *(*get_current_context)(void);
  unsigned (*query_context)(void *, void *, int, int *);
  unsigned (*choose_config)(void *, const int *, void **, int, int *);
  void *(*create_context)(void *, void *, void *, const int *);
  void *(*create_pbuffer)(void *, void *, const int *);
  static const int context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  static const int surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  int config_attrs[] = {EGL_CONFIG_ID, 0, EGL_NONE};
  void *config = NULL;
  int found = 0;

  r->egl_lib = dlopen("libEGL.so.1", RTLD_NOW);
  if (!r->egl_lib) {
    r->egl_lib = dlopen("libEGL.so", RTLD_NOW);
  }
  if (!r->egl_lib) {
    return 0;
  }

  *(void **)&get_current_display = dlsym(r->egl_lib, "eglGetCurrentDisplay");
  *(void **)&get_current_context = dlsym(r->egl_lib, "eglGetCurrentContext");
  *(void **)&query_context = dlsym(r->egl_lib, "eglQueryContext");
  *(void **)&choose_config = dlsym(r->egl_lib, "eglChooseConfig");
  *(void **)&create_context = dlsym(r->egl_lib, "eglCreateContext");
  *(void **)&create_pbuffer = dlsym(r->egl_lib, "eglCreatePbufferSurface");
  *(void **)&egl_make_current = dlsym(r->egl_lib, "eglMakeCurrent");
  *(void **)&egl_destroy_context = dlsym(r->egl_lib, "eglDestroyContext");
  *(void **)&egl_destroy_surface = dlsym(r->egl_lib, "eglDestroySurface");
  if (!get_current_display || !get_current_context || !query_context ||
      !choose_config || !create_context || !create_pbuffer ||
      !egl_make_current || !egl_destroy_context || !egl_destroy_surface) {
    return 0;
  }

  void *display = get_current_display();
  void *current = get_current_context();
  if (!display || !current ||
      !query_context(display, current, EGL_CONFIG_ID, &config_attrs[1]) ||
      !choose_config(display, config_attrs, &config, 1, &found) || !found) {
    return 0;
  }

  r->egl_display = display;
  r->egl_context = create_context(display, config, current, context_attrs);
  if (!r->egl_context) {
    return 0;
  }

  r->egl_surface = create_pbuffer(display, config, surface_attrs);
  return r->egl_surface != NULL;
}

static void *r_compile_thread(void *data) {
  struct render_backend *r = data;

  egl_make_current(r->egl_display, r->egl_surface, r->egl_surface,
                   r->egl_context);

  mutex_lock(r->cmutex);

  while (1) {
    while (!r->quit && r->qhead == r->qtail) {
      cond_wait(r->ccond, r->cmutex);
    }
    if (r->quit) {
      break;
    }

    int idx = r->cqueue[r->qhead++ % ATTR_COUNT];
    mutex_unlock(r->cmutex);

    struct shader_prog program;
    if (r_build_prog(r, &program, idx)) {
      r_save_prog(r, program.prog, idx);
    }

    /* the main thread's context may only use the program once this one is
       done with it */
    glFinish();

    mutex_lock(r->cmutex);
    r->compiled[idx] = program;
    r->done[idx] = 1;
  }

  mutex_unlock(r->cmutex);

  egl_make_current(r->egl_display, NULL, NULL, NULL);

  return NULL;
}
#endif

static void r_create_compile_thread(struct render_backend *r) {
#if !PLATFORM_WINDOWS
  if (!r->uber.prog || !r_create_compile_context(r)) {
    LOG_INFO("r_create_compile_thread no compile thread, compiling inline");
    return;
  }

  r->cmutex = mutex_create();
  r->ccond = cond_create();
  r->cthread = thread_create(&r_compile_thread, "shaders", r);
#endif
}

static void r_destroy_compile_thread(struct render_backend *r) {
#if !PLATFORM_WINDOWS
  if (r->cthread) {
    mutex_lock(r->cmutex);
    r->quit = 1;
    cond_signal(r->ccond);
    mutex_unlock(r->cmutex);
    thread_join(r->cthread, NULL);
    r->cthread = NULL;

    /* programs finished but never taken */
    for (int i = 0; i < ATTR_COUNT; i++) {
      if (r->done[i] && !r->ta_progs[i].prog) {
        r_destroy_program(&r->compiled[i]);
      }
    }

    cond_destroy(r->ccond);
    mutex_destroy(r->cmutex);
  }

  if (r->egl_surface) {
    egl_destroy_surface(r->egl_display, r->egl_surface);
  }
  if (r->egl_context) {
    egl_destroy_context(r->egl_display, r->egl_context);
  }
  if (r->egl_lib) {
    dlclose(r->egl_lib);
  }
#endif
}

static int r_ta_program_idx(const struct ta_surface *surf) {
  int idx = (int)surf->params.shade;
  if (surf->params.texture) {
    idx |= ATTR_TEXTURE;
  }
  if (surf->params.ignore_alpha) {
    idx |= ATTR_IGNORE_ALPHA;
  }
  if (surf->params.ignore_texture_alpha) {
    idx |= ATTR_IGNORE_TEXTURE_ALPHA;
  }
  if (surf->params.offset_color) {
    idx |= ATTR_OFFSET_COLOR;
  }
  if (surf->params.alpha_test) {
    idx |= ATTR_ALPHA_TEST;
  }
  if (surf->params.debug_depth) {
    idx |= ATTR_DEBUG_DEPTH_BUFFER;
  }
  return idx;
}

static struct shader_prog *r_get_ta_program(struct render_backend *r,
                                               int idx) {
  struct shader_prog *program = &r->ta_progs[idx];

  if (program->prog) {
    return program;
  }

  /* hand a variant the startup didn't have to the compile thread, standing in
     with the uber program until it's ready. debug variants aren't worth it */
  if (r->cthread && !(idx & ATTR_DEBUG_DEPTH_BUFFER)) {
    mutex_lock(r->cmutex);
    if (!r->queued[idx]) {
      r->queued[idx] = 1;
      r->cqueue[r->qtail++ % ATTR_COUNT] = idx;
      cond_signal(r->ccond);
    }
    int done = r->done[idx];
    if (done) {
      *program = r->compiled[idx];
    }
    mutex_unlock(r->cmutex);

    if (!done) {
      return &r->uber;
    }
    if (program->prog) {
      return program;
    }
  }

  /* compile here if there's no compile thread, or it failed to */
  r_queue_prog(r, idx, 1);

  return program;
}

void r_end_ui_surfaces(struct render_backend *r) {
  glDisable(GL_SCISSOR_TEST);
}

void r_draw_ui_surface(struct render_backend *r,
                       const struct ui_surface *surf) {
  if (surf->scissor) {
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)surf->scissor_rect[0], (int)surf->scissor_rect[1],
              (int)surf->scissor_rect[2], (int)surf->scissor_rect[3]);
  } else {
    glDisable(GL_SCISSOR_TEST);
  }

  if (surf->src_blend == BLEND_NONE || surf->dst_blend == BLEND_NONE) {
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendFunc(blend_funcs[surf->src_blend], blend_funcs[surf->dst_blend]);
  }

  if (surf->texture) {
    struct texture *tex = &r->textures[surf->texture];
    r_bind_texture(r, MAP_DIFFUSE, tex->texture);
  } else {
    r_bind_texture(r, MAP_DIFFUSE, r->white_texture);
  }

  if (r->ui_use_ibo) {
    glDrawElements(prim_types[surf->prim_type], surf->num_verts,
                   GL_UNSIGNED_SHORT,
                   (void *)(intptr_t)(sizeof(uint16_t) * surf->first_vert));
  } else {
    glDrawArrays(prim_types[surf->prim_type], surf->first_vert,
                 surf->num_verts);
  }
}

void r_begin_ui_surfaces(struct render_backend *r,
                         const struct ui_vertex *verts, int num_verts,
                         const uint16_t *indices, int num_indices) {
  /* setup projection matrix */
  float ortho[16];
  ortho[0] = 2.0f / (float)r->viewport.w;
  ortho[4] = 0.0f;
  ortho[8] = 0.0f;
  ortho[12] = -1.0f;

  ortho[1] = 0.0f;
  ortho[5] = -2.0f / (float)r->viewport.h;
  ortho[9] = 0.0f;
  ortho[13] = 1.0f;

  ortho[2] = 0.0f;
  ortho[6] = 0.0f;
  ortho[10] = 0.0f;
  ortho[14] = 0.0f;

  ortho[3] = 0.0f;
  ortho[7] = 0.0f;
  ortho[11] = 0.0f;
  ortho[15] = 1.0f;

  glDepthMask(0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  struct shader_prog *program = &r->ui_program;
  glBindVertexArray(r->ui_vao);
  glUseProgram(program->prog);
  glUniformMatrix4fv(program->loc[UNIFORM_PROJ], 1, GL_FALSE, ortho);

  /* bind buffers */
  glBindBuffer(GL_ARRAY_BUFFER, r->ui_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(struct ui_vertex) * num_verts, verts,
               GL_DYNAMIC_DRAW);

  if (indices) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ui_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16_t) * num_indices,
                 indices, GL_DYNAMIC_DRAW);
    r->ui_use_ibo = 1;
  } else {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    r->ui_use_ibo = 0;
  }
}

void r_end_ta_surfaces(struct render_backend *r) {
  if (r->ta_stencil) {
    glDisable(GL_STENCIL_TEST);
    r->ta_stencil = 0;
  }
}

static struct shader_prog *r_use_ta_program(struct render_backend *r,
                                            int idx) {
  struct shader_prog *program = r_get_ta_program(r, idx);

  if (program->prog != r->ta_prog) {
    glUseProgram(program->prog);
    r->ta_prog = program->prog;
  }

  /* the uber program stands in for variants still compiling */
  if (program == &r->uber && idx != program->attrs) {
    glUniform1i(program->loc[UNIFORM_ATTRS], idx);
    program->attrs = idx;
  }

  /* bind global uniforms if they've changed */
  if (program->uniform_token != r->uniform_token) {
    glUniform4fv(program->loc[UNIFORM_VIDEO_SCALE], 1, r->uniform_video_scale);
    program->uniform_token = r->uniform_token;
  }

  return program;
}

void r_draw_ta_modvols(struct render_backend *r, const struct ta_modvol *mods,
                       int num_mods, int shadow_vert) {
  /* each volume flips bit 1 of the stencil where its triangles are in front of
     the surfaces, leaving it set where they're inside it. its last triangles
     then fold that into bit 0, with what the volumes before it left there.
     this is how flycast's gles renderer does it */
  r_use_ta_program(r, 0);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthMask(GL_FALSE);
  glDepthFunc(GL_LESS);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);

  int base = 0;
  for (int i = 0; i < num_mods; i++) {
    const struct ta_modvol *mod = &mods[i];

    glEnable(GL_DEPTH_TEST);
    glStencilMask(2);
    if (mod->mode && !mod->closed) {
      /* the last triangles of a volume that isn't closed just cover it */
      glStencilFunc(GL_ALWAYS, 2, 2);
      glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    } else {
      glStencilFunc(GL_ALWAYS, 0, 2);
      glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
    }
    if (mod->num_verts) {
      glDrawArrays(GL_TRIANGLES, mod->first_vert, mod->num_verts);
    }

    if (mod->mode != 1 && mod->mode != 2) {
      continue;
    }

    glDisable(GL_DEPTH_TEST);
    glStencilMask(3);
    if (mod->mode == 1) {
      /* shadow inside: bit 0 = bit 1 | bit 0 */
      glStencilFunc(GL_LEQUAL, 1, 3);
      glStencilOp(GL_ZERO, GL_ZERO, GL_REPLACE);
    } else {
      /* shadow outside: bit 0 = !bit 1 & bit 0 */
      glStencilFunc(GL_EQUAL, 1, 3);
      glStencilOp(GL_ZERO, GL_ZERO, GL_KEEP);
    }
    int first = mods[base].first_vert;
    int n = mod->first_vert + mod->num_verts - first;
    if (n) {
      glDrawArrays(GL_TRIANGLES, first, n);
    }
    base = i + 1;
  }

  /* darken the shadow surfaces that ended up inside, leaving alpha be */
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glStencilFunc(GL_EQUAL, 0x81, 0x81);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glDrawArrays(GL_TRIANGLE_STRIP, shadow_vert, 4);

  /* r_clear clears the stencil through its mask */
  glStencilMask(0xff);
  glDisable(GL_STENCIL_TEST);
  r->ta_stencil = 0;

  r->ta_depth_write = -1;
  r->ta_depth_func = -1;
  r->ta_cull = -1;
  r->ta_blend = -1;
}

void r_draw_ta_surface(struct render_backend *r,
                       const struct ta_surface *surf) {
  int depth_write = surf->params.depth_write;
  if (depth_write != r->ta_depth_write) {
    glDepthMask(depth_write);
    r->ta_depth_write = depth_write;
  }

  int depth_func = surf->params.depth_func;
  if (depth_func != r->ta_depth_func) {
    if (depth_func == DEPTH_NONE) {
      glDisable(GL_DEPTH_TEST);
    } else {
      if (r->ta_depth_func == -1 || r->ta_depth_func == DEPTH_NONE) {
        glEnable(GL_DEPTH_TEST);
      }
      glDepthFunc(depth_funcs[depth_func]);
    }
    r->ta_depth_func = depth_func;
  }

  int cull = surf->params.cull;
  if (cull != r->ta_cull) {
    if (cull == CULL_NONE) {
      glDisable(GL_CULL_FACE);
    } else {
      if (r->ta_cull == -1 || r->ta_cull == CULL_NONE) {
        glEnable(GL_CULL_FACE);
      }
      glCullFace(cull_face[cull]);
    }
    r->ta_cull = cull;
  }

  int blend = 0;
  if (surf->params.src_blend != BLEND_NONE &&
      surf->params.dst_blend != BLEND_NONE) {
    blend = (surf->params.src_blend << 4) | surf->params.dst_blend;
  }
  if (blend != r->ta_blend) {
    if (!blend) {
      glDisable(GL_BLEND);
    } else {
      if (r->ta_blend <= 0) {
        glEnable(GL_BLEND);
      }
      glBlendFunc(blend_funcs[surf->params.src_blend],
                  blend_funcs[surf->params.dst_blend]);
    }
    r->ta_blend = blend;
  }

  int shadow = surf->params.shadow;
  if (r->ta_stencil && shadow != r->ta_shadow) {
    glStencilFunc(GL_ALWAYS, shadow ? 0x80 : 0, 0xff);
    r->ta_shadow = shadow;
  }

  struct shader_prog *program = r_use_ta_program(r, r_ta_program_idx(surf));

  /* uniform values stay with the program, so only rebind them on change */
  int alpha_ref = surf->params.alpha_ref;
  if (alpha_ref != program->alpha_ref) {
    glUniform1f(program->loc[UNIFORM_ALPHA_REF], alpha_ref / 255.0f);
    program->alpha_ref = alpha_ref;
  }

  if (surf->params.texture) {
    struct texture *tex = &r->textures[surf->params.texture];
    if (tex->texture != r->ta_texture) {
      r_bind_texture(r, MAP_DIFFUSE, tex->texture);
      r->ta_texture = tex->texture;
    }
  }

  glDrawRangeElements(GL_TRIANGLES, surf->min_vert, surf->max_vert,
                      surf->num_verts, GL_UNSIGNED_SHORT,
                      (void *)(intptr_t)(r->ta_ibo_offset +
                                         sizeof(uint16_t) * surf->first_vert));
}

struct ta_vertex *r_map_ta_vertices(struct render_backend *r, int max_verts,
                                    int *vert_buffer) {
  int i = r->ta_next_vbo;
  int size = sizeof(struct ta_vertex) * max_verts;
  r->ta_next_vbo = (i + 1) % TA_NUM_VBOS;

  glBindBuffer(GL_COPY_WRITE_BUFFER, r->ta_vbos[i]);
  if (size > r->ta_vbo_sizes[i]) {
    r->ta_vbo_sizes[i] = size;
    glBufferData(GL_COPY_WRITE_BUFFER, size, NULL, GL_STREAM_DRAW);
  }

  /* the buffer was last drawn from TA_NUM_VBOS frames ago, so the gpu is
     normally done with it and invalidating it doesn't cost new storage */
  *vert_buffer = i;
  return glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, size,
                          GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                              GL_MAP_FLUSH_EXPLICIT_BIT);
}

void r_unmap_ta_vertices(struct render_backend *r, int vert_buffer,
                         int num_verts) {
  glBindBuffer(GL_COPY_WRITE_BUFFER, r->ta_vbos[vert_buffer]);
  glFlushMappedBufferRange(GL_COPY_WRITE_BUFFER, 0,
                           sizeof(struct ta_vertex) * num_verts);
  glUnmapBuffer(GL_COPY_WRITE_BUFFER);
}

void r_begin_ta_surfaces(struct render_backend *r, int video_width,
                         int video_height, int vert_buffer,
                         const uint16_t *indices, int num_indices,
                         int shadows) {
  /* uniforms will be lazily bound for each program inside of r_draw_surface */
  r->uniform_token++;
  r->uniform_video_scale[0] = 2.0f / (float)video_width;
  r->uniform_video_scale[1] = -1.0f;
  r->uniform_video_scale[2] = -2.0f / (float)video_height;
  r->uniform_video_scale[3] = 1.0f;

  /* a target is drawn upside down, so its first row is the top of the image
     as it is in texture memory */
  if (r->target) {
    r->uniform_video_scale[2] = -r->uniform_video_scale[2];
    r->uniform_video_scale[3] = -1.0f;
  }

  r->ta_depth_write = -1;
  r->ta_depth_func = -1;
  r->ta_cull = -1;
  r->ta_blend = -1;
  r->ta_prog = 0;
  r->ta_texture = 0;

  /* each surface marks the stencil with whether it's shadowed where it's drawn,
     see r_draw_ta_modvols */
  r->ta_stencil = shadows;
  r->ta_shadow = -1;
  if (shadows) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  }

  glBindVertexArray(r->ta_vao);

  glBindBuffer(GL_ARRAY_BUFFER, r->ta_vbos[vert_buffer]);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct ta_vertex),
                        (void *)offsetof(struct ta_vertex, xyz));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct ta_vertex),
                        (void *)offsetof(struct ta_vertex, uv));
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                        sizeof(struct ta_vertex),
                        (void *)offsetof(struct ta_vertex, color));
  glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                        sizeof(struct ta_vertex),
                        (void *)offsetof(struct ta_vertex, offset_color));

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ta_ibo);
  r->ta_ibo_offset =
      r_stream_data(GL_ELEMENT_ARRAY_BUFFER, &r->ta_ibo_size, &r->ta_ibo_head,
                    indices, sizeof(uint16_t) * num_indices);
}

void r_draw_pixels(struct render_backend *r, const uint8_t *pixels, int x,
                   int y, int width, int height) {
  glBindTexture(GL_TEXTURE_2D, r->pixel_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
               GL_UNSIGNED_BYTE, pixels);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, r->pixel_fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(x, y + height, x + width, y, r->viewport.x, r->viewport.y,
                    r->viewport.x + r->viewport.w,
                    r->viewport.y + r->viewport.h, GL_COLOR_BUFFER_BIT,
                    GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void r_viewport(struct render_backend *r, int x, int y, int width, int height) {
  r->viewport.x = x;
  r->viewport.y = y;
  r->viewport.w = width;
  r->viewport.h = height;
  glViewport(r->viewport.x, r->viewport.y, r->viewport.w, r->viewport.h);
}

void r_clear(struct render_backend *r) {
  glDepthMask(1);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  /* stencil too: a tiled gpu otherwise loads it, packed with depth, from memory */
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void r_destroy_texture(struct render_backend *r, texture_handle_t handle) {
  if (!handle) {
    return;
  }

  r->dead_textures[r->num_dead_textures++] = handle;
}

void r_delete_dead_textures(struct render_backend *r) {
  for (int i = 0; i < r->num_dead_textures; i++) {
    struct texture *tex = &r->textures[r->dead_textures[i]];

    /* stop waiting for its mipmaps, or the next texture given the handle
       would get them */
    if (tex->mip_filter) {
      int j = 0;
      while (r->mip_textures[j] != r->dead_textures[i]) {
        j++;
      }
      r->num_mip_textures--;
      memmove(&r->mip_textures[j], &r->mip_textures[j + 1],
              (r->num_mip_textures - j) * sizeof(r->mip_textures[0]));
      tex->mip_filter = 0;
    }

    glDeleteTextures(1, &tex->texture);
    tex->texture = 0;
  }
  r->num_dead_textures = 0;
}

void r_make_mipmaps(struct render_backend *r) {
  /* four a frame costs the frame a few milliseconds at most */
  int n = MIN(r->num_mip_textures, 4);
  if (!n) {
    return;
  }

  for (int i = 0; i < n; i++) {
    struct texture *tex = &r->textures[r->mip_textures[i]];
    glBindTexture(GL_TEXTURE_2D, tex->texture);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tex->mip_filter);
    tex->mip_filter = 0;
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  r->num_mip_textures -= n;
  memmove(r->mip_textures, r->mip_textures + n,
          r->num_mip_textures * sizeof(r->mip_textures[0]));
}

void r_begin_target(struct render_backend *r, texture_handle_t handle,
                    int width, int height) {
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &r->target_prev_fbo);

  if (!r->target_fbo) {
    glGenFramebuffers(1, &r->target_fbo);
    glGenRenderbuffers(1, &r->target_depth);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, r->target_fbo);

  if (width != r->target_width || height != r->target_height) {
    glBindRenderbuffer(GL_RENDERBUFFER, r->target_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, r->target_depth);
    r->target_width = width;
    r->target_height = height;
  }

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         r->textures[handle].texture, 0);
  glViewport(0, 0, width, height);
  r_clear(r);

  r->target = 1;
}

void r_end_target(struct render_backend *r) {
  r->target = 0;

  glBindFramebuffer(GL_FRAMEBUFFER, r->target_prev_fbo);
  glViewport(r->viewport.x, r->viewport.y, r->viewport.w, r->viewport.h);

  /* the target was drawn from the vertices converted last, which aren't
     drawn again, so the next context converted can have their buffer */
  r->ta_next_vbo = (r->ta_next_vbo + TA_NUM_VBOS - 1) % TA_NUM_VBOS;
}

texture_handle_t r_create_texture(struct render_backend *r,
                                  enum pxl_format format,
                                  enum filter_mode filter,
                                  enum wrap_mode wrap_u, enum wrap_mode wrap_v,
                                  int mipmaps, int width, int height,
                                  const uint8_t *buffer) {
  /* find next open texture entry */
  texture_handle_t handle;
  for (handle = 1; handle < MAX_TEXTURES; handle++) {
    struct texture *tex = &r->textures[handle];
    if (!tex->texture) {
      break;
    }
  }
  CHECK_LT(handle, MAX_TEXTURES);

  GLuint internal_fmt = internal_formats[format];
  GLuint pixel_fmt = pixel_formats[format];

  struct texture *tex = &r->textures[handle];
  glGenTextures(1, &tex->texture);
  glBindTexture(GL_TEXTURE_2D, tex->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter_funcs[filter]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter_funcs[filter]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_modes[wrap_u]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_modes[wrap_v]);
  glTexImage2D(GL_TEXTURE_2D, 0, internal_fmt, width, height, 0, internal_fmt,
               pixel_fmt, buffer);

  /* the mipmaps are left to r_make_mipmaps */
  if (mipmaps) {
    tex->mip_filter = filter_funcs[NUM_FILTER_MODES + filter];
    r->mip_textures[r->num_mip_textures++] = handle;
  }

  glBindTexture(GL_TEXTURE_2D, 0);

  return handle;
}

int r_height(struct render_backend *r) {
  return r->height;
}

int r_width(struct render_backend *r) {
  return r->width;
}

void r_destroy(struct render_backend *r) {
  r_destroy_vertex_arrays(r);
  r_destroy_shaders(r);
  r_destroy_textures(r);

  free(r);
}

struct render_backend *r_create(int width, int height,
                                const char *prog_file) {
  struct render_backend *r = calloc(1, sizeof(struct render_backend));

  r->width = width;
  r->height = height;

  r_create_textures(r);
  r_create_shaders(r, prog_file);
  r_create_vertex_arrays(r);
  r_set_initial_state(r);

  return r;
}
