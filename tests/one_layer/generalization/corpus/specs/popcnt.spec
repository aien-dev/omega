desc number of one bits among the 32 bits of an unsigned 32-bit value (0..32)
pre true
post and le res 32 eq eq res 0 eq x 0
example 255 -> 8
limit counts all 32 bits; to count part of the bits, mask first
