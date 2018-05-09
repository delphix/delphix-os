<!---
CDDL HEADER START

This file and its contents are supplied under the terms of the
Common Development and Distribution License ("CDDL"), version 1.0.
You may only use this file in accordance with the terms of version
1.0 of the CDDL.

A full copy of the text of the CDDL should have accompanied this
source.  A copy of the CDDL is also available via the Internet at
http://www.illumos.org/license/CDDL.

CDDL HEADER END

Copyright (c) 2017 by Delphix. All rights reserved.
-->

Delphix OS
==========

This is the Delphix operating system, and it is maintained by the Systems Platform team. This document describes the tools and processes Delphix employees use to modify this codebase.

Development
-----------

To get started developing Delphix OS, you should create a local clone of the `git-utils` repository and add the `bin` subdirectory to your shell's `PATH`. This will give you access to our automated build tools:

- `git zfs-make`: Running this from your OS repository will give you a quick build of some commonly-modified parts of the OS (including, but not limited to, ZFS). It usually takes about 15 minutes the first time you run it on a new branch, and then about 2 minutes for subsequent builds. You can also use it to run our various linters / checkers with the `-l`, `-n`, and `-p` options.
- `git zfs-load`: This takes build artifacts created by a previous `git zfs-make` and loads them into a VM that you specify. This will also reboot the machine you load the bits onto so that kernel modifications will take effect (unless you specify `-n`).
- `git build-os`: This command runs a full, clean build of the OS through our Jenkins automation server and writes the output to a location of your choosing, specified through the `--archive-host` and `--archive-dir` parameters. This generally takes around 45 minutes to run. You will need your own `jenkins`-accessible directory on a build server to dump the resulting files into, which you can create by running `/usr/local/bin/create_data_dir $USER` on the most recent build server and then making a subdirectory which is `chown`ed to the `jenkins` user.

The general rule for C style is to follow the style of whatever code you're modifying and to use C99 features whenever possible, but ideally all code we touch would adhere to the *C Style and Coding Standards for SunOS*, which is stored at `tools/cstyle.pdf` in this repository. Try to follow that unless the code you're working in uses drastically different style or you have a good reason not to (like this code was ported from another OS and you want to reduce future merge pain by keeping the style the same as the original).

We also have a few automated testing tools:

- `git zfs-test`: If you have a VM that's been set up using `git zfs-load` and you want to kick off an automated test run, this will start a job asynchronously on our Jenkins server. You can use it for both `zloop` and `zfstest` runs (see **Testing** below for more details on these).
- `git zfs-perf-test`: This runs the performance suite from `zfstest` on dedicated hardware to help you get a reliable measure of how your changes impact ZFS performance. These tests generate artificial load through `fio` so none of them are a substitute for real-world testing, but they are a good set of benchmarks to start with.
- `git zfs-precommit`: This is a way to kick off the equivalent of a `git build-os` followed by a `git zfs-test`. For most bugs you should have a passing version of this before you post your code for review (unless you explicitly label the review as a work in progress).

All of the commands above have `-h` options that you can pass to them to learn more about their capabilities.

Here are some additional resources for learning about the implementation of a specific subsystem:

- Many ZFS features are documented through links on the [OpenZFS website](http://www.open-zfs.org/wiki/Developer_resources)
- For device drivers check out the illumos [Writing Device Drivers](https://illumos.org/books/wdd/) guide
- Many other subsystems are described in detail in the [Solaris Internals](https://www.amazon.com/Solaris-Internals-OpenSolaris-Architecture-paperback/dp/0134185978) book

Testing
-------

### `zfstest`

The `zfstest` suite is a set of `ksh` scripts that were created to exercise as much user facing functionality in ZFS as possible. As such, there are *many* test cases, and it takes several hours to run all of them. The set of tests that will be run is specified by a runfile, and the one used by all of our tools is `usr/src/test/zfs-tests/runfiles/delphix.run`. You can create your own runfiles and run `zfstest` on them manually using the command `/opt/zfs-tests/bin/zfstest -a -c custom.run`.

There are a few known failures in the default set of tests, and we keep track of these in our bug tracker by filing a bug for the problem and adding "labels" with the names of the tests that fail as a result of the bug. If you run `zfstest` through Jenkins, the Jenkins job will look at the set of failed tests and only mark the run as failed if there are some unknown ones (that your code probably introduced). Occasionally you may need to search the bug tracker manually for a failed test to see if a bug was recently closed and you haven't pulled in the fix yet, etc.

### `ztest`

`ztest` is a userland program that runs many `zfs`- and `zpool`-like operations randomly and in parallel, providing a kind of stress test. It compiles against a userland library called `libzpool` that includes most of ZFS's kernel implementation, and it uses `libfakekernel` to replace the parts of the kernel that ZFS depends on.

There are several big reasons to use (and add new tests to) `ztest`:

- if you're modifying a really important part of the system, you can use `ztest` to test your changes without fear of making your VM inaccessible
- you can access functionality which isn't available to the `zfstest` suite because you're calling into the implementation directly
- `ztest` exercises data races and other interactions between features in a way that is hard to mimic manually, or through shell commands in a `zfstest` test

However, it has a couple of limitations, too:

- dependencies outside of ZFS are mocked out, so you don't get their real behaviors
- the ZFS POSIX Layer (ZPL) currently isn't included in `ztest`
- the logic for `zfs send` and `zfs receive` currently isn't included in `ztest`

#### `zloop`

`zloop` is a convenient wrapper around `ztest` that allows you to run it for a certain amount of time and switch between pool configurations every few minutes. It also will continue running even if an individual `ztest` run fails, which is nice for kicking a lot of testing off at once that you can revisit the results of in bulk later on.

Integration Process
-------------------

When your code is passing tests, you can use the `git review` tool to post the diff to our peer review tool. It will not be publicly visible until you hit the "Publish" button. Before you do so, there are a couple of requirements:

- add a good description of the bug, your fix for it, and any notes to make reviewing your code easier
- add the relevant reviewers (specific people, or larger groups like "zfs")
- add a link to a passing `git zfs-precommit` Jenkins job in the "Testing" section (or a failing one along with an explanation of why the failures are unrelated to your changes)

When you're ready, publish the review. If you don't get any comments within a couple of days, it may be worth pinging your reviewers directly so they know you need their help. You will need at least two reviewers to push, and one of those must be a gatekeeper.

When you have all your reviews, you're ready to push. We have some custom git hooks that will make sure your commit message and code changes follow our internal guidelines, and that the bug you're fixing has all the necessary fields filled out. If there's an issue, `git push` will emit a message describing the problem. If you're getting a warning that you would like to ignore, you can run `git push` a second time to bypass it, but errors must be resolved before pushing.

### Backporting

When a bug affects previous releases of the Delphix Engine, we may want to apply the same fix to a previous release. This involves TODO

Open Source
-----------

Delphix OS is open source, but to make our code more useful to the community we push nearly all our bug fixes and (at a slightly slower cadence) features to the repository we're based on, [illumos](https://github.com/illumos/illumos-gate). We use [this document](https://docs.google.com/document/d/1fUIOtDvVvyA87L8QaaCMpAQaC_a5P51Sbs-5906Pv_Y) to track our upstreaming activity. The things we don't upstream are:

- features that haven't shipped yet (to ensure their stability)
- *[rare]* features that aren't general enough to be useful outside of Delphix
- *[rare]* integrations with infrastructure that's only available inside Delphix
- *[rare]* bug fixes which are too hacky or Delphix-specific to upstream

We also pull in changes from upstream, and we call this activity "doing an illumos sync". Although we are some of the primary contributers to ZFS and the paravirtual device drivers in illumos, other members of the community provide additional features and bug fixes to many other parts of the system that we rely on, so this is a very valuable activity for us. To stay informed about what's going on in the broader illumos community, you can sign up for the [developer mailing list](http://wiki.illumos.org/display/illumos/illumos+Mailing+Lists).

Debugging
---------

The OS has its own set of debugging tools. We don't have a source-level debugger like `gdb`, so if that's what you're used to there'll be a slight learning curve.

### DTrace

DTrace is a framework that allows you to run simple callbacks when various events happen, which allows you to programmatically debug what a program or the kernel is doing. The most useful events tend to be standard probe points that have been purposely placed into the kernel, the entry or exit of kernel functions, and a timer event that fires *N* times a second. There are many good resources on DTrace:

- [DTrace Bootcamp](http://dtrace.org/resources/ahl/dtrace_course.2005.8.18.pdf) by Adam Leventhal
- [DTrace by Example](http://www.oracle.com/technetwork/server-storage/solaris/dtrace-tutorial-142317.html) by Ricky Weisner
- [Summary page with 1-liners](http://www.brendangregg.com/dtrace.html) by Brendan Gregg
- The [illumos DTrace guide](http://dtrace.org/guide/chp-intro.html)
- [Delphix enhancements to DTrace](https://docs.google.com/presentation/d/1sRJTlZD6wt937Nn2wWy0010BbvzJ3WOm4tIVLOgrrnM) by Matt Ahrens
- Many, many undocumented ZFS-related scripts in Matt's home directory

Because all the interesting stuff in `ztest` happens in subprocesses, running `dtrace` against it can be tricky. You should copy the format of `~mahrens/follow.d` to invoke your own `child.d` script on every subprocess that gets spawned from the main `ztest` instance.

### `mdb` and `kmdb`

The program `mdb` allows you to debug a userland core file or kernel crash dump after a failure. Core and dump files are configured using `coreadm` / `dumpadm` to be written to `/var/crash/`. To debug a core file you can run MDB directly on the file like `mdb /var/crash/core.xyz.1234`, and for crash dumps you first expand them using `sudo savecore -vf vmdump.0` and then run `mdb 0` to start debugging. `mdb -p` and `mdb -k` allow you to connect to a running process's / kernel's address space in a similar way.

`kmdb` is similar to `mdb -k`, but it additionally allows you to do live kernel debugging with breakpoints. To use `kmdb`, you must enable it with one of these methods:

- Hit space / escape during boot when the illumos boot menu is displayed on the console, navigate to the `Configure Boot Options` screen, and turn the `kmdb` option on. (Note that you may have to use Ctrl-H instead of backspace to get back to the main menu to continue booting. Also, this method does not persist across reboots.)
- Create a file in `/boot/conf.d/` which has the line `boot_kmdb=YES`.

After you've enabled `kmdb` and rebooted, you can drop into the debugger in a few different ways:

- To drop in as soon as possible during boot (to debug a boot failure, etc.) turn the bootloader option for `Debug` on, or add another line to the bootloader's config file with `boot_debug=YES`.
- To drop in at a random time of your choosing, run `sudo mdb -K` from the console after the machine is booted.
- To drop in during a non-maskable interrupt (especially useful for debugging hangs), add `set apic_kmdb_on_nmi=1` to your `/etc/system` file (and remove the line which sets `apic_panic_on_nmi` if it's present) and reboot. You can generate NMIs on demand through whatever hypervisor you're using.

You can only issue `kmdb` commands via the console (i.e. not over SSH), so make sure you have access to the console before you try to drop into it!

There are many good guides introducing you to the basics of MDB commands:

- [The Modular Debugger](http://www.solarisinternals.com/si/reading/chpt_mdb_os.pdf), a chapter from *Solaris Internals*
- [Diagnosing kernel hangs/panics with kmdb and moddebug](https://blogs.oracle.com/dmick/entry/diagnosing_kernel_hangs_panics_with) by Dan Mick
- [Solaris Core Analysis, Part 1: mdb](http://cuddletech.com/?p=436) by Ben Rockwood
- [An MDB reference](http://www.solarisinternals.com/si/tools/mdb/index.php) by Jonathan Adams
- GDB to MDB, [Part 1](https://blogs.oracle.com/eschrock/entry/gdb_to_mdb) and [Part 2](https://blogs.oracle.com/eschrock/entry/gdb_to_mdb_migration_part) by Eric Schrock

A lot of the most useful parts of MDB are commands (known as `dcmd`s in MDB-speak) that have been written in concert with some kernel feature to help you visualize what's going on. For instance, there are many custom `dcmd`s you can use to look at the in memory state of ZFS, the set of interrupts that are available to you, etc. MDB also uses the idea of pipelines (like from `bash`), so you can pipe the results of one command to another. There's also a special kind of command called a "walker" that can generate many outputs, and these are frequently used as inputs to pipes. For a mildly contrived example, if you want to pretty-print every `arc_buf_t` object in the kernel's memory space, you could do `::walk arc_buf_t | ::print arc_buf_t`. You can even pipe out to actual shell commands using `!` instead of `|` as the pipeline character if you want to use `grep` or `awk` to do a little postprocessing of your data (although this is not available from `kmdb`).

Finally, to enable some extremely useful `dcmd`s, turn on `kmem` debugging by putting `set kmem_flags=0xf` in `/etc/system` and then rebooting. After doing this, the command `::whatis` can tell you the stack trace where every buffer was allocated or freed, and `::findleaks` can be used to search the heap for memory leaks.

Here are some of the most commonly useful `mdb` commands:
```
# description of how this core file was created
::status
# print assertion failures, system logs, etc.
::msgbuf
# print panicstr (last resort if ::status / ::msgbuf don't work)
*panicstr/s
# backtrace of current thread
::stack
# print register state
::regs
# print contents of register rax (can also do "<rax::print <type>")
<rax=Z
# allocation backtrace for a buffer located at 0x1234
0x1234::whatis
```

Here are a few to help you learn about new features of MDB:
```
# show the formats you can use with the "<addr>/<format>" syntax
::formats
# print out all dcmds with descriptions
::dcmds
# print out all walkers
::walkers
# get information about a dcmd "::foo"
::help foo
```

Here are some OS-specific `dcmd`s:
```
# view interrupt table
::interrupts
# similar to "prtconf", prints device tree
::prtconf
# similar to "zpool status"
::spa -v
# prints out all stacks with >= 1 frame in the zfs kernel module
::stacks -m zfs
# prints human-readable block pointer located at memory address 0x1234
0x1234::blkptr
# see status of all active zios
::zio_state -r
# see the debug messages ZFS has logged recently
::zfs_dbgmsg
# look at SCSI state
::walk sd_state | ::sd_state
```

Here are some commands we use commonly to debug kernel and user out-of-memory issues:
```
# look at Java threads in kernel dump
::pgrep java | ::walk thread | ::findstack -v
# get Java gcore from kernel dump (although often it is incomplete)
::pgrep java | ::gcore
# high-level view of kernel memory use
::memstat
# allocation statistics for kmem / umem (depending on if we're in kernel or not)
::kmastat / ::umastat
# allocation backtraces for largest kmem / umem allocations
::kmausers / ::umausers
# sum up anonymous memory used by all Java processes
::pgrep java | ::pmap -q ! grep anon | awk 'BEGIN{sum=0} {sum+=$3} END{print(sum)}'
```

Here are the miscellaneous useful ones you may want one day:
```
# expand terminal width to 1024 columns
1024 $w
# ignore all asserts from here onwards
aok/w 1
```

### `zdb`

`zdb` is a ZFS-specific tool which is useful for inspecting logical on-disk structures. When you add a new on-disk feature to ZFS, you should augment `zdb` to print the format intelligently to help you debug issues later on. `zdb` can also be used to verify the on-disk format of a pool, and it is used from `ztest` for this reason at the end of each run.

### `zinject`

`zinject` is another ZFS-specific tool which is useful for simulating slow or failing disks. You can use it to write corruptions into specific objects or device labels, add artificial I/O latency for performance testing, or even pause all I/Os.

### Early boot debugging

First, if the thing you're working on is part of ZFS, you may be able to test it using `ztest` (see above) so that you don't have to deal with early boot debugging.

If that's not possible and you're working on something that you know is likely to cause boot problems when you mess up, it's a good idea to preemptively add the lines `boot_{kmdb,verbose,debug}=YES` to a file in `/boot/conf.d/` and `set apic_kmdb_on_nmi=1` in `/etc/system` as described above. This will allow you to set breakpoints immediately at boot, and will drop into `kmdb` if there is a panic or an NMI. For more information about how to configure the bootloader, see [this guide](https://docs.google.com/document/d/1FxZKpymWf5EnR9eohH-B3EesvyNZUIkzu9Yo9sKWm40).

A common hurdle when setting `kmdb` breakpoints during boot is that the module you're trying to debug hasn't been loaded yet, so doing `function_to_debug:b` doesn't work. The easiest way to work around this is to scope your breakpoint location by doing ``::bp module_name`function_to_debug``, so that `kmdb` will automatically set the breakpoint when the module is loaded (assuming you spelled the module and function name correctly).

If you forget to set up `kmdb` and your kernel panics during (or shortly after) boot three times in a row, you will be rebooted into the Delphix OS recovery environment. This is a stripped-down version of Delphix OS which allows you to SSH into the VM as `root`, run diagnostics, and (maybe) fix things up. You can find more information about the recovery environment [here](https://docs.google.com/document/d/1J_JQTHirXaXdzGBoaIDjQX9EFzBByhMpR3Ufx88w3hM).
