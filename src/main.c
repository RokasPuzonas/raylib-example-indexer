#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/param.h>
#include <dirent.h>
#include <string.h>
#include <assert.h>

#include "stb_c_lexer.h"

#define MAX_FUNCS_TO_PARSE 1024 // Maximum number of functions to parse
#define MAX_FUNC_USAGES    1024 // Maximum number of usages per function

typedef struct function_info {
    char name[64];
	int name_size;
	int param_count;
} function_info;

typedef struct function_usage {
	char filename[PATH_MAX];
	int line_number;
	int line_offset;
} function_usage;

typedef struct line_range {
	int from, to; // [from, to) - from inclusive, to exclusive
} line_range;

static int get_file_size(FILE *file) {
	fseek(file, 0, SEEK_END);
	int size = ftell(file);
	fseek(file, 0, SEEK_SET);
	return size;
}

static bool get_next_line(line_range *line, char *text, int text_size, int from) {
	for (int i = from; i < text_size; i++) {
		if (text[i] == '\n') {
			line->from = from;
			line->to = i;
			return true;
		}
	}
	return false;
}

static int skip_next_lines(char *text, int text_size, int line_count, int from) {
	int next_line_from = from;
	line_range curr = { 0 };

	for (int i = 0; i < line_count; i++) {
		if (!get_next_line(&curr, text, text_size, next_line_from)) break;
		next_line_from = curr.to+1;
	}

	return next_line_from;
}

static int find_start_of_function_block(char *raylib_api, int raylib_api_size) {
	int next_line_from = 0;
	line_range curr = { 0 };
	while (get_next_line(&curr, raylib_api, raylib_api_size, next_line_from)) {
		int line_size = curr.to - curr.from;
		char *line = &raylib_api[curr.from];

		if (line_size >= sizeof("Functions found:") && !strncmp(line, "Functions found:", sizeof("Functions found:")-1)) {
			line_range next_line;
			if (get_next_line(&next_line, raylib_api, raylib_api_size, curr.to+1)) {
				return next_line.to+1;
			} else {
				return -1;
			}
		}

		next_line_from = curr.to+1;
	}

	return -1;
}

static bool parse_function_info(char *line, int line_size, function_info *info) {
	char *name = strchr(line, ':') + 2;
	int name_size = strchr(line, '(') - name;
	strncpy(info->name, name, name_size);
	info->name_size = name_size;

	int param_count = strtoul(name + name_size + 4, NULL, 10);
	info->param_count = param_count;

	return true;
}

static int parse_funcs_from_raylib_api(char *raylib_api, int raylib_api_size, function_info *funcs, int max_funcs) {
	int start_of_functions = find_start_of_function_block(raylib_api, raylib_api_size);
	if (start_of_functions == -1) {
		return -1;
	}

	int count = 0;

	int next_line_from = start_of_functions;
	line_range curr = { 0 };
	while (get_next_line(&curr, raylib_api, raylib_api_size, next_line_from)) {
		int line_size = curr.to - curr.from;
		char *line = &raylib_api[curr.from];

		function_info *func_info = &funcs[count];
		if (!parse_function_info(line, line_size, func_info)) {
			fprintf(stderr, "Failed to parse function line: %.*s\n", line_size, line);
			return -1;
		}
		count++;

		int skip_count = 3 + MAX(func_info->param_count, 1);
		next_line_from = skip_next_lines(raylib_api, raylib_api_size, skip_count, curr.to+1);

		if (max_funcs == count) break;
	}

	return count;
}

static int get_func_from_identifier(char *id, function_info *funcs, int func_count) {
	int id_size = strlen(id);
	for (int i = 0; i < func_count; i++) {
		function_info *func = &funcs[i];
		if (id_size != func->name_size) continue;
		if (!strncmp(id, func->name, func->name_size)) {
			return i;
		}
	}
	return -1;
}

static bool collect_function_usages_from_file(char *directory, char *file_path, function_usage *usages[], int *usage_counts, function_info *funcs, int func_count) {
	char full_path[PATH_MAX] = { 0 };
	snprintf(full_path, sizeof(full_path), "%s/%s", directory, file_path);
	FILE *file = fopen(full_path, "r");
	if (file == NULL) {
		fprintf(stderr, "Failed to open file '%s'\n", full_path);
		return false;
	}

	int file_size = get_file_size(file);
	char *example_code = malloc(file_size);
	fread(example_code, sizeof(char), file_size, file);

	stb_lexer lexer;
	char string_store[512];
	stb_c_lexer_init(&lexer, example_code, example_code+file_size, string_store, sizeof(string_store));
	while (stb_c_lexer_get_token(&lexer)) {
		if (lexer.token != CLEX_id) continue;

		int func_idx = get_func_from_identifier(lexer.string, funcs, func_count);
		if (func_idx != -1) {
			stb_lex_location loc;
			stb_c_lexer_get_location(&lexer, lexer.where_firstchar, &loc);
			int *usage_count = &usage_counts[func_idx];
			assert(*usage_count < MAX_FUNC_USAGES);
			function_usage *usage = &usages[func_idx][*usage_count];
			usage->line_number = loc.line_number;
			usage->line_offset = loc.line_offset;
			strncpy(usage->filename, file_path, strlen(file_path));
			(*usage_count)++;
		}
	}

	free(example_code);
	fclose(file);

	return true;
}

static void collect_function_usages_from_folder(char *cwd, char *dir, function_usage *usages[], int *usage_counts, function_info *funcs, int func_count) {
	char dir_path[PATH_MAX];
	snprintf(dir_path, sizeof(dir_path), "%s/%s", cwd, dir);
	DIR *dirp = opendir(dir_path);
	if (dirp == NULL) {
		fprintf(stderr, "Failed to open directory '%s'\n", dir_path);
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(dirp)) != NULL) {
		if (entry->d_type != DT_REG) continue;

		char *extension = strrchr(entry->d_name, '.');
		if (!strcmp(extension, ".c")) {
			char file_path[PATH_MAX];
			snprintf(file_path, sizeof(file_path), "%s/%s", dir, entry->d_name);
			collect_function_usages_from_file(cwd, file_path, usages, usage_counts, funcs, func_count);
		}
	}

	closedir(dirp);
}

int main(int argc, char **argv) {
	if (argc != 4) {
		printf("Usage: %s <raylib_api.txt> <examples-dir> <output-file>\n", argv[0]);
		return -1;
	}

	char *raylib_api_path = argv[1];
	char *raylib_examples_path = argv[2];
	char *output_path = argv[3];

	char *output_extension = strrchr(output_path, '.');
	if (output_extension == NULL) {
		fprintf(stderr, "ERROR: Missing extension on output file\n");
		return -1;
	}

	function_info funcs[MAX_FUNCS_TO_PARSE];
	int funcs_count = 0;

	{ // Collect function definitions
		FILE *raylib_api_file = fopen(raylib_api_path, "r");
		if (raylib_api_file == NULL) {
			fprintf(stderr, "Failed to open file '%s'\n", raylib_api_path);
			return -1;
		}
		int raylib_api_size = get_file_size(raylib_api_file);
		char *raylib_api = malloc(raylib_api_size);
		fread(raylib_api, sizeof(char), raylib_api_size, raylib_api_file);
		fclose(raylib_api_file);

		funcs_count = parse_funcs_from_raylib_api(raylib_api, raylib_api_size, funcs, MAX_FUNCS_TO_PARSE);

		free(raylib_api);
	}

	function_usage *usages[MAX_FUNCS_TO_PARSE] = { 0 };
	for (int i = 0; i < funcs_count; i++) {
		usages[i] = malloc(MAX_FUNC_USAGES * sizeof(function_usage));
	}
	int usage_counts[MAX_FUNCS_TO_PARSE] = { 0 };

	{ // Collect function usages
		DIR *dirp = opendir(raylib_examples_path);
		if (dirp == NULL) {
			fprintf(stderr, "Failed to open directory '%s'\n", raylib_examples_path);
			return -1;
		}
		struct dirent *entry;
		while ((entry = readdir(dirp)) != NULL) {
			if (entry->d_type != DT_DIR) continue;
			if (entry->d_name[0] == '.') continue;

			collect_function_usages_from_folder(raylib_examples_path, entry->d_name, usages, usage_counts, funcs, funcs_count);
		}
		closedir(dirp);
	}

	// Output function usages
	FILE *output_file = fopen(output_path, "w");
	if (output_file == NULL) {
		fprintf(stderr, "Failed to open file '%s\n'", output_path);
		return -1;
	}

	fwrite("{\n", sizeof(char), 2, output_file);
	for (int func_idx = 0; func_idx < funcs_count; func_idx++) {
		function_info *info = &funcs[func_idx];

		fwrite("\t\"", sizeof(char), 2, output_file);
		fwrite(info->name, sizeof(char), info->name_size, output_file);
		fwrite("\": [", sizeof(char), 4, output_file);

		int usage_count = usage_counts[func_idx];
		if (usage_count > 0) {
			fwrite("\n", sizeof(char), 1, output_file);

			for (int i = 0; i < usage_count; i++) {
				function_usage *usage = &usages[func_idx][i];
				char *example_name = strchr(usage->filename, '/')+1;
				int example_name_size = strchr(usage->filename, '.') - example_name;

				char *entry_format = ""
					"\t\t{\n"
					"\t\t\t\"exampleName\": \"%.*s\",\n"
					"\t\t\t\"lineNumber\": %d,\n"
					"\t\t\t\"lineOffset\": %d\n"
					"\t\t}";
				char entry[1024];
				int entry_size = snprintf(entry, sizeof(entry), entry_format,
						example_name_size,
						example_name,
						usage->line_number,
						usage->line_offset);

				fwrite(entry, sizeof(char), entry_size, output_file);
				if (i < usage_count-1) {
					fwrite(",", sizeof(char), 1, output_file);
				}
				fwrite("\n", sizeof(char), 1, output_file);
			}

			fwrite("\t", sizeof(char), 1, output_file);
		}

		fwrite("]", sizeof(char), 1, output_file);
		if (func_idx < funcs_count-1) {
			fwrite(",", sizeof(char), 1, output_file);
		}
		fwrite("\n", sizeof(char), 1, output_file);
	}

	fwrite("}", sizeof(char), 1, output_file);
	fclose(output_file);

	for (int i = 0; i < funcs_count; i++) {
		free(usages[i]);
	}

	return 0;
}
