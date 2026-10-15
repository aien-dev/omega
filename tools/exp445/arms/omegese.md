REPRESENTATION: omegese

Write the program in Omegese: one statement per line, words separated by single spaces. No brackets,
no commas, no semicolons, no indentation, no comments. A checker reads it line by line and refuses
the first invalid line with a reason; a valid program becomes the program.

Types (Omegese name = type): bool = bool, uchar = u8, ushort = u16, uint = u32, ulong = u64,
byte = i8, short = i16, int = i32, long = i64. The names u8 .. i64 are accepted too.

Statements:

    fn <name> <param> <type> <param> <type> ... to <type>     (or: to void); end closes the function
    <name> = <value>          write a value: a new name, or an existing name of the same type
    if <cond>  ...  else  ...  end           (else is optional)
    while <cond> bound <N>  ...  end         N in 1..1000000; more than N iterations is trap 4
    for <i> <type> <count>  ...  end         i = 0, 1, ..., count-1 (count is a number or a name)
    break                     leave the innermost while or for
    ret <value>               (ret alone in a void function)
    put <array> <index> <value>
    free <array>
    trap <code>
    <function> <args>         call a void function

Values (exactly one operation per line; operands are names or plain numbers, never nested):

    <name>  or  <number>
    add sub mul div rem and or xor shl shr  <a> <b>
    eq ne lt le gt ge  <a> <b>                (gives bool)
    neg <a>  (signed)     not <a>  (bool: logical; number: bitwise)
    <type> <number>       a typed number, e.g. uint 0
    <type> <name>         convert, e.g. ulong x (trap 5 if the value does not fit)
    get <array> <index>
    new <type> <len> <init>
    own <array>  edit <array>  view <array>
    <function> <args>     call a function written earlier in the program

Rules:
- Types are inferred: a new name takes the type of its value. A plain number takes the type of the
  other operand (`add x 1`); where there is none, write the type (`s = uint 0`). Both operands of an
  op have one type; convert explicitly.
- A condition is a bool name, true, false, or one comparison (`while ne b 0 bound 64`).
- A name first written inside if/else/while/for exists only until that block's end. To use a value
  after the block, write it before the block, then overwrite it inside.
- Every name you write must be read later. Parameters may be overwritten. A for counter may not.
- Every path through a function that returns a value must end in ret or trap.
- Parameters of array type: own|edit|view <type> <len>, e.g. `v view uint 4`.

Example:

```
fn digits x uint to uint
n = uint 1
while ge x 10 bound 10
x = div x 10
n = add n 1
end
ret n
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

