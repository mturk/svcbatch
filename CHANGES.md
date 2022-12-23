# SvcBatch CHANGES

This is a high-level summary of the most important changes.
For a full list of changes, see the [git commit log][log]

  [log]: https://github.com/mturk/svcbatch/commits/


## v1.5.0

  * In development
  * Drop external log application for shutdown batch file
  * Change default log rotation extension
  * Do not rotate shutdown log files
  * Do not rotate each 90 days

### New Features

 * Add -t command option that will truncate log file
 * Use -m command option to replace previous -r0...9
 * Add SVCBATCH_SERVICE_ROOT environment variable
 * Add support for log rotation at each full hour
 * Add strftime support for log file names

## v1.4.0

 * Terminate external log application if it fails to exit within 5 seconds after closing log files
 * Use service batch file as shutdown batch file if -a was defined and -s was not
 * Drop experimental support for user rotate service control for external log applications
 * Drop SVCBATCH_SERVICE_MODE environment variable
 * Drop SVCBATCH_SERVICE_LOGDIR environment variable
 * Stop service if write to logfile fails

## v1.3.4

 * Remove automatic log flush
 * Use high-resolution timer for internal logging
 * Fix issues with reporting errors to Windows Event Log
 * Fix quoting arguments for batch files

### New Features

 * Add -a command option that enables to provide arguments to shutdown batch file


## v1.3.3

 * Not released


## v1.3.2

 * Report to system Event Log if external log application fails at run time

### New Features

 * Enable external log applications to handle user rotate service control
 * Add -l command option to use local instead system time for log rotation and messages


## v1.3.1

 * Do not start rotate thread if log rotation is disabled
 * Kill service if log file is closed at runtime

### New Features

 * Add -e command option that enables to start external log application
 * Add -n command option that enables to change default log file name prefix
 * Add SVCBATCH_SERVICE_MODE environment variable

## v1.3.0

 * Fix SvcBatch.log flushing
 * Correctly report exit codes from child processes

### New Features

 * Send additional arguments to batch file if defined at install time
 * Add full support for building with msys2 or cygwin environment
 * Add -q command option for disabling internal status logging

## v1.2.2

 * Disable log rotation if `-r 0` is defined at install time
 * Create rotated log names using current system time instead last write time
 * Fix service stop when shutdown script is unresponsive
 * Create svcbatch.h from svcbatch.h.in at build time
 * Create svcbatch.manifest from svcbatch.manifest.in at build time
 * Add more advanced command line option parser

## v1.2.1

 * Fix dead services waiting on `Terminate batch job (Y/N)?`
 * Add -d command option for debug tracing
 * Fix propagating CTRL_C_EVENT from shutdown process
 * Use the same rotation rules for shutdown log
 * Add rule option to define SVC_MAX_LOGS at runtime
 * Add option to disable log rotation

## v1.2.0

 * Drop experimental RunBatch feature
 * Allow relative paths for SVCBATCH_SERVICE_HOME
 * Make sure that SVCBATCH_SERVICE_BASE is set to service batch file directory.
 * In case both batch file and working directory are relative paths use svcbatch.exe as home.

## v1.1.1

 * Not released
 * Use -s option for SvcBatchEnd batch file
 * Remove -c (clean path) support
 * Ensure that SvcBatchRun and SvcBatchEnd processes are closed in timely manner
 * Use Job Objects for SvcBatchRun and SvcBatchEnd

## v1.1.0

 * Add RunBatch feature
 * Add feature to run batch file on service stop.
 * Add Service preshutdown option
 * Increase SVC_MAX_LOGS to 9
 * Drop interactive mode
 * Remove _RUN_API_TEST
 * Remove _DBGVIEW_SAVE
 * Remove experimental support for service pause/continue

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
