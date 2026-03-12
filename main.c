#define _GNU_SOURCE
#include "files.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_MULTI 8192

static void free_yank(char **names, int count) {
	for (int i = 0; i < count; i++) free(names[i]);
	free(names);
}

typedef struct { char *path; FileList *fl; int mode; } SortCtx;

static int cmp_files(const void *a, const void *b, void *ctx) {
	SortCtx *c = ctx;
	int ia = *(int*)a, ib = *(int*)b;
	if (c->mode == 0) return strcasecmp(c->fl->files[ia], c->fl->files[ib]);
	if (c->mode == 1) return strcasecmp(c->fl->files[ib], c->fl->files[ia]);
	char pa[PATH_MAX], pb[PATH_MAX];
	snprintf(pa, sizeof(pa), "%s/%s", c->path, c->fl->files[ia]);
	snprintf(pb, sizeof(pb), "%s/%s", c->path, c->fl->files[ib]);
	struct stat sa = {0}, sb = {0};
	stat(pa, &sa); stat(pb, &sb);
	if (c->mode == 2) return (sa.st_size > sb.st_size) ? 1 : (sa.st_size < sb.st_size) ? -1 : 0;
	if (c->mode == 3) return (sa.st_size < sb.st_size) ? 1 : (sa.st_size > sb.st_size) ? -1 : 0;
	if (c->mode == 4) return (sa.st_mtime > sb.st_mtime) ? 1 : (sa.st_mtime < sb.st_mtime) ? -1 : 0;
	if (c->mode == 5) return (sa.st_mtime < sb.st_mtime) ? 1 : (sa.st_mtime > sb.st_mtime) ? -1 : 0;
	return 0;
}


static void save_config(int show_size, int show_time) {
	const char *home = getenv("HOME");
	if (!home) return;
	char dir[PATH_MAX], path[PATH_MAX];
	snprintf(dir,  sizeof(dir),  "%s/.config/grim", home);
	snprintf(path, sizeof(path), "%s/.config/grim/config", home);
	mkdir(dir, 0755);
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "show_size=%d\nshow_time=%d\n", show_size, show_time);
	fclose(f);
}

int main() {
	initscr();
	noecho();
	set_escdelay(0);
	keypad(stdscr, TRUE);
	start_color();
	use_default_colors();
	init_pair(1, COLOR_GREEN, -1);
	init_pair(2, COLOR_CYAN, -1);
	init_pair(3, COLOR_WHITE, -1);
	int multi_pair = 2;

	char current_path[PATH_MAX];
	if (!getcwd(current_path, sizeof(current_path))) {
		const char *home = getenv("HOME");
		strcpy(current_path, home ? home : "/");
	}

	FileList fl = read_directory(current_path);
	int selected = 0;
	int page = 0;
	char query[256] = "";
	int search_mode = 0;
	int show_keys = 0;
	int keys_page = 0;

	int   prompt_mode    = 0;
	char  prompt_buf[PATH_MAX] = "";
	int   prompt_cursor  = 0;
	int   confirm_delete = 0;
	int   confirm_ext_change = 0;
	char  pending_src[PATH_MAX] = "";
	char  pending_dst[PATH_MAX] = "";
	char  pending_old_ext[64]   = "";
	char  pending_new_ext[64]   = "";
	char  err_msg[256]   = "";

	int   show_hidden   = 1;
	int   sort_mode     = 0;
	int   show_size     = 0;
	int   show_time     = 0;
	int   show_settings = 0;
	int   settings_sel  = 0;
	char  yank_dir[PATH_MAX] = "";
	char **yank_names = NULL;
	int   yank_count  = 0;
	int   multi_sel[MAX_MULTI] = {0};
	int   multi_count = 0;

	// Load config
	{
		char cfg_path[PATH_MAX];
		const char *home = getenv("HOME");
		if (home) {
			snprintf(cfg_path, sizeof(cfg_path), "%s/.config/grim/config", home);
			FILE *cfg = fopen(cfg_path, "r");
			if (cfg) {
				char line[256];
				while (fgets(line, sizeof(line), cfg)) {
					int val;
					if (sscanf(line, "show_size=%d", &val) == 1) show_size = val ? 1 : 0;
					if (sscanf(line, "show_time=%d", &val) == 1) show_time = val ? 1 : 0;
				}
				fclose(cfg);
			}
		}
	}

	while (1) {
		int items_per_page = LINES - 1;

		// Build filtered list
		int filtered[fl.count > 0 ? fl.count : 1];
		int filtered_count = 0;
		for (int i = 0; i < fl.count; i++) {
			if (!show_hidden && fl.files[i][0] == '.') continue;
			if (query[0] == '\0' || strcasestr(fl.files[i], query) != NULL)
				filtered[filtered_count++] = i;
		}
		SortCtx sctx = { current_path, &fl, sort_mode };
		qsort_r(filtered, filtered_count, sizeof(int), cmp_files, &sctx);

		if (filtered_count == 0) selected = 0;
		else if (selected >= filtered_count) selected = filtered_count - 1;

		int page_start = page * items_per_page;
		int page_end   = page_start + items_per_page;
		if (page_end > filtered_count) page_end = filtered_count;
		int total_pages = (filtered_count + items_per_page - 1) / items_per_page;
		if (total_pages < 1) total_pages = 1;

		clear();

		if (show_keys) {
			static const char *keys[] = {
				"  j / k           move down / up",
				"  arrows          navigate / page",
				"  Enter           open file or enter dir",
				"  Backspace       go up one directory",
				"  t               open file in new terminal",
				"",
				"  /               file search",
				"  Esc             clear search / selection",
				"",
				"  d               delete item",
				"  r               rename item",
				"  m               move item",
				// page 2
				"  c               copy item",
				"  n               new file",
				"  N               new directory",
				"",
				"  y               yank item",
				"  p               paste yanked",
				"  Space           toggle select",
				"  o               open with...",
				"",
				"  H / .           toggle hidden",
				"  z               toggle size display",
				"  i               toggle modified time",
				"  s / S           cycle sort (name/size/date)",
				"  ?               toggle this help",
				"  ,               open settings",
				"  q               quit",
			};
			int nkeys = sizeof(keys) / sizeof(keys[0]);
			int kpp = 12;
			int ktotal = (nkeys + kpp - 1) / kpp;
			if (keys_page >= ktotal) keys_page = ktotal - 1;
			int kstart = keys_page * kpp;
			int kend = kstart + kpp;
			if (kend > nkeys) kend = nkeys;
			if (ktotal > 1)
				mvprintw(0, 0, "  Keybindings  [%d/%d]", keys_page + 1, ktotal);
			else
				mvprintw(0, 0, "  Keybindings");
			for (int ki = kstart; ki < kend; ki++)
				mvprintw(1 + (ki - kstart), 0, "%s", keys[ki]);
			if (ktotal > 1)
				mvprintw(LINES-1, 0, "  j/k to scroll  ? or Esc to close");
			else
				mvprintw(LINES-1, 0, "  press ? or Esc to close");
		} else if (show_settings) {
			mvprintw(0, 0, "  Settings");
			if (settings_sel == 0) attron(A_REVERSE);
			mvprintw(2, 2, "[%c] Show file sizes by default", show_size ? 'x' : ' ');
			if (settings_sel == 0) attroff(A_REVERSE);
			if (settings_sel == 1) attron(A_REVERSE);
			mvprintw(3, 2, "[%c] Show last modified by default", show_time ? 'x' : ' ');
			if (settings_sel == 1) attroff(A_REVERSE);
			mvprintw(LINES-1, 0, "  j/k to move  Space/Enter to toggle  , or Esc to close");
		} else {

		// Draw loop
		for (int fi = page_start; fi < page_end; fi++) {
			int i = filtered[fi];
			char full_path[PATH_MAX];
			snprintf(full_path, sizeof(full_path), "%s/%s", current_path, fl.files[i]);

			struct stat st;
			int stat_ok = (stat(full_path, &st) == 0);
			int is_dir = stat_ok && S_ISDIR(st.st_mode);
			int is_multi = (i < MAX_MULTI) && multi_sel[i];

			// Build info suffix
			char info_buf[64] = "";
			if (stat_ok && (show_size || show_time)) {
				char size_str[20] = "";
				char time_str[24] = "";
				if (show_size && !is_dir) {
					off_t sz = st.st_size;
					if      (sz < 1024LL)           snprintf(size_str, sizeof(size_str), "%lldB", (long long)sz);
					else if (sz < 1024LL*1024)      snprintf(size_str, sizeof(size_str), "%.1fKb", sz/1024.0);
					else if (sz < 1024LL*1024*1024) snprintf(size_str, sizeof(size_str), "%.1fMb", sz/(1024.0*1024));
					else                            snprintf(size_str, sizeof(size_str), "%.1fGb", sz/(1024.0*1024*1024));
				}
				if (show_time) {
					struct tm *tm = localtime(&st.st_mtime);
					strftime(time_str, sizeof(time_str), "%d-%m-%Y %H:%M", tm);
				}
				int has_size = show_size && !is_dir;
				if (has_size && show_time)
					snprintf(info_buf, sizeof(info_buf), "  [%s | %s]", size_str, time_str);
				else if (has_size)
					snprintf(info_buf, sizeof(info_buf), "  [%s]", size_str);
				else if (show_time)
					snprintf(info_buf, sizeof(info_buf), "  [%s]", time_str);
			}

			if (fi == selected) attron(A_REVERSE);
			if (is_multi)    attron(A_BOLD | COLOR_PAIR(multi_pair));
			else if (is_dir) attron(A_BOLD | COLOR_PAIR(1));

			printw("%s%s", is_dir ? "/" : "", fl.files[i]);
			if (info_buf[0]) {
				if (is_multi) {
					attroff(A_BOLD);  // keep COLOR_PAIR(multi_pair) active
					printw("%s", info_buf);
				} else {
					attroff(A_BOLD | COLOR_PAIR(1) | COLOR_PAIR(multi_pair));
					if (fi == selected) {
						printw("%s", info_buf);  // A_REVERSE still active
					} else {
						attron(A_DIM | COLOR_PAIR(3));
						printw("%s", info_buf);
						attroff(A_DIM | COLOR_PAIR(3));
					}
				}
			}
			printw("\n");

			attroff(A_BOLD | COLOR_PAIR(1) | COLOR_PAIR(multi_pair));
			if (fi == selected) attroff(A_REVERSE);
		}

		// Status / query bar
		if (confirm_ext_change) {
			mvprintw(LINES-1, 0, "Change extension .%s -> .%s? [y/N]", pending_old_ext, pending_new_ext);
		} else if (confirm_delete) {
			if (multi_count > 0)
				mvprintw(LINES-1, 0, "Delete %d selected items? [y/N]", multi_count);
			else if (filtered_count > 0) {
				int i = filtered[selected];
				mvprintw(LINES-1, 0, "Delete \"%s\"? [y/N]", fl.files[i]);
			}
		} else if (prompt_mode) {
			const char *labels[] = {"", "Rename: ", "Move to: ", "Copy to: ", "New file: ", "New dir: ", "Open with: "};
			int label_len = strlen(labels[prompt_mode]);
			mvprintw(LINES-1, 0, "%s", labels[prompt_mode]);
			// chars before cursor
			if (prompt_cursor > 0) mvprintw(LINES-1, label_len, "%.*s", prompt_cursor, prompt_buf);
			// cursor character (reverse highlight, or '_' if at end)
			int buf_len = strlen(prompt_buf);
			if (prompt_cursor < buf_len) {
				attron(A_REVERSE);
				mvprintw(LINES-1, label_len + prompt_cursor, "%c", prompt_buf[prompt_cursor]);
				attroff(A_REVERSE);
			} else {
				mvprintw(LINES-1, label_len + prompt_cursor, "_");
			}
			// chars after cursor
			if (prompt_cursor < buf_len) mvprintw(LINES-1, label_len + prompt_cursor + 1, "%s", prompt_buf + prompt_cursor + 1);
		} else if (err_msg[0] != '\0') {
			mvprintw(LINES-1, 0, "! %s", err_msg);
			err_msg[0] = '\0';
		} else if (search_mode || query[0] != '\0') {
			if (total_pages > 1) { mvprintw(LINES-1, 0, "> %s_  (%d matches) [%d/%d]", query, filtered_count, page + 1, total_pages); }
			else { mvprintw(LINES-1, 0, "> %s_  (%d matches)", query, filtered_count); }
		} else {
			// Build display path: current dir + up to 4 parents, prepend ../ if deeper
			char disp_path[PATH_MAX];
			{ char tmp[PATH_MAX]; strncpy(tmp, current_path, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
			  char *comps[256]; int nc = 0;
			  char *tok = strtok(tmp, "/");
			  while (tok && nc < 256) { comps[nc++] = tok; tok = strtok(NULL, "/"); }
			  int start = nc > 5 ? nc - 5 : 0;
			  disp_path[0] = '\0';
			  if (start > 0) strcpy(disp_path, "../");
			  for (int i = start; i < nc; i++) { strcat(disp_path, comps[i]); if (i < nc-1) strcat(disp_path, "/"); } }
			const char *snames[] = {"name [desc]", "name [asc]", "size [desc]", "size [asc]", "date [desc]", "date [asc]"};
			char sel_suffix[64] = "";
			if (multi_count > 0) snprintf(sel_suffix, sizeof(sel_suffix), ", %d selected", multi_count);
			if (total_pages > 1)
				mvprintw(LINES-1, 0, "@%s (%d items%s, sort: %s) [%d/%d]", disp_path, fl.count, sel_suffix, snames[sort_mode], page + 1, total_pages);
			else
				mvprintw(LINES-1, 0, "@%s (%d items%s, sort: %s)", disp_path, fl.count, sel_suffix, snames[sort_mode]);
			mvprintw(LINES-1, COLS-36, "[/] Search  [?] Help  [,] Settings");
		}

		} // end !show_keys

		refresh();
		int key = getch();

		if (key == 'q' && !search_mode && !prompt_mode && !confirm_delete && !confirm_ext_change) break;

		if (key == '?' && !search_mode && !prompt_mode && !confirm_delete && !confirm_ext_change) { show_keys = !show_keys; keys_page = 0; continue; }
		if (key == ',' && !search_mode && !prompt_mode && !confirm_delete && !confirm_ext_change) { show_settings = !show_settings; settings_sel = 0; continue; }

		if (show_settings) {
			if (key == 27 || key == ',') { show_settings = 0; }
			else if (key == KEY_UP   || key == 'k') { if (settings_sel > 0) settings_sel--; }
			else if (key == KEY_DOWN || key == 'j') { if (settings_sel < 1) settings_sel++; }
			else if (key == ' ' || key == 10 || key == '\n') {
				if (settings_sel == 0) show_size = !show_size;
				else                   show_time = !show_time;
				save_config(show_size, show_time);
			}
			continue;
		}

		if (show_keys) {
			int _kpp = 12;
			int _ktotal = (26 + _kpp - 1) / _kpp;
			if (key == KEY_DOWN || key == 'j' || key == KEY_RIGHT) { if (keys_page < _ktotal - 1) keys_page++; continue; }
			if (key == KEY_UP   || key == 'k' || key == KEY_LEFT)  { if (keys_page > 0) keys_page--; continue; }
		}

		if (confirm_ext_change) {
			if (key == 'y' || key == 'Y') {
				if (rename(pending_src, pending_dst) != 0)
					snprintf(err_msg, sizeof(err_msg), "Rename failed");
				else { navigate_to(current_path, &fl, current_path, &selected); page = 0; memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0; }
			}
			confirm_ext_change = 0;
			continue;
		} else if (confirm_delete) {
			if (key == 'y' || key == 'Y') {
				if (multi_count > 0) {
					for (int j = 0; j < fl.count && j < MAX_MULTI; j++) {
						if (!multi_sel[j]) continue;
						char fp[PATH_MAX];
						snprintf(fp, sizeof(fp), "%s/%s", current_path, fl.files[j]);
						if (remove(fp) != 0) {
							char cmd[PATH_MAX + 20];
							snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", fp);
							system(cmd);
						}
					}
					memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
				} else if (filtered_count > 0) {
					int i = filtered[selected];
					char full_path[PATH_MAX];
					snprintf(full_path, sizeof(full_path), "%s/%s", current_path, fl.files[i]);
					if (remove(full_path) != 0) {
						char cmd[PATH_MAX + 20];
						snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", full_path);
						system(cmd);
					}
				}
				navigate_to(current_path, &fl, current_path, &selected);
				page = 0; memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
			}
			confirm_delete = 0;
			continue;
		} else if (prompt_mode) {
			if (key == 27) { prompt_mode = 0; prompt_buf[0] = '\0'; prompt_cursor = 0; }
			else if (key == KEY_LEFT) {
				if (prompt_cursor > 0) prompt_cursor--;
			} else if (key == KEY_RIGHT) {
				if (prompt_cursor < (int)strlen(prompt_buf)) prompt_cursor++;
			} else if (key == KEY_BACKSPACE || key == 8 || key == 127) {
				int len = strlen(prompt_buf);
				if (prompt_cursor > 0) {
					memmove(prompt_buf + prompt_cursor - 1, prompt_buf + prompt_cursor, len - prompt_cursor + 1);
					prompt_cursor--;
				}
			} else if (key == 10 || key == '\n') {
				if (prompt_buf[0] != '\0') {
					int i = filtered_count > 0 ? filtered[selected] : -1;
					char src[PATH_MAX];
					if (i >= 0) snprintf(src, sizeof(src), "%s/%s", current_path, fl.files[i]);

					char dst[PATH_MAX];
					if (prompt_buf[0] == '/') snprintf(dst, sizeof(dst), "%s", prompt_buf);
					else snprintf(dst, sizeof(dst), "%s/%s", current_path, prompt_buf);

					int needs_reload = 1;
					switch (prompt_mode) {
						case 1: // rename
						{
							const char *old_dot = strrchr(fl.files[i >= 0 ? i : 0], '.');
							const char *new_dot = strrchr(prompt_buf, '.');
							const char *old_ext = old_dot ? old_dot + 1 : "";
							const char *new_ext = new_dot ? new_dot + 1 : "";
							if (strcasecmp(old_ext, new_ext) != 0) {
								strncpy(pending_src, src, sizeof(pending_src)-1);
								strncpy(pending_dst, dst, sizeof(pending_dst)-1);
								strncpy(pending_old_ext, old_ext, sizeof(pending_old_ext)-1);
								strncpy(pending_new_ext, new_ext, sizeof(pending_new_ext)-1);
								confirm_ext_change = 1;
								needs_reload = 0;
							} else {
								if (rename(src, dst) != 0) snprintf(err_msg, sizeof(err_msg), "Rename failed");
							}
							break;
						}
						case 2: // move
							if (multi_count > 0) {
								for (int j = 0; j < fl.count && j < MAX_MULTI; j++) {
									if (!multi_sel[j]) continue;
									char ms[PATH_MAX], md[PATH_MAX];
									snprintf(ms, sizeof(ms), "%s/%s", current_path, fl.files[j]);
									snprintf(md, sizeof(md), "%s/%s", dst, fl.files[j]);
									if (rename(ms, md) != 0) snprintf(err_msg, sizeof(err_msg), "Move failed: %s", fl.files[j]);
								}
								memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
							} else {
								if (rename(src, dst) != 0) snprintf(err_msg, sizeof(err_msg), "Move failed");
							}
							break;
						case 3: // copy
							if (multi_count > 0) {
								for (int j = 0; j < fl.count && j < MAX_MULTI; j++) {
									if (!multi_sel[j]) continue;
									char ms[PATH_MAX], md[PATH_MAX];
									snprintf(ms, sizeof(ms), "%s/%s", current_path, fl.files[j]);
									snprintf(md, sizeof(md), "%s/%s", dst, fl.files[j]);
									char cmd[PATH_MAX*2+20];
									snprintf(cmd, sizeof(cmd), "cp -r \"%s\" \"%s\"", ms, md);
									if (system(cmd) != 0) snprintf(err_msg, sizeof(err_msg), "Copy failed: %s", fl.files[j]);
								}
								memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
							} else {
								char cmd[PATH_MAX*2+20];
								snprintf(cmd, sizeof(cmd), "cp -r \"%s\" \"%s\"", src, dst);
								if (system(cmd) != 0) snprintf(err_msg, sizeof(err_msg), "Copy failed");
							}
							break;
						case 4: // new file
							{ FILE *f = fopen(dst, "wx");
							  if (f) fclose(f);
							  else snprintf(err_msg, sizeof(err_msg), "Could not create file"); }
							break;
						case 5: // new dir
							if (mkdir(dst, 0755) != 0) snprintf(err_msg, sizeof(err_msg), "Could not create dir");
							break;
					}
					if (needs_reload) { navigate_to(current_path, &fl, current_path, &selected); page = 0; memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0; }
				}
				prompt_mode = 0; prompt_buf[0] = '\0'; prompt_cursor = 0;
			} else if (key >= 32 && key < 127) {
				int len = strlen(prompt_buf);
				if (len < (int)sizeof(prompt_buf)-1) {
					memmove(prompt_buf + prompt_cursor + 1, prompt_buf + prompt_cursor, len - prompt_cursor + 1);
					prompt_buf[prompt_cursor] = key;
					prompt_cursor++;
				}
			}
		} else if (key == '/') {
			search_mode = 1; query[0] = '\0'; selected = 0; page = 0;
		} else if (search_mode) {
			if (key == 27) { // Escape
				search_mode = 0; query[0] = '\0'; selected = 0; page = 0;
			} else if (key == KEY_BACKSPACE || key == 8 || key == 127) {
				int qlen = strlen(query);
				if (qlen > 0) { query[qlen - 1] = '\0'; selected = 0; page = 0; }
				else { search_mode = 0; }
			} else if (key >= 32 && key < 127) {
				int qlen = strlen(query);
				if (qlen < (int)sizeof(query) - 1) {
					query[qlen] = (char)key;
					query[qlen + 1] = '\0';
					selected = 0; page = 0;
				}
			} else if (key == KEY_LEFT) {
				if (page > 0) { page--; selected = page * items_per_page; }
			} else if (key == KEY_RIGHT) {
				if (page < total_pages - 1) { page++; selected = page * items_per_page; }
			} else if (key == KEY_UP || key == 'k') {
				if (selected > 0) {
					selected--;
					if (selected < page_start) page--;
				}
			} else if (key == KEY_DOWN || key == 'j') {
				if (selected < filtered_count - 1) {
					selected++;
					if (selected >= page_end) page++;
				}
			} else if (key == KEY_ENTER || key == 10 || key == '\n') {
				if (filtered_count > 0) {
					int i = filtered[selected];
					char new_path[PATH_MAX];
					char resolved[PATH_MAX];
					snprintf(new_path, sizeof(new_path), "%s/%s", current_path, fl.files[i]);
					if (realpath(new_path, resolved) != NULL) {
						struct stat st;
						if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
							navigate_to(current_path, &fl, resolved, &selected);
							memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
							search_mode = 0; query[0] = '\0'; page = 0;
						} else {
							endwin();
							char cmd[PATH_MAX + 10];
							snprintf(cmd, sizeof(cmd), "micro \"%s\"", resolved);
							system(cmd);
							refresh();
							clearok(stdscr, TRUE);
						}
					}
				}
			}
		} else {
			if (key == 27) {
				show_keys = 0;
				memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
			}
			if (key == KEY_UP || key == 'k') {
				if (selected > 0) {
					selected--;
					if (selected < page_start) page--;
				}
			}
			if (key == KEY_DOWN || key == 'j') {
				if (selected < filtered_count - 1) {
					selected++;
					if (selected >= page_end) page++;
				}
			}
			if (key == KEY_LEFT) {
				if (page > 0) { page--; selected = page * items_per_page; }
			}
			if (key == KEY_RIGHT) {
				if (page < total_pages - 1) { page++; selected = page * items_per_page; }
			}
			if (key == KEY_ENTER || key == 10 || key == '\n') {
				if (filtered_count > 0) {
					int i = filtered[selected];
					char new_path[PATH_MAX];
					char resolved[PATH_MAX];
					snprintf(new_path, sizeof(new_path), "%s/%s", current_path, fl.files[i]);
					if (realpath(new_path, resolved) != NULL) {
						struct stat st;
						if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
							navigate_to(current_path, &fl, resolved, &selected);
							memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
							search_mode = 0; query[0] = '\0'; page = 0;
						} else {
							endwin();
							char cmd[PATH_MAX + 10];
							snprintf(cmd, sizeof(cmd), "micro \"%s\"", resolved);
							system(cmd);
							refresh();
							clearok(stdscr, TRUE);
						}
					}
				}
			}
			if (key == KEY_BACKSPACE || key == 8 || key == 127) {
				char *last_slash = strrchr(current_path, '/');
				if (last_slash && last_slash != current_path) { *last_slash = '\0'; }
				else { strcpy(current_path, "/"); }
				navigate_to(current_path, &fl, current_path, &selected);
				memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
				search_mode = 0; query[0] = '\0'; page = 0;
			}
			if (key == 't') {
				if (filtered_count > 0) {
					int i = filtered[selected];
					char full_path[PATH_MAX];
					snprintf(full_path, sizeof(full_path), "%s/%s", current_path, fl.files[i]);
					struct stat st;
					if (stat(full_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
						char cmd[PATH_MAX + 50];
						snprintf(cmd, sizeof(cmd), "foot micro \"%s\" &", full_path);
						system(cmd);
					}
				}
			}
			if (key == 'd' && !show_keys && filtered_count > 0) confirm_delete = 1;
			if (key == 'r' && !show_keys && filtered_count > 0) {
				prompt_mode = 1;
				int i = filtered[selected];
				strncpy(prompt_buf, fl.files[i], sizeof(prompt_buf)-1);
				prompt_cursor = strlen(prompt_buf);
			}
			if (key == 'm' && !show_keys && filtered_count > 0) { prompt_mode = 2; prompt_buf[0] = '\0'; prompt_cursor = 0; }
			if (key == 'c' && !show_keys && filtered_count > 0) { prompt_mode = 3; prompt_buf[0] = '\0'; prompt_cursor = 0; }
			if (key == 'n' && !show_keys) { prompt_mode = 4; prompt_buf[0] = '\0'; prompt_cursor = 0; }
			if (key == 'N' && !show_keys) { prompt_mode = 5; prompt_buf[0] = '\0'; prompt_cursor = 0; }
			if ((key == 'H' || key == '.') && !show_keys) { show_hidden = !show_hidden; selected = 0; page = 0; }
		if (key == 'z' && !show_keys) show_size = !show_size;
		if (key == 'i' && !show_keys) show_time = !show_time;
			if (key == 's' && !show_keys) sort_mode = (sort_mode + 1) % 6;
			if (key == 'S' && !show_keys) sort_mode = (sort_mode + 5) % 6;
			if (key == 'y' && !show_keys && filtered_count > 0) {
				if (yank_names) free_yank(yank_names, yank_count);
				strncpy(yank_dir, current_path, sizeof(yank_dir)-1);
				if (multi_count > 0) {
					yank_names = malloc(multi_count * sizeof(char*));
					yank_count = 0;
					for (int j = 0; j < fl.count && j < MAX_MULTI; j++) {
						if (multi_sel[j]) yank_names[yank_count++] = strdup(fl.files[j]);
					}
					snprintf(err_msg, sizeof(err_msg), "Yanked %d items", yank_count);
				} else {
					int i = filtered[selected];
					yank_names = malloc(sizeof(char*));
					yank_names[0] = strdup(fl.files[i]);
					yank_count = 1;
					snprintf(err_msg, sizeof(err_msg), "Yanked %s", fl.files[i]);
				}
			}
			if (key == 'p' && !show_keys && yank_count > 0) {
				for (int j = 0; j < yank_count; j++) {
					char src[PATH_MAX], dst[PATH_MAX];
					snprintf(src, sizeof(src), "%s/%s", yank_dir, yank_names[j]);
					snprintf(dst, sizeof(dst), "%s/%s", current_path, yank_names[j]);
					char cmd[PATH_MAX*2+20];
					snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", src, dst);
					if (system(cmd) != 0) snprintf(err_msg, sizeof(err_msg), "Paste failed: %s", yank_names[j]);
				}
				free_yank(yank_names, yank_count); yank_names = NULL; yank_count = 0;
				navigate_to(current_path, &fl, current_path, &selected); page = 0;
				memset(multi_sel, 0, sizeof(multi_sel)); multi_count = 0;
			}
			if (key == ' ' && !show_keys && filtered_count > 0) {
				int i = filtered[selected];
				if (i < MAX_MULTI) {
					if (multi_sel[i]) { multi_sel[i] = 0; multi_count--; }
					else              { multi_sel[i] = 1; multi_count++; }
				}
			}
			if (key == 'o' && !show_keys && filtered_count > 0) {
				prompt_mode = 6; prompt_buf[0] = '\0'; prompt_cursor = 0;
			}
		}
	}

	endwin();
	free_files(&fl);
	if (yank_names) free_yank(yank_names, yank_count);
	return 0;
}
