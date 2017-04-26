#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
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
  PAS_STARTED = 0,
  PAS_BOARD,
  PAS_BOARD_ORDER,
  PAS_BOARD_LAST,
  PAS_UNBOARD,
  PAS_UNBOARD_ORDER,
  PAS_UNBOARD_LAST,
  PAS_FINISHED,
  CAR_STARTED,
  CAR_LOAD,
  CAR_RUN,
  CAR_UNLOAD,  
  CAR_FINISHED
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
void print_event(enum events event);	// tiskne udalosti do souboru proj2.out dle zadani
void destroy_semaphores(void);		// zrusi alokovane semafory
void get_params(char **argv);		// kontrola vstupnich parametru z terminalu
void* alloc_shared_mem(size_t);
sem_t* create_sem(unsigned);
struct Number get_num_or_err(char* str);
void free_all_resources(void);

// print error with prefix e.g. "Adults count: ..."
void error(int, ...);

/* sdilena pamet mezi procesy*/
int *pas_id;		// generuje interni identifikator pro proces pasazera v rozsahu <1, P>
int *j;			// pocitadlo akci
int *car_id;		// generuej interni identifikator pro cozik
int *actual_capacity;	// aktualni kapacita voziku slouzici k vypsani poradi nastupovani/vystupovani pasazeru

/* sdilene semafory mezi procesy */
sem_t *board;		// semafor ridici nastupovani	
sem_t *unboard;		// semafor ridici vystupovani	
sem_t *mutex2;		// mutex do kriticke oblasti v procesu pasazera
sem_t *mutex;		// mutex do kriticke oblasti pri zapisu do souboru
sem_t *load;		// semafor davajici povel k nastupovani do voziku
sem_t *unload;		// semafor davajici povel k vystupovani z voziku
sem_t *wait_till_finish;// semafor blokujici procesy, ktere se nakonci radne ukonci a potom hlavni proces

/* lokalni identifikator procesu <1, MAX_PROCESS> */
int local_pasid;

/* ukazatel na otevreny soubor */
FILE *file;      

/* promenne pro uchovani hodnot z parametru spusteneho programu */

struct options {
    long adults_count;		// pocet pasazeru
    long childern_count;		// kapacita voziku
    long next_adult;	// doba, za kterou se vygeneruje novy proces pasazera
    long next_child;	// doba, za kterou se vygeneruje novy proces pasazera
    long adult_works;	// doba, za kterou se vygeneruje novy proces pasazera
    long child_works;	// doba, za kterou se vygeneruje novy proces pasazera
} opts;

long passenger_count;
long generate_next_passanger;
long carriage_capacity;
long duration_of_ride;

int main(int argc, char **argv)
{   
  if (argc != 6)
  {
     error(BAD_INPUT, "Usage:", argv[0], "A C AGT CGT AWT CWT", NULL);
  } 
  
  get_params(argv);
  
  errno = 0;	
  file = fopen("proj2.out", "w");
  
  if (file == NULL)
  {
      error(SYS_CALL_FAIL, "fopen:", NULL);
  }

  pid_t car = -1;		// id procesu voziku v ramci systemu
  pid_t passenger = -1;	// id procesu pasazera v ramci systemu
  pid_t help_process_pid = -1;	// id pomocneho procesu pro generovani pasazeru v ramci systemu
  pid_t *psn_pids;		// pole pro ukladani id procesu pasazeru v ramci systemu 
  
  /* alokace sdilene pameti mezi procesy pro semafory */
  board = create_sem(0);
  mutex = create_sem(1);
  mutex2 = create_sem(1);
  load = create_sem(0);
  unload = create_sem(0); 
  wait_till_finish = create_sem(0); 
  unboard = create_sem(0);
  
  /* alokace sdilene pameti pro sdilene promenne */
  pas_id = alloc_shared_mem(sizeof(int));
  j = alloc_shared_mem(sizeof(int));
  actual_capacity = alloc_shared_mem(sizeof(int));
  car_id = alloc_shared_mem(sizeof(int));
  psn_pids = alloc_shared_mem(sizeof(pid_t)*passenger_count);
  
  // pomocny proces generuje procesy pasazeru
  // pokud se nepodarilo vytvorit proces vykonej patricne kroky
  errno = 0;
  if ((help_process_pid = fork()) < 0) {
     error(SYS_CALL_FAIL, "fork help_process_id:", NULL);
  } else if (help_process_pid == 0) {
    for (int i = 1; i <= passenger_count; i++) {
        if (generate_next_passanger > 0)
          usleep(rand() % generate_next_passanger + 1); // uspime v nahodnem intervalu <0,PT>
        // pokud se nepodarilo vytvorit proces vykonej patricne kroky
        errno = 0;
        if ((passenger = fork()) < 0) {
             error(SYS_CALL_FAIL, "fork passenger:", NULL);
        } else if (passenger == 0) {
          break;
        }
    }
    // pokud se vygenerovali vsici pasazeri tak cekat dokonce, nez se ukonci ostatni procesy
    if (passenger != 0) {
        // ceka na ukonceni procesu pasazeru
        for (int i = 0; i < passenger_count; i++)
            waitpid(psn_pids[i], NULL, 0);
        // aktivuj ukonceni dalsiho procesu
        sem_post(wait_till_finish); 
        exit(0);
    }
  }
  
   // proces pasazera
  if (passenger == 0) {
    sem_wait(mutex2);			// vstup do kriticke oblasti (inkrementace sdilene promenne)
    local_pasid = ++*pas_id;		// lokalni cislo procesu = unikatni cislo pro jeho identifikaci <1, P>
    psn_pids[local_pasid-1] = getpid();	// ulozime id procesu v ramci systemu, aby mohl pomocny proces cekat na pasazery nez se ukonci
    
    /* tisk udalosti, vstup do kriticke oblasti */
    sem_wait(mutex);
    print_event(PAS_STARTED);    
    sem_post(mutex);
    /* vystup z kriticke oblasti po zapisu do souboru */
    
    sem_post(mutex2); 
    
    // pasazer ceka na povel k nastupovani
    sem_wait(load);
  
    // vstup do kriticke oblasti (promenna *actual_capacity)
    sem_wait(mutex2);
    // pokud jsou splneny podminky nastupovani, pasazeri zacinaji nastupovat
    if (*actual_capacity <= carriage_capacity) {   
      print_event(PAS_BOARD);
      (*actual_capacity)++;   
      // pokud je pasazer posledni, vypis udalost
      if (*actual_capacity == carriage_capacity) {
	sem_wait(mutex);
	print_event(PAS_BOARD_LAST);
	sem_post(mutex);
	*actual_capacity = 0;
	// oznam voziku, ze nastupovani je u konce
	sem_post(board);	
      } else {
	sem_wait(mutex);
	print_event(PAS_BOARD_ORDER);
	sem_post(mutex);
      }
      sem_post(mutex2);          
      // pasazer ceka, dokud vozik nevyda povel k vystupovani
      sem_wait(unload);
    }
    
    // vozik vyvolal operaci unload, pasazeri mohou vystupovat
    sem_wait(mutex2);
    if (*actual_capacity <= carriage_capacity) {   
      sem_wait(mutex);
      print_event(PAS_UNBOARD);
      sem_post(mutex);
      (*actual_capacity)++;  
      // pokud je posledni pasazer, vypis patricne informace
      if (*actual_capacity == carriage_capacity) {
	  sem_wait(mutex);
	  print_event(PAS_UNBOARD_LAST);
	  sem_post(mutex);
	  *actual_capacity = 0;
	  // oznam voziku, ze vsichni pasazeri vystoupili
	  sem_post(unboard);
      } else {
	  sem_wait(mutex);
	  print_event(PAS_UNBOARD_ORDER);
	  sem_post(mutex);
      }
    }
    sem_post(mutex2);
    // pasazer ceka na spolecne ukonceni procesu
    sem_wait(wait_till_finish);
    // aktivuj ukonceni dalsiho procesu
    sem_post(wait_till_finish);
    sem_wait(mutex2);
    print_event(PAS_FINISHED);
    sem_post(mutex2);
    exit(0);
  }
  
  // pokud se nepodarilo vytvorit proces vykonej patricne kroky
  errno = 0;
  if ((car = fork()) < 0) {
     error(SYS_CALL_FAIL, "fork car:", NULL);
  } else if (car == 0) {
    
    (*car_id)++; // unikatni cislo voziku
    
    sem_wait(mutex);
    print_event(CAR_STARTED);	// vozik zacal fungovat
    sem_post(mutex);
    
    // vozik pracuje v cyklu
    for (int i = 1; i <= passenger_count/carriage_capacity; i++) {   
      sem_wait(mutex);
      print_event(CAR_LOAD);
      sem_post(mutex);
      
      // proces vyvola operaci load (zahajeni nastupovani), semaforem projde tolik procesu, kolik je kapacita voziku
      for (int i = 1; i <= carriage_capacity; i++)
	sem_post(load);
      
      // proces ceka, dokud nenastoupi vsichni pasazeri
      sem_wait(board); 
      
      // jakmile je vozik plny, proces vyvola operaci run
      sem_wait(mutex);
      print_event(CAR_RUN);     
      sem_post(mutex);
      if (duration_of_ride > 0)
	usleep(rand() % duration_of_ride + 1); // uspi se na nahodnou dobu z intervalu <0, RT>
      
      // proces vyvola operaci unload
      sem_wait(mutex);
      print_event(CAR_UNLOAD);
      sem_post(mutex);
      
      // po probuzeni vyvola proces operaci unload (zahajeni vystupovani), semaforem projde tolik procesu, kolik je kapacita voziku 
      for (int i = 1; i <= carriage_capacity; i++)
	sem_post(unload);

      // proces nemuze zahajit operaci load, dokud nevystoupi vsichni pasazeri 
      sem_wait(unboard);
    }

    // vozik dokoncil praci, aktivuj ukonceni vsech procesu
    sem_post(wait_till_finish);
    // ceka na ukonceni pomocneho procesu pro generovani pasazeru
    waitpid(help_process_pid, NULL, 0);
    sem_wait(mutex);
    print_event(CAR_FINISHED);
    sem_post(mutex);
    exit(0);
  }    
  
  // hlavni proces ceka na ukonceni vsech procesu pak se ukonci sam
  // hlavni proces ceka na ukonceni pomocneho procesu
  waitpid(help_process_pid, NULL, 0);
  // hlavni proces ceka na ukonceni voziku
  waitpid(car, NULL, 0); 

  // vycisti alokovane semafory
  destroy_semaphores();
  //uvolni sdilenou pamet
  munmap(board, sizeof(sem_t));
  munmap(mutex, sizeof(sem_t));
  munmap(unboard, sizeof(sem_t));
  munmap(mutex2, sizeof(sem_t));
  munmap(load, sizeof(sem_t));
  munmap(wait_till_finish, sizeof(sem_t));
  munmap(unload, sizeof(sem_t));
  // zavri soubor
  fclose(file);
  
  exit(0);
}


void print_event(enum events event)
{
  (*j)++;
  setbuf(file, NULL);
  switch (event) {
    case PAS_STARTED: fprintf(file, "%i: P %i: started\n", *j, local_pasid); break;
    case PAS_BOARD: fprintf(file, "%i: P %i: board\n", *j, local_pasid); break;
    case PAS_BOARD_ORDER: fprintf(file, "%i: P %i: board order %i\n", *j, local_pasid, *actual_capacity); break;
    case PAS_BOARD_LAST: fprintf(file, "%i: P %i: board last\n", *j, local_pasid); break;
    case PAS_UNBOARD: fprintf(file, "%i: P %i: unboard\n", *j, local_pasid); break;
    case PAS_UNBOARD_ORDER: fprintf(file, "%i: P %i: unboard order %i\n", *j, local_pasid, *actual_capacity); break;
    case PAS_UNBOARD_LAST: fprintf(file, "%i: P %i: unboard last\n", *j, local_pasid); break;
    case PAS_FINISHED: fprintf(file, "%i: P %i: finished\n", *j, local_pasid); break;
    case CAR_STARTED: fprintf(file, "%i: C %i: started\n", *j, *car_id); break;
    case CAR_LOAD: fprintf(file, "%i: C %i: load\n", *j, *car_id); break;
    case CAR_RUN: fprintf(file, "%i: C %i: run\n", *j, *car_id); break;
    case CAR_UNLOAD: fprintf(file, "%i: C %i: unload\n", *j, *car_id); break;
    case CAR_FINISHED: fprintf(file, "%i: C %i: finished\n", *j, *car_id); break;
  }
  fflush(file);
}

void destroy_semaphores(void)
{
  errno = 0;
  
  if (
    sem_destroy(board) == -1 ||
    sem_destroy(mutex) == -1 ||
    sem_destroy(unboard) == -1 ||
    sem_destroy(mutex2) == -1 ||
    sem_destroy(load) == -1 ||
    sem_destroy(wait_till_finish) == -1 ||
    sem_destroy(unload) == -1
  ) {
    perror("destroy_semaphores");
    _exit(SYS_CALL_FAIL);
  }
}

void error(int severity, ...)
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

    if (severity) _exit(severity);
}

void* alloc_shared_mem(size_t sz)
{
    errno = 0;
    void* mem = mmap(0, sz, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_SHARED, -1, 0); 

    if (mem == (void*)-1)
    {
        perror("mmap");
        _exit(SYS_CALL_FAIL);
    }

    return mem;
}

sem_t* create_sem(unsigned value)
{
    sem_t* sem = alloc_shared_mem(sizeof(*sem));

    errno = 0;
    if (sem_init(sem, 1, value) == -1)
    {
        perror("sem_init");
        _exit(SYS_CALL_FAIL);
    }

    return sem;
}

void free_all_resources(void)
{
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
            error(BAD_INPUT, "Adults count:", n.err.what, NULL);
        } 

        if (n.num < 0) 
        {
            error(BAD_INPUT, "Adults count:", "Count must be greater than 0!", NULL);
        }

        opts.adults_count = n.num;
    }

    // get childern count
    {
        struct Number n = get_num_or_err(argv[CHILDERN_COUNT]);

        if (n.err.what)
        {
            error(BAD_INPUT, "Childern count:", n.err.what, NULL);
        } 

        if (n.num < 0)
        {
            error(BAD_INPUT, "Childern count:", "Count must be greater than 0!", NULL);
        }

        opts.childern_count = n.num;
    }

    // get next adult generate time
    {
        struct Number n = get_num_or_err(argv[ADULT_GEN_TIME]);

        if (n.err.what)
        {
            error(BAD_INPUT, "Adult generate time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error(BAD_INPUT, "Adult generate time:", "Time is out of range!", NULL);
        }

        opts.next_adult = n.num;
    }
                
    // get next child generate time
    {
        struct Number n = get_num_or_err(argv[CHILD_GEN_TIME]);

        if (n.err.what)
        {
            error(BAD_INPUT, "Child generate time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error(BAD_INPUT, "Child generate time:", "Time is out of range!", NULL);
        }

        opts.next_child = n.num;
    }

    // get adult work time
    {
        struct Number n = get_num_or_err(argv[ADULT_WORK_TIME]);

        if (n.err.what)
        {
            error(BAD_INPUT, "Adult work time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error(BAD_INPUT, "Adult work time:", "Time is out of range!", NULL);
        }

        opts.adult_works = n.num;
    }
                
    // get child work time
    {
        struct Number n = get_num_or_err(argv[CHILD_WORK_TIME]);

        if (n.err.what)
        {
            error(BAD_INPUT, "Child work time:", n.err.what, NULL);
        } 

        if (!(n.num >= 0 && n.num < 5001))
        {
            error(BAD_INPUT, "Child work time:", "Time is out of range!", NULL);
        }

        opts.child_works = n.num;
    }
}
