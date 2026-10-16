desc average of two unsigned 32-bit values rounded down, with no intermediate wrap
pre true
post eq ulong res div add ulong a ulong b 2
example 7 8 -> 7
limit rounds down (7 and 8 give 7); not the rounded-up average
