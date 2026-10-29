#include <chd.h>
#include "core/core.h"
#include "core/ftrace.h"
#include "core/thread.h"
#include "guest/gdrom/disc.h"
#include "guest/gdrom/gdrom_types.h"

/* decoding a hunk (8 sectors) takes ~2 ms on a 1.5 GHz cortex-a53, and games
   stream several at once, so a worker thread decodes them ahead of the reader
   into a cache instead of the emulation thread stalling on each one */
#define CHD_CACHE_HUNKS 256
#define CHD_READAHEAD 16

struct chd_hunk {
  int num;
  unsigned stamp;
  uint8_t *data;
};

struct chd {
  struct disc;

  struct session sessions[DISC_MAX_SESSIONS];
  int num_sessions;
  struct track tracks[DISC_MAX_TRACKS];
  int num_tracks;

  chd_file *chd;

  /* only the worker touches chd once it's running. the rest is shared, under
     mutex */
  thread_t thread;
  mutex_t mutex;
  cond_t wake;
  cond_t done;
  int quit;
  /* hunk last read, the worker decodes it and the ones after it */
  int want;
  struct chd_hunk cache[CHD_CACHE_HUNKS];
  unsigned stamp;
  uint8_t *workmem;
};

static struct chd_hunk *chd_lookup(struct chd *chd, int num) {
  for (int i = 0; i < CHD_CACHE_HUNKS; i++) {
    if (chd->cache[i].num == num) {
      return &chd->cache[i];
    }
  }
  return NULL;
}

static int chd_next_hunk(struct chd *chd) {
  const chd_header *head = chd_get_header(chd->chd);
  if (chd->want < 0) {
    return -1;
  }
  for (int i = 0; i < CHD_READAHEAD; i++) {
    int num = chd->want + i;
    if (num >= (int)head->totalhunks) {
      break;
    }
    if (!chd_lookup(chd, num)) {
      return num;
    }
  }
  return -1;
}

static void *chd_thread(void *data) {
  struct chd *chd = data;

  ftrace_thread("chd");

  mutex_lock(chd->mutex);

  while (1) {
    int num;
    while (!chd->quit && (num = chd_next_hunk(chd)) < 0) {
      cond_wait(chd->wake, chd->mutex);
    }
    if (chd->quit) {
      break;
    }
    mutex_unlock(chd->mutex);

    FT_B(chd_decode, num);
    FZ_B(z_chd_decode);
    int err = chd_read(chd->chd, num, chd->workmem);
    CHECK_EQ(err, CHDERR_NONE, "chd_read failed hunk=%d", num);
    FZ_E(z_chd_decode);
    FT_E(chd_decode, num);
    ftrace_flush();

    mutex_lock(chd->mutex);

    /* replace the least recently used hunk */
    struct chd_hunk *hunk = &chd->cache[0];
    for (int i = 1; i < CHD_CACHE_HUNKS; i++) {
      if (chd->cache[i].stamp < hunk->stamp) {
        hunk = &chd->cache[i];
      }
    }
    uint8_t *tmp = hunk->data;
    hunk->data = chd->workmem;
    chd->workmem = tmp;
    hunk->num = num;
    hunk->stamp = ++chd->stamp;

    cond_signal(chd->done);
  }

  mutex_unlock(chd->mutex);

  return NULL;
}

static void chd_read_sector(struct disc *disc, struct track *track, int fad,
                            void *dst) {
  struct chd *chd = (struct chd *)disc;
  const chd_header *head = chd_get_header(chd->chd);

  int cad = fad - track->file_offset;
  int hunknum = (cad * head->unitbytes) / head->hunkbytes;
  int hunkofs = (cad * head->unitbytes) % head->hunkbytes;
  CHECK(hunknum >= 0 && hunknum < (int)head->totalhunks,
        "chd_read_sector failed fad=%d", fad);

  FZ_B(z_chd_read);
  mutex_lock(chd->mutex);

  if (chd->want != hunknum) {
    chd->want = hunknum;
    cond_signal(chd->wake);
  }

  struct chd_hunk *hunk = chd_lookup(chd, hunknum);
  if (!hunk) {
    FT_I(chd_miss, hunknum);
    FT_B(chd_wait, hunknum);
    FZ_B(z_chd_wait);
    while (!(hunk = chd_lookup(chd, hunknum))) {
      cond_wait(chd->done, chd->mutex);
    }
    FZ_E(z_chd_wait);
    FT_E(chd_wait, hunknum);
  }
  hunk->stamp = ++chd->stamp;

  memcpy(dst, hunk->data + hunkofs + track->header_size, 2048);

  mutex_unlock(chd->mutex);
  FZ_E(z_chd_read);
}

static void chd_get_toc(struct disc *disc, int area, struct track **first_track,
                        struct track **last_track, int *leadin_fad,
                        int *leadout_fad) {
  struct chd *chd = (struct chd *)disc;

  /* chd's have one toc per area, and there is one session per area */
  struct session *session = &chd->sessions[area];

  *first_track = &chd->tracks[session->first_track];
  *last_track = &chd->tracks[session->last_track];
  *leadin_fad = session->leadin_fad;
  *leadout_fad = session->leadout_fad;
}

static struct track *chd_get_track(struct disc *disc, int n) {
  struct chd *chd = (struct chd *)disc;
  CHECK_LT(n, chd->num_tracks);
  return &chd->tracks[n];
}

static int chd_get_num_tracks(struct disc *disc) {
  struct chd *chd = (struct chd *)disc;
  return chd->num_tracks;
}

static struct session *chd_get_session(struct disc *disc, int n) {
  struct chd *chd = (struct chd *)disc;
  CHECK_LT(n, chd->num_sessions);
  return &chd->sessions[n];
}

static int chd_get_num_sessions(struct disc *disc) {
  struct chd *chd = (struct chd *)disc;
  return chd->num_sessions;
}

static int chd_get_format(struct disc *disc) {
  return GD_DISC_GDROM;
}

static void chd_destroy(struct disc *disc) {
  struct chd *chd = (struct chd *)disc;

  if (chd->thread) {
    mutex_lock(chd->mutex);
    chd->quit = 1;
    cond_signal(chd->wake);
    mutex_unlock(chd->mutex);
    thread_join(chd->thread, NULL);
    cond_destroy(chd->done);
    cond_destroy(chd->wake);
    mutex_destroy(chd->mutex);
  }

  for (int i = 0; i < CHD_CACHE_HUNKS; i++) {
    free(chd->cache[i].data);
  }
  free(chd->workmem);

  chd_close(chd->chd);
}

static int chd_parse(struct disc *disc, const char *filename, int verbose) {
  struct chd *chd = (struct chd *)disc;

  chd_error err = chd_open(filename, CHD_OPEN_READ, 0, &chd->chd);
  if (err != CHDERR_NONE) {
    return 0;
  }

  /* allocate storage for sector reads */
  const chd_header *head = chd_get_header(chd->chd);
  for (int i = 0; i < CHD_CACHE_HUNKS; i++) {
    chd->cache[i].num = -1;
    chd->cache[i].data = malloc(head->hunkbytes);
  }
  chd->workmem = malloc(head->hunkbytes);
  chd->want = -1;

  /* parse tracks */
  char tmp[512];
  int cad = 0;
  int fad = GDROM_PREGAP;

  while (1) {
    int tkid = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;
    char type[64], subtype[32], pgtype[32], pgsub[32];

    /* try to read the new v5 metadata tag */
    err = chd_get_metadata(chd->chd, GDROM_TRACK_METADATA_TAG, chd->num_tracks,
                           tmp, sizeof(tmp), NULL, NULL, NULL);
    if (err == CHDERR_NONE) {
      sscanf(tmp, GDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames,
             &pad, &pregap, pgtype, pgsub, &postgap);
    } else {
      /* try to read the v3/v4 metadata tag */
      err =
          chd_get_metadata(chd->chd, CDROM_TRACK_METADATA2_TAG, chd->num_tracks,
                           tmp, sizeof(tmp), NULL, NULL, NULL);
      if (err == CHDERR_NONE) {
        sscanf(tmp, CDROM_TRACK_METADATA2_FORMAT, &tkid, type, subtype, &frames,
               &pregap, pgtype, pgsub, &postgap);
      } else {
        /* try to read the old v3/v4 metadata tag */
        err = chd_get_metadata(chd->chd, CDROM_TRACK_METADATA_TAG,
                               chd->num_tracks, tmp, sizeof(tmp), NULL, NULL,
                               NULL);

        if (err == CHDERR_NONE) {
          sscanf(tmp, CDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype,
                 &frames);
        } else {
          /* if there's no valid metadata, this is the end of the TOC */
          break;
        }
      }
    }

    /* sanity checks */
    CHECK_EQ(tkid, chd->num_tracks + 1);

    if (strcmp(subtype, "NONE")) {
      LOG_WARNING("chd_parse track subtype %s unsupported", subtype);
      return 0;
    }

    if (pregap != 0 || postgap != 0) {
      LOG_WARNING("chd_parse expected zero-length pre and postgap", type);
      return 0;
    }

    /* figure out sector type */
    int sector_mode = 0;
    int sector_size = 0;

    if (!strcmp(type, "AUDIO")) {
      sector_mode = 0;
      sector_size = 2352;
    } else if (!strcmp(type, "MODE1")) {
      sector_mode = 1;
      sector_size = 2336;
    } else if (!strcmp(type, "MODE1_RAW")) {
      sector_mode = 1;
      sector_size = 2352;
    } else {
      LOG_WARNING("chd_parse unexpected mode %s", type);
      return 0;
    }

    /* add track */
    CHECK_LT(chd->num_tracks, ARRAY_SIZE(chd->tracks));
    struct track *track = &chd->tracks[chd->num_tracks++];

    if (!track_set_layout(track, sector_mode, sector_size)) {
      LOG_WARNING("chd_parse unsupported track layout mode=%d sector_size=%d",
                  sector_mode, sector_size);
      return 0;
    }

    track->num = chd->num_tracks;
    track->fad = fad;
    track->ctrl = strcmp(type, "AUDIO") == 0 ? 0 : 4;
    track->file_offset = fad - cad;

    if (verbose) {
      LOG_INFO("chd_parse '%s' track=%d fad=%d secsz=%d", tmp, track->num,
               track->fad, track->sector_size);
    }

    /* chd block addresses are padded to a 4-frame boundary */
    cad += ALIGN_UP(frames, 4);
    fad += frames;
  }

  /* gdroms contains two sessions, one for the single density area (tracks 0-1)
     and one for the high density area (tracks 3+) */
  chd->num_sessions = 2;

  /* single density area starts at 00:00:00 (fad 0x0) and can hold up to 4
     minutes of data (18,000 sectors at 75 sectors per second) */
  struct session *single = &chd->sessions[0];
  single->leadin_fad = 0x0;
  single->leadout_fad = 0x4650;
  single->first_track = 0;
  single->last_track = 0;

  /* high density area starts at 10:00:00 (fad 0xb05e) and can hold up to
     504,300 sectors (112 minutes, 4 seconds at 75 sectors per second) */
  struct session *high = &chd->sessions[1];
  high->leadin_fad = 0xb05e;
  high->leadout_fad = 0x861b4;
  high->first_track = 2;
  high->last_track = chd->num_tracks - 1;

  chd->mutex = mutex_create();
  chd->wake = cond_create();
  chd->done = cond_create();
  chd->thread = thread_create(&chd_thread, "chd", chd);
  CHECK_NOTNULL(chd->thread);

  return 1;
}

struct disc *chd_create(const char *filename, int verbose) {
  struct chd *chd = calloc(1, sizeof(struct chd));

  chd->destroy = &chd_destroy;
  chd->get_format = &chd_get_format;
  chd->get_num_sessions = &chd_get_num_sessions;
  chd->get_session = &chd_get_session;
  chd->get_num_tracks = &chd_get_num_tracks;
  chd->get_track = &chd_get_track;
  chd->get_toc = &chd_get_toc;
  chd->read_sector = &chd_read_sector;

  struct disc *disc = (struct disc *)chd;

  if (!chd_parse(disc, filename, verbose)) {
    chd_destroy(disc);
    return NULL;
  }

  return disc;
}
