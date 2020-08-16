# testrunner

Given an input set of tests to run, **testrunner** runs the tests sequentially
on either the local host machine or a remote Fuchsia device. It then emits test
results in a structured format for processing by infrastructure and other tools.

testrunner is generally executed by
[botanist](https://fuchsia.googlesource.com/fuchsia/+/HEAD/tools/botanist) in
a Swarming task launched by an infra recipe. When the Swarming task is
complete, the recipe downloads testrunner's output files from the task's
Isolated outputs and parses the test results to determine which tests passed
or failed, and to determine the location of tests' log files within the
testrunner outputs.

## Input

testrunner takes a single positional argument, which is the path to a JSON file
containing the list of tests to run. The entries in this file must conform to
the `testsharder.Test` schema.

This file generally corresponds to a single shard generated by the
[testsharder](https://fuchsia.googlesource.com/fuchsia/+/HEAD/tools/integration/testsharder)
tool.

## Output

After it has finished running all tests, testrunner writes the results to a
`summary.json` file that conforms to the `runtests.TestSummary` schema.
testrunner places this file in the root of the `-out-dir` directory, which in
turn is in the directory specified by the `$FUCHSIA_TEST_OUTDIR` environment
variable. When executed in a Swarming task by recipes, `$FUCHSIA_TEST_OUTDIR`
corresponds to the Swarming task's Isolated output directory, which will be
automatically uploaded to Isolate upon completion of the task.

For each test, testrunner also captures stdout and stderr and writes those logs
to another file in the output directory. Each test's stdout/stderr file is
identified by the `output_file` field in its `summary.json` entry.

## Test execution modes

testrunner decides how to run each test primarily based on the test's `os`
field, which specifies the operating system that the test was built for.

### Subprocess

If the test's target operating system corresponds to the operating system that
testrunner is running on, testrunner will run the test as a subprocess. Since
testrunner always runs on a Mac or Linux host, this only applies to host-side
tests.

If the test's `command` field is set, testrunner will execute that exact
command. Otherwise, it will run the executable specified by the `path` field.

### SSH

If the test's target operating system is Fuchsia and the `$FUCHSIA_SSH_KEY`
environment variable is set, testrunner will use that key to connect over SSH
to the device with the hostname specified by the `$FUCHSIA_NODENAME`
environment variable. (These environment variables are generally set by
botanist when it invokes testrunner as a subprocess.)

testrunner executes most Fuchsia tests by running
`run-test-suite <package_url>` or `run-test-component <package_url>`,
depending on the format of the test's `package_url` field.

### Serial

If `$FUCHSIA_SSH_KEY` is not set, testrunner falls back to running tests via the
target device's serial port, using the socket specified by the
`$FUCHSIA_SERIAL_SOCKET` environment variable.

This codepath applies primarily to tests built to run in the bringup product,
which includes minimal networking capabilities, so it's not possible to run
bringup tests over SSH.