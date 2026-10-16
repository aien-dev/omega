desc sum of the four unsigned 32-bit values of a 4-value sequence, as unsigned 64-bit (never wraps)
pre true
post and and and and ge res ulong get v 0 ge res ulong get v 1 ge res ulong get v 2 ge res ulong get v 3 le res 17179869180
example 1 2 3 4 -> 10
limit fixed length 4; every value has weight 1
