# SvcBatch CHANGES

This is a high-level summary of the most important changes.
For a full list of changes, see the [git commit log][log]

  [log]: https://github.com/mturk/svcbatch/commits/


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
