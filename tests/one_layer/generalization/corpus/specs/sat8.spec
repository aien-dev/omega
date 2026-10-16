desc convert an unsigned 32-bit value to unsigned 8-bit, saturating: anything above 255 gives 255
pre true
post or and gt x 255 eq res 255 and le x 255 eq uint res x
example 300 -> 255
limit saturates; it does not keep the low 8 bits (300 gives 255, not 44)
