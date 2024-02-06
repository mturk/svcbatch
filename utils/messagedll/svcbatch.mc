;/**
; * mc.exe -z messages svcbatch.mc
; *
; * Rename messages_MSG00001.bin to svcbatch.bin
; * Remove messages.h and messages.rc
; *
;*/

MessageId=2301
SymbolicName=SVCBATCH_EVENTLOG_MSG_1
Language=English
%1
.

MessageId=2302
SymbolicName=SVCBATCH_EVENTLOG_MSG_2
Language=English
%1%2
.

MessageId=2303
SymbolicName=SVCBATCH_EVENTLOG_MSG_3
Language=English
%1%2%3
.
