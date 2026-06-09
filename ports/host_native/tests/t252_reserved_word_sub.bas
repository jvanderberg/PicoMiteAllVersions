' EXPECT_BOTH_ERROR:
' A SUB named after a command can be defined, but calling it puts the command
' keyword at statement start — so the call is the CHAIN command, which both
' engines reject (CHAIN expects a filename string).
SUB chain(n)
  n = 1
END SUB
DIM c = 0
chain c
PRINT c
