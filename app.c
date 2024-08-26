#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_PATH_LENGTH 1024
#define TOP_N 10

typedef struct {
    char name[MAX_PATH_LENGTH];
    long long size;
} Directory;

typedef struct {
    Directory directories[TOP_N];
    int count;
} MinHeap;

// Función para inicializar el min-heap
void init_min_heap(MinHeap *heap) {
    heap->count = 0;
}

// Función para agregar un nuevo directorio al min-heap
void add_to_heap(MinHeap *heap, Directory dir) {
    if (heap->count < TOP_N) {
        heap->directories[heap->count] = dir;
        heap->count++;
    } else {
        if (dir.size > heap->directories[0].size) {
            heap->directories[0] = dir;
        }
    }

    // Reorganizar el heap para que el menor siempre esté en la raíz
    for (int i = (heap->count / 2) - 1; i >= 0; i--) {
        int smallest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        if (left < heap->count && heap->directories[left].size < heap->directories[smallest].size) {
            smallest = left;
        }
        if (right < heap->count && heap->directories[right].size < heap->directories[smallest].size) {
            smallest = right;
        }
        if (smallest != i) {
            Directory temp = heap->directories[i];
            heap->directories[i] = heap->directories[smallest];
            heap->directories[smallest] = temp;
        }
    }
}

// Función recursiva para obtener el tamaño del directorio y sus subdirectorios
long long get_directory_size(const char *directory, MinHeap *heap) {
    long long total_size = 0;
    WIN32_FIND_DATA find_data;
    HANDLE hFind;
    char path[MAX_PATH_LENGTH];

    snprintf(path, sizeof(path), "%s\\*.*", directory);
    hFind = FindFirstFile(path, &find_data);

    if (hFind == INVALID_HANDLE_VALUE) {
        printf("Error al abrir el directorio: %s\n", directory);
        return 0;
    }

    do {
        // Ignorar "." y ".."
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0)
            continue;

        snprintf(path, sizeof(path), "%s\\%s", directory, find_data.cFileName);

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Si es un directorio, realizar una llamada recursiva
            Directory subdir;
            strncpy(subdir.name, path, sizeof(subdir.name));
            subdir.size = get_directory_size(path, heap);
            add_to_heap(heap, subdir);

            total_size += subdir.size;
        } else {
            // Sumar el tamaño del archivo
            LARGE_INTEGER file_size;
            file_size.HighPart = find_data.nFileSizeHigh;
            file_size.LowPart = find_data.nFileSizeLow;
            total_size += file_size.QuadPart;
        }
    } while (FindNextFile(hFind, &find_data) != 0);

    FindClose(hFind);

    // Agregar el directorio actual al heap
    Directory dir;
    strncpy(dir.name, directory, sizeof(dir.name));
    dir.size = total_size;
    add_to_heap(heap, dir);

    return total_size;
}

// Función para mostrar los directorios más grandes
void print_largest_directories(MinHeap *heap) {
    printf("Directorios más grandes:\n");
    for (int i = 0; i < heap->count; i++) {
        printf("%s: %.2f MB\n", heap->directories[i].name, heap->directories[i].size / (1024.0 * 1024.0));
    }
}

int main() {
    const char *root_dir = "C:\\";
    MinHeap heap;
    init_min_heap(&heap);

    get_directory_size(root_dir, &heap);
    print_largest_directories(&heap);

    scanf("Hola");

    return 0;
}
