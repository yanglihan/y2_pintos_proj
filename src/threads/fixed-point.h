// convert real number into p.q by * f, f = 2^q
// convert fixed-point back to int, / f

#define Q 14

#define F 1 << Q

#define to_FP (n) (n * F)

#define to_Int_towards_zero (x) (x / F)

#define to_Int_round_nearest (x) ((x > 0) ? (x + f / 2) / 2 : (x - f / 2) / 2)

#define add_FP_FP (x, y) (x + y)

#define subtract_FP_FP (x, y) (x - y)

#define add_FP_int (x, n) (x + n * F)

#define subtract_FP_int (x, n) (x - n * F)

#define multiply_FP_FP (x, y) (((int64_t) x) * y / F)

#define divide_FP_FP (x, y) (((int64_t) x) * F / y)

#define divide_FP_int (x, n) (x / n)
