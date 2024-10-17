#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#define Q 14

#define F ((1 << Q))

#define TO_FP(n) ((n) * (F))

#define TO_INT_ROUND_DOWN(x) ((x) / (F))

#define TO_INT_ROUND_NEAREST(x) (((x) > 0) ? ((x) + (F) / 2) / (F) : ((x) - (F) / 2) / (F))

#define ADD_FP_FP(x, y) ((x) + (y))

#define SUBTRACT_FP_FP(x, y) ((x) - (y))

#define ADD_FP_INT(x, n) ((x) + (n) * (F))

#define SUBTRACT_FP_INT(x, n) ((x) - (n) * (F))

#define MULTIPLY_FP_FP(x, y) (((int64_t) (x)) * (y) / (F))

#define MULTIPLY_FP_INT(x, n) ((x) * (n))

#define DIVIDE_FP_FP(x, y) (((int64_t) (x)) * (F) / (y))

#define DIVIDE_FP_INT(x, n) ((x) / (n))

#endif
