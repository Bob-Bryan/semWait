#!/usr/bin/env python
# -*- coding: utf-8 -*-
# vim: ts=4 sw=4 ai expandtab
# This will provide a python routine
# that will emulate the C semWait.c program
# from Python
# Fri May 10 12:37:24 EDT 2019
#
"""
This is a routine that will wait for or set a semaphore
   create a semaphore (if it does not exist) with a default number
   set    a semaphore to a higher number (giving back, non-blocking)
   set    a semaphore to a lower number (taking from, possible blocking)
   delete a semaphore for some reason, presumably to reinit everything

This is a little different from the C version.  If the -P (--permanent) option is not specified,
all changes are reverted on program exit.

"""
import argparse
import time
import ctypes
import ctypes.util
import errno
import os
import sys
from datetime import datetime

NUM_SEM=128 # max value from  fourth field in /proc/sys/kernel/sem
SEM_STAT = 18
SEM_INFO = 19

IPC_CREAT=0o01000    # Create key if key does not exist. 
IPC_RMID=0           # remove resource 
IPC_SET= 1           # set ipc_perm options
IPC_STAT=2           # get ipc_perm options
IPC_INFO=3           # see ipcs

#  semop flags
SEM_UNDO=0x1000      # undo the operation on exit

#  semctl Command Definitions. 
GETPID =11           # get sempid
GETVAL =12           # get semval
GETALL =13           # get all semval's
GETNCNT=14           # get semncnt
GETZCNT=15           # get semzcnt
SETVAL =16           # set semval
SETALL =17           # set all semval's

verbose=3
permanent=False
ignoreSemError=False

# While I don't much care for these complicated structures
# these appear to be what is needed to access the libc version of the sem operations
class struct_ipc_perm(ctypes.Structure):
    _fields_ = [
        ('key',        ctypes.c_int32),   # Key supplied to semget
        ('uid',        ctypes.c_uint32),  # Effective UID of owner
        ('gid',        ctypes.c_uint32),  # Effective GID of owner
        ('cuid',       ctypes.c_uint32),  # Effective UID of creator
        ('cgid',       ctypes.c_uint32),  # Effective GID of creator
        ('mode',       ctypes.c_uint16),  # Permissions + SHM_DEST and SHM_LOCKED flags
        ('_pad1',      ctypes.c_uint16),  
        ('_seq',       ctypes.c_uint16),  # Sequence number
        ('_pad2',      ctypes.c_uint16),
        ('_reserved1', ctypes.c_ulong),
        ('_reserved2', ctypes.c_ulong)]

class struct_sembuf(ctypes.Structure):
    _fields_ = [
        ('sem_num',    ctypes.c_uint16),  # semaphore number
        ('sem_op',     ctypes.c_int16),   # semaphore operation
        ('sem_flg',    ctypes.c_int16)]   # operation flags */

class struct_timespec(ctypes.Structure):
    _fields_ = [
        ("tv_sec",     ctypes.c_int),     # Seconds
        ("tv_nsec",    ctypes.c_int)]     # Nanoseconds.


class struct_semid_ds(ctypes.Structure):
    _fields_ = [
        ('sem_perm',   struct_ipc_perm),  # ownership and permissions
        ('sem_otime',  ctypes.c_uint64),  # last semop time
        ('_reserved1', ctypes.c_ulong),
        ('sem_ctime',  ctypes.c_uint64),  # last change time by semctl
        ('_reserved2', ctypes.c_ulong),
        ('sem_nsems',  ctypes.c_ulong),   # number of semaphores in set
        ('_reserved3', ctypes.c_ulong),
        ('_reserved4', ctypes.c_ulong)]

class struct_seminfo(ctypes.Structure):
    _fields_ = [
        ('semmap',    ctypes.c_uint32),   # Number of entries in semaphore map; unused within kernel
        ('semmni',    ctypes.c_uint32),   # Maximum number of semaphore sets
        ('semmns',    ctypes.c_uint32),   # Maximum number of semaphores in all semaphore sets
        ('semmnu',    ctypes.c_uint32),   # System-wide maximum number of undo structures; unused within kernel
        ('semmsl',    ctypes.c_uint32),   # Maximum number of semaphores in a set
        ('semopm',    ctypes.c_uint32),   # Maximum number of operations for semop(2)
        ('semume',    ctypes.c_uint32),   # Maximum number of undo entries per process; unused within kernel
        ('semusz',    ctypes.c_uint32),   # Size of struct sem_undo
        ('semvmx',    ctypes.c_uint32),   # Maximum semaphore value
        ('semaem',    ctypes.c_uint32)]   # Max. value that can be recorded for semaphore adjustment (SEM_UNDO)

class struct_semun(ctypes.Union):
    _fields_ = [ 
        ('val',        ctypes.c_uint32),                # Value for SETVAL
        ('buf',        ctypes.POINTER(struct_semid_ds)),# Buffer for IPC_STAT, IPC_SET
        ('array',      ctypes.POINTER(ctypes.c_uint16)),# Array for GETALL, SETALL
        ('__buf',      ctypes.POINTER(struct_seminfo))] # Buffer for IPC_INFO (linux specific)

def ErrnoResult(value):
    """ctypes validator for function which returns -1 and set errno on error"""
    if value == -1:
        errnum = ctypes.get_errno()
        raise OSError(errnum, os.strerror(errnum))
    return value 

#    gobbleUp:
#      If we need N semaphores but only N-2 are available, we can't just wait
#      because others coming in, asking for 1 will get the 1 while we are still waiting for the N
#      The solution is to gobble up all the semaphores that are available
#      and keep coming back, taking any that become available until we get what we want
#      or we time out.
def sem_gobbleUp(key,index,increment,waitSec):
    print("Starting gobble process: increment"+str(increment)+"  index="+str(index))
    if increment > 0:
        ret=sem_set(key,index,increment,waitSec)
    else:
        endTime=time.time()+waitSec
        for ii in range(abs(increment)):
            if verbose>0:
                now = datetime.now()
                print(now.strftime("%Y-%m-%d %H:%M:%S")+" gobbling: index="+str(index)+"  count="+str(ii))
            ret=sem_set(key,index,-1,int(endTime-time.time()))
            if ret!=0:
                break
    if verbose>1:
        print("Gobble ret="+str(ret))
    return ret
            

    
#
# This will set the value up or down
# going up is easy, it always happens.
# going down, not so much, we can block if we need to get more
# that are out there or timeout trying.
#
# The big difference from this and the C version is 
# the use of the SEM_UNDO flag.  When we call the semWait.c program
# from the command line, we know and expect the program will exit with
# at status code of the request.  This would be of limited value if undo when we exited.
# In Python, however, we expect the application will request the semaphore and if it exits,
# the semaphore should be released so we include the SEM_UNDO flag in this Python version.
# The default is to include the SEM_UNDO but we can turn that off with the --permanent flag
#
def sem_set(key,index, increment,waitSec):
    flags = 0o0740
    if verbose > 1:
        print("Calling sem_set with: key="+hex(key)+" index="+str(index)+" incr="+str(increment)+" wait="+str(waitSec))
    try:
        semId=libc.semget(key,1, flags)
    except OSError as ose:
        print("semget gives error: "+str(ose))
        raise

    waitStruct=struct_timespec()
    waitStruct.tv_sec=waitSec
    waitStruct.tv_nsec=0

    opBuf=struct_sembuf()
    opBuf.sem_num=index
    opBuf.sem_op=increment
    if not permanent:
        opBuf.sem_flg=0x1000   # SEM_UNDO
    
    try:
        ret=libc.semtimedop(semId,ctypes.pointer(opBuf),1,ctypes.pointer(waitStruct))
    except OSError as ose:
        if ose.errno == errno.EFBIG and ignoreSemError:
            if verbose > 1:
                print("Semaphore index error, ignoring")
            ret=0
        elif ose.errno != errno.EAGAIN:  # An operation could not proceed time limit specified in timeout expired.
            print("#1 semtimedop gives error:"+str(ose))
            exit(3)
        else:
            ret=errno.EAGAIN

    if verbose > 1:
        print("SET ret="+str(ret)+" increment="+str(increment))
    return ret 

#
# Create the semaphore with the count at the beginning.
# We have a single key and we need a number of semaphores, one for each piece
# of the application so we create N semaphores for the one key.
#
def sem_create(key, nCount, val):
    flags = 0o0740
    array_type=ctypes.c_uint16*nCount
    pyarr = [val]*nCount
    createUnion=struct_semun()
    createUnion.array=array_type(*pyarr)
    if verbose > 0:
        print("key="+hex(key))

    if verbose>1:
        print("create request: key:"+hex(key)+" nCount:"+str(nCount)+" val:"+str(val))

    if verbose>0:
        print("Checking if already exists")

    sem_list()

    try:
        semId=libc.semget(key,nCount, flags)
        yesFound=True
        if verbose>1:
            print("semId="+str(semId))
    except OSError as exc:
        # errno=2    descr=No such file or directory
        if exc.errno == 2:
            yesFound=False
        else:
            print("errno="+str(exc.errno)+ "   descr="+exc.strerror)
            raise
        # errno=22   descr=Invalid argument
        # others?
    if yesFound:
        print("Semaphore with key: "+hex(key)+" already exists")
        return 3 

    if verbose>1:
        print("Doing create with semget")
    flags |= IPC_CREAT
    try:
        semId=libc.semget(key,nCount, flags)
        if verbose>1:
            print("semId="+str(semId))
    except OSError as exc:
        print("Error: semget gives: "+str(semId)+" in create")
        print("exception="+str(exc))
        return 4 
    if verbose>1:
        print("We got the id:"+str(semId))

    if verbose>1:
        print("Doing semctl to set values to "+str(semId)+" with semctl")
    try:
        ### ret=libb.semctl(semId,0,SETALL, array_type(*array))
        ret=libc.semctl(semId,0,SETALL, createUnion)
        if verbose > 2:
            print("ret="+str(ret))
    except OSError as exc:
        print("semctl Fail:"+str(exc))
        ret=exc.errno
        raise
    if verbose>2:
        print("Created: returning: "+str(ret))
    sem_list()
    return ret 


libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)
libc.semctl.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, struct_semun]
libc.semctl.restype = ErrnoResult
libc.semget.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
libc.semget.restype = ErrnoResult
libc.semtimedop.argtypes = [ctypes.c_int, ctypes.POINTER(struct_sembuf), ctypes.c_int16, ctypes.POINTER(struct_timespec)]
libc.semtimedop.restype = ErrnoResult

# Key supplied to semget(2) */
# Effective UID of owner */
# Effective GID of owner */
# Effective UID of creator */
# Effective GID of creator */
# Permissions */
# Sequence number */

#
# If you really want the details, here they are
# I'm not calling this these days but it still works
#
def printSemDetailsNow(semDs):
    print("    key     : "+hex(semDs.sem_perm.key))   # Key supplied to semget(2) */
    print("    uid     : "+str(semDs.sem_perm.uid))   # Effective UID of owner */
    print("    gid     : "+str(semDs.sem_perm.gid))   # Effective GID of owner */
    print("    cuid    : "+str(semDs.sem_perm.cuid))  # Effective UID of creator */
    print("    cgid    : "+str(semDs.sem_perm.cgid))  # Effective GID of creator */
    print("    mode    : "+oct(semDs.sem_perm.mode))  # Permissions */
    print("    seq     : "+oct(semDs.sem_perm._seq))  # Sequence number */

#
# we want to know what the counts are for our semaphores for the key
# this will read them out.
#
def sem_query(key, index):
    flags = 0o0740
    if verbose > 0:
        print("Querying")
    try:
        if verbose > 0:
            print("key="+hex(key))
        semId=libc.semget(key,1, flags)
    except OSError as ose:
        if ose.errno == errno.ENOENT:
            sys.stderr.write("No semaphore set exists for key="+hex(key)+"\n")
            return ose.errno 
        else:
            sys.stderr.write("semget gives error: "+str(ose)+"\n")
        return 3 
    if verbose > 0:
        print("Query: semId="+str(semId))
    semDs=struct_semid_ds()
    semUn=struct_semun()
    semUn.buf=ctypes.pointer(semDs)
    try:
        maxid = libc.semctl(semId, NUM_SEM, IPC_STAT, semUn)
    except OSError as ose:
        sys.stderr.write("semctl gives error: "+str(ose)+"\n")
        return 4 
    if verbose > 1:
        print("maxId="+str(maxid))
        print("    permission={0:03o} ".format(semDs.sem_perm.mode) )
        print("    lastOpTime="+datetime.utcfromtimestamp(semDs.sem_otime).strftime('%Y-%m-%d %H:%M:%S'))
        print("    lastChTime="+datetime.utcfromtimestamp(semDs.sem_ctime).strftime('%Y-%m-%d %H:%M:%S'))

        print("    nsem="+str(semDs.sem_nsems))
        printSemDetails(semDs)

    if index < 0:
        start=0
        end=semDs.sem_nsems
    else:
        start=index
        end=index+1
        print("start="+str(start)+" end="+str(end))
    for  ii in range(start,end):
        semncnt=-314
        sempid=-314
        semval=-314
        semzcnt=-314
        try:
            semncnt=libc.semctl(semId, ii, GETNCNT,semUn);
            sempid=libc.semctl(semId, ii, GETPID,semUn);
            semval=libc.semctl(semId, ii, GETVAL,semUn);
            semzcnt=libc.semctl(semId, ii, GETZCNT,semUn);
        except OSError as exc:
            print("failure with semctl:")
            print("    semncnt="+str(semncnt))
            print("    sempid ="+str(sempid))
            print("    semval ="+str(semval))
            print("    semzcnt="+str(semzcnt))
            print(" error:"+str(exc))
            raise
        if verbose>0 or index<0:
            print("  sem[%2d]=( val=%2d zcnt=%2d ncnt=%2d pid=%d )"%(ii,semval,semncnt,semzcnt,sempid))
        else:
            print("val="+str(semval))

    
#
# This is kind of like the ipcs -s output
# Not used much, if ever.
#
def sem_list():
    """List System V semaphore"""

    theDS=struct_semid_ds()
    semary = struct_semun()
    semary.buf=ctypes.pointer(theDS)

    maxid = libc.semctl(0, 0, SEM_INFO, semary)
    if maxid == 0:
        print("No System V semaphore")
        return 5 
    if verbose>4:
        print("")
        print("  SEM ID     Key ID      Owner      Creator  Mode #sems")
        for i in range(maxid + 1):
            try:
                semid = libc.semctl(i, 0, SEM_STAT, semary)
            except OSError as exc:
                if exc.errno == errno.EACCES:
                    print("   SEM {0:d}: access denied".format(i))
                    continue
                elif exc.errno == errno.EINVAL:
                    print("   SEM {0:d}: currently unused".format(i))
                    continue
                raise
            print("{0:10d} 0x{1:08x} {2:5d}:{3:<5d} {4:5d}:{5:<5d} {6:3o} {7:5d}".format(
                semid, theDS.sem_perm.key,
                theDS.sem_perm.uid,  theDS.sem_perm.gid,
                theDS.sem_perm.cuid, theDS.sem_perm.cgid,
                theDS.sem_perm.mode, theDS.sem_nsems))

#
#  Delete the sem, leaving nothing behind.
#this could as easily be done with ipcrm but here it is
#
def sem_delete(key):
    deleteUnion=struct_semun()
    flags = 0o0740
    if verbose > 1:
        print("requesting DELETE of key=0x{0:08x}".format(key))
    try:
        semId=libc.semget(key,1, flags)
    except OSError as ose:
        if ose.errno == errno.ENOENT:
            sys.stderr.write("No semaphore set exists for key="+hex(key)+"\n")
            return ose.errno 
        else:
            sys.stderr.write("semget gives error: "+str(ose)+"\n")
        return 3 
    if verbose > 0:
        print("requesting DELETE of semID={0:10d}".format(semId))

    try:
        ret=libc.semctl(semId, 1, IPC_RMID,deleteUnion);
    except OSError as ose:
        sys.stderr.write("Failure of semctl remove: {0:10d}\n".format(semId))
        sys.stderr.write("semctl:"+str(ose))
        exit(4)
    if verbose > 1:
        print("DELETE ret="+str(ret))
    return ret 
   


# This is the main interface for accessing sem_wait something in python
# This gets called by __main__ below
#
# This gets a list of text arguments from the caller
# the list is detailed below, if I add them here, that would be too much
# 
def sem_wait(argv=None):
    global verbose
    global permanent
    global ignoreSemError
    key=0x426f62
    index=-1
    nCount=1
    waitSec=1

    parser = argparse.ArgumentParser(add_help=False,
        description="List information about IPC facilities")
    parser.add_argument("-v","--verbosity",help="Increase Verbosity", action="count")
    parser.add_argument("-h","--help","--??",help="Ask for Help", action="help")
    parser.add_argument("-k","--key",      action="store",help="Specify key used for the semaphore (Defaults to 0x426f62)",default="0x426f62")
    parser.add_argument("-n","--count",    action="store",help="Specify the N count of how many to create for that key (default=1)")
    parser.add_argument("-i","--index",    action="store",help="Specify the index of the semaphore in the set to operate on (default=0)")
    parser.add_argument("-w","--wait",     action="store",help="Specify Wait time in seconds (default non-blocking)")
    parser.add_argument("-G","--gobble",action="store_true", help="Gobble up all the semaphore values that are available and wait until the rest are available.")
    parser.add_argument("-P","--permanent",action="store_true",help="Make the changes survive exit (no-SEM_UNDO)")
    parser.add_argument("-I","--ignoreSem",action="store_true",help="Ignore semaphore errors");

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("-c","--create", action="store",       help="Creating semaphore with value N",nargs="?",const=1)
    group.add_argument("-q","--query",  action="store_true",  help="Query semaphore with key K"     )
    group.add_argument("-s","--set",    action="store",       help="Setting semaphore with key K"   )
    group.add_argument("-d","--delete", action="store_true",  help="Deleting semaphore with key K"  )

    args=parser.parse_args(argv)

    verbose=args.verbosity
    if args.key:
        try:
            key=int(args.key,16)
        except Exception as e:
            print("Invalid key: "+args.key+" requested")
            return 6 

    if args.count:
        try:
            nCount=int(args.count)
            if nCount > 127:
                print("Excess count requested:"+str(nCount))
                return 7 
        except Exception as e:
            print("Invalid count: "+args.count+" requested")
            return 9 

    if args.index:
        try:
            index=int(args.index)
            if index>127:
                print("Excess index requested:"+str(index))
                return 7 
        except Exception as e:
            print("Invalid count: "+args.index+" requested")
            return 7 

    if args.wait:
        try:
            waitSec=int(args.wait)
            if waitSec < 0:
                waitSec=0
        except Exception as e:
            print("Invalid wait Seconds: "+args.wait)
            return 7 
    if args.ignoreSem:
        ignoreSemError=True

    if args.permanent:
        permanent=True

    if args.create:
        try:
            increment=int(args.create)
        except Exception as e:
            print("Invalid create value requested"+str(args.create))
        ret=sem_create(key,nCount,increment)
        return ret 

    elif args.set:
        if index<0:
            index=0
        try:
            increment=int(args.set)
        except Exception as e:
            print("Invalid set value requested"+str(args.create))
        if args.gobble:
            if verbose>1:
                print("Gobbling: index="+str(index)+"  semCount="+str(increment))
            ret=sem_gobbleUp(key,index,increment,waitSec)
        else:
            ret=sem_set(key,index,increment,waitSec)
        return ret
    elif args.query:
        ret=sem_query(key,index)
        return ret
    elif args.delete:
        ret=sem_delete(key)
        return ret
    else:
        print("Problem with selections")
        return 4

# If you call this from the command line, it comes here
# we are mostly expecting this to be called from a python program
# but this works.
if __name__ == '__main__':
    print("Starting:"+str(sys.argv))
    ret=sem_wait(sys.argv[1:])
    exit(ret)
