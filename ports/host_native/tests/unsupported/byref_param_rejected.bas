' EXPECT_ERROR: BYREF not supported
' The VM call ABI passes scalars by value, so an explicit BYREF parameter
' cannot be honoured. Reject it at compile time rather than silently mutate a
' copy (which would diverge from the interpreter).
SUB modify(BYREF a)
  a = 99
END SUB
DIM INTEGER x = 5
modify x
PRINT x
