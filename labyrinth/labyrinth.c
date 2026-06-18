#include "labyrinth.h"
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <testkit.h>

void printUsage();
void printMap(Labyrinth *labyrinth);
static const char *baseName(const char *path);
static int firstOptionIndex(int argc, char *argv[]);

typedef struct {
    const char *map_file;
    const char *move_direction;
    char player_id;
    int show_version;
} Labyrinth_Options;

struct option long_options[] = {{"map", required_argument, NULL, 'm'},
                                {"player", required_argument, NULL, 'p'},
                                {"move", required_argument, NULL, 'd'},
                                {"version", no_argument, NULL, 'v'},
                                {NULL, 0, NULL, 0}};

int main(int argc, char *argv[]) {
    // TODO: Implement this function

    int opt;

    optind = firstOptionIndex(argc, argv);

    Labyrinth_Options labyrinth_options = {0};
    while ((opt = getopt_long(argc, argv, "m:p:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'm':
            labyrinth_options.map_file = optarg;
            break;
        case 'p':
            labyrinth_options.player_id = optarg[0];
            break;
        case 'd':
            labyrinth_options.move_direction = optarg;
            break;
        case 'v':
            labyrinth_options.show_version = 1;
            break;
        default:
            printUsage();
            return 1;
        }
    }

    if (optind < argc) {
        printUsage();
        return 1;
    }

    if (labyrinth_options.show_version) {
        printf("%s\n", VERSION_INFO);
        return 0;
    }

    if (labyrinth_options.map_file == NULL) {
        fprintf(stderr, "Error: Missing map file\n");
        return 1;
    }

    if (!isValidPlayer(labyrinth_options.player_id)) {
        fprintf(stderr, "Error: Invalid player ID\n");
        return 1;
    }

    Labyrinth labyrinth = {0};
    if (!loadMap(&labyrinth, labyrinth_options.map_file)) {
        fprintf(stderr, "Error: Failed to load map\n");
        return 1;
    }

    if (!isConnected(&labyrinth)) {
        fprintf(stderr, "Error: Map is not fully connected\n");
        return 1;
    }

    printMap(&labyrinth);

    Position position = findPlayer(&labyrinth, labyrinth_options.player_id);
    if (position.row == -1 || position.col == -1) {
        Position first_empty_position = findFirstEmptySpace(&labyrinth);
        labyrinth.map[first_empty_position.row][first_empty_position.col] =
            labyrinth_options.player_id;
    }

    if (labyrinth_options.move_direction) {
        if (!movePlayer(&labyrinth, labyrinth_options.player_id,
                        labyrinth_options.move_direction)) {
            fprintf(stderr, "Error: Failed to move player\n");
            return 1;
        }

        if (!saveMap(&labyrinth, labyrinth_options.map_file)) {
            fprintf(stderr, "Error: Failed to save map\n");
            return 1;
        }
    }

    // printMap(&labyrinth);
    return 0;
}

static const char *baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int firstOptionIndex(int argc, char *argv[]) {
    if (argc > 1 && strcmp(baseName(argv[0]), baseName(argv[1])) == 0) {
        return 2;
    }
    return 1;
}

void printMap(Labyrinth *labyrinth) {
    for (int i = 0; i < labyrinth->rows; i++) {
        printf("%s\n", labyrinth->map[i]);
    }
}

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    // TODO: Implement this function
    if (playerId >= '0' && playerId <= '9') {
        return true;
    }
    return false;
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: Unable to open file %s\n", filename);
        return false;
    }

    char line[MAX_COLS + 3];
    int rows = 0;
    int cols = -1;

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t len = strcspn(line, "\n");
        bool has_newline = line[len] == '\n';

        if (!has_newline && !feof(file)) {
            fprintf(stderr, "Error: Map row exceeds maximum width\n");
            fclose(file);
            return false;
        }

        if (len > 0 && line[len - 1] == '\r') {
            len--;
        }

        if (len == 0 || len > MAX_COLS || rows >= MAX_ROWS) {
            fprintf(stderr, "Error: Invalid map dimensions\n");
            fclose(file);
            return false;
        }

        if (cols == -1) {
            cols = (int)len;
        } else if (cols != (int)len) {
            fprintf(stderr, "Error: Inconsistent map row lengths\n");
            fclose(file);
            return false;
        }

        for (int col = 0; col < (int)len; col++) {
            char cell = line[col];
            if (cell != '#' && cell != '.' && !isValidPlayer(cell)) {
                fprintf(stderr, "Error: Invalid map character\n");
                fclose(file);
                return false;
            }
            labyrinth->map[rows][col] = cell;
        }
        if (len < MAX_COLS) {
            labyrinth->map[rows][len] = '\0';
        }
        rows++;
    }

    if (ferror(file) || rows == 0) {
        fprintf(stderr, "Error: Unable to read map\n");
        fclose(file);
        return false;
    }

    labyrinth->rows = rows;
    labyrinth->cols = cols;

    fclose(file);
    return true;
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    // TODO: Implement this function
    Position pos = {-1, -1};
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            if (labyrinth->map[i][j] == playerId) {
                pos.row = i;
                pos.col = j;
                return pos;
            }
        }
    }
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    // TODO: Implement this function
    Position pos = {-1, -1};
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            if (labyrinth->map[i][j] == '.') {
                pos.row = i;
                pos.col = j;
                return pos;
            }
        }
    }
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    // TODO: Implement this function
    if (row >= 0 && row < labyrinth->rows && col >= 0 &&
        col < labyrinth->cols) {
        return labyrinth->map[row][col] == '.';
    }
    return false;
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    // TODO: Implement this function
    Position playerPos = findPlayer(labyrinth, playerId);
    if (playerPos.row == -1 || playerPos.col == -1) {
        return false;
    }

    int newRow = playerPos.row;
    int newCol = playerPos.col;

    if (strcmp(direction, "up") == 0) {
        newRow--;
    } else if (strcmp(direction, "down") == 0) {
        newRow++;
    } else if (strcmp(direction, "left") == 0) {
        newCol--;
    } else if (strcmp(direction, "right") == 0) {
        newCol++;
    } else {
        return false;
    }

    if (isEmptySpace(labyrinth, newRow, newCol)) {
        labyrinth->map[playerPos.row][playerPos.col] = '.';
        labyrinth->map[newRow][newCol] = playerId;
        return true;
    }

    return false;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    // TODO: Implement this function
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: Unable to open file %s for writing\n",
                filename);
        return false;
    }

    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            fputc(labyrinth->map[i][j], file);
        }
        fputc('\n', file);
    }

    fclose(file);
    return true;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col,
         bool visited[MAX_ROWS][MAX_COLS]) {
    if (row < 0 || row >= labyrinth->rows || col < 0 ||
        col >= labyrinth->cols || visited[row][col] ||
        labyrinth->map[row][col] == '#') {
        return;
    }

    visited[row][col] = true;
    dfs(labyrinth, row - 1, col, visited);
    dfs(labyrinth, row + 1, col, visited);
    dfs(labyrinth, row, col - 1, visited);
    dfs(labyrinth, row, col + 1, visited);
}

bool isConnected(Labyrinth *labyrinth) {
    bool visited[MAX_ROWS][MAX_COLS] = {false};
    int start_row = -1;
    int start_col = -1;

    for (int row = 0; row < labyrinth->rows; row++) {
        for (int col = 0; col < labyrinth->cols; col++) {
            if (labyrinth->map[row][col] != '#') {
                start_row = row;
                start_col = col;
                break;
            }
        }
        if (start_row != -1) {
            break;
        }
    }

    if (start_row == -1) {
        return true;
    }

    dfs(labyrinth, start_row, start_col, visited);

    for (int row = 0; row < labyrinth->rows; row++) {
        for (int col = 0; col < labyrinth->cols; col++) {
            if (labyrinth->map[row][col] != '#' && !visited[row][col]) {
                return false;
            }
        }
    }

    return true;
}
