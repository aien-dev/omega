desc limit an unsigned 32-bit x to lo..hi: below lo gives lo, above hi gives hi, otherwise x
pre le lo hi
post and and ge res lo le res hi or eq res x or eq res lo eq res hi
example 12 3 9 -> 9
limit needs lo <= hi; with lo > hi the result is outside lo..hi (lo when x < lo, else hi when x > hi)
limit unsigned 32-bit values only
counter 5 9 3 -> 9
counter 50 9 3 -> 3
