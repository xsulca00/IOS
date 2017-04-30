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

/* konstanty pro rozliseni tisku udalosti */
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

/* Error values */
enum {
    NO_ERROR =  0,
    BAD_INPUT = 1, 
    SYS_CALL_FAIL = 2
};

struct Number {
    /* number */
    long num;
    /* possible error description or NULL*/
    struct err {
        const char* const what;
    } err;
};

/* prototypy funkci */
void print_event(enum events event, int);	// tiskne udalosti do souboru proj2.out dle zadani
void get_params(char **argv);		// kontrola vstupnich parametru z terminalu
void* alloc_shared_mem(size_t);
sem_t* create_sem(unsigned);        // create semaphore with initial value
void destroy_sem(sem_t*);
struct Number get_num_or_err(char* str);
void dealloc_shared_mem(void*, size_t);
void free_all_resources(void);
void create_adult_generator(void);
void create_child_generator(void);

// print error with prefix e.g. "Adults count: ...", NULL serves as end of strings
void error_and_die(int, ...);

/* sdilena pamet mezi procesy*/

int* event_id;			// pocitadlo akci
int* adult_id;		    // celociselny identifikator pro procesy adult
int* child_id;		    // celociselny identifikator pro procesy child
int* adults_count;      // poc
int* childern_count;    // pocet deti pritomnych v centru
bool* last_adult_left;

/* sdilene semafory mezi procesy */
sem_t *mutex;		// mutex do kriticke oblasti pri zapisu do souboru
sem_t *mutex1;		// mutex do kriticke oblasti pri zapisu do souboru
sem_t *mutex2;		// mutex do kriticke oblasti v procesu pasazera
sem_t *wait_adults_till_finish;     // adult procesy cekaji na ukonceni
sem_t *wait_childern_till_finish;   // child procesy cekaji na ukonceni
sem_t* adults_wait;
sem_t* childern_wait;
sem_t* child_group;

/* ukazatel na otevreny soubor */
FILE *file;      

/* promenne pro uchovani hodnot z parametru spusteneho programu */

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

    /* alokace sdilene pameti mezi procesy pro semafory */
    mutex = create_sem(1);
    mutex1 = create_sem(1);
    mutex2 = create_sem(1);
    wait_childern_till_finish = create_sem(0);
    wait_adults_till_finish = create_sem(0);
    adults_wait = create_sem(0);
    childern_wait = create_sem(0);

    /* alokace sdilene pameti pro sdilene promenne */
    event_id = alloc_shared_mem(sizeof(int));
    adult_id = alloc_shared_mem(sizeof(int));
    child_id = alloc_shared_mem(sizeof(int));
    adults_count = alloc_shared_mem(sizeof(int));
    childern_count = alloc_shared_mem(sizeof(int));
    {
        child_group = alloc_shared_mem(sizeof(sem_t)*opts.adults_count+1);

        for (int i = 0; i != opts.adults_count; i++)
            sem_init(&child_group[i], 1, 0);
    }
    last_adult_left = alloc_shared_mem(sizeof(bool));

    // index od adult and child begins from one
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
        printf("GEN %i finished!\n", i);
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
    // TODO: dodelat waiting
    case ADULT_WAIT: fprintf(file, "%i: A %i: waiting: %i: %i\n", *event_id, id, *adults_count, *childern_count); break;
    case ADULT_LEAVE: fprintf(file, "%i: A %i: leave\n", *event_id, id); break;
    case ADULT_FINISHED: fprintf(file, "%i: A %i: finished\n", *event_id, id); break;

    case CHILD_STARTED: fprintf(file, "%i: C %i: started\n", *event_id, id); break;
    case CHILD_ENTER: fprintf(file, "%i: C %i: enter\n", *event_id, id); break;
    case CHILD_TRY_LEAVE: fprintf(file, "%i: C %i: trying to leave\n", *event_id, id); break;
    // TODO: dodelat waiting
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

sem_t* create_sem(unsigned value)
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
    if (wait_adults_till_finish)
        destroy_sem(wait_adults_till_finish);
    if (wait_childern_till_finish)
        destroy_sem(wait_childern_till_finish);
    if (adults_wait)
        destroy_sem(adults_wait);
    if (childern_wait)
        destroy_sem(childern_wait);

    if (event_id)
        dealloc_shared_mem(event_id, sizeof(*event_id));
    if (adult_id)
        dealloc_shared_mem(adult_id, sizeof(*adult_id));
    if (child_id)
        dealloc_shared_mem(child_id, sizeof(*child_id));
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
                // get pid
                sem_wait(mutex1);
                int aid = ++*adult_id;
                sem_post(mutex1);

                // adult process started
                print_event(ADULT_STARTED, aid);
                usleep(200);

                sem_wait(mutex1);
                (*adults_count)++;
                print_event(ADULT_ENTER, aid);
                sem_post(mutex1);
                usleep(200);

                sem_post(&child_group[aid]);

                // adult works
                if (opts.adult_works > 0)
                    usleep(rand() % (opts.adult_works + 1));   

                print_event(ADULT_TRY_LEAVE, aid);
                usleep(200);

                sem_wait(mutex1);
                /*
                sem_wait(mutex);
                fprintf(file, "A %i: %i > (%i-1)*3\n", aid, *childern_count, *adults_count);
                sem_post(mutex);
                */
                if (*childern_count > (*adults_count-1)*3)
                {
                    // waiting
                    print_event(ADULT_WAIT, aid);
                    sem_post(mutex1);

                    sem_wait(adults_wait);
                }
                else
                {
                    (*adults_count)--;
                    print_event(ADULT_LEAVE, aid);
                    sem_post(mutex1);
                    usleep(200);
                }

                /*
                if (aid >= opts.adults_count)
                    sem_post(wait_adults_till_finish);
                else
                    sem_wait(wait_adults_till_finish);
                    */

                print_event(ADULT_FINISHED, aid);
                exit(NO_ERROR);
            }
        }

        // waiting for adult processes to end 
        if (adult != 0) 
        {
            for (int i = 0; i != opts.adults_count; i++)
            {
                if (i == opts.adults_count)
                    *last_adult_left = true;
                wait(NULL);
                printf("ADULT: %i finished!\n", i);
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
                int cid = ++*child_id;
                // child process started
                print_event(CHILD_STARTED, cid);
                sem_post(mutex2);

                usleep(200);

                // trying to enter
                usleep(200);
                sem_wait(mutex2);
                /*
                    sem_wait(mutex);
                    fprintf(file, "C %i: (%i+1) > %i*3\n", cid, *childern_count, *adults_count);
                    sem_post(mutex);
                    */
                usleep(200);
                if ((*childern_count+1) > *adults_count*3 && !last_adult_left)
                {
                    // waiting
                    print_event(CHILD_WAIT, cid);
                    printf("WAIT C: %i\n", *childern_count);
                    sem_post(mutex2);

                    sem_wait(&child_group[*childern_count/4]);
                }
                else
                {
                    (*childern_count)++;
                    print_event(CHILD_ENTER, cid);
                    sem_post(mutex2);
                    usleep(200);
                }

                // child works
                if (opts.child_works > 0)
                    usleep(rand() % (opts.child_works + 1));   

                print_event(CHILD_TRY_LEAVE, cid);
                usleep(200);


                sem_wait(mutex2);
                (*childern_count)--;
                if (*childern_count / 3 || *childern_count == 0)
                {
                    printf("C: %i\n", *childern_count);
                    sem_post(adults_wait);
                }
                print_event(CHILD_LEAVE, cid);
                sem_post(mutex2);

                /*
                if (cid >= opts.childern_count)
                    sem_post(wait_childern_till_finish);
                else
                    sem_wait(wait_childern_till_finish);
                    */

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
                printf("CHILD: %i finished!\n", i);
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
