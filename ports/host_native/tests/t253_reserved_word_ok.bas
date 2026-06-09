' Command-keyword names ARE allowed away from statement start: as a DIM target,
' and read inside an expression. Locks in that the VM does not over-reject.
OPTION EXPLICIT
DIM chain = 7
DIM trace$ = "hi"
DIM y = chain + 1
PRINT y
PRINT chain
PRINT trace$
