desc distance between two unsigned 32-bit values: the larger minus the smaller, never wraps
pre true
post or eq add ulong res ulong b ulong a eq add ulong res ulong a ulong b
example 3 10 -> 7
limit unsigned 32-bit values only; the argument order does not change the result
limit not a signed difference: it never says which argument was larger
