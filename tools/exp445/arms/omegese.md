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

Word card: every fixed word and what it does.

    fn      start a function: fn <name> <param> <type> ... to <type>
    to      in a fn line: the return type follows
    void    in a fn line: the function returns nothing (to void)
    end     close the innermost fn, if, else, while or for
    if      if <cond>: run the lines up to else or end when cond is true
    else    the lines run when the if condition is false
    while   while <cond> bound <N>: repeat while cond is true
    bound   in a while line: the most passes allowed; one more is trap 4
    for     for <i> <type> <count>: run with i = 0, 1, ..., count-1
    break   leave the innermost while or for
    ret     return a value (ret alone in a void function)
    trap    trap <code>: stop the run with that trap code
    put     put <array> <index> <value>: store into an array
    free    free <array>: release an array made with new
    true    the bool value true
    false   the bool value false
    add     add <a> <b>: a + b
    sub     sub <a> <b>: a - b
    mul     mul <a> <b>: a * b
    div     div <a> <b>: a / b, truncated toward zero
    rem     rem <a> <b>: remainder of a / b, sign of a
    and     and <a> <b>: bitwise and; on bools, logical and
    or      or <a> <b>: bitwise or; on bools, logical or
    xor     xor <a> <b>: bitwise exclusive or
    shl     shl <a> <b>: a shifted left by b bits
    shr     shr <a> <b>: a shifted right by b bits
    eq      eq <a> <b>: a == b (bool)
    ne      ne <a> <b>: a != b (bool)
    lt      lt <a> <b>: a < b (bool)
    le      le <a> <b>: a <= b (bool)
    gt      gt <a> <b>: a > b (bool)
    ge      ge <a> <b>: a >= b (bool)
    neg     neg <a>: -a (signed)
    not     not <a>: logical not of a bool, bitwise not of a number
    get     get <array> <index>: read an element
    new     <name> = new <type> <len> <init>: a new array of len elements, all init
    bool    type bool; bool <v> converts or types a value
    uchar   type u8 (unsigned 8-bit); uchar <v> converts or types a value
    ushort  type u16 (unsigned 16-bit)
    uint    type u32 (unsigned 32-bit)
    ulong   type u64 (unsigned 64-bit)
    byte    type i8 (signed 8-bit)
    short   type i16 (signed 16-bit)
    int     type i32 (signed 32-bit)
    long    type i64 (signed 64-bit)
    own     own <array>: move the array (a parameter: own <type> <len>)
    edit    edit <array>: lend the array for writing (a parameter: edit <type> <len>)
    view    view <array>: lend the array for reading (a parameter: view <type> <len>)
