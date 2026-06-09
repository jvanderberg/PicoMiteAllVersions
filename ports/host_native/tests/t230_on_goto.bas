' ON expr GOTO dispatch to one of several labels.
DIM INTEGER n = 2
ON n GOTO first, second, third
PRINT "fallthrough"
first:
PRINT "1st"
GOTO done
second:
PRINT "2nd"
GOTO done
third:
PRINT "3rd"
done:
PRINT "end"
