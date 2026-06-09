' JSON$() — object fields, array indexing, nested paths.
DIM a%(100)
q$ = CHR$(34)
s$ = "{" + q$ + "name" + q$ + ":" + q$ + "picomite" + q$ + ","
s$ = s$ + q$ + "vals" + q$ + ":[1,2.5,true],"
s$ = s$ + q$ + "obj" + q$ + ":{" + q$ + "x" + q$ + ":42},"
s$ = s$ + q$ + "arr" + q$ + ":[{" + q$ + "k" + q$ + ":" + q$ + "v" + q$ + "}]}"
LONGSTRING CLEAR a%()
LONGSTRING APPEND a%(), s$
PRINT "name="; JSON$(a%(), "name")
PRINT "v0="; JSON$(a%(), "vals[0]")
PRINT "v1="; JSON$(a%(), "vals[1]")
PRINT "v2="; JSON$(a%(), "vals[2]")
PRINT "x="; JSON$(a%(), "obj.x")
PRINT "k="; JSON$(a%(), "arr[0].k")
