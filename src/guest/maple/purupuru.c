#include "core/core.h"
#include "guest/maple/maple.h"

struct purupuru {
  struct maple_device;
  int port;

  /* last condition set by the game, read back with GETCOND */
  uint32_t vibset;

  /* auto-stop time, the longest the motor runs before stopping itself */
  uint32_t ast;
};

static int purupuru_ast_ms(struct purupuru *puru) {
  return puru->ast * 250 + 250;
}

/* the condition word:
   31-24      23-16  15   14-12     11   10-8      0
   increment  freq   inh  neg power exh  pos power continuous */
static void purupuru_vibrate(struct purupuru *puru) {
  uint32_t vibset = puru->vibset;
  int pow_pos = (vibset >> 8) & 0x7;
  int pow_neg = (vibset >> 12) & 0x7;
  int freq = (vibset >> 16) & 0xff;
  int inc = (vibset >> 24) & 0xff;
  int cnt = vibset & 0x1;

  /* the increment only applies when converging or diverging */
  if (!(vibset & 0x8800)) {
    inc = 0;
  }

  float power = MIN((pow_pos + pow_neg) / 7.0f, 1.0f);
  int ast_ms = purupuru_ast_ms(puru);
  int duration_ms = ast_ms;

  /* a one-shot pulse lasts a cycle, a ramp lasts until it reaches its power */
  if (freq && (!cnt || inc)) {
    int cycles = inc ? inc * MAX(pow_pos, pow_neg) : 1;
    duration_ms = MIN(1000 * cycles / freq, ast_ms);
  }

  maple_vibrate(puru->mp, puru->port, power, duration_ms);
}

static int purupuru_frame(struct maple_device *dev,
                          const union maple_frame *req,
                          union maple_frame *res) {
  struct purupuru *puru = (struct purupuru *)dev;

  switch (req->cmd) {
    case MAPLE_REQ_DEVINFO: {
      char *name = "Puru Puru Pack";
      char *license = "Produced By or Under License From SEGA ENTERPRISES,LTD.";
      struct maple_device_info info = {0};
      info.func = MAPLE_FUNC_PURUPURU;
      info.data[0] = 0x00000101;
      info.region = 0xff;
      strncpy_pad_spaces(info.name, name, sizeof(info.name));
      strncpy_pad_spaces(info.license, license, sizeof(info.license));
      info.standby_power = 0x00c8;
      info.max_power = 0x0640;

      res->cmd = MAPLE_RES_DEVINFO;
      res->num_words = sizeof(info) >> 2;
      memcpy(res->params, &info, sizeof(info));
    } break;

    case MAPLE_REQ_GETCOND: {
      res->cmd = MAPLE_RES_TRANSFER;
      res->num_words = 2;
      res->params[0] = MAPLE_FUNC_PURUPURU;
      res->params[1] = puru->vibset;
    } break;

    case MAPLE_REQ_GETMEMINFO: {
      /* the vibration source's capabilities */
      res->cmd = MAPLE_RES_TRANSFER;
      res->num_words = 2;
      res->params[0] = MAPLE_FUNC_PURUPURU;
      res->params[1] = 0x3b07e010;
    } break;

    case MAPLE_REQ_BLKREAD: {
      res->cmd = MAPLE_RES_TRANSFER;
      res->num_words = 3;
      res->params[0] = MAPLE_FUNC_PURUPURU;
      res->params[1] = 0;
      res->params[2] = (puru->ast << 16) | 0x2;
    } break;

    case MAPLE_REQ_BLKWRITE: {
      puru->ast = (req->params[2] >> 16) & 0xff;
      res->cmd = MAPLE_RES_ACK;
    } break;

    case MAPLE_REQ_SETCOND: {
      puru->vibset = req->params[1];
      purupuru_vibrate(puru);
      res->cmd = MAPLE_RES_ACK;
    } break;

    default:
      res->cmd = MAPLE_RES_BADCMD;
      break;
  }

  return 1;
}

static void purupuru_destroy(struct maple_device *dev) {
  struct purupuru *puru = (struct purupuru *)dev;
  free(puru);
}

struct maple_device *purupuru_create(struct maple *mp, int port) {
  struct purupuru *puru = calloc(1, sizeof(struct purupuru));

  puru->mp = mp;
  puru->type = MAPLE_DEV_PURUPURU;
  puru->destroy = &purupuru_destroy;
  puru->frame = &purupuru_frame;
  puru->port = port;

  /* 5 seconds */
  puru->ast = 19;

  return (struct maple_device *)puru;
}
