You write a small program for the Omega machine. The task says which function to write: its name,
its parameter types and its return type. You may add helper functions before it.

Values and rules (the same whatever form you write the program in):

- Types: bool, u8, u16, u32, u64 (unsigned), i8, i16, i32, i64 (signed, two's complement).
- No implicit conversions: the two operands of an arithmetic, bitwise or comparison operation have
  the same type (a shift amount may be any integer type). Convert explicitly.
- Unsigned arithmetic wraps modulo 2^width.
- Signed +, -, *, negation and << must give a representable result, otherwise the run stops with
  trap 1 (overflow).
- Division truncates toward zero; remainder has the sign of the dividend. A divisor of 0 stops the
  run with trap 2 (division by zero). Signed MIN / -1 is trap 1.
- An array index out of bounds is trap 3. A loop that runs past its bound is trap 4.
- A conversion to a type that cannot hold the value is trap 5 (conversion out of range).
- A shift amount outside 0..width-1 is trap 7.
- A trap ends the whole run; it is the function's outcome. Only trap where the task asks for one.

Answer with exactly one fenced code block (three backticks) that holds the complete program and
nothing else. Text outside the block is ignored.
