## Simple piped log example for SvcBatch

This is basic example that reads from
standard input and writes to a file defined
as first argument to the program.

If `SVCBATCH_SERVICE_MODE` is set to `1`,
the example creates a thread that waits on
`Local\SvcBatch-Dorotate-%SVCBATCH_SERVICE_UUID%` event
simulating our custom user service rotate control.
