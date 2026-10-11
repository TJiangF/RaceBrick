#include "input.h"
#include "config.h"
#include "driver/gpio.h"

typedef struct {
    bool  down;
    float t0;
    bool  longFired;
    float nextRepeat;
} bstate_t;

static bstate_t s_btn[BTN_COUNT];
static float    s_clock = 0;

#define QN 32
static event_t s_q[QN];
static int s_head = 0, s_tail = 0;

static bool repeatable(btn_id_t b) { return b == BTN_UP || b == BTN_DOWN; }

static void emit(ev_type_t type, btn_id_t b) {
    int nx = (s_tail + 1) % QN;
    if (nx == s_head) return; /* drop when full */
    s_q[s_tail].type = type;
    s_q[s_tail].button = b;
    s_q[s_tail].t = s_clock;
    s_tail = nx;
}

void input_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn[i].down = false;
        s_btn[i].t0 = 0;
        s_btn[i].longFired = false;
        s_btn[i].nextRepeat = 0;
    }
    s_head = s_tail = 0;
    s_clock = 0;

#if BTN_ENABLE
    const int pins[BTN_COUNT] = {PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_PUSH,
                                 PIN_BTN_OK, PIN_BTN_BACK};
    for (int i = 0; i < BTN_COUNT; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = BTN_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = BTN_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }
#endif
}

void input_press(btn_id_t b) {
    if (b < 0 || b >= BTN_COUNT) return;
    bstate_t *s = &s_btn[b];
    if (s->down) return;
    s->down = true;
    s->t0 = s_clock;
    s->longFired = false;
    s->nextRepeat = 0;
}

void input_release(btn_id_t b) {
    if (b < 0 || b >= BTN_COUNT) return;
    bstate_t *s = &s_btn[b];
    if (!s->down) return;
    if (!s->longFired) emit(EV_PRESS, b);
    s->down = false;
    emit(EV_RELEASED, b);
}

void input_tick(float dt_ms) {
    s_clock += dt_ms;
    for (int i = 0; i < BTN_COUNT; i++) {
        bstate_t *s = &s_btn[i];
        if (!s->down) continue;
        float held = s_clock - s->t0;
        if (!s->longFired && held >= LONG_PRESS_MS) {
            s->longFired = true;
            emit(EV_LONG, (btn_id_t)i);
            if (repeatable((btn_id_t)i)) s->nextRepeat = s_clock + REPEAT_MS;
        } else if (s->longFired && repeatable((btn_id_t)i)) {
            if (s_clock >= s->nextRepeat) {
                s->nextRepeat += REPEAT_MS;
                emit(EV_REPEAT, (btn_id_t)i);
            }
        }
    }
}

bool input_pop(event_t *out) {
    if (s_head == s_tail) return false;
    *out = s_q[s_head];
    s_head = (s_head + 1) % QN;
    return true;
}

bool input_is_down(btn_id_t b) {
    if (b < 0 || b >= BTN_COUNT) return false;
    return s_btn[b].down;
}

#if BTN_ENABLE
static bool s_deb[BTN_COUNT];
static float s_deb_t[BTN_COUNT];

void input_poll_hw(void) {
    const int pins[BTN_COUNT] = {PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_PUSH,
                                 PIN_BTN_OK, PIN_BTN_BACK};
    for (int i = 0; i < BTN_COUNT; i++) {
        int raw = gpio_get_level((gpio_num_t)pins[i]);
        bool active = BTN_ACTIVE_LOW ? !raw : !!raw;
        if (active != s_deb[i]) {
            if (s_clock - s_deb_t[i] >= DEBOUNCE_MS) {
                s_deb[i] = active;
                s_deb_t[i] = s_clock;
                if (active) input_press((btn_id_t)i);
                else input_release((btn_id_t)i);
            }
        } else {
            s_deb_t[i] = s_clock;
        }
    }
}
#else
void input_poll_hw(void) {}
#endif
