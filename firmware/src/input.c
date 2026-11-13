#include "input.h"
#include "config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    bool  down;
    float t0;
    bool  longFired;
    float nextRepeat;
    bool  dblPending;     /* PUSH: a short press is waiting to see if a 2nd follows */
    float dblDeadline;
} bstate_t;

static bstate_t s_btn[BTN_COUNT];
static float    s_clock = 0;

#define QN 32
static event_t s_q[QN];
static int s_head = 0, s_tail = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_raw[BTN_COUNT];
static bool s_logical[BTN_COUNT];
static bool s_center_blocked;

#if BTN_ENABLE
static void input_scan_once(void);
static void button_task(void *arg);
#endif

static bool repeatable(btn_id_t b) {
    return b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT;
}

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
        s_btn[i].dblPending = false;
        s_btn[i].dblDeadline = 0;
    }
    s_head = s_tail = 0;
    s_clock = 0;

#if BTN_ENABLE
    const int pins[BTN_COUNT] = {PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT,
                                 PIN_BTN_RIGHT, PIN_BTN_PUSH, PIN_BTN_BACK};
    for (int i = 0; i < BTN_COUNT; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = BTN_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = BTN_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        int raw = gpio_get_level((gpio_num_t)pins[i]);
        s_raw[i] = BTN_ACTIVE_LOW ? !raw : !!raw;
        s_logical[i] = s_raw[i];
    }
    xTaskCreate(button_task, "buttons", 3072, NULL, 5, NULL);
#endif
}

void input_press(btn_id_t b) {
    if (b < 0 || b >= BTN_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    bstate_t *s = &s_btn[b];
    if (!s->down) {
        s->down = true;
        s->t0 = s_clock;
        s->longFired = false;
        s->nextRepeat = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

void input_release(btn_id_t b) {
    if (b < 0 || b >= BTN_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    bstate_t *s = &s_btn[b];
    if (s->down) {
        if (!s->longFired) {
            if (b == BTN_PUSH) {
                /* defer the single press briefly so a quick 2nd press = EV_DOUBLE */
                if (s->dblPending && s_clock < s->dblDeadline) {
                    s->dblPending = false;
                    emit(EV_DOUBLE, b);
                } else {
                    s->dblPending = true;
                    s->dblDeadline = s_clock + DOUBLE_MS;
                }
            } else {
                emit(EV_PRESS, b);
            }
        }
        s->down = false;
        emit(EV_RELEASED, b);
    }
    portEXIT_CRITICAL(&s_lock);
}

void input_tick(float dt_ms) {
    portENTER_CRITICAL(&s_lock);
    s_clock += dt_ms;
    for (int i = 0; i < BTN_COUNT; i++) {
        bstate_t *s = &s_btn[i];
        if (s->dblPending && !s->down && s_clock >= s->dblDeadline) {
            s->dblPending = false;
            emit(EV_PRESS, (btn_id_t)i);   /* no 2nd press -> plain single */
        }
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
    portEXIT_CRITICAL(&s_lock);
}

bool input_pop(event_t *out) {
    bool got = false;
    portENTER_CRITICAL(&s_lock);
    if (s_head != s_tail) {
        *out = s_q[s_head];
        s_head = (s_head + 1) % QN;
        got = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return got;
}

bool input_is_down(btn_id_t b) {
    if (b < 0 || b >= BTN_COUNT) return false;
    portENTER_CRITICAL(&s_lock);
    bool down = s_btn[b].down;
    portEXIT_CRITICAL(&s_lock);
    return down;
}

void input_debug(btn_debug_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    out->push = s_raw[BTN_PUSH];
    out->up = s_raw[BTN_UP];
    out->down = s_raw[BTN_DOWN];
    out->left = s_raw[BTN_LEFT];
    out->right = s_raw[BTN_RIGHT];
    out->back = s_raw[BTN_BACK];
    portEXIT_CRITICAL(&s_lock);
}

#if BTN_ENABLE
static void input_scan_once(void) {
    const int pins[BTN_COUNT] = {PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT,
                                 PIN_BTN_RIGHT, PIN_BTN_PUSH, PIN_BTN_BACK};
    bool raw[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; i++) {
        int level = gpio_get_level((gpio_num_t)pins[i]);
        raw[i] = BTN_ACTIVE_LOW ? !level : !!level;
    }
    /* RKJXM1015004: PUSH is the common line and is low for center + every
     * direction. Only a PUSH with no direction is a real center press. */
    bool dir = raw[BTN_UP] || raw[BTN_DOWN] || raw[BTN_LEFT] || raw[BTN_RIGHT];
    /* A direction pulls the common PUSH line low too. Do not let the release
     * sequence generate a phantom center press: center must be re-armed by
     * fully releasing PUSH after a direction. */
    if (dir) s_center_blocked = true;
    if (!raw[BTN_PUSH]) s_center_blocked = false;
    bool logical[BTN_COUNT];
    logical[BTN_UP] = raw[BTN_UP];
    logical[BTN_DOWN] = raw[BTN_DOWN];
    logical[BTN_LEFT] = raw[BTN_LEFT];
    logical[BTN_RIGHT] = raw[BTN_RIGHT];
    logical[BTN_PUSH] = raw[BTN_PUSH] && !dir && !s_center_blocked;
    logical[BTN_BACK] = raw[BTN_BACK];

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < BTN_COUNT; i++) s_raw[i] = raw[i];
    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < BTN_COUNT; i++) {
        if (logical[i] == s_logical[i]) continue;
        s_logical[i] = logical[i];
        if (logical[i]) input_press((btn_id_t)i);
        else input_release((btn_id_t)i);
    }
}

static void button_task(void *arg) {
    (void)arg;
    for (;;) {
        input_scan_once();
        vTaskDelay(pdMS_TO_TICKS(1));   /* hardware RC debounce; no software filter */
    }
}

void input_poll_hw(void) {
    /* Kept for compatibility; the real scan runs in button_task(). */
    input_scan_once();
}
#else
void input_poll_hw(void) {}
#endif
