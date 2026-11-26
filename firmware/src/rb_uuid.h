#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t b[16];
} rb_uuid_t;

void rb_uuid_clear(rb_uuid_t *u);
bool rb_uuid_is_zero(const rb_uuid_t *u);
bool rb_uuid_equal(const rb_uuid_t *a, const rb_uuid_t *b);
void rb_uuid_new(rb_uuid_t *u);
void rb_uuid_format(const rb_uuid_t *u, char *out, int cap);
bool rb_uuid_parse(const char *text, rb_uuid_t *u);
