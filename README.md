# os-latency-tests
Command line application that measures the OS context switch latencies from the userspace.
This application is used to compare latency and throughput of different kernels 
(normal, low-latency, real-time) available in various distributions like Ubuntu and/or 
the impact of CONFIG_PREEMPT_RT and other kernel configs on various embedded boards.
The code in written in modern C++ so at least C++ 11 is required.

More details:
============

Calling kernel's I/O functions (syscalls) from user-space cause the scheduler to suspend the current
task until the I/O event occurs. The time from I/O event happens and the task waiting for it is resumed is
kernel task resume latency and has some impact in real-time and low-latency appliances. 
It is recomended to be measured on each platform where the requirements are tight.
For realtime applications, the distribution is important, the absolute maximumm ( for hard realtime ) has to be assesed,
for soft-realtime applications, the 95% percentile or so is to be taken in account.

For example: 

read()        -> blocking read will suspend the current task until the device ( file, socket ) has something to return.
mutex.lock()  -> will suspend the current thread until other thread will unlock it.
thread.join() -> will suspend the current thread until the other thread finishes.
sleep()       -> will suspend the current thread until the time passes.

The goal of this code is measure the thread resume latency from user space.

Approach:

Four time points are recorded in each loop:

1. Just before creating another thread, in the main thread.
2. Right after the thread is entered, in the secondary thread.
3. Just before the second thread is about to exit, in the secondary thread.
4. Right after thread join, in the main thread.


Thread creation latency = duration from 1. to 2.
Worker thread work load duration from 2. to 3. - depending on test, see below.
Thread join latency = duration from 3. to 4.

For the thread creation / join latency, the workload is none, so there is nothing done between 2. and 3.

For other tests, like various syscalls, the test is done like this:

Mutex Resume Latency:
====================

mutex created in the main thread and locked once
worke thread that unlocks the same mutex started
mutex locked one more time in the main thread, waiting for the worker thread to unlock it.

Mutex resume latency duration = time from time before unlock in the worker thread to time the second lock exits.


Condition variable wait() resume latency
========================================


Others
======


Network latency ( lo interface ):
- send UDP packet in one thread, receive in another  
- same for TCP?
- 
Named pipe latency:
-
-
-

Other
-----

Malloc latency:

- measure malloc / free duration for various block size 

Memory latency after fragmentation:
- same approach as before but after intensive allocs/free of bigger and smaller blocks to cause fragmentation


At the end of the day, you'll get a picture of sources of latency in real-time applications.


Command line options:
--------------------

-a [single|multiple] - sets the affinity of secondary thread on the same core ( single ) or on different cores ( if available, multiple )
-t [test name1] -t [test name2] ... -t[test_nameN]
   - test_name1, ..., test_nameN - names of the tests to be executed, default is all
