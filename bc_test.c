/*
 * bc_test.c -- Test harness for bytecode VM (FTEST command)
 *
 * Implements bc_run_tests() which is called by cmd_ftest().
 * Each test loads a BASIC program into ProgMemory, compiles it with
 * bc_compile(), runs it on the VM with output capture, and compares
 * the captured output against expected output.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include "MMBasic.h"
#include "bytecode.h"

/* External MMBasic globals */
extern void MMPrintString(char *s);
extern void error(char *msg, ...);
#ifdef MMBASIC_HOST
#define BC_ALLOC(sz)   calloc(1, (sz))
#define BC_FREE(p)     free((p))
#else
extern void *GetMemory(int size);
extern void FreeMemory(unsigned char *addr);
#define BC_ALLOC(sz)   GetMemory((sz))
#define BC_FREE(p)     FreeMemory((unsigned char *)(p))
#endif
extern void tokenise(int console);
extern unsigned char inpbuf[];
extern unsigned char tknbuf[];
extern unsigned char *ProgMemory;
extern int PSize;
extern void CheckAbort(void);
extern jmp_buf mark;

/* ======================================================================
 * Test case structure
 * ====================================================================== */
typedef struct {
    const char *name;           /* e.g. "T01: Integer arithmetic" */
    const char *source;         /* BASIC source code, lines separated by \n */
    const char *expected;       /* Expected PRINT output (exact bytes) */
    int         skip_compare;   /* 1 = benchmark test, don't compare output exactly */
    const char *partial_match;  /* if skip_compare, check output contains this string */
} BCTestCase;


/* ======================================================================
 * Helper: load a multi-line BASIC program into ProgMemory
 *
 * Tokenises each line and builds the ProgMemory format:
 *   T_NEWLINE T_LINENBR lo hi [tokenised content] 0x00
 *   ...
 *   0x00 0x00   (program terminator)
 * ====================================================================== */
static int load_test_program(const char *source) {
    memset(ProgMemory, 0, 4096);  /* clear enough for test programs */
    unsigned char *dest = ProgMemory;
    const char *line_start = source;
    int line_num = 1;

    while (*line_start) {
        /* Find end of line */
        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        int line_len = (int)(line_end - line_start);

        /* Skip blank lines and comment-only lines (starting with ') */
        if (line_len > 0) {
            /* Check if line is only whitespace */
            const char *p = line_start;
            while (p < line_end && (*p == ' ' || *p == '\t')) p++;

            if (p < line_end && *p != '\'') {
                /* Copy line to inpbuf (as if user typed it) */
                memset(inpbuf, 0, STRINGSIZE);
                memcpy(inpbuf, line_start, line_len);
                inpbuf[line_len] = 0;

                /* Tokenise it */
                tokenise(0);

                /* tknbuf now has the tokenised line.
                 * Build ProgMemory entry: T_NEWLINE T_LINENBR lo hi content 0x00
                 */
                *dest++ = T_NEWLINE;
                *dest++ = T_LINENBR;
                /* MMBasic T_LINENBR is big-endian: high byte first */
                *dest++ = (unsigned char)((line_num >> 8) & 0xFF);
                *dest++ = (unsigned char)(line_num & 0xFF);

                /* Copy tokenised content from tknbuf.
                 * tokenise() may prepend T_NEWLINE + T_LINENBR — skip those. */
                unsigned char *src = tknbuf;
                if (*src == T_NEWLINE) src++;
                if (*src == T_LINENBR) {
                    src++;  /* skip T_LINENBR */
                    src++;  /* skip line_num low byte */
                    src++;  /* skip line_num high byte */
                }

                while (*src) {
                    *dest++ = *src++;
                }
                *dest++ = 0;  /* end of this statement */
            }
        }

        line_num++;
        line_start = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    /* Terminate program with two null bytes */
    *dest++ = 0;
    *dest++ = 0;

    PSize = (int)(dest - ProgMemory);
    return 0;
}


/* ======================================================================
 * Test case definitions — all 40 tests from the plan
 * ====================================================================== */

/* ----- T01: Integer Arithmetic ----- */
static const char T01_source[] =
    "DIM a% = 10, b% = 3, c%\n"
    "c% = a% + b%\n"
    "PRINT c%\n"
    "c% = a% - b%\n"
    "PRINT c%\n"
    "c% = a% * b%\n"
    "PRINT c%\n"
    "c% = a% \\ b%\n"
    "PRINT c%\n"
    "c% = a% MOD b%\n"
    "PRINT c%\n"
    "c% = -a%\n"
    "PRINT c%\n"
    "c% = a% + b% * 2\n"
    "PRINT c%\n"
    "c% = (a% + b%) * 2\n"
    "PRINT c%\n";

static const char T01_expected[] =
    " 13 \r\n"
    " 7 \r\n"
    " 30 \r\n"
    " 3 \r\n"
    " 1 \r\n"
    "-10 \r\n"
    " 16 \r\n"
    " 26 \r\n";

/* ----- T02: Float Arithmetic ----- */
static const char T02_source[] =
    "DIM x! = 10.5, y! = 3.2, z!\n"
    "z! = x! + y!\n"
    "PRINT z!\n"
    "z! = x! - y!\n"
    "PRINT z!\n"
    "z! = x! * y!\n"
    "PRINT z!\n"
    "z! = x! / y!\n"
    "PRINT z!\n"
    "z! = -x!\n"
    "PRINT z!\n"
    "z! = x! ^ 2\n"
    "PRINT z!\n";

static const char T02_expected[] =
    " 13.7 \r\n"
    " 7.3 \r\n"
    " 33.6 \r\n"
    " 3.28125 \r\n"
    "-10.5 \r\n"
    " 110.25 \r\n";

/* ----- T03: Mixed Int/Float Promotion ----- */
static const char T03_source[] =
    "DIM a% = 5, b! = 2.5, c!\n"
    "c! = a% + b!\n"
    "PRINT c!\n"
    "c! = a% * b!\n"
    "PRINT c!\n"
    "c! = a% / b!\n"
    "PRINT c!\n";

static const char T03_expected[] =
    " 7.5 \r\n"
    " 12.5 \r\n"
    " 2 \r\n";

/* ----- T04: String Operations ----- */
static const char T04_source[] =
    "DIM a$ = \"Hello\", b$ = \" World\", c$\n"
    "c$ = a$ + b$\n"
    "PRINT c$\n"
    "PRINT LEN(c$)\n"
    "PRINT LEFT$(c$, 5)\n"
    "PRINT RIGHT$(c$, 5)\n"
    "PRINT MID$(c$, 4, 5)\n"
    "PRINT UCASE$(c$)\n"
    "PRINT LCASE$(c$)\n";

static const char T04_expected[] =
    "Hello World\r\n"
    " 11 \r\n"
    "Hello\r\n"
    "World\r\n"
    "lo Wo\r\n"
    "HELLO WORLD\r\n"
    "hello world\r\n";

/* ----- T05: String Comparison ----- */
static const char T05_source[] =
    "DIM a$ = \"abc\", b$ = \"def\", c$ = \"abc\"\n"
    "IF a$ = c$ THEN PRINT \"EQ pass\" ELSE PRINT \"EQ fail\"\n"
    "IF a$ <> b$ THEN PRINT \"NE pass\" ELSE PRINT \"NE fail\"\n"
    "IF a$ < b$ THEN PRINT \"LT pass\" ELSE PRINT \"LT fail\"\n"
    "IF b$ > a$ THEN PRINT \"GT pass\" ELSE PRINT \"GT fail\"\n";

static const char T05_expected[] =
    "EQ pass\r\n"
    "NE pass\r\n"
    "LT pass\r\n"
    "GT pass\r\n";

/* ----- T06: Integer Comparison ----- */
static const char T06_source[] =
    "DIM a% = 10, b% = 20\n"
    "IF a% < b% THEN PRINT \"LT pass\" ELSE PRINT \"LT fail\"\n"
    "IF b% > a% THEN PRINT \"GT pass\" ELSE PRINT \"GT fail\"\n"
    "IF a% <= 10 THEN PRINT \"LE pass\" ELSE PRINT \"LE fail\"\n"
    "IF b% >= 20 THEN PRINT \"GE pass\" ELSE PRINT \"GE fail\"\n"
    "IF a% = 10 THEN PRINT \"EQ pass\" ELSE PRINT \"EQ fail\"\n"
    "IF a% <> b% THEN PRINT \"NE pass\" ELSE PRINT \"NE fail\"\n";

static const char T06_expected[] =
    "LT pass\r\n"
    "GT pass\r\n"
    "LE pass\r\n"
    "GE pass\r\n"
    "EQ pass\r\n"
    "NE pass\r\n";

/* ----- T07: Float Comparison ----- */
static const char T07_source[] =
    "DIM x! = 1.5, y! = 2.5\n"
    "IF x! < y! THEN PRINT \"LT pass\" ELSE PRINT \"LT fail\"\n"
    "IF y! > x! THEN PRINT \"GT pass\" ELSE PRINT \"GT fail\"\n"
    "IF x! <= 1.5 THEN PRINT \"LE pass\" ELSE PRINT \"LE fail\"\n"
    "IF y! >= 2.5 THEN PRINT \"GE pass\" ELSE PRINT \"GE fail\"\n"
    "IF x! = 1.5 THEN PRINT \"EQ pass\" ELSE PRINT \"EQ fail\"\n"
    "IF x! <> y! THEN PRINT \"NE pass\" ELSE PRINT \"NE fail\"\n";

static const char T07_expected[] =
    "LT pass\r\n"
    "GT pass\r\n"
    "LE pass\r\n"
    "GE pass\r\n"
    "EQ pass\r\n"
    "NE pass\r\n";

/* ----- T08: Bitwise Operations ----- */
static const char T08_source[] =
    "DIM a% = &HFF, b% = &H0F, c%\n"
    "c% = a% AND b%\n"
    "PRINT HEX$(c%)\n"
    "c% = a% OR &HF00\n"
    "PRINT HEX$(c%)\n"
    "c% = a% XOR b%\n"
    "PRINT HEX$(c%)\n"
    "c% = NOT 0\n"
    "PRINT c%\n"
    "c% = a% << 4\n"
    "PRINT HEX$(c%)\n"
    "c% = a% >> 4\n"
    "PRINT HEX$(c%)\n";

static const char T08_expected[] =
    "F\r\n"
    "FFF\r\n"
    "F0\r\n"
    "-1 \r\n"
    "FF0\r\n"
    "F\r\n";

/* ----- T09: Single-Line IF ----- */
static const char T09_source[] =
    "DIM a% = 5\n"
    "IF a% = 5 THEN PRINT \"yes\" ELSE PRINT \"no\"\n"
    "IF a% = 6 THEN PRINT \"no\" ELSE PRINT \"yes\"\n"
    "IF a% > 3 THEN PRINT \"gt3\"\n"
    "IF a% < 3 THEN PRINT \"SHOULD NOT PRINT\"\n"
    "PRINT \"done\"\n";

static const char T09_expected[] =
    "yes\r\n"
    "yes\r\n"
    "gt3\r\n"
    "done\r\n";

/* ----- T10: Multi-Line IF ----- */
static const char T10_source[] =
    "DIM x% = 2\n"
    "IF x% = 1 THEN\n"
    "  PRINT \"one\"\n"
    "ELSEIF x% = 2 THEN\n"
    "  PRINT \"two\"\n"
    "ELSEIF x% = 3 THEN\n"
    "  PRINT \"three\"\n"
    "ELSE\n"
    "  PRINT \"other\"\n"
    "ENDIF\n";

static const char T10_expected[] =
    "two\r\n";

/* ----- T11: Nested IF ----- */
static const char T11_source[] =
    "DIM a% = 1, b% = 2\n"
    "IF a% = 1 THEN\n"
    "  IF b% = 2 THEN\n"
    "    PRINT \"a=1,b=2\"\n"
    "  ELSE\n"
    "    PRINT \"a=1,b<>2\"\n"
    "  ENDIF\n"
    "ELSE\n"
    "  PRINT \"a<>1\"\n"
    "ENDIF\n";

static const char T11_expected[] =
    "a=1,b=2\r\n";

/* ----- T12: FOR/NEXT Integer ----- */
static const char T12_source[] =
    "DIM i%, s%\n"
    "s% = 0\n"
    "FOR i% = 1 TO 10\n"
    "  s% = s% + i%\n"
    "NEXT i%\n"
    "PRINT s%\n";

static const char T12_expected[] =
    " 55 \r\n";

/* ----- T13: FOR/NEXT with STEP ----- */
static const char T13_source[] =
    "DIM i%\n"
    "FOR i% = 0 TO 20 STEP 5\n"
    "  PRINT i%;\n"
    "NEXT i%\n"
    "PRINT\n"
    "FOR i% = 10 TO 1 STEP -1\n"
    "  PRINT i%;\n"
    "NEXT i%\n"
    "PRINT\n";

static const char T13_expected[] =
    " 0  5  10  15  20 \r\n"
    " 10  9  8  7  6  5  4  3  2  1 \r\n";

/* ----- T14: FOR/NEXT Float ----- */
static const char T14_source[] =
    "DIM x!, s!\n"
    "s! = 0\n"
    "FOR x! = 0.5 TO 5.0 STEP 0.5\n"
    "  s! = s! + x!\n"
    "NEXT x!\n"
    "PRINT s!\n";

static const char T14_expected[] =
    " 27.5 \r\n";

/* ----- T15: Nested FOR ----- */
static const char T15_source[] =
    "DIM i%, j%, c%\n"
    "c% = 0\n"
    "FOR i% = 1 TO 5\n"
    "  FOR j% = 1 TO 5\n"
    "    c% = c% + 1\n"
    "  NEXT j%\n"
    "NEXT i%\n"
    "PRINT c%\n";

static const char T15_expected[] =
    " 25 \r\n";

/* ----- T16: DO/LOOP WHILE ----- */
static const char T16_source[] =
    "DIM i% = 0\n"
    "DO WHILE i% < 5\n"
    "  i% = i% + 1\n"
    "LOOP\n"
    "PRINT i%\n";

static const char T16_expected[] =
    " 5 \r\n";

/* ----- T17: DO/LOOP UNTIL ----- */
static const char T17_source[] =
    "DIM i% = 0\n"
    "DO\n"
    "  i% = i% + 1\n"
    "LOOP UNTIL i% = 10\n"
    "PRINT i%\n";

static const char T17_expected[] =
    " 10 \r\n";

/* ----- T18: EXIT FOR / EXIT DO ----- */
static const char T18_source[] =
    "DIM i%\n"
    "FOR i% = 1 TO 100\n"
    "  IF i% = 7 THEN EXIT FOR\n"
    "NEXT i%\n"
    "PRINT i%\n"
    "i% = 0\n"
    "DO\n"
    "  i% = i% + 1\n"
    "  IF i% = 12 THEN EXIT DO\n"
    "LOOP\n"
    "PRINT i%\n";

static const char T18_expected[] =
    " 7 \r\n"
    " 12 \r\n";

/* ----- T19: GOTO ----- */
static const char T19_source[] =
    "DIM x% = 0\n"
    "10 x% = x% + 1\n"
    "IF x% < 5 THEN GOTO 10\n"
    "PRINT x%\n";

static const char T19_expected[] =
    " 5 \r\n";

/* ----- T20: GOSUB/RETURN ----- */
static const char T20_source[] =
    "DIM g% = 0\n"
    "GOSUB 100\n"
    "PRINT g%\n"
    "GOSUB 100\n"
    "PRINT g%\n"
    "GOTO 200\n"
    "100 g% = g% + 10\n"
    "RETURN\n"
    "200 PRINT \"done\"\n";

static const char T20_expected[] =
    " 10 \r\n"
    " 20 \r\n"
    "done\r\n";

/* ----- T21: SUB ----- */
static const char T21_source[] =
    "DIM result%\n"
    "SUB AddPrint(a%, b%)\n"
    "  LOCAL c%\n"
    "  c% = a% + b%\n"
    "  PRINT c%\n"
    "END SUB\n"
    "AddPrint 3, 4\n"
    "AddPrint 10, 20\n";

static const char T21_expected[] =
    " 7 \r\n"
    " 30 \r\n";

/* ----- T22: FUNCTION ----- */
static const char T22_source[] =
    "FUNCTION Square%(n%)\n"
    "  Square% = n% * n%\n"
    "END FUNCTION\n"
    "PRINT Square%(5)\n"
    "PRINT Square%(12)\n"
    "DIM x% = Square%(7) + Square%(3)\n"
    "PRINT x%\n";

static const char T22_expected[] =
    " 25 \r\n"
    " 144 \r\n"
    " 58 \r\n";

/* ----- T23: Recursive FUNCTION ----- */
static const char T23_source[] =
    "FUNCTION Factorial%(n%)\n"
    "  IF n% <= 1 THEN\n"
    "    Factorial% = 1\n"
    "  ELSE\n"
    "    Factorial% = n% * Factorial%(n% - 1)\n"
    "  ENDIF\n"
    "END FUNCTION\n"
    "PRINT Factorial%(1)\n"
    "PRINT Factorial%(5)\n"
    "PRINT Factorial%(10)\n";

static const char T23_expected[] =
    " 1 \r\n"
    " 120 \r\n"
    " 3628800 \r\n";

/* ----- T24: LOCAL Variable Scoping ----- */
static const char T24_source[] =
    "DIM x% = 100\n"
    "SUB test()\n"
    "  LOCAL x%\n"
    "  x% = 999\n"
    "  PRINT x%\n"
    "END SUB\n"
    "PRINT x%\n"
    "test\n"
    "PRINT x%\n";

static const char T24_expected[] =
    " 100 \r\n"
    " 999 \r\n"
    " 100 \r\n";

/* ----- T25: Integer Arrays ----- */
static const char T25_source[] =
    "DIM a%(10)\n"
    "DIM i%\n"
    "FOR i% = 0 TO 10\n"
    "  a%(i%) = i% * i%\n"
    "NEXT i%\n"
    "FOR i% = 0 TO 10\n"
    "  PRINT a%(i%);\n"
    "NEXT i%\n"
    "PRINT\n";

static const char T25_expected[] =
    " 0  1  4  9  16  25  36  49  64  81  100 \r\n";

/* ----- T26: Float Arrays ----- */
static const char T26_source[] =
    "DIM a!(5)\n"
    "DIM i%\n"
    "FOR i% = 0 TO 5\n"
    "  a!(i%) = i% * 1.1\n"
    "NEXT i%\n"
    "FOR i% = 0 TO 5\n"
    "  PRINT a!(i%);\n"
    "NEXT i%\n"
    "PRINT\n";

static const char T26_expected[] =
    " 0  1.1  2.2  3.3  4.4  5.5 \r\n";

/* ----- T27: String Arrays ----- */
static const char T27_source[] =
    "DIM a$(3)\n"
    "a$(0) = \"hello\"\n"
    "a$(1) = \"world\"\n"
    "a$(2) = \"foo\"\n"
    "a$(3) = \"bar\"\n"
    "DIM i%\n"
    "FOR i% = 0 TO 3\n"
    "  PRINT a$(i%)\n"
    "NEXT i%\n";

static const char T27_expected[] =
    "hello\r\n"
    "world\r\n"
    "foo\r\n"
    "bar\r\n";

/* ----- T28: 2D Arrays ----- */
static const char T28_source[] =
    "DIM m%(3,3)\n"
    "DIM i%, j%\n"
    "FOR i% = 0 TO 3\n"
    "  FOR j% = 0 TO 3\n"
    "    m%(i%, j%) = i% * 4 + j%\n"
    "  NEXT j%\n"
    "NEXT i%\n"
    "FOR i% = 0 TO 3\n"
    "  FOR j% = 0 TO 3\n"
    "    PRINT m%(i%, j%);\n"
    "  NEXT j%\n"
    "  PRINT\n"
    "NEXT i%\n";

static const char T28_expected[] =
    " 0  1  2  3 \r\n"
    " 4  5  6  7 \r\n"
    " 8  9  10  11 \r\n"
    " 12  13  14  15 \r\n";

/* ----- T29: SELECT CASE ----- */
static const char T29_source[] =
    "DIM i%\n"
    "FOR i% = 1 TO 5\n"
    "  SELECT CASE i%\n"
    "    CASE 1\n"
    "      PRINT \"one\"\n"
    "    CASE 2, 3\n"
    "      PRINT \"two or three\"\n"
    "    CASE 4\n"
    "      PRINT \"four\"\n"
    "    CASE ELSE\n"
    "      PRINT \"other\"\n"
    "  END SELECT\n"
    "NEXT i%\n";

static const char T29_expected[] =
    "one\r\n"
    "two or three\r\n"
    "two or three\r\n"
    "four\r\n"
    "other\r\n";

/* ----- T30: WHILE/LOOP ----- */
static const char T30_source[] =
    "DIM n% = 1\n"
    "WHILE n% <= 32\n"
    "  PRINT n%;\n"
    "  n% = n% * 2\n"
    "LOOP\n"
    "PRINT\n";

static const char T30_expected[] =
    " 1  2  4  8  16  32 \r\n";

/* ----- T31: Operator Precedence ----- */
static const char T31_source[] =
    "DIM r%\n"
    "r% = 2 + 3 * 4\n"
    "PRINT r%\n"
    "r% = (2 + 3) * 4\n"
    "PRINT r%\n"
    "r% = 2 ^ 3 + 1\n"
    "PRINT r%\n"
    "r% = 10 - 2 - 3\n"
    "PRINT r%\n"
    "r% = 2 * 3 + 4 * 5\n"
    "PRINT r%\n"
    "r% = 100 \\ 3 MOD 2\n"
    "PRINT r%\n";

static const char T31_expected[] =
    " 14 \r\n"
    " 20 \r\n"
    " 9 \r\n"
    " 5 \r\n"
    " 26 \r\n"
    " 1 \r\n";

/* ----- T32: Logical Operators ----- */
static const char T32_source[] =
    "DIM a% = 1, b% = 0\n"
    "IF a% AND NOT b% THEN PRINT \"pass1\" ELSE PRINT \"fail1\"\n"
    "IF a% OR b% THEN PRINT \"pass2\" ELSE PRINT \"fail2\"\n"
    "IF NOT (a% AND b%) THEN PRINT \"pass3\" ELSE PRINT \"fail3\"\n"
    "IF (a% OR b%) AND (NOT b%) THEN PRINT \"pass4\" ELSE PRINT \"fail4\"\n";

static const char T32_expected[] =
    "pass1\r\n"
    "pass2\r\n"
    "pass3\r\n"
    "pass4\r\n";

/* ----- T33: 64-bit Integer Boundaries ----- */
static const char T33_source[] =
    "DIM a%, b%, c%\n"
    "a% = 2147483647\n"
    "b% = 1\n"
    "c% = a% + b%\n"
    "PRINT c%\n"
    "a% = &H7FFFFFFFFFFFFFFF\n"
    "PRINT a%\n"
    "a% = -1\n"
    "PRINT a%\n";

static const char T33_expected[] =
    " 2147483648 \r\n"
    " 9223372036854775807 \r\n"
    "-1 \r\n";

/* ----- T34: Math Functions ----- */
static const char T34_source[] =
    "PRINT ABS(-42)\n"
    "PRINT ABS(-3.14)\n"
    "PRINT SGN(-5)\n"
    "PRINT SGN(0)\n"
    "PRINT SGN(5)\n"
    "PRINT INT(3.7)\n"
    "PRINT INT(-3.7)\n"
    "PRINT FIX(3.7)\n"
    "PRINT FIX(-3.7)\n"
    "PRINT SQR(144)\n";

static const char T34_expected[] =
    " 42 \r\n"
    " 3.14 \r\n"
    "-5 \r\n"
    " 0 \r\n"
    " 5 \r\n"
    " 3 \r\n"
    "-4 \r\n"
    " 3 \r\n"
    "-3 \r\n"
    " 12 \r\n";

/* ----- T35: Fibonacci Benchmark ----- */
static const char T35_source[] =
    "FUNCTION Fib%(n%)\n"
    "  IF n% <= 1 THEN\n"
    "    Fib% = n%\n"
    "  ELSE\n"
    "    Fib% = Fib%(n% - 1) + Fib%(n% - 2)\n"
    "  ENDIF\n"
    "END FUNCTION\n"
    "DIM t! = TIMER\n"
    "PRINT Fib%(25)\n"
    "t! = TIMER - t!\n"
    "PRINT \"Time: \"; t!; \" sec\"\n";

/* T35 partial match: we check for the Fibonacci result but not the time */
static const char T35_partial[] = " 75025 \r\n";

/* ----- T36: Sieve of Eratosthenes Benchmark ----- */
static const char T36_source[] =
    "DIM flags%(8192)\n"
    "DIM i%, j%, count%, t!\n"
    "t! = TIMER\n"
    "FOR i% = 0 TO 8191\n"
    "  flags%(i%) = 1\n"
    "NEXT i%\n"
    "count% = 0\n"
    "FOR i% = 2 TO 8191\n"
    "  IF flags%(i%) THEN\n"
    "    count% = count% + 1\n"
    "    FOR j% = i% + i% TO 8191 STEP i%\n"
    "      flags%(j%) = 0\n"
    "    NEXT j%\n"
    "  ENDIF\n"
    "NEXT i%\n"
    "t! = TIMER - t!\n"
    "PRINT \"Primes: \"; count%\n"
    "PRINT \"Time: \"; t!; \" sec\"\n";

static const char T36_partial[] = "Primes:  1028 \r\n";

/* ----- T37: Nested Function Calls ----- */
static const char T37_source[] =
    "FUNCTION Double%(n%)\n"
    "  Double% = n% * 2\n"
    "END FUNCTION\n"
    "FUNCTION AddOne%(n%)\n"
    "  AddOne% = n% + 1\n"
    "END FUNCTION\n"
    "PRINT Double%(AddOne%(5))\n"
    "PRINT AddOne%(Double%(5))\n"
    "PRINT Double%(Double%(Double%(1)))\n";

static const char T37_expected[] =
    " 12 \r\n"
    " 11 \r\n"
    " 8 \r\n";

/* ----- T38: Strings in Expressions ----- */
static const char T38_source[] =
    "DIM a$ = \"Hello\"\n"
    "DIM b$ = \"World\"\n"
    "DIM c$ = a$ + \" \" + b$ + \"!\"\n"
    "PRINT c$\n"
    "PRINT LEN(a$ + b$)\n"
    "DIM d$ = MID$(c$, 1, 5)\n"
    "PRINT d$\n";

static const char T38_expected[] =
    "Hello World!\r\n"
    " 10 \r\n"
    "Hello\r\n";

/* ----- T39: Matrix Multiply Benchmark ----- */
static const char T39_source[] =
    "DIM N% = 20\n"
    "DIM a!(N%, N%), b!(N%, N%), c!(N%, N%)\n"
    "DIM i%, j%, k%, t!\n"
    "FOR i% = 0 TO N%\n"
    "  FOR j% = 0 TO N%\n"
    "    a!(i%, j%) = i% + j%\n"
    "    b!(i%, j%) = i% - j%\n"
    "  NEXT j%\n"
    "NEXT i%\n"
    "t! = TIMER\n"
    "FOR i% = 0 TO N%\n"
    "  FOR j% = 0 TO N%\n"
    "    c!(i%, j%) = 0\n"
    "    FOR k% = 0 TO N%\n"
    "      c!(i%, j%) = c!(i%, j%) + a!(i%, k%) * b!(k%, j%)\n"
    "    NEXT k%\n"
    "  NEXT j%\n"
    "NEXT i%\n"
    "t! = TIMER - t!\n"
    "PRINT \"c(0,0)=\"; c!(0, 0)\n"
    "PRINT \"c(10,10)=\"; c!(10, 10)\n"
    "PRINT \"Time: \"; t!; \" sec\"\n";

/* T39: matrix multiply results are deterministic, time is not.
 * c(0,0) = sum_{k=0}^{20} (0+k)*(k-0) = sum k^2 = 20*21*41/6 = 2870
 * c(10,10) = sum_{k=0}^{20} (10+k)*(k-10) = sum (k^2 - 100) = 2870 - 2100 = 770
 */
static const char T39_partial[] = "c(0,0)= 2870";

/* ----- T40: Type Enforcement ----- */
static const char T40_source[] =
    "DIM i% = 1\n"
    "DIM f! = 1.5\n"
    "DIM s$ = \"test\"\n"
    "PRINT i%; f!; s$\n";

static const char T40_expected[] =
    " 1  1.5 test\r\n";


/* ======================================================================
 * Master test table
 * ====================================================================== */
static const BCTestCase test_cases[] = {
    { "T01: Integer arithmetic",       T01_source, T01_expected, 0, NULL },
    { "T02: Float arithmetic",         T02_source, T02_expected, 0, NULL },
    { "T03: Mixed int/float",          T03_source, T03_expected, 0, NULL },
    { "T04: String operations",        T04_source, T04_expected, 0, NULL },
    { "T05: String comparison",        T05_source, T05_expected, 0, NULL },
    { "T06: Integer comparison",       T06_source, T06_expected, 0, NULL },
    { "T07: Float comparison",         T07_source, T07_expected, 0, NULL },
    { "T08: Bitwise operations",       T08_source, T08_expected, 0, NULL },
    { "T09: Single-line IF",           T09_source, T09_expected, 0, NULL },
    { "T10: Multi-line IF",            T10_source, T10_expected, 0, NULL },
    { "T11: Nested IF",                T11_source, T11_expected, 0, NULL },
    { "T12: FOR/NEXT integer",         T12_source, T12_expected, 0, NULL },
    { "T13: FOR/NEXT with STEP",       T13_source, T13_expected, 0, NULL },
    { "T14: FOR/NEXT float",           T14_source, T14_expected, 0, NULL },
    { "T15: Nested FOR",               T15_source, T15_expected, 0, NULL },
    { "T16: DO/LOOP WHILE",            T16_source, T16_expected, 0, NULL },
    { "T17: DO/LOOP UNTIL",            T17_source, T17_expected, 0, NULL },
    { "T18: EXIT FOR / EXIT DO",       T18_source, T18_expected, 0, NULL },
    { "T19: GOTO",                     T19_source, T19_expected, 0, NULL },
    { "T20: GOSUB/RETURN",             T20_source, T20_expected, 0, NULL },
    { "T21: SUB",                      T21_source, T21_expected, 0, NULL },
    { "T22: FUNCTION",                 T22_source, T22_expected, 0, NULL },
    { "T23: Recursive FUNCTION",       T23_source, T23_expected, 0, NULL },
    { "T24: LOCAL variable scoping",   T24_source, T24_expected, 0, NULL },
    { "T25: Integer arrays",           T25_source, T25_expected, 0, NULL },
    { "T26: Float arrays",             T26_source, T26_expected, 0, NULL },
    { "T27: String arrays",            T27_source, T27_expected, 0, NULL },
    { "T28: 2D arrays",                T28_source, T28_expected, 0, NULL },
    { "T29: SELECT CASE",              T29_source, T29_expected, 0, NULL },
    { "T30: WHILE/LOOP",               T30_source, T30_expected, 0, NULL },
    { "T31: Operator precedence",      T31_source, T31_expected, 0, NULL },
    { "T32: Logical operators",        T32_source, T32_expected, 0, NULL },
    { "T33: 64-bit integers",          T33_source, T33_expected, 0, NULL },
    { "T34: Math functions",           T34_source, T34_expected, 0, NULL },
    { "T35: Fibonacci benchmark",      T35_source, NULL,         1, T35_partial },
    { "T36: Sieve benchmark",          T36_source, NULL,         1, T36_partial },
    { "T37: Nested function calls",    T37_source, T37_expected, 0, NULL },
    { "T38: String expressions",       T38_source, T38_expected, 0, NULL },
    { "T39: Matrix multiply",          T39_source, NULL,         1, T39_partial },
    { "T40: Type enforcement",         T40_source, T40_expected, 0, NULL },
};

#define NUM_TESTS (int)(sizeof(test_cases) / sizeof(test_cases[0]))


/* ======================================================================
 * Helper: clean up VM arrays and string globals
 * ====================================================================== */
static void cleanup_vm(BCVMState *vm, BCCompiler *cs) {
    int i;
    (void)cs;
    /* Free array data (allocated via GetMemory in OP_DIM_ARR).
     * String element buffers were GetTempMemory — don't free individually. */
    for (i = 0; i < BC_MAX_SLOTS; i++) {
        if (vm->arrays[i].data) {
            BC_FREE(vm->arrays[i].data);
            vm->arrays[i].data = NULL;
        }
    }
    /* Free local array data */
    for (i = 0; i < VM_MAX_LOCALS; i++) {
        if (vm->local_arrays[i].data) {
            BC_FREE(vm->local_arrays[i].data);
            vm->local_arrays[i].data = NULL;
        }
    }
}

static int run_compiler_selftests(char *msg, size_t msglen) {
    int i;
    BCCompiler *cs = (BCCompiler *)BC_ALLOC(sizeof(BCCompiler));
    if (!cs) {
        snprintf(msg, msglen, "compiler self-test OOM");
        return -1;
    }
    memset(cs, 0, sizeof(BCCompiler));

    if (bc_compiler_alloc(cs) != 0) {
        BC_FREE(cs);
        snprintf(msg, msglen, "compiler self-test alloc failed");
        return -1;
    }

    bc_compiler_init(cs);
    for (i = 0; i < BC_MAX_LINEMAP; i++) {
        if (bc_add_linemap_entry(cs, (uint16_t)(i + 1), (uint32_t)(i * 3)) != 0) {
            snprintf(msg, msglen, "line-map fill failed early at %d/%d: %s",
                     i + 1, BC_MAX_LINEMAP, cs->error_msg);
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    if (bc_add_linemap_entry(cs, (uint16_t)(BC_MAX_LINEMAP + 1), 0) == 0) {
        snprintf(msg, msglen, "line-map overflow was accepted");
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    if (!cs->has_error || strstr(cs->error_msg, "Too many line map entries") == NULL) {
        snprintf(msg, msglen, "line-map overflow reported wrong error: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "FASTGFX CREATE\n"
            "FASTGFX SWAP\n"
            "FASTGFX SYNC\n"
            "FASTGFX CLOSE\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "FASTGFX native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_swap = 0;
        int saw_sync = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_FASTGFX_SWAP) saw_swap = 1;
            if (cs->code[i] == OP_FASTGFX_SYNC) saw_sync = 1;
        }
        if (!saw_swap || !saw_sync) {
            snprintf(msg, msglen, "FASTGFX SWAP/SYNC did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "CLS RGB(BLACK)\n"
            "BOX 4, 5, 6, 4, 0, , RGB(RED)\n"
            "END\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "BOX native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_box = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_BOX) saw_box = 1;
        }
        if (!saw_box) {
            snprintf(msg, msglen, "BOX did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "DIM INTEGER x%(1), y%(1), r%(1)\n"
            "x%(0)=10 : x%(1)=20\n"
            "y%(0)=10 : y%(1)=20\n"
            "r%(0)=5  : r%(1)=5\n"
            "CIRCLE x%(), y%(), r%()\n"
            "END\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "CIRCLE native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_circle = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_CIRCLE) saw_circle = 1;
        }
        if (!saw_circle) {
            snprintf(msg, msglen, "CIRCLE did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "LINE 1, 2, 3, 4, , RGB(RED)\n"
            "END\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "LINE native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_draw_line = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_DRAW_LINE) saw_draw_line = 1;
        }
        if (!saw_draw_line) {
            snprintf(msg, msglen, "LINE did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "DIM s$ = \"A\", j$ = \"LT\"\n"
            "TEXT 1, 2, s$, j$, , , RGB(WHITE), RGB(BLACK)\n"
            "END\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "TEXT native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_text = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_TEXT) saw_text = 1;
        }
        if (!saw_text) {
            snprintf(msg, msglen, "TEXT did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "CLS RGB(BLUE)\n"
            "CLS\n"
            "END\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "CLS native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_cls = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_CLS) saw_cls = 1;
        }
        if (!saw_cls) {
            snprintf(msg, msglen, "CLS did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_init(cs);
    if (load_test_program(
            "DIM x%(2), y%(2), c%(2)\n"
            "PIXEL x%(), y%(), c%()\n"
            "END\n") != 0 ||
        bc_compile(cs, ProgMemory, PSize) != 0) {
        snprintf(msg, msglen, "PIXEL native opcode compile failed: %s", cs->error_msg);
        bc_compiler_free(cs);
        BC_FREE(cs);
        return -1;
    }

    {
        int saw_pixel = 0;
        for (i = 0; i < cs->code_len; i++) {
            if (cs->code[i] == OP_PIXEL) saw_pixel = 1;
        }
        if (!saw_pixel) {
            snprintf(msg, msglen, "PIXEL did not compile natively");
            bc_compiler_free(cs);
            BC_FREE(cs);
            return -1;
        }
    }

    bc_compiler_free(cs);
    BC_FREE(cs);
    snprintf(msg, msglen, "compiler self-tests passed");
    return 0;
}


/* ======================================================================
 * bc_run_tests — main entry point called by cmd_ftest()
 * ====================================================================== */
void bc_run_tests(void) {
    int passed = 0;
    int failed = 0;
    int errors = 0;
    int i;
    char msg[256];

    /* Save ProgMemory state so we can restore after tests */
    unsigned char *saved_prog = (unsigned char *)BC_ALLOC(PSize + 16);
    int saved_psize = PSize;
    if (saved_prog) {
        memcpy(saved_prog, ProgMemory, PSize);
    }

    MMPrintString("=== FTEST: Bytecode VM Test Suite ===\r\n");
    snprintf(msg, sizeof(msg), "Running %d tests...\r\n\r\n", NUM_TESTS);
    MMPrintString(msg);

    if (run_compiler_selftests(msg, sizeof(msg)) != 0) {
        MMPrintString("SELFTEST FAIL: ");
        MMPrintString(msg);
        MMPrintString("\r\n\r\n");
        errors++;
    }

    for (i = 0; i < NUM_TESTS; i++) {
        const BCTestCase *tc = &test_cases[i];
        char *capture_buf = NULL;
        int test_error = 0;

        /* Heap-allocate structs — BCVMState is ~6 KB, overflows device stack */
        BCCompiler *cs = (BCCompiler *)BC_ALLOC(sizeof(BCCompiler));
        BCVMState  *vm = (BCVMState  *)BC_ALLOC(sizeof(BCVMState));
        if (cs) memset(cs, 0, sizeof(BCCompiler));
        if (vm) memset(vm, 0, sizeof(BCVMState));
        if (!cs || !vm) {
            if (cs) BC_FREE(cs);
            if (vm) BC_FREE(vm);
            MMPrintString("ERROR: out of memory (structs)\r\n");
            errors++;
            continue;
        }

        /* Allow user to abort with Ctrl-C */
        CheckAbort();

        snprintf(msg, sizeof(msg), "[%2d/%2d] %-30s ", i + 1, NUM_TESTS, tc->name);
        MMPrintString(msg);

        /* Load the test program into ProgMemory */
        load_test_program(tc->source);

        if (bc_compiler_alloc(cs) != 0) {
            BC_FREE(cs);
            BC_FREE(vm);
            MMPrintString("ERROR: out of memory (compiler)\r\n");
            errors++;
            continue;
        }

        if (bc_vm_alloc(vm) != 0) {
            bc_compiler_free(cs);
            BC_FREE(cs);
            BC_FREE(vm);
            MMPrintString("ERROR: out of memory (VM)\r\n");
            errors++;
            continue;
        }

        /* Allocate capture buffer */
        capture_buf = (char *)BC_ALLOC(4096);
        if (capture_buf) memset(capture_buf, 0, 4096);
        if (!capture_buf) {
            bc_vm_free(vm);
            bc_compiler_free(cs);
            BC_FREE(cs);
            BC_FREE(vm);
            MMPrintString("ERROR: out of memory (capture)\r\n");
            errors++;
            continue;
        }

        /* Compile the program */
        bc_compiler_init(cs);
        if (bc_compile(cs, ProgMemory, PSize) != 0) {
            snprintf(msg, sizeof(msg), "COMPILE ERROR at line %d: %s\r\n",
                     cs->error_line, cs->error_msg);
            MMPrintString(msg);
            BC_FREE(capture_buf);
            bc_vm_free(vm);
            bc_compiler_free(cs);
            BC_FREE(cs);
            BC_FREE(vm);
            errors++;
            continue;
        }

        /* Initialize and execute the VM with output capture */
        bc_vm_init(vm, cs);
        bc_vm_start_capture(vm, capture_buf, 4096);

        /* Use setjmp for error recovery — bc_vm_execute and bc_vm_error
         * call error() which longjmps through the 'mark' jmp_buf.
         * We save and restore 'mark' around each test. */
        {
            jmp_buf saved_mark;
            memcpy(saved_mark, mark, sizeof(jmp_buf));

            if (setjmp(mark) == 0) {
                bc_vm_execute(vm);
            } else {
                /* An error occurred during execution */
                test_error = 1;
            }

            memcpy(mark, saved_mark, sizeof(jmp_buf));
        }

        if (test_error) {
            snprintf(msg, sizeof(msg), "RUNTIME ERROR\r\n");
            MMPrintString(msg);
            cleanup_vm(vm, cs);
            BC_FREE(capture_buf);
            bc_vm_free(vm);
            bc_compiler_free(cs);
            BC_FREE(cs);
            BC_FREE(vm);
            errors++;
            continue;
        }

        /* Compare output */
        if (tc->skip_compare) {
            /* Benchmark test: check for partial match */
            if (tc->partial_match && strstr(capture_buf, tc->partial_match)) {
                MMPrintString("PASS (benchmark)\r\n");
                passed++;
            } else if (tc->partial_match) {
                snprintf(msg, sizeof(msg), "FAIL\r\n  Expected to contain: \"");
                MMPrintString(msg);
                /* Print partial_match with visible escapes */
                {
                    const char *p = tc->partial_match;
                    while (*p) {
                        if (*p == '\r') MMPrintString("\\r");
                        else if (*p == '\n') MMPrintString("\\n");
                        else { char ch[2] = { *p, 0 }; MMPrintString(ch); }
                        p++;
                    }
                }
                MMPrintString("\"\r\n  Got: \"");
                {
                    const char *p = capture_buf;
                    int count = 0;
                    while (*p && count < 120) {
                        if (*p == '\r') MMPrintString("\\r");
                        else if (*p == '\n') MMPrintString("\\n");
                        else { char ch[2] = { *p, 0 }; MMPrintString(ch); }
                        p++;
                        count++;
                    }
                }
                MMPrintString("\"\r\n");
                failed++;
            } else {
                /* No partial match specified — just pass if it ran without error */
                MMPrintString("PASS (ran OK)\r\n");
                passed++;
            }
        } else {
            /* Exact match test */
            if (strcmp(capture_buf, tc->expected) == 0) {
                MMPrintString("PASS\r\n");
                passed++;
            } else {
                MMPrintString("FAIL\r\n");

                /* Show expected (with visible escapes, truncated) */
                MMPrintString("  Expected: \"");
                {
                    const char *p = tc->expected;
                    int count = 0;
                    while (*p && count < 120) {
                        if (*p == '\r') MMPrintString("\\r");
                        else if (*p == '\n') MMPrintString("\\n");
                        else if (*p == '\t') MMPrintString("\\t");
                        else { char ch[2] = { *p, 0 }; MMPrintString(ch); }
                        p++;
                        count++;
                    }
                    if (*p) MMPrintString("...");
                }
                MMPrintString("\"\r\n");

                /* Show actual (with visible escapes, truncated) */
                MMPrintString("  Got:      \"");
                {
                    const char *p = capture_buf;
                    int count = 0;
                    while (*p && count < 120) {
                        if (*p == '\r') MMPrintString("\\r");
                        else if (*p == '\n') MMPrintString("\\n");
                        else if (*p == '\t') MMPrintString("\\t");
                        else { char ch[2] = { *p, 0 }; MMPrintString(ch); }
                        p++;
                        count++;
                    }
                    if (*p) MMPrintString("...");
                }
                MMPrintString("\"\r\n");

                /* Show first difference position */
                {
                    int pos = 0;
                    const char *e = tc->expected;
                    const char *g = capture_buf;
                    while (*e && *g && *e == *g) { e++; g++; pos++; }
                    snprintf(msg, sizeof(msg), "  First diff at byte %d\r\n", pos);
                    MMPrintString(msg);
                }

                failed++;
            }
        }

        /* Clean up this test */
        cleanup_vm(vm, cs);
        BC_FREE(capture_buf);
        bc_vm_free(vm);
        bc_compiler_free(cs);
        BC_FREE(cs);
        BC_FREE(vm);
    }

    /* Restore ProgMemory */
    if (saved_prog) {
        memcpy(ProgMemory, saved_prog, saved_psize);
        PSize = saved_psize;
        BC_FREE(saved_prog);
    }

    /* Print summary */
    MMPrintString("\r\n=== Test Summary ===\r\n");
    snprintf(msg, sizeof(msg), "  Passed:  %d\r\n", passed);
    MMPrintString(msg);
    snprintf(msg, sizeof(msg), "  Failed:  %d\r\n", failed);
    MMPrintString(msg);
    snprintf(msg, sizeof(msg), "  Errors:  %d\r\n", errors);
    MMPrintString(msg);
    snprintf(msg, sizeof(msg), "  Total:   %d/%d\r\n", passed, NUM_TESTS);
    MMPrintString(msg);

    if (passed == NUM_TESTS) {
        MMPrintString("\r\nAll tests PASSED.\r\n");
    } else {
        MMPrintString("\r\nSome tests FAILED.\r\n");
    }
}
