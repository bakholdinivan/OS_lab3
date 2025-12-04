/*
 * ============================================================================
 * Лабораторная работа №3 - Родительский процесс (parent)
 *
 *
 * 
 * Описание: Программа создаёт дочерний процесс и обменивается с ним данными
 * через memory-mapped файл. Синхронизация через POSIX семафоры.
 * ============================================================================
 */

/* Feature test macros - ДОЛЖНЫ быть ДО всех #include */
#define _POSIX_C_SOURCE 200809L  // Включает POSIX.1-2008 функции (sem_open, mmap и т.д.)
#define _XOPEN_SOURCE 700        // Включает X/Open 7 расширения (для совместимости)

/* === ЗАГОЛОВОЧНЫЕ ФАЙЛЫ === */
#include <unistd.h>       // POSIX API: read(), write(), fork(), close(), execv(), _exit(), ftruncate()
#include <fcntl.h>        // File control: open(), O_RDWR, O_CREAT, O_EXCL флаги
#include <sys/mman.h>     // Memory management: mmap(), munmap(), msync(), MAP_SHARED, PROT_READ, PROT_WRITE
#include <sys/types.h>    // Базовые типы: pid_t (ID процесса), size_t (размер), ssize_t (знаковый размер)
#include <sys/wait.h>     // Ожидание процессов: wait(), waitpid(), макросы WIFEXITED и т.д.
#include <sys/stat.h>     // Права доступа: S_IRUSR (user read), S_IWUSR (user write)
#include <semaphore.h>    // POSIX семафоры: sem_t, sem_open(), sem_close(), sem_wait(), sem_post(), sem_unlink()
#include <signal.h>       // Сигналы: kill(), SIGTERM (для аварийного завершения дочернего)
#include <stdlib.h>       // Стандартная библиотека: _exit() (завершение без cleanup)
#include <string.h>       // Строковые функции: strlen(), strchr(), memset()
#include <errno.h>        // Коды ошибок: errno (глобальная переменная), EINTR, ERANGE

/* === КОНСТАНТЫ === */
#define BUF_SIZE 256                        // Размер буфера для ввода имени файла (255 символов + '\0')
#define MMAP_FILE "/tmp/os_lab3_mmap"       // Путь к файлу для mmap (в tmpfs = в RAM, быстро)
#define SEM_READY "/os_lab3_sem_ready"      // Имя семафора "родитель сигнализирует: данные готовы"
#define SEM_DONE "/os_lab3_sem_done"        // Имя семафора "дочерний сигнализирует: обработка завершена"
#define MMAP_SIZE 8192                      // Размер отображаемой области = 8 КБ (одна страница памяти x2)

/*
 * SharedData - структура данных в разделяемой памяти
 * 
 * КРИТИЧНО: Эта же структура должна быть в child.c!
 * Оба процесса работают с одной и той же областью памяти.
 */
typedef struct {
    size_t data_size;                                    // Количество актуальных байт в data[] (8 байт на x64)
    char data[MMAP_SIZE - sizeof(size_t)];              // Буфер для данных (8192 - 8 = 8184 байт)
} SharedData;

/*
 * safe_write - Надёжная запись данных в файловый дескриптор
 * 
 * Зачем нужна: write() может записать МЕНЬШЕ чем count байт или быть прерван сигналом.
 * safe_write гарантирует запись ВСЕХ байт.
 */
static ssize_t safe_write(int fd, const void *buf, size_t count) {
    const char *p = buf;                     // Указатель на текущую позицию в буфере (для сдвига)
    size_t left = count;                     // Счётчик оставшихся байт для записи
    
    while (left > 0) {                       // Цикл пока есть незаписанные байты
        ssize_t written = write(fd, p, left); // write() - системный вызов записи, возвращает количество записанных байт
        
        if (written < 0) {                   // Ошибка записи (возвращено -1)
            if (errno == EINTR) continue;    // EINTR = прерван сигналом (например SIGCHLD), повторить попытку
            return -1;                       // Другая ошибка - выход с ошибкой
        }
        
        p += written;                        // Сдвинуть указатель на количество записанных байт
        left -= written;                     // Уменьшить счётчик оставшихся байт
    }
    
    return count;                            // Успех: все байты записаны
}

int main(void) {
    char filename[BUF_SIZE] = {0};           // Буфер для имени файла, инициализирован нулями

    /* === ВВОД ИМЕНИ ФАЙЛА === */
    safe_write(STDOUT_FILENO, "Enter filename: ", 16); // STDOUT_FILENO = 1 (стандартный вывод)
    
    ssize_t len = read(STDIN_FILENO, filename, BUF_SIZE - 1); // STDIN_FILENO = 0, читаем до 255 байт (резервируем место для '\0')
    if (len <= 0) {                          // Ошибка чтения (EOF или ошибка)
        safe_write(STDERR_FILENO, "Error reading filename\n", 23); // STDERR_FILENO = 2 (стандартный поток ошибок)
        return 1;                            // Код возврата 1 = ошибка
    }
    
    char *newline = strchr(filename, '\n');  // strchr() ищет первое вхождение '\n', возвращает указатель или NULL
    if (newline) *newline = '\0';            // Заменяем '\n' на '\0' (нуль-терминатор C-строки)

    /* ====================================================================
     * СЕМАФОРЫ: Создание именованных POSIX семафоров
     * 
     * Что такое семафор?
     * - Механизм синхронизации процессов/потоков
     * - Содержит счётчик (целое число) и очередь ожидающих процессов
     * - Две атомарные операции: P (wait/уменьшить) и V (post/увеличить)
     * 
     * Именованные семафоры (named semaphores):
     * - Хранятся в /dev/shm/ (tmpfs в оперативной памяти)
     * - Доступны разным процессам по имени
     * - Переживают завершение процесса (нужен sem_unlink!)
     * - Альтернатива: неименованные (memory-based, только для потоков)
     * ==================================================================== */
    
    /* sem_open() - создаёт или открывает именованный семафор */
    sem_t *sem_ready = sem_open(              // sem_t* - дескриптор семафора (похож на FILE*)
        SEM_READY,                           // const char *name - имя (должно начинаться с '/')
        O_CREAT | O_EXCL,                    // int oflag - O_CREAT создать, O_EXCL ошибка если существует
        0600,                                // mode_t mode - права доступа: 0600 = rw------- (только владелец)
        0                                    // unsigned int value - начальное значение счётчика (0 = заблокирован)
    );
    
    if (sem_ready == SEM_FAILED) {           // SEM_FAILED = (sem_t*)-1 - специальное значение при ошибке
        safe_write(STDERR_FILENO, "sem_open ready failed\n", 22);
        return 1;
    }

    /* Создание второго семафора для обратной связи (дочерний → родитель) */
    sem_t *sem_done = sem_open(              // Все параметры аналогичны первому семафору
        SEM_DONE,                            // Другое имя - это независимый семафор
        O_CREAT | O_EXCL,                    // O_EXCL гарантирует что мы создаём новый (не открываем старый)
        0600,                                // Права доступа: только владелец
        0                                    // Начальное значение 0: sem_wait() сразу заблокирует
    );
    
    if (sem_done == SEM_FAILED) {            // Проверка ошибки
        safe_write(STDERR_FILENO, "sem_open done failed\n", 21);
        sem_close(sem_ready);                // Закрыть дескриптор первого семафора
        sem_unlink(SEM_READY);               // Удалить семафор из /dev/shm/ (иначе останется "висеть")
        return 1;
    }

    /* ====================================================================
     * MEMORY-MAPPED FILES: Создание файла для mmap
     * 
     * Что такое memory-mapped file?
     * - Файл отображается в виртуальное адресное пространство процесса
     * - Работа с файлом как с массивом в памяти (без read/write)
     * - Изменения автоматически синхронизируются с диском (через Page Cache)
     * - MAP_SHARED позволяет нескольким процессам видеть одну память
     * 
     * Преимущества:
     * - Нет копирования данных (user space ↔ kernel space)
     * - Эффективное использование памяти (demand paging)
     * - Упрощение кода (указатели вместо системных вызовов)
     * ==================================================================== */
    
    int mmap_fd = open(                      // open() - системный вызов открытия/создания файла
        MMAP_FILE,                           // const char *pathname - путь к файлу
        O_RDWR | O_CREAT,                    // int flags - O_RDWR чтение+запись (нужно для mmap), O_CREAT создать если нет
        S_IRUSR | S_IWUSR                    // mode_t mode - S_IRUSR user read (0400), S_IWUSR user write (0200), итого 0600
    );
    
    if (mmap_fd < 0) {                       // Ошибка: возвращено -1
        safe_write(STDERR_FILENO, "Cannot create mmap file\n", 24);
        sem_close(sem_ready);                // Очистка всех уже созданных ресурсов
        sem_close(sem_done);
        sem_unlink(SEM_READY);
        sem_unlink(SEM_DONE);
        return 1;
    }

    /* ====================================================================
     * ftruncate() - установка размера файла
     * 
     * КРИТИЧНО для mmap!
     * - Новый файл имеет размер 0 байт
     * - mmap() НЕ МОЖЕТ отобразить файл размером 0 байт
     * - ftruncate() расширяет файл до MMAP_SIZE байт (заполняет '\0')
     * 
     * Как работает:
     * - Если новый размер > текущего → расширяет (добавляет нули)
     * - Если новый размер < текущего → обрезает (теряет данные)
     * - Изменяет метаданные файла (inode->i_size)
     * ==================================================================== */
    if (ftruncate(mmap_fd, MMAP_SIZE) == -1) { // ftruncate(int fd, off_t length) возвращает 0 при успехе, -1 при ошибке
        safe_write(STDERR_FILENO, "ftruncate error\n", 16);
        close(mmap_fd);                      // close() - закрывает файловый дескриптор (освобождает номер в таблице дескрипторов)
        unlink(MMAP_FILE);                   // unlink() - удаляет файл из файловой системы (уменьшает link count)
        sem_close(sem_ready);
        sem_close(sem_done);
        sem_unlink(SEM_READY);
        sem_unlink(SEM_DONE);
        return 1;
    }

    /* ====================================================================
     * mmap() - отображение файла в память
     * 
     * Самая важная функция для IPC через shared memory!
     * 
     * Как работает на низком уровне:
     * 1. Выделяет диапазон виртуальных адресов (например 0x40000000-0x40002000)
     * 2. Создаёт VMA (Virtual Memory Area) структуру в kernel
     * 3. Добавляет записи в Page Table (но НЕ выделяет физическую память!)
     * 4. При первом обращении → Page Fault → kernel загружает страницу с диска
     * 5. Последующие обращения → напрямую к RAM (без системных вызовов)
     * 
     * MAP_SHARED vs MAP_PRIVATE:
     * - MAP_SHARED: изменения попадают в файл, видны другим процессам (IPC)
     * - MAP_PRIVATE: Copy-on-Write, изменения в приватной копии (НЕ IPC)
     * ==================================================================== */
    SharedData *shared = mmap(               // void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
        NULL,                                // void *addr - NULL = ОС сама выберет виртуальный адрес (рекомендуется)
        MMAP_SIZE,                           // size_t length - размер отображения в байтах (8192)
        PROT_READ | PROT_WRITE,              // int prot - PROT_READ разрешить чтение, PROT_WRITE разрешить запись
        MAP_SHARED,                          // int flags - MAP_SHARED КРИТИЧНО! Изменения видны другим процессам
        mmap_fd,                             // int fd - файловый дескриптор открытого файла
        0                                    // off_t offset - смещение в файле (0 = начало файла, должно быть кратно page size)
    );
    
    if (shared == MAP_FAILED) {              // MAP_FAILED = (void*)-1 - специальное значение при ошибке
        safe_write(STDERR_FILENO, "mmap error\n", 11);
        close(mmap_fd);
        unlink(MMAP_FILE);
        sem_close(sem_ready);
        sem_close(sem_done);
        sem_unlink(SEM_READY);
        sem_unlink(SEM_DONE);
        return 1;
    }

    memset(shared, 0, MMAP_SIZE);            // memset() - заполняет область памяти указанным байтом (0 = '\0')

    /* ====================================================================
     * fork() - создание дочернего процесса
     * 
     * Что происходит при fork():
     * 1. Kernel создаёт копию структуры task_struct (дескриптор процесса)
     * 2. Копирует таблицу страниц (Page Table) - НЕ саму память!
     * 3. Обычные страницы помечаются Copy-on-Write (при записи → копируются)
     * 4. mmap MAP_SHARED страницы НЕ копируются (остаются shared)
     * 5. Оба процесса продолжают выполнение с одной инструкции
     * 
     * Возвращаемое значение fork():
     * - В родителе: PID дочернего процесса (> 0)
     * - В дочернем: 0
     * - При ошибке: -1
     * ==================================================================== */
    pid_t child_pid = fork();                // pid_t - тип для ID процесса (обычно int)
    
    if (child_pid < 0) {                     // Ошибка fork() (не хватило памяти, превышен лимит процессов и т.д.)
        safe_write(STDERR_FILENO, "fork error\n", 11);
        munmap(shared, MMAP_SIZE);           // munmap() - отменяет отображение файла в память
        close(mmap_fd);
        unlink(MMAP_FILE);
        sem_close(sem_ready);
        sem_close(sem_done);
        sem_unlink(SEM_READY);
        sem_unlink(SEM_DONE);
        return 1;
    }
    
    if (child_pid == 0) {
        /* === ДОЧЕРНИЙ ПРОЦЕСС === */
        
        close(mmap_fd);                      // Закрываем дескриптор (не нужен, отображение уже активно)
        
        char *args[] = {"./build/child", MMAP_FILE, NULL}; // Массив аргументов: argv[0], argv[1], NULL-терминатор
        execv("./build/child", args);        // execv() - заменяет текущий процесс новой программой (НЕ создаёт процесс!)
        
        /* Если execv() вернул управление - ОШИБКА! */
        safe_write(STDERR_FILENO, "exec error\n", 11);
        _exit(1);                            // _exit() - немедленный выход БЕЗ вызова atexit() обработчиков (быстрее exit())
    }
    else {
        /* === РОДИТЕЛЬСКИЙ ПРОЦЕСС === */

        int file_fd = open(filename, O_RDONLY); // O_RDONLY - только для чтения (read-only)
        if (file_fd < 0) {
            safe_write(STDERR_FILENO, "Cannot open file\n", 17);
            kill(child_pid, SIGTERM);        // kill() - отправляет сигнал процессу, SIGTERM = 15 (мягкое завершение)
            wait(NULL);                      // wait() - ждёт завершения любого дочернего, NULL = не интересует статус
            munmap(shared, MMAP_SIZE);
            close(mmap_fd);
            unlink(MMAP_FILE);
            sem_close(sem_ready);
            sem_close(sem_done);
            sem_unlink(SEM_READY);
            sem_unlink(SEM_DONE);
            return 1;
        }

        ssize_t bytes_read = read(file_fd, shared->data, sizeof(shared->data) - 1); // read() - читает из дескриптора в буфер
        close(file_fd);                      // Файл прочитан, дескриптор больше не нужен
        
        if (bytes_read < 0) {                // Ошибка чтения
            safe_write(STDERR_FILENO, "Error reading file\n", 19);
            kill(child_pid, SIGTERM);
            wait(NULL);
            munmap(shared, MMAP_SIZE);
            close(mmap_fd);
            unlink(MMAP_FILE);
            sem_close(sem_ready);
            sem_close(sem_done);
            sem_unlink(SEM_READY);
            sem_unlink(SEM_DONE);
            return 1;
        }

        shared->data[bytes_read] = '\0';     // Добавляем нуль-терминатор (превращаем в C-строку)
        shared->data_size = (size_t)bytes_read; // Сохраняем размер данных (size_t - беззнаковый тип)

        /* ================================================================
         * msync() - синхронизация memory-mapped региона с файлом
         * 
         * КРИТИЧНО для корректного IPC через mmap!
         * 
         * Зачем нужен:
         * Без msync() изменения могут "застрять" на разных уровнях:
         * 1. CPU Store Buffer - буфер записи процессора
         * 2. CPU Cache (L1, L2, L3) - кэш процессора
         * 3. TLB (Translation Lookaside Buffer) - кэш адресных трансляций
         * 4. Page Cache (kernel) - кэш страниц в ядре
         * 
         * Другой процесс может читать СТАРЫЕ данные из своего кэша!
         * 
         * Что делает msync(MS_SYNC):
         * 1. Выполняет memory barrier (инструкции mfence/sfence на x86)
         * 2. Сбрасывает CPU cache (инструкция clflush на x86)
         * 3. Записывает dirty pages из Page Cache на диск
         * 4. Обновляет metadata файла (mtime, atime)
         * 5. БЛОКИРУЕТ процесс до завершения физической записи
         * 
         * Флаги msync():
         * - MS_SYNC: блокирующий, гарантия записи на диск
         * - MS_ASYNC: асинхронный, только запланировать запись
         * - MS_INVALIDATE: обновить все копии в памяти
         * ================================================================ */
        msync(shared, MMAP_SIZE, MS_SYNC);   // int msync(void *addr, size_t length, int flags)

        /* ================================================================
         * sem_post() - увеличение счётчика семафора (V операция)
         * 
         * Атомарный алгоритм sem_post():
         * 1. Атомарно увеличить счётчик на 1 (используя lock prefix на x86)
         * 2. Если счётчик был ≤ 0 (есть ожидающие процессы):
         *    - Убрать один процесс из очереди ожидания
         *    - Разбудить его (перевести в состояние RUNNABLE)
         *    - Scheduler выберет когда запустить
         * 
         * В нашем случае:
         * - Счётчик был 0
         * - Становится 1
         * - Дочерний процесс в sem_wait(ready) разблокируется
         * 
         * Гарантии атомарности:
         * - x86: LOCK ADD инструкция (блокировка шины памяти)
         * - ARM: LDREX/STREX (Load/Store Exclusive)
         * - Работает корректно на SMP (multi-CPU) системах
         * ================================================================ */
        sem_post(sem_ready);                 // int sem_post(sem_t *sem)

        /* ================================================================
         * sem_wait() - уменьшение счётчика семафора (P операция)
         * 
         * Атомарный алгоритм sem_wait():
         * 1. Атомарно уменьшить счётчик на 1
         * 2. Если результат < 0:
         *    - Добавить текущий процесс в очередь ожидания
         *    - Перевести процесс в состояние SLEEPING (не потребляет CPU)
         *    - Передать управление scheduler (context switch)
         * 3. Процесс "спит" до вызова sem_post() другим процессом
         * 
         * В нашем случае:
         * - Счётчик sem_done был 0
         * - 0 - 1 = -1
         * - -1 < 0 → родитель засыпает
         * - Проснётся когда дочерний вызовет sem_post(sem_done)
         * 
         * Отличие от busy-wait:
         * - while(flag) {} - CPU постоянно проверяет (100% загрузка)
         * - sem_wait() - процесс спит (0% CPU)
         * 
         * Отличие от pause():
         * - pause() - ждёт ЛЮБОГО сигнала (небезопасно)
         * - sem_wait() - ждёт конкретного события (безопасно)
         * ================================================================ */
        sem_wait(sem_done);                  // int sem_wait(sem_t *sem)

        /* msync() для чтения свежих данных от дочернего процесса */
        msync(shared, MMAP_SIZE, MS_SYNC);   // Обновляем наш кэш из Page Cache (kernel)

        safe_write(STDOUT_FILENO, "Result:\n", 8);
        if (shared->data_size > 0) {
            safe_write(STDOUT_FILENO, shared->data, shared->data_size);
        }

        wait(NULL);                          // Ждём завершения дочернего (предотвращаем zombie процесс)

        /* === ОЧИСТКА РЕСУРСОВ === */
        munmap(shared, MMAP_SIZE);           // munmap() - отменяет отображение (НЕ удаляет файл!)
        close(mmap_fd);                      // close() - закрывает дескриптор
        unlink(MMAP_FILE);                   // unlink() - удаляет файл (уменьшает link count → 0)
        
        sem_close(sem_ready);                // sem_close() - закрывает дескриптор семафора (НЕ удаляет!)
        sem_close(sem_done);
        sem_unlink(SEM_READY);               // sem_unlink() - удаляет семафор из /dev/shm/
        sem_unlink(SEM_DONE);
    }
    
    return 0;                                // Успешное завершение
}