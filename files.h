#ifndef FILES_H
#define FILES_H

#include <limits.h>

typedef struct {
	char **files;
	int count;
} FileList;

FileList read_directory(char *path);
void free_files(FileList *fl);
void navigate_to(char *current_path, FileList *fl, char *new_path, int *selected);

#endif
