#ifndef SACRED_CPU_TYPES_H
#define SACRED_CPU_TYPES_H
#include <stdint.h>
/* Explicit extended significand and sign/exponent; never host long double. */
typedef struct X87Value {
    uint64_t signif;
    uint16_t signExp;
} X87Value;
enum X87Operation {
    X87_ADD,
    X87_SUB,
    X87_MUL,
    X87_DIV,
    X87_SQRT,
    X87_ROUND,
    X87_SCALE,
    X87_PREM,
    X87_PREM1,
    X87_SIN,
    X87_COS,
    X87_SINCOS,
    X87_TAN,
    X87_ATAN,
    X87_2XM1,
    X87_YL2X,
    X87_YL2XP1
};
#endif
