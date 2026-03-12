#include "files.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

FileList read_directory(char *path) {
	FileList result;
	result.files = NULL;
	result.count = 0;

	int capacity = 10;
	int count = 0;
	char **files = malloc(capacity * sizeof(char *));
	if(!files) { return result; }

	DIR *dir = opendir(path);
	if (!dir) { free(files); return result; }

	struct dirent *entry;
	while((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") ==0) continue;

		if (count >= capacity) {
			capacity *=2;
			char **tmp = realloc(files, capacity * sizeof(char *));
			if (!tmp) { closedir(dir); result.files = files; result.count = count; return result; }
			files = tmp;
		}

		files[count] = strdup(entry->d_name);
		if (!files[count]) { closedir(dir); result.files = files; result.count = count; return result; }
		count++;
	}

	closedir(dir);
	result.files = files;
	result.count = count;
	return result;
}

void free_files(FileList *fl) {
	if (!fl->files) return;
	for (int i = 0; i < fl->count; i++) free(fl->files[i]);
	free(fl->files);
	fl->files = NULL;
	fl->count = 0;
}

void navigate_to(char *current_path, FileList *fl, char *new_path, int *selected) {
	free_files(fl);
	*fl = read_directory(new_path);
	if (fl->files == NULL) return;
	strncpy(current_path, new_path, PATH_MAX - 1);
	current_path[PATH_MAX - 1] = '\0';
	*selected = 0;
}
