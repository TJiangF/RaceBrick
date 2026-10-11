/* Input layer. Emits the exact same events as the web demo:
 *   PRESS   short press (on release)
 *   LONG    held >= LONG_PRESS_MS
 *   REPEAT  continuous repeat for Up/Down after LONG
 *   RELEASED physical release
 * Sources: real GPIO (BTN_ENABLE=1) or an external injector (autoplay demo).
 */
#pragma once
#include <stdbool.h>

typedef enum { BTN_UP = 0, BTN_DOWN, BTN_PUSH, BTN_OK, BTN_BACK, BTN_COUNT } btn_id_t;
typedef enum { EV_PRESS, EV_LONG, EV_REPEAT, EV_RELEASED, EV_DOUBLE } ev_type_t;

typedef struct {
    ev_type_t type;
    btn_id_t  button;
    float     t;
} event_t;

void input_init(void);
void input_tick(float dt_ms);
void input_poll_hw(void);              /* only meaningful when BTN_ENABLE */
void input_press(btn_id_t b);
void input_release(btn_id_t b);
bool input_pop(event_t *out);
bool input_is_down(btn_id_t b);
