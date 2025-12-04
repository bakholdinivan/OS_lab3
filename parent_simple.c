#define _XOPEN_SOURCE 700

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <semaphore.h>

#define BUF_SIZE 256
#define MAX_RESULTS 1024

typedef struct {
    int count;
    float results[MAX_RESULTS];
} SharedOutput;

static ssize_t safe_write(int fd, const void *buf, size_t count) {
    const char *p = buf;
    size_t left = count;
    
    while (left > 0) {
        ssize_t written = write(fd, p, left);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += written;
        left -= written;
    }
    return count;
}

ssize_t read_line(int fd, char *buf, size_t max_len) {
    size_t total = 0;
    
    while (total < max_len - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        
        if (n == 0) break;
        if (c == '\n') break;
        
        buf[total++] = c;
    }
    
    buf[total] = '\0';
    return total;
}

void write_float(float num) {
    char buf[64];
    int pos = 0;

    buf[pos++] = 'S'; buf[pos++] = 'u'; buf[pos++] = 'm';
    buf[pos++] = ':'; buf[pos++] = ' ';

    if (num < 0) {
        buf[pos++] = '-';
        num = -num;
    }

    int whole = (int)num;
    if (whole == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[20];
        int tp = 0;
        while (whole > 0) {
            tmp[tp++] = '0' + (whole % 10);
            whole /= 10;
        }
        while (tp > 0) {
            buf[pos++] = tmp[--tp];
        }
    }

    buf[pos++] = '.';

    int frac = (int)((num - (int)num) * 100 + 0.5);
    if (frac >= 100) frac = 99;
    buf[pos++] = '0' + (frac / 10);
    buf[pos++] = '0' + (frac % 10);
    buf[pos++] = '\n';

    safe_write(STDOUT_FILENO, buf, pos);
}

/* Функция обработки данных (будет выполняться в child) */
void process_data(const char *input_data, size_t input_size, SharedOutput *output) {
    float sum = 0.0;
    const char *p = input_data;
    const char *end = input_data + input_size;
    int line_has_data = 0;
    
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        
        if (p >= end) break;
        
        if (*p == '\n') {
            if (line_has_data) {
                if (output->count < MAX_RESULTS) {
                    output->results[output->count++] = sum;
                }
                sum = 0.0;
                line_has_data = 0;
            }
            p++;
            continue;
        }
        
        char *endptr;
        errno = 0;
        float val = strtof(p, &endptr);
        
        if (endptr == p) {
            safe_write(STDERR_FILENO, "Parse error\n", 12);
            _exit(1);
        }
        
        if (errno == ERANGE) {
            safe_write(STDERR_FILENO, "Number too large\n", 17);
            _exit(1);
        }
        
        sum += val;
        line_has_data = 1;
        p = endptr;
    }
    
    if (line_has_data) {
        if (output->count < MAX_RESULTS) {
            output->results[output->count++] = sum;
        }
    }
}

int main() {
    char filename[BUF_SIZE] = {0};
    
    safe_write(STDOUT_FILENO, "Enter filename: ", 16);
    
    ssize_t len = read_line(STDIN_FILENO, filename, BUF_SIZE);
    
    if (len <= 0) {
        safe_write(STDERR_FILENO, "Error: empty filename\n", 22);
        return 1;
    }

    int input_fd = open(filename, O_RDONLY);
    if (input_fd < 0) {
        safe_write(STDERR_FILENO, "Cannot open file\n", 17);
        return 1;
    }

    struct stat st;
    if (fstat(input_fd, &st) < 0) {
        safe_write(STDERR_FILENO, "Cannot stat file\n", 17);
        close(input_fd);
        return 1;
    }

    void *input_map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, input_fd, 0);
    if (input_map == MAP_FAILED) {
        safe_write(STDERR_FILENO, "mmap input failed\n", 18);
        close(input_fd);
        return 1;
    }
    close(input_fd);

    char output_file[] = "/tmp/os_lab3_output_XXXXXX";
    int output_fd = mkstemp(output_file);
    if (output_fd < 0) {
        safe_write(STDERR_FILENO, "Cannot create output file\n", 26);
        munmap(input_map, st.st_size);
        return 1;
    }

    if (ftruncate(output_fd, sizeof(SharedOutput)) < 0) {
        safe_write(STDERR_FILENO, "ftruncate failed\n", 17);
        munmap(input_map, st.st_size);
        close(output_fd);
        unlink(output_file);
        return 1;
    }

    SharedOutput *output_map = mmap(NULL, sizeof(SharedOutput),
                                    PROT_READ | PROT_WRITE, 
                                    MAP_SHARED, output_fd, 0);
    if (output_map == MAP_FAILED) {
        safe_write(STDERR_FILENO, "mmap output failed\n", 19);
        munmap(input_map, st.st_size);
        close(output_fd);
        unlink(output_file);
        return 1;
    }
    close(output_fd);

    memset(output_map, 0, sizeof(SharedOutput));

    char sem_name[] = "/os_lab3_simple_sem";
    sem_unlink(sem_name);
    
    sem_t *sem = sem_open(sem_name, O_CREAT | O_EXCL, 0600, 0);
    if (sem == SEM_FAILED) {
        safe_write(STDERR_FILENO, "sem_open failed\n", 16);
        munmap(input_map, st.st_size);
        munmap(output_map, sizeof(SharedOutput));
        unlink(output_file);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        safe_write(STDERR_FILENO, "fork failed\n", 12);
        sem_close(sem);
        sem_unlink(sem_name);
        munmap(input_map, st.st_size);
        munmap(output_map, sizeof(SharedOutput));
        unlink(output_file);
        return 1;
    }
    
    if (pid == 0) {
        /* CHILD - БЕЗ EXECV! */
        
        /* Обрабатываем данные напрямую */
        process_data((const char *)input_map, st.st_size, output_map);
        
        /* Сигнализируем parent */
        sem_post(sem);
        sem_close(sem);
        
        _exit(0);
    }
    
    /* PARENT */
    sem_wait(sem);
    
    safe_write(STDOUT_FILENO, "Results:\n", 9);
    
    for (int i = 0; i < output_map->count && i < MAX_RESULTS; i++) {
        write_float(output_map->results[i]);
    }
    
    wait(NULL);
    
    sem_close(sem);
    sem_unlink(sem_name);
    munmap(input_map, st.st_size);
    munmap(output_map, sizeof(SharedOutput));
    unlink(output_file);
    
    return 0;
}