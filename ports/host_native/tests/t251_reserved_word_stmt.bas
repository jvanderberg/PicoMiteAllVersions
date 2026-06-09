' EXPECT_BOTH_ERROR:
' A statement that begins with a command keyword IS that command, even when a
' same-named variable was declared. `trace$ = ...` is the TRACE command at
' statement start, not an assignment, so both engines must reject it.
DIM trace$
trace$ = "hello"
PRINT trace$
