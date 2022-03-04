# SvcBatch CHANGES

This is a high-level summary of the most important changes.
For a full list of changes, see the [git commit log][log]

  [log]: https://github.com/mturk/svcbatch/commits/


## v1.1.0

 * In development
 * Simplify alloc functions
 * Improve consolemode so it can be used from service
 * Increase SVC_MAX_LOGS to 9
 * Use RunBatch.log for Console mode
 * Remove _RUN_API_TEST
 * Add support to execute batch file using SVCBATCH_CTRL_EXEC signal

## v1.0.6

 * Fix log rotation timeout processing
 * Improve _DGVIEW_SAVE thread safety
 * Report error to Service Manager if createiopipes fails
 * Log SvcBatch features defined at install time
 * Add interactive mode
 * Added support for service pause/continue

## v1.0.5

 * Improve release creation procedure
 * Drop NULL DACL and use default security descriptor associated with the access token of the calling process

## v1.0.4

 * Add support for log file auto rotation
 * Rotate logs if service is running more then 30 days and auto rotation is not defined at create time
 * Add support for SvcBatch.dbg if `_DBGSAVE=1` is defined at build time
 * Add support for vendor version suffix
 * Update examples and documentation
 * Fix issues with corrupt log files due to thread synchronization

## v1.0.3

 * No need to report SERVICE_CONTROL_INTERROGATE
 * Use Job to close child processes instead tlhelp32 API


## v1.0.2

 * New release
 * Drop make install

## v1.0.1

 * Use static msvcrt for release builds

## v1.0.0

 * First release
 * Cleanup code

## v0.9.9

 * Cleanup code
 * Add PowerShell examples

## v0.9.7

 * Drop entire ABI concept
 * Add SVCBATCH_SERVICE_LOGDIR variable
 * Drop console shutdown signals

## v0.9.6

 * Restore original SvcBach.log if rotation fails

## v0.9.5

 * Increased the number of log files to 4
 * Switched ABI to 20201209
 * Dropped support for exitonerror
 * Cleaned-up code and removed unused and duplicate stuff
 * Updated documentation and added more examples

## v0.9.4

 * Dropped support for parent shutdown signal
 * Cleaned-up code inconsistency

## v0.9.3

 * Added support to send shutdown signal from batch files to parent
 * Sending CTRL_BREAK must now be explicitly enabled with /b switch

## v0.9.2

 * Kill entire child process tree even if child already terminated

## v0.9.1-dev

 * Initial release
