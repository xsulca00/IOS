#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdbool.h>
#define __USE_XOPEN_EXTENDED
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <errno.h>

enum events {
  ADULT_STARTED = 0,
  ADULT_ENTER,
  ADULT_TRY_LEAVE,
  ADULT_WAIT,
  ADULT_LEAVE,
  ADULT_FINISHED,
  CHILD_STARTED,
  CHILD_ENTER,
  CHILD_TRY_LEAVE,
  CHILD_WAIT,
  CHILD_LEAVE,
  CHILD_FINISHED,
};

// return values
enum {
    NO_ERROR =  0,
    BAD_INPUT = 1, 
    SYS_CALL_FAIL = 2
};

// holds number and/or error 
struct Number {
    /* number */
    long num;
    /* possible error description or NULL*/
    struct err {
        const char* const what;
    } err;
};

// function prototypes
void print_event(enum events event, int); // print out events	
void get_params(char **argv);	    // get program parameters & validate	
void* alloc_shared_mem(size_t)      ;
sem_t* create_shared_sem(unsigned);        // create semaphore with initial value
void destroy_sem(sem_t*);
struct Number get_num_or_err(char* str); //get number
void dealloc_shared_mem(void*, size_t);
void free_all_resources(void);
void create_adult_generator(void);  // create adult process generator
void create_child_generator(void);  // create child process generator

// print error with prefix e.g. "Adults count: ...", NULL serves as end of strings
// print system error according to errno also
void error_and_die(int, ...);

// shared variables amongst the processes
int* event_id;			// event counter printed out to file
int* adult_id;		    // ID of the adult process
int* child_id;		    // ID of the child process
int* adults_count;      // number of entered adults
int* childern_count;    // number of entered childern
bool* last_adult_left;  // no adults at all?
int* child_wait_count;  // number of waiting adults
int* adults_wait_count; // number of waiting adults

// shared semaphores amongst the processes
sem_t *mutex;	    	// mutex for priting into file
sem_t *mutex1;		    // mutex for adult process
sem_t *mutex2;		    // mutex for childern process
sem_t* adults_wait;     // adults waiting till childern leave 
sem_t* child_group;     // childern waiting till adult enter 
sem_t* wait_till_finish;// processes waiting till finish

// output file pointer
FILE *file;      

// stores input parameters
struct options {
    long adults_count;		
    long childern_count;
    long next_adult;
    long next_child;	
    long adult_works;
    long child_works;	
} opts;

int main(int argc, char **argv)
{   
    if (argc != 7)
    {
        error_and_die(BAD_INPUT, "Usage:", argv[0], "A C AGT CGT AWT CWT", NULL);
    } 

    get_params(argv);

    errno = 0;	
    if ((file = fopen("proj2.out", "w"))  == NULL)
    {
        error_and_die(SYS_CALL_FAIL, "fopen:", NULL);
    }

    // shared semaphores creation
    mutex = create_shared_sem(1);
    mutex1 = create_shared_sem(1);
    mutex2 = create_shared_sem(1);
    adults_wait = create_shared_sem(0);
    wait_till_finish = create_shared_sem(0);

    // allocate memory for shared variables
    event_id = alloc_shared_mem(sizeof(int));
    adult_id = alloc_shared_mem(sizeof(int));
    child_id = alloc_shared_mem(sizeof(int));
    adults_count = alloc_shared_mem(sizeof(int));
    child_wait_count = alloc_shared_mem(sizeof(int));
    adults_wait_count = alloc_shared_mem(sizeof(int));

    childern_count = alloc_shared_mem(sizeof(int));

    // create array of mutexes for childern groups
    {
        child_group = alloc_shared_mem(sizeof(sem_t)*opts.adults_count+1);

        for (int i = 0; i != opts.adults_count; i++)
            sem_init(&child_group[i], 1, 0);
    }

    last_adult_left = alloc_shared_mem(sizeof(bool));

    // index of adult and child
    *adult_id = 0;
    *child_id = 0;

    *adults_count = 0;
    *childern_count = 0;

    *last_adult_left = false;
  
    create_adult_generator();
    create_child_generator();

    // wait for TWO generating processes to end
    for (int i = 0; i != 2; i++)
    {
        wait(NULL);
    }

    exit(NO_ERROR);
}

void print_event(enum events event, int id)
{
  sem_wait(mutex);
  (*event_id)++;
  setbuf(file, NULL);
  switch (event) {
    case ADULT_STARTED: fprintf(file, "%i: A %i: started\n", *event_id, id); break;
    case ADULT_ENTER: fprintf(file, "%i: A %i: enter\n", *event_id, id); break;
    case ADULT_TRY_LEAVE: fprintf(file, "%i: A %i: trying to leave\n", *event_id, id); break;
    case ADULT_WAIT: fprintf(file, "%i: A %i: waiting: %i: %i\n", *event_id, id, *adults_count, *childern_count); break;
    case ADULT_LEAVE: fprintf(file, "%i: A %i: leave\n", *event_id, id); break;
    case ADULT_FINISHED: fprintf(file, "%i: A %i: finished\n", *event_id, id); break;

    case CHILD_STARTED: fprintf(file, "%i: C %i: started\n", *event_id, id); break;
    case CHILD_ENTER: fprintf(file, "%i: C %i: enter\n", *event_id, id); break;
    case CHILD_TRY_LEAVE: fprintf(file, "%i: C %i: trying to leave\n", *event_id, id); break;
    case CHILD_WAIT: fprintf(file, "%i: C %i: waiting: %i: %i\n", *event_id, id, *adults_count, *childern_count); break;
    case CHILD_LEAVE: fprintf(file, "%i: C %i: leave\n", *event_id, id); break;
    case CHILD_FINISHED: fprintf(file, "%i: C %i: finished\n", *event_id, id); break;
  }
  fflush(file);
  sem_post(mutex);
}

void destroy_sem(sem_t* sem)
{
    errno = 0;
    if (sem_destroy(sem) == -1)
    {
        error_and_die(SYS_CALL_FAIL, __func__, "sem_destroy: ", NULL);
    }

    errno = 0;
    if (munmap(sem, sizeof(sem_t)) == -1)
    {
        error_and_die(SYS_CALL_FAIL, __func__, "munmap: ", NULL);
    }
}

void dealloc_shared_mem(void* mem, size_t size)
{
    errno = 0;
    if (munmap(mem, size) == -1)
    {
        error_and_die(SYS_CALL_FAIL, __func__, "munmap: ", NULL);
    }
}

void error_and_die(int severity, ...)
{
    va_list ap;
    va_start(ap, severity);

    for (;;)
    {
        char* p = va_arg(ap, char*);
        if (p == NULL) break;
        fprintf(stderr, "%s ", p);
    }

    va_end(ap);

    fprintf(stderr, "\n");

    if (errno) perror("error");

    free_all_resources();

    if (severity) _exit(severity);
}

void* alloc_shared_mem(size_t sz)
{
    errno = 0;
    void* mem = mmap(0, sz, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_SHARED, -1, 0); 

    if (mem == (void*)-1)
    {
        error_and_die(SYS_CALL_FAIL, __func__, "mmap: ", NULL);
    }

    return mem;
}

sem_t* create_shared_sem(unsigned value)
{
    sem_t* sem = alloc_shared_mem(sizeof(*sem));

    errno = 0;
    if (sem_init(sem, 1, value) == -1)
    {
        error_and_die(SYS_CALL_FAIL, __func__, "sem_init: ", NULL);
    }

    return sem;
}

void free_all_resources(void)
{
    if (mutex)
        destroy_sem(mutex);
    if (mutex1)
        destroy_sem(mutex1);
    if (mutex2)
        destroy_sem(mutex2);
    if (adults_wait)
        destroy_sem(adults_wait);
    if (wait_till_finish)
        destroy_sem(wait_till_finish);

    if (event_id)
        dealloc_shared_mem(event_id, sizeof(*event_id));
    if (adult_id)
        dealloc_shared_mem(adult_id, sizeof(*adult_id));
    if (child_id)
        dealloc_shared_mem(child_id, sizeof(*child_id));
    if (child_wait_count)
        dealloc_shared_mem(child_wait_count, sizeof(*child_wait_count));
    if (adults_wait_count)
        dealloc_shared_mem(adults_wait_count, sizeof(*adults_wait_count));

    if (child_group)
    {
        child_group = alloc_shared_mem(sizeof(sem_t)*opts.adults_count);

        for (int i = 0; i != opts.adults_count; i++)
            sem_destroy(&child_group[i]);

        munmap(child_group, sizeof(sem_t)*opts.adults_count);
    }

    if (file)
        fclose(file);
}

// generate adult process
void create_adult_generator()
{
    pid_t adult_gen = -1;
    pid_t adult = -1;

    errno = 0;
    if ((adult_gen = fork()) < 0) 
    {
        error_and_die(SYS_CALL_FAIL, "fork adult_gen:", NULL);
    }
    else if (adult_gen == 0)
    {
        // generate adult processes
        for (int i = 0; i != opts.adults_count; i++)
        {
            // next adult process after some optional break
            if (opts.next_adult > 0)
                usleep(rand() % (opts.next_adult + 1));   

            errno = 0;
            if ((adult = fork()) < 0) 
            {
                error_and_die(SYS_CALL_FAIL, "fork adult:", NULL);
            } 
            // adult process
            else if (adult == 0) 
            {
                sem_wait(mutex1);
                    // adult ID for print
                    int aid = ++*adult_id;
                    // adult process started
                    print_event(ADULT_STARTED, aid);
                sem_post(mutex1);

                sem_wait(mutex1);
                    (*adults_count)++;
                    print_event(ADULT_ENTER, aid);
                    {
                        // release waiting childern
                        for (int i = 1; i <= *child_wait_count; i++)
                            sem_post(&child_group[*childern_count/3]);
                    }
                sem_post(mutex1);

                // adult works
                if (opts.adult_works > 0)
                    usleep(rand() % (opts.adult_works + 1));   

                sem_wait(mutex1);
                print_event(ADULT_TRY_LEAVE, aid);
                if (*childern_count > (*adults_count-1)*3)
                {
                    // waiting
                    print_event(ADULT_WAIT, aid);
                    sem_post(mutex1);

                    (*adults_wait_count)++;
                    sem_wait(adults_wait);

                    sem_wait(mutex1);
                    (*adults_wait_count)--;
                    (*adults_count)--;
                    // check whether is this adult last one of all
                    if (*adults_count == 0 && *adult_id == opts.adults_count)
                    {
                        *last_adult_left = true;

                        for (int i = 1; i <= *child_wait_count; i++)
                            sem_post(&child_group[*childern_count/3]);

                    }
                    print_event(ADULT_LEAVE, aid);
                    sem_post(mutex1);
                }
                else
                {
                    // continue and leave
                    (*adults_count)--;
                    print_event(ADULT_LEAVE, aid);
                    // check whether is this adult last one of all
                    if (*adults_count == 0 && *adult_id == opts.adults_count)
                    {
                        *last_adult_left = true;

                        for (int i = 1; i <= *child_wait_count; i++)
                            sem_post(&child_group[*childern_count/3]);
                    }
                    sem_post(mutex1);
                }

                // all processed ended, should it be released?
                if (*last_adult_left &&
                    *childern_count == 0 &&
                    *child_id == opts.childern_count
                    )
                {
                    sem_post(wait_till_finish);
                }
                else
                    sem_wait(wait_till_finish);

                sem_post(wait_till_finish);

                print_event(ADULT_FINISHED, aid);
                exit(NO_ERROR);
            }
        }

        // waiting for adult processes to end 
        if (adult != 0) 
        {
            for (int i = 0; i != opts.adults_count; i++)
            {
                wait(NULL);
            }
            exit(NO_ERROR);
        }
    }
}

// child processes generator
void create_child_generator()
{
    pid_t child_gen = -1;
    pid_t child = -1;

    errno = 0;
    if ((child_gen = fork()) < 0) 
    {
        error_and_die(SYS_CALL_FAIL, "fork child_gen:", NULL);
    }
    else if (child_gen == 0)
    {
        // generate childern processes
        for (int i = 0; i != opts.childern_count; i++)
        {
            // generate after some optional break
            if (opts.next_child > 0)
                usleep(rand() % (opts.next_child + 1)); 

            // child process
            errno = 0;
            if ((child = fork()) < 0) 
            {
                error_and_die(SYS_CALL_FAIL, "fork child:", NULL);
            } 
            else if (child == 0) 
            {
                sem_wait(mutex2);
                    // child ID for print
                    int cid = ++*child_id;
                    print_event(CHILD_STARTED, cid);
                sem_post(mutex2);

                // trying to enter
                sem_wait(mutex2);
                if (!*last_adult_left && ((*childern_count+1) > *adults_count*3))
                {
                    // waiting
                    (*child_wait_count)++;
                    print_event(CHILD_WAIT, cid);
                    sem_post(mutex2);

                    sem_wait(&child_group[*childern_count/3]);

                    sem_wait(mutex2);
                    (*childern_count)++;
                    (*child_wait_count)--;
                    print_event(CHILD_ENTER, cid);
                    sem_post(mutex2);
                }
                else
                {
                    // enter
                    (*child_wait_count)--;
                    (*childern_count)++;
                    print_event(CHILD_ENTER, cid);
                    sem_post(mutex2);
                }

                // child works
                if (opts.child_works > 0)
                    usleep(rand() % (opts.child_works + 1));   

                print_event(CHILD_TRY_LEAVE, cid);

                sem_wait(mutex2);
                // release adults if enough childern left
                (*childern_count)--;
                if (*childern_count / 3 || *childern_count == 0)
                {
                    for (int i = 1; i <= *adults_wait_count; i++)
                        sem_post(adults_wait);
                }
                print_event(CHILD_LEAVE, cid);
                sem_post(mutex2);

                // all processed ended, should it be released?
                if (*last_adult_left &&
                    *childern_count == 0 && 
                    *child_id == opts.childern_count
                   )
                {
                    sem_post(wait_till_finish);
                }
                else
                    sem_wait(wait_till_finish);

                sem_post(wait_till_finish);

                print_event(CHILD_FINISHED, cid);
                exit(NO_ERROR);
            }
        }

        // waiting for childern processes to end
        if (child != 0) 
        {
            for (int i = 0; i != opts.childern_count; i++)
            {
                wait(NULL);
            }
            exit(NO_ERROR);
        }
    }
}

// string to number and possibly error
struct Number get_num_or_err(char* str)
{
    char *err = NULL;

    errno = 0;
    long num = strtol(str, &err, 10);

    // check for possible buffer overflow
    if (errno == ERANGE) 
    {
        return (struct Number) {num, {"Number overflowed!"}};
    }

    // check if string contains only numbers
    if (*err != '\0') 
    {
        return (struct Number) {num, {"String is not a valid number!"}};
    }

    return (struct Number) {num, {NULL}};
}

void get_params(char **argv)
{
    // argument position in argv
    enum {
        ADULTS_COUNT = 1,
        CHILDERN_COUNT,
        ADULT_GEN_TIME,
        CHILD_GEN_TIME,
        ADULT_WORK_TIME,
        CHILD_WORK_TIME
    };

    // get adults count
    {
        struct Number n = get_num_or_err(argv[ADULTS_COUNT]);

        if (n.err.what)
        {
            error_and_die(BAD_INPUT, "Adults count:", n.err.what, NULL);
        } 

        if (n.num < 0) 
        {
            error_and_die(BAD_INPUT, "Adults count:", "Count must be greater than 0!", NULL);
        }

        opts.adults_count = n.num;
    }

    // get childern count
    {
        struct Number n = get_num_or_err(argv[CHILDERN_COUNT]);

        if (n.err.what)
        {
            error_and_die(BAD_INPUT, "Childern count:", n.err.what, NULL);
        } 

        if (n.num < 0)
        {
            error_and_die(BAD_INPUT, "Childern count:", "Count must be greater than 0!", NULL);
        }

        opts.childern_count = n.num;
    }

    // get next adult generate time
    {
        struct Number n = get_num_or_err(argv[ADULT_GEN_TIME]);

        if (n.err.what)
        {
            error_and_die(BAD_INPUT, "Adult generate time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error_and_die(BAD_INPUT, "Adult generate time:", "Time is out of range!", NULL);
        }

        opts.next_adult = n.num;
    }
                
    // get next child generate time
    {
        struct Number n = get_num_or_err(argv[CHILD_GEN_TIME]);

        if (n.err.what)
        {
            error_and_die(BAD_INPUT, "Child generate time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error_and_die(BAD_INPUT, "Child generate time:", "Time is out of range!", NULL);
        }

        opts.next_child = n.num;
    }

    // get adult work time
    {
        struct Number n = get_num_or_err(argv[ADULT_WORK_TIME]);

        if (n.err.what)
        {
            error_and_die(BAD_INPUT, "Adult work time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error_and_die(BAD_INPUT, "Adult work time:", "Time is out of range!", NULL);
        }

        opts.adult_works = n.num;
    }
                
    // get child work time
    {
        struct Number n = get_num_or_err(argv[CHILD_WORK_TIME]);

        if (n.err.what)
        {
            error_and_die(BAD_INPUT, "Child work time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error_and_die(BAD_INPUT, "Child work time:", "Time is out of range!", NULL);
        }

        opts.child_works = n.num;
    }
}
