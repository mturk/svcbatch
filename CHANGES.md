# SvcBatch CHANGES

This is a high-level summary of the most important changes.
For a full list of changes, see the [git commit log][log]

  [log]: https://github.com/mturk/svcbatch/commits/

## v0.9.5


## v0.9.4

 * Drop support for parent shutdown signal
 * Cleanup code inconsistency

## v0.9.3

 * Add support to send shutdown signal from batch files to parent
 * Sending CTRL_BREAK must now be explicitly enabled with /b switch

## v0.9.2

 * Kill entire child process tree even if child already terminated

## v0.9.1-dev

 * Initial release
