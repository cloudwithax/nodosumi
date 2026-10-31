/*
 * a minimal libretro frontend for the android app. everything here except the
 * audio callback runs on the GLSurfaceView's render thread
 */

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <libretro.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "nodosumi"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#define JNI(name) Java_io_github_cloudwithax_nodosumi_NativeLibrary_##name

#define VIDEO_WIDTH 640
#define VIDEO_HEIGHT 480
#define AUDIO_FREQ 44100
#define MAX_PORTS 4

static char sysdir[1024];
static char game_path[1024];
static int started;

/*
 * options, set by the app before start and handed out through GET_VARIABLE
 */
#define MAX_OPTIONS 32

static struct {
  char key[64];
  char value[64];
} options[MAX_OPTIONS];
static int num_options;

static const char *option_get(const char *key) {
  for (int i = 0; i < num_options; i++) {
    if (!strcmp(options[i].key, key)) {
      return options[i].value;
    }
  }
  return NULL;
}

static void option_set(const char *key, const char *value, int overwrite) {
  int i = 0;
  while (i < num_options && strcmp(options[i].key, key)) {
    i++;
  }
  if (i == num_options) {
    if (num_options == MAX_OPTIONS) {
      return;
    }
    num_options++;
  } else if (!overwrite) {
    return;
  }
  snprintf(options[i].key, sizeof(options[i].key), "%s", key);
  snprintf(options[i].value, sizeof(options[i].value), "%s", value);
}

/*
 * audio, a single producer / single consumer ring between retro_run and the
 * aaudio callback
 */
#define RING_FRAMES 8192
/* how full the ring is kept, the pacing leans on this */
#define RING_TARGET 2048

static int16_t ring[RING_FRAMES * 2];
static atomic_uint ring_read;
static atomic_uint ring_write;
static AAudioStream *stream;
static atomic_int stream_lost;

static unsigned ring_fill() {
  return atomic_load(&ring_write) - atomic_load(&ring_read);
}

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user,
                                              void *data, int32_t frames) {
  int16_t *out = data;
  unsigned r = atomic_load_explicit(&ring_read, memory_order_relaxed);
  unsigned avail = atomic_load_explicit(&ring_write, memory_order_acquire) - r;
  unsigned n = avail < (unsigned)frames ? avail : (unsigned)frames;

  for (unsigned i = 0; i < n; i++) {
    unsigned idx = ((r + i) % RING_FRAMES) * 2;
    out[i * 2] = ring[idx];
    out[i * 2 + 1] = ring[idx + 1];
  }
  memset(out + n * 2, 0, (frames - n) * 2 * sizeof(int16_t));

  atomic_store_explicit(&ring_read, r + n, memory_order_release);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_error_cb(AAudioStream *s, void *user, aaudio_result_t err) {
  /* the stream can't be reopened from its own callback, the render thread
     picks this up */
  atomic_store(&stream_lost, 1);
}

static void audio_close() {
  if (stream) {
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream = NULL;
  }
}

static void audio_open() {
  AAudioStreamBuilder *b;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
    return;
  }
  AAudioStreamBuilder_setSampleRate(b, AUDIO_FREQ);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
  AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
  AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
  AAudioStreamBuilder_setErrorCallback(b, audio_error_cb, NULL);
  if (AAudioStreamBuilder_openStream(b, &stream) != AAUDIO_OK) {
    LOGW("audio_open failed");
    stream = NULL;
  }
  AAudioStreamBuilder_delete(b);
  atomic_store(&stream_lost, 0);
  if (stream) {
    AAudioStream_requestStart(stream);
  }
}

static size_t audio_batch(const int16_t *data, size_t frames) {
  unsigned w = atomic_load_explicit(&ring_write, memory_order_relaxed);
  unsigned space = RING_FRAMES - ring_fill();
  size_t n = frames < space ? frames : space;

  for (size_t i = 0; i < n; i++) {
    unsigned idx = ((w + i) % RING_FRAMES) * 2;
    ring[idx] = data[i * 2];
    ring[idx + 1] = data[i * 2 + 1];
  }

  atomic_store_explicit(&ring_write, w + (unsigned)n, memory_order_release);
  return frames;
}

static void audio_sample(int16_t left, int16_t right) {
  int16_t frame[2] = {left, right};
  audio_batch(frame, 1);
}

/*
 * input
 */
static struct {
  uint32_t buttons;
  int16_t lx, ly;
} pads[MAX_PORTS];

static void input_poll() {}

static int16_t input_state(unsigned port, unsigned device, unsigned index,
                           unsigned id) {
  if (port >= MAX_PORTS) {
    return 0;
  }
  if (device == RETRO_DEVICE_JOYPAD) {
    return (pads[port].buttons >> id) & 1;
  }
  if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
    return id == RETRO_DEVICE_ID_ANALOG_X ? pads[port].lx : pads[port].ly;
  }
  return 0;
}

/*
 * rumble, the app polls the strength after each frame
 */
static uint16_t rumble_strength[MAX_PORTS][2];

static bool set_rumble_state(unsigned port, enum retro_rumble_effect effect,
                             uint16_t strength) {
  if (port >= MAX_PORTS || effect > RETRO_RUMBLE_WEAK) {
    return false;
  }
  rumble_strength[port][effect] = strength;
  return true;
}

/*
 * video, the core draws into an offscreen framebuffer that's scaled onto the
 * window
 */
static struct retro_hw_render_callback hw_render;
static void *gles;
static GLuint fbo, fbo_color, fbo_depth;
static int surface_width, surface_height;

static uintptr_t get_current_framebuffer() {
  return fbo;
}

static retro_proc_address_t get_proc_address(const char *sym) {
  /* eglGetProcAddress isn't guaranteed to return core functions */
  void *fn = gles ? dlsym(gles, sym) : NULL;
  if (!fn) {
    fn = (void *)eglGetProcAddress(sym);
  }
  return (retro_proc_address_t)fn;
}

static void video_refresh(const void *data, unsigned width, unsigned height,
                          size_t pitch) {}

static void fbo_destroy() {
  glDeleteFramebuffers(1, &fbo);
  glDeleteRenderbuffers(1, &fbo_color);
  glDeleteRenderbuffers(1, &fbo_depth);
  fbo = fbo_color = fbo_depth = 0;
}

static void fbo_create() {
  glGenRenderbuffers(1, &fbo_color);
  glBindRenderbuffer(GL_RENDERBUFFER, fbo_color);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, VIDEO_WIDTH, VIDEO_HEIGHT);

  glGenRenderbuffers(1, &fbo_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, VIDEO_WIDTH,
                        VIDEO_HEIGHT);

  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, fbo_color);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, fbo_depth);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* the core letterboxes inside its 640x480 frame for a 4:3 screen, take just
   the picture and fit it to the window */
static void present() {
  const char *aspect = option_get("nodosumi_aspect");
  int sx0 = 0, sy0 = 0, sx1 = VIDEO_WIDTH, sy1 = VIDEO_HEIGHT;
  int dx0 = 0, dy0 = 0, dx1 = surface_width, dy1 = surface_height;

  if (!aspect || strcmp(aspect, "stretch")) {
    float ratio = 4.0f / 3.0f;
    if (aspect && !strcmp(aspect, "16:9")) {
      int h = VIDEO_WIDTH * 9 / 16;
      sy0 = (VIDEO_HEIGHT - h) / 2;
      sy1 = sy0 + h;
      ratio = 16.0f / 9.0f;
    }
    int w = surface_width;
    int h = (int)(w / ratio);
    if (h > surface_height) {
      h = surface_height;
      w = (int)(h * ratio);
    }
    dx0 = (surface_width - w) / 2;
    /* in portrait the touch controls take the bottom, keep the picture up top
       (gl's y runs bottom up) */
    dy0 = surface_height > surface_width ? surface_height - h
                                         : (surface_height - h) / 2;
    dx1 = dx0 + w;
    dy1 = dy0 + h;
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, surface_width, surface_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

/*
 * disc control
 */
static struct retro_disk_control_callback disk_control;

/*
 * environment
 */
static bool environment(unsigned cmd, void *data) {
  switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = sysdir;
      return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      return true;

    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      struct retro_hw_render_callback *cb = data;
      if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3) {
        return false;
      }
      cb->get_current_framebuffer = get_current_framebuffer;
      cb->get_proc_address = get_proc_address;
      hw_render = *cb;
      return true;
    }

    case RETRO_ENVIRONMENT_SET_VARIABLES: {
      /* "Description; default|other|..." */
      for (const struct retro_variable *v = data; v->key; v++) {
        const char *def = strchr(v->value, ';');
        if (!def) {
          continue;
        }
        def++;
        while (*def == ' ') {
          def++;
        }
        char value[64];
        snprintf(value, sizeof(value), "%.*s", (int)strcspn(def, "|"), def);
        option_set(v->key, value, 0);
      }
      return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *var = data;
      var->value = option_get(var->key);
      return var->value != NULL;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = false;
      return true;

    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
      ((struct retro_rumble_interface *)data)->set_rumble_state =
          set_rumble_state;
      return true;

    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
      disk_control = *(struct retro_disk_control_callback *)data;
      return true;

    default:
      return false;
  }
}

/*
 * pacing, the display's vsync drives frame() but the core wants 60 calls a
 * second. the rate leans a little on how full the audio ring is, so sound
 * neither runs dry nor backs up
 */
#define FRAME_NS (1000000000LL / 60)

static int64_t last_ns;
static int64_t owed_ns;
static int paused;

static int64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int frames_due() {
  int64_t now = now_ns();
  int64_t dt = last_ns ? now - last_ns : FRAME_NS;
  last_ns = now;

  /* +-1% around 60hz */
  float lean = ((float)ring_fill() - RING_TARGET) / RING_TARGET;
  lean = lean < -1.0f ? -1.0f : lean > 1.0f ? 1.0f : lean;
  int64_t period = (int64_t)(FRAME_NS * (1.0f + 0.01f * lean));

  owed_ns += dt;
  /* a vsync landing a hair early still counts */
  int n = 0;
  while (owed_ns >= period - period / 8 && n < 2) {
    owed_ns -= period;
    n++;
  }
  /* don't race to catch up after a stall */
  if (owed_ns > period) {
    owed_ns = 0;
  }
  return n;
}

/*
 * jni
 */
JNIEXPORT void JNICALL JNI(setOption)(JNIEnv *env, jobject thiz, jstring key,
                                      jstring value) {
  const char *k = (*env)->GetStringUTFChars(env, key, NULL);
  const char *v = (*env)->GetStringUTFChars(env, value, NULL);
  option_set(k, v, 1);
  (*env)->ReleaseStringUTFChars(env, key, k);
  (*env)->ReleaseStringUTFChars(env, value, v);
}

JNIEXPORT jboolean JNICALL JNI(start)(JNIEnv *env, jobject thiz,
                                      jstring jsysdir, jstring jpath) {
  if (started) {
    return JNI_TRUE;
  }

  const char *s = (*env)->GetStringUTFChars(env, jsysdir, NULL);
  const char *p = (*env)->GetStringUTFChars(env, jpath, NULL);
  snprintf(sysdir, sizeof(sysdir), "%s", s);
  snprintf(game_path, sizeof(game_path), "%s", p);
  (*env)->ReleaseStringUTFChars(env, jsysdir, s);
  (*env)->ReleaseStringUTFChars(env, jpath, p);

  gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);

  retro_set_environment(environment);
  retro_set_video_refresh(video_refresh);
  retro_set_audio_sample(audio_sample);
  retro_set_audio_sample_batch(audio_batch);
  retro_set_input_poll(input_poll);
  retro_set_input_state(input_state);
  retro_init();

  struct retro_game_info info = {game_path, NULL, 0, NULL};
  if (!retro_load_game(&info)) {
    LOGW("retro_load_game failed for %s", game_path);
    return JNI_FALSE;
  }

  audio_open();
  started = 1;
  return JNI_TRUE;
}

JNIEXPORT void JNICALL JNI(stop)(JNIEnv *env, jobject thiz) {
  if (!started) {
    return;
  }
  audio_close();
  /* flushes the vmu and flash out to disk */
  retro_unload_game();
  retro_deinit();
  started = 0;
}

JNIEXPORT void JNICALL JNI(surfaceCreated)(JNIEnv *env, jobject thiz) {
  if (!started) {
    return;
  }
  /* a new egl context, everything from the old one is gone */
  if (fbo) {
    hw_render.context_destroy();
    fbo = fbo_color = fbo_depth = 0;
  }
  fbo_create();
  hw_render.context_reset();
}

JNIEXPORT void JNICALL JNI(surfaceChanged)(JNIEnv *env, jobject thiz,
                                           jint width, jint height) {
  surface_width = width;
  surface_height = height;
}

JNIEXPORT void JNICALL JNI(frame)(JNIEnv *env, jobject thiz) {
  if (!started) {
    return;
  }

  if (atomic_load(&stream_lost)) {
    audio_close();
    audio_open();
  }

  int n = frames_due();
  for (int i = 0; i < n && !paused; i++) {
    retro_run();
  }
  present();
}

JNIEXPORT void JNICALL JNI(setPaused)(JNIEnv *env, jobject thiz,
                                      jboolean value) {
  paused = value;
  last_ns = 0;
  owed_ns = 0;
  if (stream) {
    if (paused) {
      AAudioStream_requestPause(stream);
    } else {
      AAudioStream_requestStart(stream);
    }
  }
}

JNIEXPORT void JNICALL JNI(setInput)(JNIEnv *env, jobject thiz, jint port,
                                     jint buttons, jint lx, jint ly) {
  if (port < 0 || port >= MAX_PORTS) {
    return;
  }
  pads[port].buttons = (uint32_t)buttons;
  pads[port].lx = (int16_t)lx;
  pads[port].ly = (int16_t)ly;
}

JNIEXPORT jint JNICALL JNI(rumble)(JNIEnv *env, jobject thiz, jint port) {
  if (port < 0 || port >= MAX_PORTS) {
    return 0;
  }
  uint16_t *s = rumble_strength[port];
  return s[0] > s[1] ? s[0] : s[1];
}

JNIEXPORT void JNICALL JNI(reset)(JNIEnv *env, jobject thiz) {
  if (started) {
    retro_reset();
  }
}

JNIEXPORT jboolean JNICALL JNI(saveState)(JNIEnv *env, jobject thiz,
                                          jstring jpath) {
  if (!started) {
    return JNI_FALSE;
  }
  size_t size = retro_serialize_size();
  void *data = malloc(size);
  int ok = data && retro_serialize(data, size);

  const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
  char tmp[1100];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if (ok) {
    /* write aside and swap in, so a failed write keeps the old state */
    FILE *fp = fopen(tmp, "wb");
    ok = fp && fwrite(data, 1, size, fp) == size;
    if (fp) {
      ok = !fclose(fp) && ok;
    }
    ok = ok && !rename(tmp, path);
    if (!ok) {
      remove(tmp);
    }
  }
  (*env)->ReleaseStringUTFChars(env, jpath, path);
  free(data);
  return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL JNI(loadState)(JNIEnv *env, jobject thiz,
                                          jstring jpath) {
  if (!started) {
    return JNI_FALSE;
  }
  size_t size = retro_serialize_size();
  void *data = malloc(size);
  int ok = 0;

  const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
  FILE *fp = fopen(path, "rb");
  if (fp && data) {
    ok = fread(data, 1, size, fp) == size && retro_unserialize(data, size);
  }
  if (fp) {
    fclose(fp);
  }
  (*env)->ReleaseStringUTFChars(env, jpath, path);
  free(data);
  return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI(discCount)(JNIEnv *env, jobject thiz) {
  return started && disk_control.get_num_images
             ? (jint)disk_control.get_num_images()
             : 0;
}

JNIEXPORT jint JNICALL JNI(discIndex)(JNIEnv *env, jobject thiz) {
  return started && disk_control.get_image_index
             ? (jint)disk_control.get_image_index()
             : 0;
}

JNIEXPORT jboolean JNICALL JNI(swapDisc)(JNIEnv *env, jobject thiz,
                                         jint index) {
  if (!started || !disk_control.set_eject_state) {
    return JNI_FALSE;
  }
  /* open the tray, swap, close it again */
  bool ok = disk_control.set_eject_state(true) &&
            disk_control.set_image_index((unsigned)index);
  ok = disk_control.set_eject_state(false) && ok;
  return ok ? JNI_TRUE : JNI_FALSE;
}
