/*
 * This is a program that will wait for or set a semaphore
 * Sun Oct 21 19:12:55 EDT 2018
 * This will now:
 *    create a semaphore (if it does not exist) with a default number
 *    set    a semaphore to a higher number (giving back, non-blocking)
 *    set    a semaphore to a lower number (taking from, possible blocking)
 *    delete a semaphore for some reason, presumably to reinit everything
 *
 * Wed Jan 30 11:41:49 EST 2019
 *
 *    usage:
 *       waitSem [-v] [-k 0x53514d ] [-n totalindex] [-i index] [-w seconds] [-G] [ -c | -q | -s | -d ] increment
 *    where:
 *       -v verbose (can be repeated) (if you see the key, the ascii chars are displayed, also)
 *       -k is the key used for the semaphore (Defaults to 53514d (SQM))
 *       -n is the N count of how many to create for that key
 *          Several semaphores can exist under one key. This is how many to create.
 *       -i the index of the semaphore in the set to operate on (required for set)
 *       -w Wait time in seconds
 *       -G Gobble, Gobble up all the semaphore values that are available and wait until the rest are available.
 *          If we are waiting for a bunch, we don't want anybody cutting in line because they want fewer.
 *       -h or --?? requests the exciting help
 *    One of these must be specified:
 *       -c is create request
 *       -q is query request
 *       -s is set (and wait) request
 *       -d is delete request
 *       increment (a positive or negative number between -32767 and 32768) (Defaults to 1)
 *       (Ignored for -q and -d)
 *
 * We had some big shell scripts that needed to coordinate access to other processes.
 * This is an attempt to provide full access to SystemV semaphores to the command line.
 * There is a python callable version also. 
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#define TOTAL_SEM 64

#define DIM_OF(x) (sizeof(x)/sizeof(x[0]))
#define NUM_SEM 128  /* max value from  fourth field in /proc/sys/kernel/sem */

extern int semtimedop(int semid, struct sembuf *sops, unsigned nsops, struct timespec *timeout);
static void semError(int err);


static int semMaxValue=TOTAL_SEM;

/*
 * They say I have to define this myself
 */
union semun {
    int              val;    /* Value for SETVAL */
    struct semid_ds *buf;    /* Buffer for IPC_STAT, IPC_SET */
    unsigned short  *array;  /* Array for GETALL, SETALL */
    struct seminfo  *__buf;  /* Buffer for IPC_INFO
                                (Linux-specific) */
};

static int verbose=0;    /* print more than usual */
static int gobble=0;     /* Gobble says gooble up all the sema values that are out there
                          * and keep going until we get all we want.
                          */

enum {
    CREATE=1,
    QUERY=2,
    SET=3,
    DELETE=4
};

static char *opName[]={
    "Invalid",
    "CREATE",
    "QUERY",
    "SET",
    "DELETE"
};

static void printUsage()
{
    printf("usage: \n");
    printf("   waitSem [-v] [-k 0x53514d ] [-n totalindex] [-i index] [-w seconds] [-G] [ -c | -q | -s | -d ] increment \n");
    printf("where: \n");
    printf("   -v verbose (can be repeated) \n");
    printf("   -k is the key used for the semaphore (Defaults to 53514d) \n");
    printf("   -n is the N count of how many to create for that key \n");
    printf("      Several semaphores can exist under one key. This is how many to create. \n");
    printf("      (Defaults to 1) \n");
    printf("   -i the index of the semaphore in the set to operate on (Defaults to 1) \n");
    printf("   -w Wait time in seconds \n");
    printf("   -G Gobble, Gobble up all the semaphore values that are available and wait until the rest are available. \n");
    printf("      If we are waiting for a bunch, we don't want anybody cutting in line because they want fewer. \n");
    printf("   -h or --?? requests this exciting help\n");
    printf("One of these must be specified: \n");
    printf("   -c is create request \n");
    printf("   -q is query request \n");
    printf("   -s is set (and wait) request \n");
    printf("   -d is delete request \n");
    printf("   increment (a positive or negative number between -32767 and 32768) (Defaults to 1) \n");
    printf("   (Ignored for -q and -d) \n");
    exit(0);
}
/*
 * Display the characters that make up the key (if they are printable)
 *
 * We want to display them in order so we need to swap first
 */
#define SWAP32(in) ((((in)&0xff000000)>>24) | (((in)&0x00ff0000)>>8) | (((in)&0x0000ff00)<<8 ) | (((in)&0x000000ff)<<24) )
static char *dumpKey(key_t key)
{
    static char retBuf[16];
    int ii;
    key_t inKey=SWAP32(key);
    unsigned char *pKey=(unsigned char *)&inKey;

    memset(retBuf,0,sizeof(retBuf));
    for(ii=0; ii<sizeof(key); ii++)
    {
        snprintf(&retBuf[ii],2, "%c",isprint(pKey[ii])?pKey[ii]:'.');
    }
    return(retBuf);
}
/*
 * You get the idea,
 * What do we know about this semaphore?
 */
static void printSemDetails(struct semid_ds *semDs)
{
    printf("    key     : 0x%08x (%s)\n", semDs->sem_perm.__key,dumpKey(semDs->sem_perm.__key));   /* Key supplied to semget(2) */
    printf("    uid     : %d\n",   semDs->sem_perm.uid);     /* Effective UID of owner */
    printf("    gid     : %d\n",   semDs->sem_perm.gid);     /* Effective GID of owner */
    printf("    cuid    : %d\n",   semDs->sem_perm.cuid);    /* Effective UID of creator */
    printf("    cgid    : %d\n",   semDs->sem_perm.cgid);    /* Effective GID of creator */
    printf("    mode    : %o\n",   semDs->sem_perm.mode);    /* Permissions */
    printf("    seq     : %o\n",   semDs->sem_perm.__seq);   /* Sequence number */
    
}

/*
 * This is where we are creating the semaphore
 * observe
 */
static int create(key_t key, int nCount, int val)
{
    int semId;
    int flags = 0740 ;
    int ret;
    short array[NUM_SEM]; /* the max for our systems (128) */
    union semun createUnion;
    int ii;
    for(ii=0; ii<NUM_SEM; ii++) { array[ii]=val; }
    createUnion.array=(unsigned short *)array;

    if( verbose > 1 )
    {
        printf("create request: key:%08x nCount=%d val:%d\n",
            key, nCount, val);
    }
    if( nCount > NUM_SEM )
    {
        fprintf(stderr,"Error: excess sem count: %d max=%d\n", nCount, NUM_SEM);
        exit(3);
    }

    if( verbose > 2 ) { printf("Checking if already exists\n"); }
    /* first check to see if it already exists */
    semId=semget(key,nCount, flags);
    if( semId > 0 )
    {
        fprintf(stderr,"Semaphore with key: 0x%08x already exists, Id=%d\n",
            key,semId);
        exit(1);
    }
    if( verbose > 2 ) { printf("Doing create with semget\n"); }
    flags |= IPC_CREAT ;
    semId=semget(key,nCount, flags);
    if( semId < 0 )
    {
        fprintf(stderr,"Error: semget gives: %d in create\n", semId);
        perror("semget");
        exit(3);
    }

    if( verbose > 2 ) { printf("Doing semctl to set values to %d with semctl\n", val); }
    ret=semctl(semId, 0, SETALL, createUnion);
    if( ret < 0 )
    {
        fprintf(stderr,"semctl gives: %d in %s\n", ret, __func__);
        perror("semctl");
    }
    if( verbose > 2 ) { printf("Created: returning: %d\n", ret); }
    return(ret);
}

static int query(key_t key, int index)
{
    int ret=0;
    int semId;
    int flags = 0740 ;
    int start;
    int end;
    unsigned short  semval=0;   /* semaphore value */
    unsigned short  semzcnt=0;  /* # waiting for zero */
    unsigned short  semncnt=0;  /* # waiting for increase */
    pid_t           sempid=0;   /* process that did last op */
    struct semid_ds semDs={{0}};

#if 0
    if( verbose > 2 ) { printf("Starting with %s key: 0x%08x index=%d\n", __func__, key); }
#endif

    semId=semget(key,1, flags);
    if( semId < 0 )
    {
        if( ENOENT == errno ) {
            fprintf(stderr,"Error in %s: No semaphore set exists for key=0x%08x\n", __func__, key);
        } else
        {
            fprintf(stderr,"semget gives: %d in %s key=0x%08x\n", semId,__func__, key);
            perror("semget");
        }
        exit(3);
    }
    ret=semctl(semId, NUM_SEM,IPC_STAT,&semDs);
    if( ret < 0 )
    {
        fprintf(stderr,"semctl gives:: %d in %s\n", ret, __func__);
        perror("semctl");
        exit(3);
    }
    if( verbose > 1 ) {

        if( verbose>2) printf("QUERY: ret=%d\n", ret);
        printf("    permission=%o\n", semDs.sem_perm.mode );
        printf("    lastOpTime=%s", ctime(&semDs.sem_otime));
        printf("    lastChTime=%s", ctime(&semDs.sem_ctime));
    }
    if( verbose > 1 )
    {
        printf("    nsem=%"PRIi64"\n", semDs.sem_nsems);
        printSemDetails(&semDs);
    }

    if( index < 0 ) {
        start=0;
        end=semDs.sem_nsems;
    } else {
        start=index;
        end=index+1;
    }
    int ii;
    for(ii=start; ii<end; ii++)
    {
        semncnt=semctl(semId, ii, GETNCNT);
        sempid=semctl(semId, ii, GETPID);
        semval=semctl(semId, ii, GETVAL);
        semzcnt=semctl(semId, ii, GETZCNT);
        if( semncnt<0 || sempid <0 || semval <0 || semzcnt < 0 )
        {
            fprintf(stderr,"  ret=( val=%2d zcnt=%2d ncnt=%2d)\n",
                semval,semzcnt,semncnt);
            fprintf(stderr,"semctl gives:2 %d in %s\n", ret, __func__);
            perror("semctl");
            /*exit(3);*/
        } else
        {
            if( verbose || index<0 ) {
                char workBuf[64]={0};
                sprintf(workBuf,"pid=%d", sempid);
                printf("  sem[%2d]=( val=%2d ncnt=%2d %s)\n",
                    ii,semval,semncnt,workBuf);
            } else
            {
                printf("%d\n", semval);
            }
        }
    }

    if( verbose > 2 ) { printf("Done with %s returning: %d\n", __func__,ret); }
    return(ret);
}
/*
 * This will set the value up or down
 * going up is easy, it always happens.
 * going down, not so much, we can block if we need to get more
 * that are out there or timeout trying.
 *
 * Return value:
 *    0: success, we were able to do what we needed
 * -999: Failure because we timed out
 *   -1: (or other <0) failure because of semapore trouble
 */
static int set(key_t key, int index, int increment, int waitSec)
{
    int semId;
    int ret;
    int flags = 0740 ;
    struct timespec waitStruct={0};

    waitStruct.tv_sec=waitSec;

    struct sembuf opBuf[NUM_SEM]={{0}};
    if( verbose > 1 ) {
        printf("Callling SET with key:0x%08x index=%d incr=%d\n",
            key,index,increment);
    }
    semId=semget(key,index, flags);
    if( semId < 0 )
    {
        fprintf(stderr,"semget gives: %d in %s key=0x%08x index=%d\n", semId,__func__,key, index);
        perror("semget");
        exit(3);
    }
    if( verbose > 1 ) {
        printf("inside set: semId=%d\n", semId);
    }

    if( increment > 0 ) {
        int curVal=semctl(semId, index, GETVAL);
        if( curVal+increment > semMaxValue ) {
            increment=semMaxValue-curVal;
        }
    }
    if( 0 == increment )
    {
        ret=0;
    } else
    {
        opBuf[0].sem_num=index;
        opBuf[0].sem_op=increment;
        opBuf[0].sem_flg=0;
        ret=semtimedop(semId, opBuf, 1, &waitStruct);
        if( ret<0 && errno != EAGAIN )
        {
            fprintf(stderr,"Failure of semtimedop set: %d\n",semId);
            perror("semtimedop");
            semError(errno);
            exit(1);
        }
        if( ret<0 && EAGAIN == errno ) {
            ret=-999;
        }
        if( verbose > 1 ) {
            printf("SET ret=%d increment=%d\n", ret, increment);
        }
    }

    return(ret);
}
/*
 * In the gobbleUp routine, we need to know how many
 * are out there right now.  This tells us:
 */
static int howMany(key_t key, int index)
{
    int semId;
    int flags = 0740 ;
    int retNum;

    semId=semget(key,1, flags);
    if( semId < 0 )
    {
        fprintf(stderr,"semget gives: %d in howmany key=0x%08x\n", semId,key);
        perror("semget");
        exit(3);
    }
    if( verbose > 1 ) {
        printf("inside %s: semId=%d\n",__func__,semId);
    }

    retNum=semctl(semId, index, GETVAL);
    if( retNum < 0 )
    {
        fprintf(stderr,"Problem with howMany semctl call:%d\n", retNum);
        perror("semctl");
    }

    if( verbose > 1 ) {
        printf("howMany=%d\n", retNum);
    }
    return(retNum);
}
/*
 * gobbleUp:
 *   If we need N semaphores but only N-2 are available, we can't just wait
 *   because others coming in, asking for one will get the one while we are still waiting
 *   The solution is to gobble up all the semaphores that are available
 *   and keep coming back, taking any that become available until we get what we want
 *   or we time out.
 */
static int gobbleUp(key_t key, int index, int increment, int waitSec)
{
    int left;
    int weGot=0;
    int remaining;
    int quitTime;
    int startTime=time(NULL);
    int ret=0;
    int putBack=howMany(key,index); /* This is how many we need to put back if we time out */
    char howLong[44]="";

    printf("waitSec=%d nine=%d diff=%d arg=%d\n",
        waitSec, 999999 , 999999-waitSec, (waitSec <999999 ));
    if( waitSec <999999 ) {
        sprintf(howLong, "of %d", waitSec);
    }

    left=putBack;

    quitTime=time(NULL)+waitSec;
    if( verbose )
    {
        printf("Gobbling count=%d until: %d\n", abs(increment), quitTime);
    }
    /*
     * we have to walk through rather than getting them all at once
     * because, somebody else may have gotten one after our howMany call
     * and before now.
     */
    int ii;
    for(ii=0; ii<abs(increment) && 0==ret; ii++)
    {
        if( verbose )
        {
            printf("Gobbling up %d of %d\n", ii, abs(increment));
        }
        ret=set(key,index,-1,0);
        if( ret < 0 ) {
            if( ret != -999 ){
                fprintf(stderr,"Problem with gobble set: ii=%d incr=%d left=%d\n", 
                        ii, abs(increment), left);
            }
            continue;
        }
        left--;
        weGot++;
    }
    if( verbose ) {
        printf("Exited gobble loop: incr=%d left=%d\n",
                abs(increment), left);
    }
    if( verbose ) {
        printf("Gobble: we Got: %d out of %d\n", weGot, abs(increment));
    }
    remaining=howMany(key,index);
    /* this should be zero */
    if( remaining != 0 )
    {
        fprintf(stderr,"Gobble; remaining not zero:%d\n", remaining);
    }
    while(weGot < abs(increment) && time(NULL) < quitTime )
    {
        ret=set(key,index,-1,1); /* one sem, one second */
        if( ret == 0 )
        {
            weGot++;
            if( verbose ) {
                printf("Gobble: one more: %d out of %d\n", weGot, abs(increment));
            }
            continue;
        } 
        else if( ret != -999 ){
            fprintf(stderr,"Failure of Gobble get loop: %d\n", ret);
            exit(9);
        }
        if( verbose ) {
            printf("Gobble: looping back: %d out of %d  elapsed=%-3"PRIi64" %s\n",
                    weGot, abs(increment), time(NULL)-startTime, howLong );
        }
    }
    if( weGot >= abs(increment) ) {
        ret=0;
    } else
    {
        ret=set(key,index,putBack,0);
        if( ret < 0 )
        {
            fprintf(stderr,"Problem putting back after Gobble failure: %d index=%d putBack=%d\n",
                ret, index, putBack);
        }
        ret=5; /* timeout */
    }
    return(ret);
}
/*
 * Delete the sem, leaving nothing behind.
 * this could as easily be done with ipcrm but here it is
 */
static int delete(key_t key)
{
    int semId;
    int ret;
    int flags = 0740 ;

    if( verbose > 1 ) {
        printf("requesting DELETE of key=0x%08x\n", key);
    }
    semId=semget(key,1, flags);
    if( semId < 0 )
    {
#if 0
        fprintf(stderr,"semget gives: %d in %s key=0x%08x\n",__func__, semId,key);
#endif
        perror("semget");
        exit(3);
    }
    if( verbose > 1 ) {
        printf("requesting DELETE of semID=%d\n", semId);
    }
    ret=semctl(semId, 1, IPC_RMID);
    if( ret<0 )
    {
        fprintf(stderr,"Failure of semctl remove: %d\n",semId);
        perror("semctl");
        exit(1);
    }
    if( verbose > 1 ) {
        printf("DELETE ret=%d\n", ret);
    }
    return(ret);
}
/*
 * I like to have the keys be printable ascii
 * this will turn printable ascii into the required binary value
 */
static int asciiKey(char *in)
{
    int key=0;
    char *p=(char*)&key;
    p[0]=in[2];
    p[1]=in[1];
    p[2]=in[0];
    p[3]=0;
    return(key);
}

int main(int argc, char *argv[])
{
    int operation= -1;
    int ret;
    int increment=1;  /* how to change value of this sem*/
    int nCount=1;     /* how many sems to get in this set */
    int index=0;      /* which sem in this set */
    int setIndex=0;   /* has this index value been set from command line */
    int waitSec=999999;  /* how long to wait for the semaphore start=forever */
    char *end;
    key_t key=0x426f62;
    unsigned short semList[NUM_SEM+1]={0};

    int ii;
    for(ii=1; ii<argc; ii++)
    {
        if( 0 == strcmp(argv[ii],"-h") || 0 == strcmp(argv[ii],"-?") || 0 == strcmp(argv[ii],"--??") )
        {
            printUsage();
            exit(0);
        }
        if( 0 == strcmp(argv[ii],"-v") )
        {
            verbose++;
            continue;
        }
        if( 0 == strcmp(argv[ii],"-M") && (ii+1)<argc )
        {
            ii++;
            semMaxValue=atoi(argv[ii]);
            if( semMaxValue <=0 || semMaxValue  > 63)
            {
                printf("Error: invalid max semValue specified: %s\n", argv[ii]);
                exit(3);
            }
            continue;
        }
        if( 0 == strcmp(argv[ii],"-k") && (ii+1)<argc )
        {
            ii++;
            if( 0 == strncmp(argv[ii],"0x",2) )
            {
                key=strtol(argv[ii],&end,16);
            } else
            {
                key=asciiKey(argv[ii]);
                printf("Using key: ascii=%.3s 0x%08x\n", argv[ii],key);
            }
            if( end == argv[ii] )
            {
                fprintf(stderr,"Error Invalid key specified: -k %s\n", 
                        argv[ii]);
                exit(1);
            }
            continue;
        }
        if( 0 == strcmp(argv[ii],"-n") && (ii+1)<argc )
        {
            ii++;
            nCount=strtol(argv[ii],&end,10);
            if( !isdigit(argv[ii][0])  || end == argv[ii] )
            {
                fprintf(stderr,"Error invalid semaphore count specified: -n %s\n", 
                        argv[ii]);
                exit(1);
            }
            continue;
        }
        if( 0 == strcmp(argv[ii],"-i") && (ii+1)<argc )
        {
            ii++;
            index=strtol(argv[ii],&end,10);
            if( !isdigit(argv[ii][0])  || end == argv[ii] )
            {
                fprintf(stderr,"Error invalid semaphore index specified: -i %s\n", 
                        argv[ii]);
                exit(1);
            }
            setIndex=1;
            continue;
        }
        if( 0 == strcmp(argv[ii],"-w") && (ii+1)<argc )
        {
            ii++;
            waitSec=strtol(argv[ii],&end,10);
            if( !(isdigit(argv[ii][0]) || '-'==argv[ii][0])  || end == argv[ii] )
            {
                fprintf(stderr,"Error invalid wait time specified: -w %s\n", 
                        argv[ii]);
                exit(1);
            }
            if( waitSec < 0 ) {
                waitSec=0;
            }
            continue;
        }
        if( 0 == strcmp(argv[ii],"-G") )
        {
            gobble=1;
            continue;
        }
        if( 0 == strcmp(argv[ii],"-c") )
        {
            operation=CREATE;
            continue;
        }
        if( 0 == strcmp(argv[ii],"-q") )
        {
            operation=QUERY;
            if( !setIndex ) {
                index=-1; /* check them all */
            }

            /* What happens if we Query what is not there?*/
            continue;
        }
        if( 0 == strcmp(argv[ii],"-d") )
        {
            operation=DELETE;
            /* What happens if we delete what is not there?*/
            continue;
        }
        if( 0 == strcmp( argv[ii],"-s") && (ii+1)<argc )
        {
            /* What happens if we set what is not there?*/
            /* What happen if the index is out of range? */
            if( -1 == operation )
            {
                operation=SET;
            } else
            {
                if( verbose )
                {
                    printf("Create: setting value to : %s\n", argv[ii+1]);
                }
            }
            ii++;
            increment=strtol(argv[ii],&end,10);
            if( !(isdigit(argv[ii][0]) || '-'==argv[ii][0] || '+'==argv[ii][0]) || end == argv[ii] )
            {
                fprintf(stderr,"Error invalid semephore increment specified: -s %s\n", 
                        argv[ii]);
                exit(1);
            }
            continue;
        }
        if( isdigit(argv[ii][0]) || (NULL != strchr("+-",argv[ii][0]) && isdigit(argv[ii][1]) ) )
        {
            increment=atoi(argv[ii]);
            continue;
        }
        fprintf(stderr,"What are we trying to do?: %s\n", argv[ii]);
        fprintf(stderr,"Remaining args:\n");
        int qq;
        for(qq=ii; qq<argc; qq++)
        {
            fprintf(stderr,"%s\n", argv[qq]);
        }
        exit(1);
    }
    if( -1 == operation )
    {
        fprintf(stderr, "No operation specified");
        exit(2);
    }
    if( verbose > 1 )
    {
        printf("operation: %s : (v,k,c)=(%d,%x,%d)\n", opName[operation],
                verbose, 
                key,
                increment
              );
    }

    switch(operation)
    {
        case CREATE:
            ret=create(key, nCount, increment);
            exit(0);
        case SET:
            if( !setIndex )
            {
                printf("Unable to set without specifying index (-i) to set\n");
                ret=3;
            } else
            {
                if( gobble && increment < 0 )
                {
                    ret=gobbleUp(key, index, increment, waitSec);
                } else
                {
                    ret=set(key,index,increment, waitSec);
                }
            }
            exit(ret);

        case QUERY:
            ret=query(key, index);
            exit(ret);
        case DELETE:
            ret=delete(key);
            exit(ret);
    }
    if( verbose || QUERY==operation)
    {
#if 0
        ret=semctl(semId, 1, GETALL, semList);
#else
        ret = 0;
#endif
        printf("semctl GETALL: ret=%d\n", ret);
        int ii;
        for(ii=1; ii<=NUM_SEM; ii++)
        {
            printf("    val[%d]=%d\n",ii, semList[ii]);
        }
    }
    exit(0);
}



/*
from semget:
    The semaphores in a set are not initialized by semget(). In order
    to initialize the semaphores, semctl(2) must be used to perform a
    SETVAL or a SETALL operation on the semaphore set.  (Where multiple
    peers do not know who will be the first to initialize the set, checking
    for a non-zero sem_otime in the associated data structure retrieved
    by a semctl(2) IPC_STAT operation can be used to avoid races.)
    semctl
*/



static void semError(int err)
{
    fprintf(stderr,"semError:%-4d:", err);
    switch(err)
    {
        case E2BIG:  fprintf(stderr,"E2BIG:  \n"); break;
        case EACCES: fprintf(stderr,"EACCES: \n"); break;
        case EAGAIN: fprintf(stderr,"EAGAIN: \n"); break;
        case EFAULT: fprintf(stderr,"EFAULT: \n"); break;
        case EFBIG:  fprintf(stderr,"EFBIG:  \n"); break;
        case EIDRM:  fprintf(stderr,"EIDRM:  \n"); break;
        case EINTR:  fprintf(stderr,"EINTR:  \n"); break;
        case EINVAL: fprintf(stderr,"EINVAL: \n"); break;
        case ENOMEM: fprintf(stderr,"ENOMEM: \n"); break;
        case ERANGE: fprintf(stderr,"ERANGE: \n"); break;
        default: fprintf(stderr,"Unknown:%d\n", err);
    }
}
/* -v -v -n 52 -c -i 12 */
/*
 * vim: ts=8 sw=4 expandtab
 */
