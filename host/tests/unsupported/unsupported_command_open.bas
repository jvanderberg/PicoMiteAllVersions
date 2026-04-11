' EXPECT_ERROR: Unsupported VM command: Open
OPEN "vm_unsupported.tmp" FOR OUTPUT AS #1
PRINT #1, "should not reach interpreter file IO"
CLOSE #1
