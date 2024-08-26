#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define IO_BLOCK_SIZE 8192  // Tamaño de bloque para lecturas de disco
#define MAX_PATH_LENGTH 260
#define TOP_N 10
#define MAX_CONCURRENT_THREADS 8  // Ajustar según los núcleos disponibles

// Estructura para almacenar información de directorios
typedef struct {
    char path[MAX_PATH_LENGTH];
    uint64_t size;
} DirectoryInfo;

// Nodo de la cola para explorar directorios
typedef struct DirQueueNode {
    char path[MAX_PATH_LENGTH];
    struct DirQueueNode* next;
} DirQueueNode;

// Cola para explorar directorios (thread-safe)
typedef struct {
    DirQueueNode* front;
    DirQueueNode* rear;
    CRITICAL_SECTION lock;
} DirectoryQueue;

// Montículo máximo para almacenar los TOP_N directorios más grandes
typedef struct {
    DirectoryInfo dirs[TOP_N];
    int count;
    CRITICAL_SECTION lock;
} MaxHeap;

// Datos compartidos entre hilos
typedef struct {
    char drive_letter;
    uint64_t total_space;
    uint64_t free_space;
    uint64_t used_space;
    uint64_t scanned_space;
    DirectoryQueue dir_queue;
    MaxHeap max_heap;
    HANDLE semaphore;
    CRITICAL_SECTION progress_lock;
} ScanData;

// Funciones de la cola de directorios
void init_directory_queue(DirectoryQueue* queue) {
    queue->front = queue->rear = NULL;
    InitializeCriticalSection(&queue->lock);
}

void enqueue_directory(DirectoryQueue* queue, const char* path) {
    DirQueueNode* new_node = (DirQueueNode*)malloc(sizeof(DirQueueNode));
    strncpy(new_node->path, path, MAX_PATH_LENGTH);
    new_node->next = NULL;

    EnterCriticalSection(&queue->lock);
    if (queue->rear == NULL) {
        queue->front = queue->rear = new_node;
    } else {
        queue->rear->next = new_node;
        queue->rear = new_node;
    }
    LeaveCriticalSection(&queue->lock);
}

bool dequeue_directory(DirectoryQueue* queue, char* out_path) {
    EnterCriticalSection(&queue->lock);
    if (queue->front == NULL) {
        LeaveCriticalSection(&queue->lock);
        return false;
    }
    DirQueueNode* temp = queue->front;
    strncpy(out_path, temp->path, MAX_PATH_LENGTH);
    queue->front = queue->front->next;
    if (queue->front == NULL) {
        queue->rear = NULL;
    }
    LeaveCriticalSection(&queue->lock);
    free(temp);
    return true;
}

void destroy_directory_queue(DirectoryQueue* queue) {
    char temp_path[MAX_PATH_LENGTH];
    while (dequeue_directory(queue, temp_path));
    DeleteCriticalSection(&queue->lock);
}

// Funciones del montículo máximo
void init_max_heap(MaxHeap* heap) {
    heap->count = 0;
    InitializeCriticalSection(&heap->lock);
}

void swap_directory_info(DirectoryInfo* a, DirectoryInfo* b) {
    DirectoryInfo temp = *a;
    *a = *b;
    *b = temp;
}

void insert_into_heap(MaxHeap* heap, const DirectoryInfo* dir_info) {
    EnterCriticalSection(&heap->lock);

    if (heap->count < TOP_N) {
        heap->dirs[heap->count] = *dir_info;
        int i = heap->count;
        heap->count++;

        // Reajustar hacia arriba
        while (i != 0 && heap->dirs[(i - 1) / 2].size < heap->dirs[i].size) {
            swap_directory_info(&heap->dirs[i], &heap->dirs[(i - 1) / 2]);
            i = (i - 1) / 2;
        }
    } else if (dir_info->size > heap->dirs[0].size) {
        heap->dirs[0] = *dir_info;
        // Reajustar hacia abajo
        int i = 0;
        while (true) {
            int largest = i;
            int left = 2 * i + 1;
            int right = 2 * i + 2;

            if (left < heap->count && heap->dirs[left].size > heap->dirs[largest].size)
                largest = left;
            if (right < heap->count && heap->dirs[right].size > heap->dirs[largest].size)
                largest = right;
            if (largest != i) {
                swap_directory_info(&heap->dirs[i], &heap->dirs[largest]);
                i = largest;
            } else {
                break;
            }
        }
    }

    LeaveCriticalSection(&heap->lock);
}

void destroy_max_heap(MaxHeap* heap) {
    DeleteCriticalSection(&heap->lock);
}

// Función para actualizar solo el porcentaje de progreso
void update_progress_percentage(ScanData* data, uint64_t file_size) {
    EnterCriticalSection(&data->progress_lock);
    data->scanned_space += file_size;
    double percentage = (double)data->scanned_space / data->used_space * 100.0;

    printf("\rProgreso: %.2f%%", percentage);
    fflush(stdout);
    LeaveCriticalSection(&data->progress_lock);
}

// Función que explora directorios de forma recursiva
DWORD WINAPI directory_scanner(LPVOID param) {
    ScanData* data = (ScanData*)param;
    char current_path[MAX_PATH_LENGTH];

    while (dequeue_directory(&data->dir_queue, current_path)) {
        WIN32_FIND_DATA find_data;
        HANDLE hFind;
        char search_path[MAX_PATH_LENGTH];

        snprintf(search_path, MAX_PATH_LENGTH, "%s\\*", current_path);
        hFind = FindFirstFile(search_path, &find_data);

        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        uint64_t dir_total_size = 0;

        do {
            if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
                continue;
            }

            char full_path[MAX_PATH_LENGTH];
            snprintf(full_path, MAX_PATH_LENGTH, "%s\\%s", current_path, find_data.cFileName);

            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                enqueue_directory(&data->dir_queue, full_path);
            } else {
                LARGE_INTEGER file_size;
                file_size.LowPart = find_data.nFileSizeLow;
                file_size.HighPart = find_data.nFileSizeHigh;
                dir_total_size += file_size.QuadPart;

                // Actualizar espacio escaneado
                update_progress_percentage(data, file_size.QuadPart);
            }
        } while (FindNextFile(hFind, &find_data) != 0);

        FindClose(hFind);

        DirectoryInfo dir_info;
        strncpy(dir_info.path, current_path, MAX_PATH_LENGTH);
        dir_info.size = dir_total_size;

        insert_into_heap(&data->max_heap, &dir_info);
    }

    ReleaseSemaphore(data->semaphore, 1, NULL);
    return 0;
}

// Función principal para escanear una unidad
void scan_drive(char drive_letter) {
    ScanData data;
    data.drive_letter = drive_letter;
    data.scanned_space = 0;

    char root_path[4];
    snprintf(root_path, sizeof(root_path), "%c:\\", drive_letter);

    ULARGE_INTEGER total_bytes;
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceEx(root_path, NULL, &total_bytes, &free_bytes)) {
        printf("No se pudo obtener información de la unidad %c:\\\n", drive_letter);
        return;
    }

    data.total_space = total_bytes.QuadPart;
    data.free_space = free_bytes.QuadPart;
    data.used_space = data.total_space - data.free_space;

    printf("Escaneando la unidad %c:\\...\n", drive_letter);
    init_directory_queue(&data.dir_queue);
    init_max_heap(&data.max_heap);
    InitializeCriticalSection(&data.progress_lock);

    data.semaphore = CreateSemaphore(NULL, MAX_CONCURRENT_THREADS, MAX_CONCURRENT_THREADS, NULL);

    enqueue_directory(&data.dir_queue, root_path);

    DWORD thread_ids[MAX_CONCURRENT_THREADS];
    HANDLE threads[MAX_CONCURRENT_THREADS];

    for (int i = 0; i < MAX_CONCURRENT_THREADS; i++) {
        WaitForSingleObject(data.semaphore, INFINITE);
        threads[i] = CreateThread(NULL, 0, directory_scanner, &data, 0, &thread_ids[i]);
        if (threads[i] == NULL) {
            printf("Error al crear hilo\n");
            ReleaseSemaphore(data.semaphore, 1, NULL);
            break;
        }
    }

    // Esperar a que todos los hilos terminen
    WaitForMultipleObjects(MAX_CONCURRENT_THREADS, threads, TRUE, INFINITE);

    CloseHandle(data.semaphore);
    for (int i = 0; i < MAX_CONCURRENT_THREADS; i++) {
        CloseHandle(threads[i]);
    }

    DeleteCriticalSection(&data.progress_lock);

    printf("\n\nResumen de la unidad %c:\\\n", drive_letter);
    printf("----------------------------------------\n");
    printf("Espacio total     : %.2f GB\n", data.total_space / (1024.0 * 1024.0 * 1024.0));
    printf("Espacio libre     : %.2f GB\n", data.free_space / (1024.0 * 1024.0 * 1024.0));
    printf("Espacio utilizado : %.2f GB\n", data.used_space / (1024.0 * 1024.0 * 1024.0));
    printf("----------------------------------------\n\n");

    printf("Top %d directorios más grandes en %c:\\\n", TOP_N, drive_letter);
    printf("----------------------------------------\n");
    EnterCriticalSection(&data.max_heap.lock);
    for (int i = 0; i < data.max_heap.count; i++) {
        printf("%d. %s -> %.2f MB\n", i + 1, data.max_heap.dirs[i].path, data.max_heap.dirs[i].size / (1024.0 * 1024.0));
    }
    LeaveCriticalSection(&data.max_heap.lock);

    destroy_directory_queue(&data.dir_queue);
    destroy_max_heap(&data.max_heap);
}

int main() {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);

    // Configurar el número máximo de hilos concurrentes según los núcleos del procesador
    int max_threads = sysinfo.dwNumberOfProcessors * 2; // Usa 2 hilos por núcleo para mejor uso de CPU
    printf("Número de hilos configurados: %d\n", max_threads);

    DWORD drives = GetLogicalDrives();
    for (char drive = 'A'; drive <= 'Z'; drive++) {
        if (drives & (1 << (drive - 'A'))) {
            UINT drive_type = GetDriveTypeA((char[]){drive, ':', '\\', '\0'});
            if (drive_type == DRIVE_FIXED) {
                scan_drive(drive);
            }
        }
    }

    printf("\nEscaneo completado. Presiona cualquier tecla para salir.\n");
    getchar();
    return 0;
}
