#include <glad/glad.h>
#include <libretro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/core.h"
#include "core/filesystem.h"
#include "core/ftrace.h"
#include "core/string.h"
#include "core/thread.h"
#include "core/time.h"
#include "core/version.h"
#include "emulator.h"
#include "guest/aica/aica.h"
#include "host/host.h"
#include "options.h"
#include "render/render_backend.h"

#define GL_MIN_NAME "OpenGL ES 3.0 or newer"

#define AUDIO_FREQ AICA_SAMPLE_FREQ
#define VIDEO_WIDTH 640
#define VIDEO_HEIGHT 480

static struct retro_hw_render_callback hw_render;
static retro_environment_t env_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

/*
 * libretro host implementation
 */

/* clang-format off */
#define NUM_CONTROLLER_DESC          (ARRAY_SIZE(controller_desc)-1)

/* face buttons go by label, not position: a Nintendo-style A, on the right, is the dreamcast's A */
#define CONTROLLER_DESC(port)                                                                                 \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_A,     "A" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_B,     "B" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_START, "Start" },       \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },    \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },  \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },  \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_X,     "X" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_Y,     "Y" },           \
  { port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,     "Analog X" },    \
  { port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,     "Analog Y" },    \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_L2,    "L" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_R2,    "R" }

#define CONTROLLER_BUTTONS(port) \
  K_CONT_A,                      \
  K_CONT_B,                      \
  K_CONT_START,                  \
  K_CONT_DPAD_UP,                \
  K_CONT_DPAD_DOWN,              \
  K_CONT_DPAD_LEFT,              \
  K_CONT_DPAD_RIGHT,             \
  K_CONT_X,                      \
  K_CONT_Y,                      \
  K_CONT_JOYX,                   \
  K_CONT_JOYY,                   \
  K_CONT_LTRIG,                  \
  K_CONT_RTRIG

static struct retro_input_descriptor controller_desc[] = {
  CONTROLLER_DESC(0),
  CONTROLLER_DESC(1),
  CONTROLLER_DESC(2),
  CONTROLLER_DESC(3),
  { 0 },
};

static int controller_buttons[] = {
  CONTROLLER_BUTTONS(0),
  CONTROLLER_BUTTONS(1),
  CONTROLLER_BUTTONS(2),
  CONTROLLER_BUTTONS(3),
};
/* clang-format on */

struct host {
  struct emu *emu;

  struct {
    struct render_backend *r;
  } video;

  struct {
    int16_t state[NUM_CONTROLLER_DESC];
  } input;
};

struct host *g_host;

/* the frontend gave us a context with the minimum GL version */
static int g_gl_ok = 0;

/* glad's desktop loader leaves out the gles3 entry point, look it up by hand */
static PFNGLINVALIDATEFRAMEBUFFERPROC invalidate_fb;
static int g_stats = -1;

/* gpu time of each frame's draw, with NODOSUMI_FTRACE and
   GL_EXT_disjoint_timer_query */
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_GPU_DISJOINT_EXT 0x8FBB
#define GPU_QUERIES 8
static struct {
  int ok;
  GLuint ids[GPU_QUERIES];
  int64_t frame[GPU_QUERIES];
  int busy[GPU_QUERIES];
  int next;
  void (*gen)(GLsizei, GLuint *);
  void (*begin)(GLenum, GLuint);
  void (*end)(GLenum);
  void (*get)(GLuint, GLenum, GLuint *);
} gpu_q;
static int64_t g_run_frame;

static char g_game[PATH_MAX];

/* a game's discs, from its m3u playlist or the one disc it was loaded from,
   and which is in the drive */
#define MAX_DISCS 8
static char g_discs[MAX_DISCS][PATH_MAX];
static unsigned g_num_discs;
static unsigned g_disc;
static bool g_tray_open;

/* core options exposed to the frontend */
static struct retro_variable g_opts[] = {
    {"nodosumi_region", "Region; japan|usa|europe"},
    {"nodosumi_language",
     "Language; japanese|english|german|french|spanish|italian"},
    {"nodosumi_broadcast", "Broadcast; ntsc|pal|pal_m|pal_n"},
    {"nodosumi_bios", "Boot to BIOS; disabled|enabled"},
    {"nodosumi_aspect", "Aspect ratio; 4:3|16:9|stretch"},
    {"nodosumi_fast_depth", "Fast depth (some z-fighting); disabled|enabled"},
    {"nodosumi_stats", "Statistics logging; disabled|enabled"},
    {NULL, NULL},
};

static const char *get_variable(const char *key) {
  struct retro_variable var = {key, NULL};
  if (!env_cb || !env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
    return NULL;
  }
  return var.value;
}

static void set_option(char *dst, size_t size, const char *key) {
  const char *value = get_variable(key);
  if (value) {
    snprintf(dst, size, "%s", value);
  }
}

/*
 * audio
 */
/* the emu thread runs between retro_run calls, so queue samples here and
   hand them over from retro_run */
#define AUDIO_QUEUE_FRAMES 8192
static mutex_t audio_mutex;
static int16_t audio_queue[AUDIO_QUEUE_FRAMES * 2];
static int ahead;

void audio_push(struct host *host, const int16_t *data, int frames) {
  mutex_lock(audio_mutex);
  int n = MIN(frames, AUDIO_QUEUE_FRAMES - ahead);
  memcpy(&audio_queue[ahead * 2], data, n * 2 * sizeof(int16_t));
  ahead += n;
  mutex_unlock(audio_mutex);
}

/* logs the loudest sample of each second of sound, to tell music from silence */
static void audio_stat(const int16_t *data, int frames) {
  static int peak, counted;
  for (int i = 0; i < frames * 2; i++) {
    peak = MAX(peak, abs(data[i]));
  }
  counted += frames;
  if (counted >= AUDIO_FREQ) {
    fprintf(stderr, "STAT audio peak=%d\n", peak);
    peak = 0;
    counted = 0;
  }
}

static void audio_flush() {
  mutex_lock(audio_mutex);
  FT_C(audio_frames, ahead);
  if (ahead) {
    if (g_stats > 0) {
      audio_stat(audio_queue, ahead);
    }
    audio_batch_cb(audio_queue, ahead);
    ahead = 0;
  }
  mutex_unlock(audio_mutex);
}

/*
 * bot, for profiling runs: NODOSUMI_BOT=<file> drives port 0 from a script
 * read one line at a time, so lines appended while it runs are picked up and
 * a finished script replays frame for frame. lines:
 *   <frames> [a b x y start up down left right l r] [jx=<-1..1>] [jy=..]
 *   save <path> | load <path> | mark | loop | # comment
 * at the end of the file the pad is let go until more lines arrive
 */
static FILE *bot_fp;
static int bot_frames;
static int bot_line;
static long bot_mark = -1;
static int bot_mark_line;
static int16_t bot_state[NUM_CONTROLLER_DESC];
static int bot_on = -1;

bool retro_serialize(void *data, size_t size);
bool retro_unserialize(const void *data, size_t size);
size_t retro_serialize_size();

static void bot_file_state(const char *path, int save) {
  size_t size = retro_serialize_size();
  void *data = malloc(size);
  int ok = 0;
  if (save) {
    FILE *fp = fopen(path, "wb");
    if (fp && retro_serialize(data, size)) {
      ok = fwrite(data, 1, size, fp) == size;
    }
    if (fp) {
      fclose(fp);
    }
  } else {
    FILE *fp = fopen(path, "rb");
    if (fp) {
      ok = fread(data, 1, size, fp) == size && retro_unserialize(data, size);
      fclose(fp);
    }
  }
  free(data);
  fprintf(stderr, "nodosumi: bot %s %s %s\n", save ? "save" : "load", path,
          ok ? "ok" : "FAILED");
}

static void bot_set(const char *tok) {
  static const char *names[] = {"a",     "b",    "start", "up", "down",
                                "left",  "right", "x",    "y",  "jx",
                                "jy",    "l",    "r"};
  for (int i = 0; i < (int)ARRAY_SIZE(names); i++) {
    size_t n = strlen(names[i]);
    if (strncmp(tok, names[i], n)) {
      continue;
    }
    if (tok[n] == '=') {
      bot_state[i] = (int16_t)(atof(tok + n + 1) * INT16_MAX);
      return;
    }
    if (!tok[n]) {
      bot_state[i] = 1;
      return;
    }
  }
}

/* advances the bot one frame, returns 0 when it's off */
static int bot_step() {
  if (bot_on < 0) {
    const char *path = getenv("NODOSUMI_BOT");
    bot_fp = path ? fopen(path, "r") : NULL;
    bot_on = bot_fp != NULL;
  }
  if (!bot_on) {
    return 0;
  }
  while (bot_frames <= 0) {
    long pos = ftell(bot_fp);
    char line[512];
    if (!fgets(line, sizeof(line), bot_fp) || !strchr(line, '\n')) {
      /* nothing new yet, let go of the pad and look again next frame */
      fseek(bot_fp, pos, SEEK_SET);
      memset(bot_state, 0, sizeof(bot_state));
      return 1;
    }
    bot_line++;
    char *tok = strtok(line, " \t\r\n");
    if (!tok || tok[0] == '#') {
      continue;
    }
    FT_I(bot_line, bot_line);
    if (!strcmp(tok, "save") || !strcmp(tok, "load")) {
      char *path = strtok(NULL, " \t\r\n");
      if (path) {
        bot_file_state(path, tok[0] == 's');
      }
      continue;
    }
    if (!strcmp(tok, "mark")) {
      bot_mark = ftell(bot_fp);
      bot_mark_line = bot_line;
      continue;
    }
    if (!strcmp(tok, "loop")) {
      if (bot_mark >= 0) {
        fseek(bot_fp, bot_mark, SEEK_SET);
        bot_line = bot_mark_line;
      }
      continue;
    }
    memset(bot_state, 0, sizeof(bot_state));
    bot_frames = atoi(tok);
    while ((tok = strtok(NULL, " \t\r\n"))) {
      bot_set(tok);
    }
    fprintf(stderr, "nodosumi: bot line %d at frame %lld\n", bot_line,
            (long long)g_run_frame);
  }
  bot_frames--;
  return 1;
}

/*
 * input
 */
static void input_poll(struct host *host) {
  input_poll_cb();
  int bot = bot_step();

  /* send updates for any inputs that've changed */
  for (int i = 0; i < NUM_CONTROLLER_DESC; i++) {
    struct retro_input_descriptor *desc = &controller_desc[i];
    int16_t value =
        bot ? (desc->port ? 0 : bot_state[i])
            : input_state_cb(desc->port, desc->device, desc->index, desc->id);

    /* retroarch gives triggers as [0,1], the host layer wants [0,INT16_MAX] */
    if (desc->id == RETRO_DEVICE_ID_JOYPAD_L2 ||
        desc->id == RETRO_DEVICE_ID_JOYPAD_R2) {
      value = value ? INT16_MAX : 0;
    }

    if (host->input.state[i] == value) {
      continue;
    }

    int button = controller_buttons[i];

    if (host->emu) {
      emu_keydown(host->emu, desc->port, button, value);
    }

    host->input.state[i] = value;
  }
}

/*
 * internal
 */
static void video_context_destroyed() {
  if (g_host->emu) {
    /* tex cache is shared with the running frame */
    emu_sync(g_host->emu);
    emu_vid_destroyed(g_host->emu);
  }

  CHECK_NOTNULL(g_host->video.r);
  r_destroy(g_host->video.r);
  g_host->video.r = NULL;
}

static int gl_version_supported(const char *version) {
  int major = 0;
  int minor = 0;

  if (!version) {
    return 0;
  }

  /* needs an ES context, parse "OpenGL ES x.y" */
  const char *p = strstr(version, "OpenGL ES");
  if (!p) {
    return 0;
  }
  p += strlen("OpenGL ES");
  while (*p == ' ') {
    p++;
  }
  if (sscanf(p, "%d.%d", &major, &minor) != 2) {
    return 0;
  }

  return major >= 3;
}

static void video_context_reset() {
  /* link in gl functions at runtime */
  int res = gladLoadGLLoader((GLADloadproc)hw_render.get_proc_address);
  CHECK_EQ(res, 1, "GL initialization failed");

  /* check the version before touching any gl state */
  const char *version = (const char *)glGetString(GL_VERSION);
  if (!gl_version_supported(version)) {
    LOG_WARNING("nodosumi requires %s (got \"%s\")", GL_MIN_NAME,
                version ? version : "unknown");
    g_gl_ok = 0;
    return;
  }

  g_gl_ok = 1;
  invalidate_fb = (PFNGLINVALIDATEFRAMEBUFFERPROC)
      hw_render.get_proc_address("glInvalidateFramebuffer");

  /* glad loads by desktop gl version, which has these only from 4.1 */
  glad_glProgramParameteri = (PFNGLPROGRAMPARAMETERIPROC)
      hw_render.get_proc_address("glProgramParameteri");
  glad_glGetProgramBinary = (PFNGLGETPROGRAMBINARYPROC)
      hw_render.get_proc_address("glGetProgramBinary");
  glad_glProgramBinary = (PFNGLPROGRAMBINARYPROC)
      hw_render.get_proc_address("glProgramBinary");

  memset(&gpu_q, 0, sizeof(gpu_q));
  const char *exts = (const char *)glGetString(GL_EXTENSIONS);
  if (ftrace_on && exts && strstr(exts, "GL_EXT_disjoint_timer_query")) {
    gpu_q.gen = (void (*)(GLsizei, GLuint *))hw_render.get_proc_address(
        "glGenQueriesEXT");
    gpu_q.begin = (void (*)(GLenum, GLuint))hw_render.get_proc_address(
        "glBeginQueryEXT");
    gpu_q.end = (void (*)(GLenum))hw_render.get_proc_address("glEndQueryEXT");
    gpu_q.get = (void (*)(GLuint, GLenum, GLuint *))hw_render.get_proc_address(
        "glGetQueryObjectuivEXT");
    gpu_q.ok = gpu_q.gen && gpu_q.begin && gpu_q.end && gpu_q.get;
    if (gpu_q.ok) {
      gpu_q.gen(GPU_QUERIES, gpu_q.ids);
    }
  }
  if (ftrace_on) {
    fprintf(stderr, "nodosumi: ftrace gpu timer queries %s\n",
            gpu_q.ok ? "on" : "unavailable");
  }

  /* keep compiled shaders between boots */
  char cache_path[PATH_MAX] = {0};
  if (g_game[0]) {
    char dir[PATH_MAX], game[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s" PATH_SEPARATOR "shaders", fs_appdir());
    fs_basename(g_game, game, sizeof(game));
    if (fs_mkdir(dir)) {
      snprintf(cache_path, sizeof(cache_path),
               "%s" PATH_SEPARATOR "%s.bin", dir, game);
    }
  }

  CHECK(!g_host->video.r);
  g_host->video.r = r_create(VIDEO_WIDTH, VIDEO_HEIGHT,
                             cache_path[0] ? cache_path : NULL);

  if (g_host->emu) {
    emu_vid_created(g_host->emu, g_host->video.r);
  }
}

static int host_init(struct host *host) {
  /* let retroarch know about our controller mappings */
  env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, controller_desc);

  /* ask for gles 3.0 */
  hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
  hw_render.version_major = 3;
  hw_render.version_minor = 0;
  hw_render.context_reset = &video_context_reset;
  hw_render.context_destroy = &video_context_destroyed;
  hw_render.depth = true;
  /* the modifier volumes' shadows are drawn with it */
  hw_render.stencil = true;
  hw_render.bottom_left_origin = true;

  bool ret = env_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
  if (!ret) {
    LOG_WARNING("host_init failed to initialize hardware renderer");
    return 0;
  }

  return 1;
}

static void host_destroy(struct host *host) {
  free(host);
}

struct host *host_create() {
  struct host *host = calloc(1, sizeof(struct host));
  return host;
}

/*
 * libretro core implementation
 */
void retro_init() {
  if (!audio_mutex) {
    audio_mutex = mutex_create();
  }

  ftrace_init();

  /* set application directory */
  const char *sysdir = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir)) {
    char appdir[PATH_MAX];
    snprintf(appdir, sizeof(appdir), "%s" PATH_SEPARATOR "dc", sysdir);
    fs_set_appdir(appdir);
  }

  /* load base options from config */
  const char *appdir = fs_appdir();
  char config[PATH_MAX] = {0};
  snprintf(config, sizeof(config), "%s" PATH_SEPARATOR "config", appdir);
  options_read(config);

  /* advertise core options to the frontend */
  env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, g_opts);

  /* the jit map has to be enabled before the jits are created */
  if (getenv("NODOSUMI_PERF")) {
    OPTION_perf = 1;
  }

  /* create up front so retro_serialize_size works before a game is loaded */
  g_host = host_create();
  g_host->emu = emu_create(g_host);
}

void retro_deinit() {
  if (!g_host) {
    return;
  }

  if (g_host->emu) {
    emu_destroy(g_host->emu);
    g_host->emu = NULL;
  }

  host_destroy(g_host);
  g_host = NULL;

  ftrace_shutdown();
}

unsigned retro_api_version() {
  return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info) {
  info->library_name = "nodosumi";
  info->library_version = GIT_VERSION;
  info->valid_extensions = "cdi|chd|gdi|cue|m3u";
  info->need_fullpath = true;
  info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
  info->geometry.base_width = VIDEO_WIDTH;
  info->geometry.base_height = VIDEO_HEIGHT;
  info->geometry.max_width = VIDEO_WIDTH;
  info->geometry.max_height = VIDEO_HEIGHT;
  info->geometry.aspect_ratio = (float)VIDEO_WIDTH / (float)VIDEO_HEIGHT;
  info->timing.fps = 60;
  info->timing.sample_rate = AUDIO_FREQ;
}

/*
 * disc swapping, the frontend opens the tray, picks a disc and closes it
 */
static bool disk_set_eject_state(bool ejected) {
  if (!g_host || !g_host->emu || !g_num_discs) {
    return false;
  }

  if (ejected == g_tray_open) {
    return true;
  }

  if (ejected) {
    emu_open_tray(g_host->emu);
  } else if (!emu_close_tray(g_host->emu, g_discs[g_disc], 1)) {
    return false;
  }

  g_tray_open = ejected;
  return true;
}

static bool disk_get_eject_state() {
  return g_tray_open;
}

static unsigned disk_get_image_index() {
  return g_disc;
}

static bool disk_set_image_index(unsigned index) {
  if (!g_tray_open || index >= g_num_discs) {
    return false;
  }

  g_disc = index;
  return true;
}

static unsigned disk_get_num_images() {
  return g_num_discs;
}

static bool disk_replace_image_index(unsigned index,
                                     const struct retro_game_info *info) {
  return false;
}

static bool disk_add_image_index() {
  return false;
}

static struct retro_disk_control_callback disk_control = {
    &disk_set_eject_state,     &disk_get_eject_state,
    &disk_get_image_index,     &disk_set_image_index,
    &disk_get_num_images,      &disk_replace_image_index,
    &disk_add_image_index,
};

/* an m3u lists a disc a line, relative to itself, a game file is its own disc */
static void read_discs(const char *path) {
  g_num_discs = 0;
  g_disc = 0;
  g_tray_open = false;

  const char *ext = strrchr(path, '.');
  if (!ext || strcasecmp(ext, ".m3u")) {
    snprintf(g_discs[0], sizeof(g_discs[0]), "%s", path);
    g_num_discs = 1;
    return;
  }

  FILE *fp = fopen(path, "r");
  if (!fp) {
    return;
  }

  char dir[PATH_MAX];
  fs_dirname(path, dir, sizeof(dir));

  char line[PATH_MAX];
  while (g_num_discs < MAX_DISCS && fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || line[0] == '#') {
      continue;
    }

    char *disc = g_discs[g_num_discs++];
    if (line[0] == '/') {
      snprintf(disc, PATH_MAX, "%s", line);
    } else {
      snprintf(disc, PATH_MAX, "%s" PATH_SEPARATOR "%s", dir, line);
    }
  }

  fclose(fp);
}

/* a multi-disc game's state ends with the disc that was in the drive */
static size_t disc_trailer() {
  return g_num_discs > 1 ? sizeof(uint32_t) : 0;
}

void retro_set_environment(retro_environment_t env) {
  env_cb = env;
  env_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);
}

void retro_set_video_refresh(retro_video_refresh_t video) {
  video_cb = video;
}

void retro_set_audio_sample(retro_audio_sample_t audio) {
  audio_cb = audio;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t audio_batch) {
  audio_batch_cb = audio_batch;
}

void retro_set_input_poll(retro_input_poll_t input_poll) {
  input_poll_cb = input_poll;
}

void retro_set_input_state(retro_input_state_t input_state) {
  input_state_cb = input_state;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset() {
  if (!g_host || !g_host->emu || !g_game[0]) {
    return;
  }

  /* rebuild the machine, the render backend stays */
  struct render_backend *r = g_host->video.r;

  emu_destroy(g_host->emu);
  g_host->emu = emu_create(g_host);
  emu_vid_created(g_host->emu, r);
  g_tray_open = false;
  emu_load(g_host->emu, g_discs[g_disc]);
}

static void gpu_query_poll() {
  if (!gpu_q.ok) {
    return;
  }
  for (int i = 0; i < GPU_QUERIES; i++) {
    if (!gpu_q.busy[i]) {
      continue;
    }
    GLuint avail = 0;
    gpu_q.get(gpu_q.ids[i], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) {
      continue;
    }
    GLuint ns = 0;
    gpu_q.get(gpu_q.ids[i], GL_QUERY_RESULT, &ns);
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    FT_C(gpu_frame, gpu_q.frame[i]);
    FT_C(gpu_ns, disjoint ? -1 : (int64_t)ns);
    gpu_q.busy[i] = 0;
  }
}

static void gpu_query_begin() {
  if (!gpu_q.ok) {
    return;
  }
  int i = gpu_q.next;
  if (gpu_q.busy[i]) {
    gpu_q.next = -1;
    return;
  }
  gpu_q.begin(GL_TIME_ELAPSED_EXT, gpu_q.ids[i]);
  gpu_q.frame[i] = g_run_frame;
}

static void gpu_query_end() {
  if (!gpu_q.ok) {
    return;
  }
  if (gpu_q.next < 0) {
    /* every query still in flight, this frame went untimed */
    for (int i = 0; i < GPU_QUERIES; i++) {
      if (!gpu_q.busy[i]) {
        gpu_q.next = i;
      }
    }
    if (gpu_q.next < 0) {
      gpu_q.next = 0;
    }
    return;
  }
  gpu_q.end(GL_TIME_ELAPSED_EXT);
  gpu_q.busy[gpu_q.next] = 1;
  gpu_q.next = (gpu_q.next + 1) % GPU_QUERIES;
}

void retro_run() {
  if (!g_gl_ok || !g_host->video.r) {
    video_cb(NULL, VIDEO_WIDTH, VIDEO_HEIGHT, 0);
    return;
  }

  if (g_stats < 0) {
    g_stats = getenv("NODOSUMI_STATS") != NULL;
  }

  static int ft_named;
  if (!ft_named) {
    ftrace_thread("video");
    ft_named = 1;
  }
  FT_B(retro_run, g_run_frame);
  FZ_B(z_retro_run);

  int64_t stat_t0 = 0;
  int64_t stat_t1 = 0;
  if (g_stats) {
    stat_t0 = time_nanoseconds();
  }

  FZ_B(z_input);
  input_poll(g_host);
  FZ_E(z_input);
  FZ_B(z_audio_flush);
  audio_flush();
  FZ_E(z_audio_flush);
  FZ_B(z_gpu_query);
  gpu_query_poll();
  FZ_E(z_gpu_query);

  /* bind the framebuffer provided by retroarch before calling into the
     emulator */
  uintptr_t fb = hw_render.get_current_framebuffer();
  glBindFramebuffer(GL_FRAMEBUFFER, fb);

  /* draw the last frame while the emu thread runs the next one */
  FT_B(begin_frame, g_run_frame);
  gpu_query_begin();
  int updated = emu_begin_frame(g_host->emu);
  gpu_query_end();
  FT_E(begin_frame, updated);

  /* a new picture doesn't need the old depth/stencil */
  if (updated && invalidate_fb) {
    static const GLenum attachments[] = {GL_DEPTH_ATTACHMENT,
                                         GL_STENCIL_ATTACHMENT};
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    invalidate_fb(GL_FRAMEBUFFER, 2, attachments);
  }

  if (g_stats) {
    stat_t1 = time_nanoseconds();
  }

  FT_B(video_cb, updated);
  FZ_B(z_video_cb);
  video_cb(updated ? RETRO_HW_FRAME_BUFFER_VALID : NULL,
           VIDEO_WIDTH, VIDEO_HEIGHT, 0);
  FZ_E(z_video_cb);
  FT_E(video_cb, updated);

  static int64_t g_stat_out;
  if (g_stats) {
    int64_t stat_t2 = time_nanoseconds();
    fprintf(stderr, "STAT present=%.2fms total=%.2fms gap=%.2fms\n",
            (double)(stat_t2 - stat_t1) / 1e6, (double)(stat_t2 - stat_t0) / 1e6,
            g_stat_out ? (double)(stat_t0 - g_stat_out) / 1e6 : 0.0);
  }

  FT_B(end_frame, g_run_frame);
  emu_end_frame(g_host->emu);
  FT_E(end_frame, g_run_frame);

  if (g_stats) {
    g_stat_out = time_nanoseconds();
  }

  FZ_E(z_retro_run);
  FT_E(retro_run, g_run_frame);
  ftrace_flush();
  g_run_frame++;
}

size_t retro_serialize_size() {
  if (!g_host || !g_host->emu) {
    return 0;
  }

  return emu_state_size(g_host->emu) + disc_trailer();
}

bool retro_serialize(void *data, size_t size) {
  if (!g_host || !g_host->emu) {
    return false;
  }

  size_t trailer = disc_trailer();
  if (size < trailer) {
    return false;
  }

  int res = emu_save_state(g_host->emu, data, size - trailer);
  if (trailer) {
    uint32_t disc = g_disc;
    memcpy((uint8_t *)data + size - trailer, &disc, trailer);
  }
  LOG_INFO("retro_serialize size=%zu res=%d", size, res);
  return res != 0;
}

bool retro_unserialize(const void *data, size_t size) {
  if (!g_host || !g_host->emu) {
    return false;
  }

  size_t trailer = disc_trailer();
  if (size < trailer) {
    return false;
  }

  if (trailer) {
    uint32_t disc;
    memcpy(&disc, (const uint8_t *)data + size - trailer, trailer);
    if (disc >= g_num_discs ||
        (disc != g_disc && !emu_close_tray(g_host->emu, g_discs[disc], 0))) {
      return false;
    }
    g_disc = disc;
  }

  /* the state's drive comes back with its disc in and ready */
  g_tray_open = false;

  int res = emu_load_state(g_host->emu, data, size - trailer);
  LOG_INFO("retro_unserialize size=%zu res=%d", size, res);
  return res != 0;
}

void retro_cheat_reset() {}

void retro_cheat_set(unsigned index, bool enabled, const char *code) {}

bool retro_load_game(const struct retro_game_info *info) {
  /* core options -> nodosumi options */
  set_option(OPTION_region, sizeof(OPTION_region), "nodosumi_region");
  set_option(OPTION_language, sizeof(OPTION_language), "nodosumi_language");
  set_option(OPTION_broadcast, sizeof(OPTION_broadcast), "nodosumi_broadcast");
  set_option(OPTION_aspect, sizeof(OPTION_aspect), "nodosumi_aspect");

  const char *bios = get_variable("nodosumi_bios");
  OPTION_bios = (bios && strcmp(bios, "enabled") == 0);

  const char *fast_depth = get_variable("nodosumi_fast_depth");
  OPTION_fast_depth = (fast_depth && strcmp(fast_depth, "enabled") == 0);

  const char *stats = get_variable("nodosumi_stats");
  /* only turns logging on, so NODOSUMI_STATS still works */
  if (stats && strcmp(stats, "enabled") == 0) {
    setenv("NODOSUMI_STATS", "1", 1);
  }

  snprintf(g_game, sizeof(g_game), "%s", info->path);
  read_discs(info->path);
  if (!g_num_discs) {
    g_game[0] = '\0';
    return false;
  }

  if (!g_host) {
    g_host = host_create();
  }

  if (!g_host->emu) {
    g_host->emu = emu_create(g_host);
  }

  /* gl support is only known at context_reset, retro_run checks it */
  if (!host_init(g_host)) {
    emu_destroy(g_host->emu);
    g_host->emu = NULL;

    host_destroy(g_host);
    g_host = NULL;

    g_game[0] = '\0';
    return false;
  }

  if (!emu_load(g_host->emu, g_discs[0])) {
    g_game[0] = '\0';
    return false;
  }

  return true;
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info *info,
                             size_t num_info) {
  return false;
}

void retro_unload_game() {
  if (g_host->emu) {
    emu_destroy(g_host->emu);
    g_host->emu = NULL;
  }

  if (g_host) {
    host_destroy(g_host);
    g_host = NULL;
  }

  g_game[0] = '\0';
  g_num_discs = 0;
}

unsigned retro_get_region() {
  return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id) {
  if (id == RETRO_MEMORY_SYSTEM_RAM && g_host && g_host->emu) {
    return emu_ram(g_host->emu);
  }

  return NULL;
}

size_t retro_get_memory_size(unsigned id) {
  if (id == RETRO_MEMORY_SYSTEM_RAM) {
    return emu_ram_size();
  }

  return 0;
}
