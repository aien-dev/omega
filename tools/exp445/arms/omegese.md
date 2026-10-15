REPRESENTATION: omegese

Write the program in Omegese: one statement per line, words separated by single spaces. No commas,
no semicolons, no indentation, no comments. A checker reads it line by line and refuses
the first invalid line with a reason; a valid program becomes the program.

Types (Omegese name = type): bool = bool, uchar = u8, ushort = u16, uint = u32, ulong = u64,
byte = i8, short = i16, int = i32, long = i64. The names u8 .. i64 are accepted too.

Statements:

    fn <name> <param> <type> <param> <type> ... to <type>     (or: to void); end closes the function
    <name> = <value>          write a value: a new name, or an existing name of the same type
    if <cond>  ...  else  ...  end           (else is optional)
    while <cond> bound <N>  ...  end         N in 1..1000000; more than N iterations is trap 4
    for <i> <type> <count>  ...  end         i = 0, 1, ..., count-1 (count is a number or a value)
    break                     leave the innermost while or for
    ret <value>               (ret alone in a void function)
    put <array> <index> <value>
    free <array>
    trap <code>               code 1..14; like ret, it ends the path
    <function> <args>         call a void function

Values (each op takes a fixed number of values; each value may itself be an op):

    <name>  or  <number>
    add sub mul div rem and or xor shl shr  <a> <b>
    eq ne lt le gt ge  <a> <b>                (gives bool)
    neg <a>  (signed)     not <a>  (bool: logical; number: bitwise)
    <type> <number>       a typed number, e.g. uint 0
    <type> <value>        convert, e.g. ulong x (trap 5 if the value does not fit)
    get <array> <index>
    new <type> <len> <init>   only as a whole line: <name> = new ...
    own <array>  edit <array>  view <array>
    <function> <args>     call a function written earlier in the program

Values nest in prefix order: `add mul a a 1` is (a*a)+1, `eq rem n 2 0` is n%2 == 0. Brackets are
optional and change nothing: `add (mul a a) 1`. A ( must hold exactly one value.

Rules:
- Types are inferred: a new name takes the type of its value. A plain number takes the type of the
  other value of its op (`add x 1`); where there is none, write the type (`s = uint 0`). Both values
  of an op have one type; convert explicitly. Two plain numbers in one op are refused.
- A condition is any value giving bool (`while ne b 0 bound 64`, `if or lt p 0 gt p 9`).
- A name first written inside if/else/while/for exists only until that block's end. To use a value
  after the block, write it before the block, then overwrite it inside.
- Every name you write must be read later. Parameters may be overwritten. A for counter may not.
- Every path through a function that returns a value must end in ret or trap.
- Parameters of array type: own|edit|view <type> <len>, e.g. `v view uint 4`.

Example:

```
fn digitsq x uint to uint
s = uint 0
while ne x 0 bound 10
s = add s mul rem x 10 rem x 10
x = div x 10
end
ret s
end
```

Example:

```
fn sum4 v view long 4 to long
s = long 0
for i uint 4
t = get v i
if gt t 0
s = add s t
end
end
ret s
end
```

