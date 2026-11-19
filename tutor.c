#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>

#ifndef DAY_MINUTES
#define DAY_MINUTES 540
#endif

typedef struct {
    int id;
    pthread_t thread;
    sem_t called;
    sem_t finished;
    bool cancelled;
} Student;

// Waiting room
Student **line;
int seats, chance;
int head = 0, tail = 0, waiting = 0;
bool closing = false;

// Synchronization
sem_t queue_lock;
sem_t pair_ready;
sem_t seated;

// Handle interrupted semaphore waits
void down(sem_t *sem)
{
    while (sem_wait(sem) == -1) {
        if (errno != EINTR) {
            perror("sem_wait");
            exit(EXIT_FAILURE);
        }
    }
}

int positive_number(const char *s)
{
    char *end;
    errno = 0;
    long result = strtol(s, &end, 10);

    if (errno || end == s || *end != '\0' ||
        result < 1 || result > INT_MAX)
        return -1;

    return (int)result;
}

void *student(void *arg)
{
    Student *s = (Student *)arg;

    printf("Student %d: Arriving at the student center.\n",
           s->id);

    down(&queue_lock);

    if (closing || waiting == seats){
 printf("Student %d: No waiting chairs, leaving.\n",
               s->id);
        sem_post(&queue_lock);
        return NULL;
    }

    // Enter FIFO queue
    line[tail] = s;
    tail = (tail + 1) % seats;
    waiting++;

    printf("Student %d: Found a seat, waiting.\n", s->id);

    if (waiting == 2)
        sem_post(&pair_ready);

    sem_post(&queue_lock);

    // Wait to be called
    down(&s->called);

    if (s->cancelled) {
        printf("Student %d: Tutor left, going home.\n",
               s->id);
        return NULL;
    }

    printf("Student %d: Sitting at tutoring table.\n",
           s->id);

    sem_post(&seated);

    // Wait until tutoring ends
    down(&s->finished);

    printf("Student %d: Time to go!\n", s->id);
    return NULL;
}

// Called with queue_lock held
void dismiss_waiting(void)
{
    while (waiting > 0) {
        Student *s = line[head];
        head = (head + 1) % seats;
        waiting--;

        s->cancelled = true;
        sem_post(&s->called);
    }
}

void *tutor(void *arg)
{
    (void)arg;

    printf("Tutor: Beginning my workday with %d seats.\n",
           seats);
    printf("Tutor: Not enough students, doomscrolling.\n");

    for (;;) {
        down(&pair_ready);
        down(&queue_lock);

        // End workday; release remaining students
        if (closing) {
            printf("Tutor: That's enough for one day, good night!\n");
	    dismiss_waiting();
            sem_post(&queue_lock);
            break;
        }

        if (waiting < 2) {
            sem_post(&queue_lock);
            continue;
        }

        // Select two longest-waiting students
        Student *a = line[head];
        head = (head + 1) % seats;

        Student *b = line[head];
        head = (head + 1) % seats;

        waiting -= 2;

        if (waiting >= 2)
            sem_post(&pair_ready);

        sem_post(&queue_lock);

        int duration = 5 + rand() % 26;

        printf("Tutor: Students %d and %d, tutoring "
               "for %d minutes.\n",
               a->id, b->id, duration);

        sem_post(&a->called);
        sem_post(&b->called);

        // Barrier: wait until both students sit
        down(&seated);
        down(&seated);

        sleep((unsigned int)duration);

        printf("Tutor: Tutoring done!\n");

        sem_post(&a->finished);
        sem_post(&b->finished);

        down(&queue_lock);
        bool finished_day = closing;
        bool need_pair = waiting < 2;
        sem_post(&queue_lock);

        if (need_pair && !finished_day)
            printf("Tutor: Not enough students, "
                   "doomscrolling.\n");
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 3 ||
        (seats = positive_number(argv[1])) < 1 ||
        (chance = positive_number(argv[2])) < 1) {
        fprintf(stderr, "Usage: %s N C\n", argv[0]);
        return EXIT_FAILURE;
    }

    line = calloc((size_t)seats, sizeof(*line));
    if (!line) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    sem_init(&queue_lock, 0, 1);
    sem_init(&pair_ready, 0, 0);
    sem_init(&seated, 0, 0);

    Student *all[DAY_MINUTES] = {0};
    int count = 0;
    pthread_t tutor_thread;

    srand((unsigned int)time(NULL));

    if (pthread_create(&tutor_thread, NULL,
                       tutor, NULL) != 0) {
        fprintf(stderr, "Cannot create tutor thread\n");
        return EXIT_FAILURE;
    }

    // One second represents one simulated minute
    for (int minute = 0; minute < DAY_MINUTES; minute++) {

        // Student arrival probability = 1/C
        if (rand() % chance == 0) {
            Student *s = calloc(1, sizeof(*s));

            if (!s) {
                perror("calloc student");
                break;
            }

            s->id = count + 1;
            sem_init(&s->called, 0, 0);
            sem_init(&s->finished, 0, 0);

            if (pthread_create(&s->thread, NULL,
                               student, s) != 0) {
                fprintf(stderr, "Cannot create student\n");
                sem_destroy(&s->called);
                sem_destroy(&s->finished);
                free(s);
                break;
            }

            all[count++] = s;
        }

        sleep(1);
    }

    // Nine-hour workday has ended
    down(&queue_lock);
    closing = true;
    sem_post(&queue_lock);

    // Wake tutor if waiting for another pair
    sem_post(&pair_ready);

    pthread_join(tutor_thread, NULL);

    // Wait for all student threads to finish
    for (int i = 0; i < count; i++) {
        pthread_join(all[i]->thread, NULL);
        sem_destroy(&all[i]->called);
        sem_destroy(&all[i]->finished);
        free(all[i]);
    }

    sem_destroy(&seated);
    sem_destroy(&pair_ready);
    sem_destroy(&queue_lock);
    free(line);

    return EXIT_SUCCESS;
}
