#ifndef HOST_H
#define HOST_H

#include <stdint.h>

struct host;

/* audio */
void audio_push(struct host *host, const int16_t *data, int frames);

/* video */

/* input */
int input_max_controllers(struct host *host);
const char *input_controller_name(struct host *host, int port);
/* runs the port's rumble at power [0,1] for duration_ms, 0 power stops it */
void input_vibrate(struct host *host, int port, float power, int duration_ms);

/* ui */
int ui_load_game(struct host *host, const char *path);
void ui_opened(struct host *host);
void ui_closed(struct host *host);

#endif
