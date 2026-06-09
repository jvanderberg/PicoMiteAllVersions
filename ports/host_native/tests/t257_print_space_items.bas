' PRINT items may be separated by whitespace alone (no ';' or ','), mixing
' string literals, variables, and expressions. The interpreter accepts this;
' the VM must produce identical output.
DIM a = 10, b = 20
DIM s$ = "x"
PRINT "v=" a
PRINT a b
PRINT s$ a
PRINT "p" a "q" b
PRINT a + 1 b * 2
