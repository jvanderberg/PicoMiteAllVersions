' EXPECT_BOTH_ERROR: invalid (valid is
' An ON index outside 0..255 is an error in both engines (the interpreter's
' getint range check); the VM must not silently fall through.
DIM INTEGER i = -1
ON i GOTO L1, L2
L1:
PRINT "one"
L2:
PRINT "two"
