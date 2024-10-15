// convert real number into p.q by * f, f = 2^q
// convert fixed-point back to int, / f

#define Q 14

#define F (1 << Q)

#define TO_FP(n) (n * F)

#define TO_INT_ROUND_DOWN(x) (x / F)

#define TO_INT_ROUND_NEAREST(x) ((x > 0) ? (x + F / 2) / 2 : (x - F / 2) / 2)

#define ADD_FP_FP(x, y) (x + y)

#define SUBTRACT_FP_FP(x, y) (x - y)

#define ADD_FP_INT(x, n) (x + n * F)

#define SUBTRACT_FP_INT(x, n) (x - n * F)

#define MULTIPLY_FP_FP(x, y) (((int64_t) x) * y / F)

#define DIVIDE_FP_FP(x, y) (((int64_t) x) * F / y)

#define DIVIDE_FP_INT(x, n) (x / n)
