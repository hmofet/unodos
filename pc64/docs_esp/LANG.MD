# UnoC — the language

UnoC is the language of the Studio compiler: a practical subset of C, big
enough to write a real app (the shipped game DOSTRIS.C compiles unchanged),
small enough to compile reliably on the machine itself.

## Types

- `void`, `char`, `short`, `int`, `long`, and their `unsigned` forms.
- **`long` is 4 bytes; `long long` is 8 bytes** (LLP64 — matching the kernel).
  Pointers are 8 bytes. `sizeof(long) == 4`.
- Pointers, arrays (multi-dimensional), `struct`, `union`, `enum`, `typedef`,
  and function pointers.
- `const` is accepted and ignored.

## Expressions

The full C operator set: `+ - * / % & | ^ << >> && || ! ~`, comparisons,
`?:`, assignment and compound assignment (`+=` …), `++ --` (pre and post),
`[]`, `.`, `->`, `&`, `*`, casts, `sizeof`, the comma operator, and function
calls (up to 8 arguments). Pointer arithmetic is scaled by the pointee size.

## Statements

`if`/`else`, `while`, `do`/`while`, `for`, `switch`/`case`/`default`,
`break`, `continue`, `return`, blocks, and expression statements.

## Declarations and initializers

Globals and function-scope `static`s take constant initializers, including
brace initializers and **address constants**, so the idiomatic app table
works:

    static const AppInterface kIface = {
        my_draw, my_key, 0, my_tick, 0, 0, "My App", { 30, 30, 330, 260 }
    };

`char s[] = "text";` and `T v[] = { ... };` size themselves from the
initializer.

## Preprocessor

- `#include "FILE.H"` — one level deep; searched in the file's folder and in
  `SDK\`.
- `#define NAME tokens` — object-like macros only.
- `#ifdef` / `#ifndef` / `#else` / `#endif`, `#undef`.

## Not in UnoC (v1)

Floating point, `long double`; variadic functions (`...`); **function-like**
macros (`#define F(x) ...`); bitfields; `goto`; variable-length arrays; `#if`
arithmetic. Pass structs to functions **by pointer** (a 1/2/4/8-byte struct
like `Point` may go by value; larger ones must be pointers).

## Errors

Every diagnostic carries a file and line. In Studio the build pane lists them
and a click jumps the caret to the spot.
