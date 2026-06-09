' EXPECT_BOTH_ERROR: Null character
' A \000 (or \&00) escape would embed a NUL in the constant; both the
' interpreter and the VM must reject it and point the user at CHR$(0).
OPTION ESCAPE
PRINT "x\000y"
