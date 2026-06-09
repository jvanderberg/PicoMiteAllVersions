' Alternative comparison operator spellings: =< (same as <=) and => (same as >=).
PRINT 3 =< 5
PRINT 5 =< 5
PRINT 6 =< 5
PRINT 5 => 3
PRINT 5 => 5
PRINT 4 => 5
DIM a = 3, b = 3
IF a => b THEN PRINT "ge ok"
IF a =< b THEN PRINT "le ok"
