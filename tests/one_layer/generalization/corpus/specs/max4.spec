desc largest of the four unsigned 32-bit values of a 4-value sequence
pre true
post and and and and ge res get v 0 ge res get v 1 ge res get v 2 ge res get v 3 or or or eq res get v 0 eq res get v 1 eq res get v 2 eq res get v 3
example 2 9 9 1 -> 9
limit fixed length 4; returns the largest value only, not its position or the second largest
