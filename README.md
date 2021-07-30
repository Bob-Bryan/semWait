Thu Jul 29 23:03:18 EDT 2021

We had some big shell scripts that needed to coordinate access to other processes.
This is an attempt to provide full access to SystemV semaphores to the command line.
There is a python callable version also.

We had some annoying requirements.  We had a set of ksh processes and we needed to limit
the number that were working on a resource to a max of N (we mostly used four) at a time.
The bad news is, once in a while there was a Python process that had to take over
and not let anybody else with that resource for the duration.  It did not take long but
it had to have exclusive access.  After investigating endless possible ways of doing
the exclusion and limiting, I arrived at a Unix System V semaphore version that used
these routines.

I wrote them on the clock but there is nothing about them that is propriety.

If nothing else, they can show how to do System V semaphores from C and Python.

There are other Python semaphore interfaces out there but this does what I need.
The others seem to be complicated for the sake of complexity.  Maybe that adds functionality
but this is as simple as I can make it which is my style.


Regards,
/Bob Bryan
