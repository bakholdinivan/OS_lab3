/*
 * ============================================================================
 * Лабораторная работа №3 - Дочерний процесс (child)
 * 
 *
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L              // Включает POSIX.1-2008 стандарт (семафоры, mmap)
#define _XOPEN_SOURCE 700                    // Включает X/Open 7 расширения

#include <unistd.h>                          // read(), write(), close()
#include <fcntl.h>                           // open(), O_RDWR
#include <sys/mman.h>                        // mmap(), munmap(), msync()
#include <sys/types.h>                       // size_t, ssize_t
#include <semaphore.h>                       // sem_t, sem_open(), sem_close(), sem_wait(), sem_post()
#include <stdlib.h>                          // strtof() - преобразование строки в float, _exit()
#include <string.h>                          // Не используем напрямую, но нужен для некоторых деклараций
#include <errno.h>                           // errno, EINTR, ERANGE

#define MMAP_SIZE 8192                       
#define SEM_READY "/os_lab3_sem_ready"       
#define SEM_DONE "/os_lab3_sem_done"         

/* СТРУКТУРА ДОЛЖНА БЫТЬ ИДЕНТИЧНА parent.c! */
typedef struct {
    size_t data_size;                        // Размер данных (8 байт на x64)
    char data[MMAP_SIZE - sizeof(size_t)];  
} SharedData;

/* safe_write - аналогична parent.c */
static ssize_t safe_write(int fd, const void *buf, size_t count) {
    const char *p = buf;                     // Указатель на текущую позицию
    size_t left = count;                     // Оставшиеся байты
    
    while (left > 0) {                       // Цикл до полной записи
        ssize_t written = write(fd, p, left); // Попытка записи
        
        if (written < 0) {                   // Ошибка
            if (errno == EINTR) continue;    // Прерван сигналом - повторить
            return -1;                       // Другая ошибка
        }
        
        p += written;                        // Сдвиг указателя
        left -= written;                     // Уменьшение счётчика
    }
    
    return count;                            // Успех
}

/* write_float_to_buffer - форматирование float в строку БЕЗ printf */
static int write_float_to_buffer(char *buf, float num) {
    int pos = 0;                             // Текущая позиция в буфере

    buf[pos++] = 'S'; buf[pos++] = 'u'; buf[pos++] = 'm'; // Префикс "Sum: "
    buf[pos++] = ':'; buf[pos++] = ' ';

    if (num < 0) {                           // Отрицательное число
        buf[pos++] = '-';                    // Добавляем минус
        num = -num;                          // Делаем положительным
    }

    int whole = (int)num;                    // Целая часть (отбрасываем дробную)
    if (whole == 0) {                        // Особый случай: 0.XX
        buf[pos++] = '0';
    } else {
        char tmp[20];                        // Временный буфер для цифр
        int tp = 0;                          // Позиция в tmp
        
        while (whole > 0) {                  // Извлекаем цифры (в обратном порядке)
            tmp[tp++] = '0' + (whole % 10);  // Последняя цифра + ASCII '0'
            whole /= 10;                     // Убираем последнюю цифру
        }
        
        while (tp > 0) {                     // Переворачиваем и копируем
            buf[pos++] = tmp[--tp];          // Сначала старшие разряды
        }
    }

    buf[pos++] = '.';                        // Десятичная точка

    int frac = (int)((num - (int)num) * 100 + 0.5); // Дробная часть: 0.75 → 75
    if (frac >= 100) frac = 99;              // Защита от округления (0.996 → 99)
    buf[pos++] = '0' + (frac / 10);          // Первая цифра (десятки)
    buf[pos++] = '0' + (frac % 10);          // Вторая цифра (единицы)
    buf[pos++] = '\n';                       // Конец строки

    return pos;                              // Количество записанных символов
}

/* process_line - парсинг строки с числами и вычисление суммы */
static float process_line(char *line) {
    float sum = 0.0;                         // Аккумулятор суммы
    char *p = line;                          // Указатель на текущий символ
    
    while (*p) {                             // Пока не конец строки
        while (*p == ' ' || *p == '\t') p++; // Пропускаем пробелы и табы
        if (*p == '\0' || *p == '\n') break; // Конец строки
        
        char *end;                           // OUT параметр для strtof
        errno = 0;                           // Сброс errno (для проверки ERANGE)
        float val = strtof(p, &end);         // strtof() - преобразует строку в float, end указывает на символ после числа
        
        if (end == p) {                      // Не удалось распознать число (end не сдвинулся)
            safe_write(STDERR_FILENO, "Parse error\n", 12);
            _exit(1);                        // Аварийное завершение
        }
        if (errno == ERANGE) {               // Переполнение (число слишком большое/маленькое)
            safe_write(STDERR_FILENO, "Number too large\n", 17);
            _exit(1);
        }
        
        sum += val;                          // Добавляем к сумме
        p = end;                             // Переходим к следующему числу
    }
    
    return sum;                              // Возвращаем сумму
}

int main(int argc, char *argv[]) {
    if (argc < 2) {                          // argc = количество аргументов (минимум 1: argv[0])
        safe_write(STDERR_FILENO, "Usage: child <mmap_file>\n", 25);
        return 1;
    }

    /* ====================================================================
     * СЕМАФОРЫ: Открытие существующих семафоров
     * 
     * sem_open() без O_CREAT открывает СУЩЕСТВУЮЩИЙ семафор
     * Родитель должен был создать его раньше!
     * 
     * Семафоры хранятся в /dev/shm/ (tmpfs в RAM)
     * Проверить: ls -la /dev/shm/sem.os_lab3*
     * ==================================================================== */
    sem_t *sem_ready = sem_open(SEM_READY, 0); // 0 = нет флагов (только открыть, не создавать)
    if (sem_ready == SEM_FAILED) {           // SEM_FAILED = ошибка (семафор не существует или нет прав)
        safe_write(STDERR_FILENO, "sem_open ready failed in child\n", 32);
        return 1;
    }

    sem_t *sem_done = sem_open(SEM_DONE, 0); // Открываем второй семафор
    if (sem_done == SEM_FAILED) {
        safe_write(STDERR_FILENO, "sem_open done failed in child\n", 31);
        sem_close(sem_ready);                // Закрываем первый при ошибке
        return 1;
    }

    /* ====================================================================
     * MMAP: Открытие файла
     * 
     * argv[1] содержит путь "/tmp/os_lab3_mmap"
     * Родитель уже создал и расширил (ftruncate) этот файл
     * ==================================================================== */
    int mmap_fd = open(argv[1], O_RDWR);     // O_RDWR нужен для mmap с PROT_WRITE
    if (mmap_fd < 0) {                       // Ошибка: файл не найден или нет прав
        safe_write(STDERR_FILENO, "Cannot open mmap file\n", 22);
        sem_close(sem_ready);
        sem_close(sem_done);
        return 1;
    }

    /* ====================================================================
     * MMAP: Отображение в память дочернего процесса
     * 
     * КРИТИЧНО: параметры mmap ДОЛЖНЫ совпадать с parent.c!
     * - Размер: MMAP_SIZE (8192)
     * - Права: PROT_READ | PROT_WRITE
     * - Флаги: MAP_SHARED (обязательно!)
     * 
     * Виртуальный адрес может отличаться от родителя:
     * - Родитель: shared = 0x40000000
     * - Дочерний: shared = 0x50000000
     * НО физическая страница ОДНА (благодаря MAP_SHARED + один файл)
     * 
     * Как ядро связывает два процесса:
     * 1. Родитель: mmap() → VMA → Page Table Entry → inode (файл)
     * 2. Дочерний: mmap() → VMA → Page Table Entry → тот же inode
     * 3. Kernel видит что оба процесса отображают один inode с MAP_SHARED
     * 4. Выделяет ОДНУ физическую страницу для обоих
     * 5. Оба PTE указывают на одну физическую страницу
     * ==================================================================== */
    SharedData *shared = mmap(               // Параметры идентичны parent.c
        NULL,                                // ОС выбирает адрес
        MMAP_SIZE,                           // 8192 байт
        PROT_READ | PROT_WRITE,              // Чтение + запись
        MAP_SHARED,                          // КРИТИЧНО для IPC!
        mmap_fd,                             // Дескриптор файла
        0                                    // Смещение 0 (с начала)
    );
    
    if (shared == MAP_FAILED) {              // MAP_FAILED = (void*)-1
        safe_write(STDERR_FILENO, "mmap error in child\n", 20);
        close(mmap_fd);
        sem_close(sem_ready);
        sem_close(sem_done);
        return 1;
    }

    close(mmap_fd);                          // Дескриптор больше не нужен (отображение активно)

    /* ====================================================================
     * СЕМАФОРЫ: Ожидание сигнала от родителя
     * 
     * sem_wait(sem_ready) блокирует дочерний процесс
     * 
     * Детальная временная последовательность:
     * 
     * t1: Родитель создаёт семафор sem_ready со значением 0
     * t2: Родитель fork() → создаётся дочерний процесс
     * t3: Родитель продолжает: читает файл, записывает в shared->data
     * t4: Дочерний execv() → становится программой child
     * t5: Дочерний вызывает sem_wait(sem_ready)
     *     - Атомарная операция: счётчик 0 - 1 = -1
     *     - -1 < 0 → процесс блокируется!
     *     - Kernel добавляет процесс в wait queue семафора
     *     - Состояние процесса: TASK_INTERRUPTIBLE
     *     - Context switch: scheduler выбирает другой процесс
     * t6: Родитель: msync() - синхронизация данных
     * t7: Родитель: sem_post(sem_ready)
     *     - Атомарная операция: счётчик -1 + 1 = 0
     *     - Kernel убирает дочерний из wait queue
     *     - Состояние дочернего: TASK_RUNNING
     *     - Scheduler в будущем выберет дочерний для выполнения
     * t8: Дочерний: sem_wait() возвращается
     *     - Продолжает выполнение после блокировки

     * ==================================================================== */
    sem_wait(sem_ready);                     // Блокируется здесь пока родитель не вызовет sem_post

    /* ====================================================================
     * MMAP: Синхронизация для чтения свежих данных
     * 
     * ⚡ КРИТИЧНО вызвать msync() сразу после sem_wait()!
     * 
     * Проблема кэш-когерентности (cache coherency):
     * 
     * В многопроцессорной системе (SMP):
     * - Родитель работает на CPU0
     * - Дочерний работает на CPU1
     * - У каждого CPU свой L1/L2 cache
     * - L3 cache общий, но не гарантирует мгновенную синхронизацию
     * 
     * Без msync():
     *   CPU0 (родитель):              CPU1 (дочерний):
     *   L1: data[0]='X'              L1: data[0]='\0' (старое!)
     *   ↓
     *   sem_post() не сбрасывает L1!
     * 
     * С msync(MS_SYNC):
     *   CPU0 (родитель):              CPU1 (дочерний):
     *   L1: data[0]='X'              sem_wait() возвращается
     *   ↓                            ↓
     *   msync() → clflush            msync() → L1 invalidate
     *   ↓                            ↓
     *   RAM: data[0]='X' ←───────────── L1 miss → read from RAM
     * 
     * Что делает msync(MS_SYNC) на CPU уровне (x86-64):
     * 1. mfence - полный memory barrier (все записи завершены)
     * 2. clflush - сброс кэш-линий на RAM
     * 3. sfence - гарантия порядка записей
     * 
     * На ARM:
     * 1. DMB (Data Memory Barrier)
     * 2. DSB (Data Synchronization Barrier)
     * 3. Clean cache to PoC (Point of Coherency)
     * ==================================================================== */
    msync(shared, MMAP_SIZE, MS_SYNC);       // Обновляем наш кэш из RAM/Page Cache

    /* === ОБРАБОТКА ДАННЫХ === */
    char output[MMAP_SIZE];                  // Буфер для результата
    int out_pos = 0;                         // Текущая позиция в output
    
    char line[256];                          // Буфер для одной строки
    int line_pos = 0;                        // Позиция в line
    
    for (size_t i = 0; i < shared->data_size; i++) { // Проход по всем байтам
        char c = shared->data[i];            // Текущий символ
        
        if (c == '\n') {                     // Конец строки
            if (line_pos > 0) {              // Есть что обработать
                line[line_pos] = '\0';       // Нуль-терминатор
                float sum = process_line(line); // Парсим и суммируем
                out_pos += write_float_to_buffer(output + out_pos, sum); // Форматируем
                line_pos = 0;                // Сброс для новой строки
            }
        } else {
            if (line_pos < 255) {            // Защита от переполнения
                line[line_pos++] = c;        // Добавляем символ
            }
        }
    }

    if (line_pos > 0) {                      // Последняя строка без '\n'
        line[line_pos] = '\0';
        float sum = process_line(line);
        out_pos += write_float_to_buffer(output + out_pos, sum);
    }

    /* Копируем результат в shared memory */
    for (int i = 0; i < out_pos && i < (int)sizeof(shared->data); i++) {
        shared->data[i] = output[i];         // Побайтовое копирование
    }
    shared->data_size = (size_t)out_pos;     // Обновляем размер

    /* ====================================================================
     * MMAP: Синхронизация записанных данных
     * 
     * ⚡ КРИТИЧНО вызвать msync() перед sem_post()!
     * 
     * Гарантируем что родитель увидит результат:
     * 1. Дочерний записал в shared->data (в свой L1 cache CPU1)
     * 2. msync() сбрасывает L1 → L3 → RAM → Page Cache
     * 3. sem_post() разблокирует родителя
     * 4. Родитель просыпается на CPU0
     * 5. Родитель вызывает msync() → инвалидирует свой L1
     * 6. Родитель читает из RAM → видит актуальные данные ✅
     * 
     * Порядок КРИТИЧЕН:
     * ❌ НЕПРАВИЛЬНО:
     *    shared->data[0] = 'X';
     *    sem_post(done);  // ← СРАЗУ сигнал
     *    msync();         // ← Поздно! Родитель уже мог прочитать
     * 
     * ✅ ПРАВИЛЬНО:
     *    shared->data[0] = 'X';
     *    msync();         // ← СНАЧАЛА синхронизация
     *    sem_post(done);  // ← ПОТОМ сигнал
     * ==================================================================== */
    msync(shared, MMAP_SIZE, MS_SYNC);       // Сбрасываем наш кэш в RAM

    /* ====================================================================
     * СЕМАФОРЫ: Сигнализация родителю о завершении
     * 
     * sem_post(sem_done) разблокирует родителя
     * 
     * Последовательность:
     * t1: Родитель: sem_wait(done)
     *     - Счётчик: 0 - 1 = -1
     *     - Состояние: TASK_INTERRUPTIBLE (спит)
     * t2: Дочерний: обрабатывает данные...
     * t3: Дочерний: msync() - синхронизация
     * t4: Дочерний: sem_post(done)
     *     - Счётчик: -1 + 1 = 0
     *     - Kernel: wake_up_process(родитель)
     *     - Родитель: состояние → TASK_RUNNING
     * t5: Родитель: sem_wait() возвращается
     *     - Продолжает выполнение
     * 
     * Реализация sem_post() в ядре (упрощённо):
     * ```c
     * sem_post(sem) {
     *     spin_lock(&sem->lock);
     *     sem->count++;
     *     if (sem->count <= 0) {        // Есть ждущие?
     *         task = remove_from_waitqueue();
     *         wake_up_process(task);    // Разбудить процесс
     *     }
     *     spin_unlock(&sem->lock);
     * }
     * ```
     * 
     * Атомарность на CPU уровне (x86):
     * ```asm
     * lock addl $1, (%rdi)   ; LOCK префикс = атомарность
     * ```
     * LOCK префикс гарантирует:
     * - Блокировка шины памяти (memory bus lock)
     * - Другие CPU не могут обращаться к этой кэш-линии
     * - Операция выглядит атомарной для всех CPU
     * ==================================================================== */
    sem_post(sem_done);                      // Сигнализируем родителю

    /* === ОЧИСТКА РЕСУРСОВ === */
    munmap(shared, MMAP_SIZE);               // Отменяем отображение
    sem_close(sem_ready);                    // Закрываем дескрипторы семафоров
    sem_close(sem_done);                     // (sem_unlink делает родитель)

    return 0;                                // Успешное завершение
}