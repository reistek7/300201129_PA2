#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE 102
#define MAX_ARGS 12
#define HISTORY_SIZE 10

static char history[HISTORY_SIZE][MAX_LINE];
static int history_start = 0;
static int history_count = 0;

static int exec_single(char *args[], int background);

/* This function removes new line from input. */
static void trim_newline(char *line) {
    size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
    }
}

/* This function clears extra spaces. */
static void trim_spaces(char *text) {
    char *start = text;
    char *end;
    size_t len;

    while (*start == ' ' || *start == '\t') {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    len = strlen(text);
    if (len == 0) {
        return;
    }

    end = text + len - 1;
    while (end >= text && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }
}

/* This function checks empty input. */
static int is_empty_line(const char *line) {
    while (*line != '\0') {
        if (*line != ' ' && *line != '\t') {
            return 0;
        }
        line++;
    }
    return 1;
}

/* 
 * This function saves the successfully parsed user command into the history array.
 * It correctly acts as a Circular Buffer (FIFO sequence) taking capacity (HISTORY_SIZE = 10) into mind.
 * If 10 elements are reached, history_start progresses to overwrite the oldest command.
 * According to instructions, this also saves the "history" command itself.
 */
static void add_history(const char *line) {
    int index;

    if (history_count < HISTORY_SIZE) {
        index = (history_start + history_count) % HISTORY_SIZE;
        history_count++;
    } else {
        index = history_start;
        history_start = (history_start + 1) % HISTORY_SIZE;
    }

    strncpy(history[index], line, MAX_LINE - 1);
    history[index][MAX_LINE - 1] = '\0';
}

/* This function prints last 10 commands. */
static void print_history(void) {
    int i;

    for (i = 0; i < history_count; i++) {
        int index = (history_start + i) % HISTORY_SIZE;
        printf("[%d] %s\n", i + 1, history[index]);
    }
}

/* This function splits line into arguments. */
static int parse_args(char *command, char *args[]) {
    int count = 0;
    char *token = strtok(command, " ");

    while (token != NULL) {
        if (count >= MAX_ARGS - 1) {
            fprintf(stderr, "Too many arguments.\n");
            return -1;
        }

        args[count++] = token;
        token = strtok(NULL, " ");
    }

    args[count] = NULL;
    return count;
}

/* This function checks built-in command name. */
static int is_builtin_command(const char *name) {
    return strcmp(name, "cd") == 0 ||
           strcmp(name, "pwd") == 0 ||
           strcmp(name, "mkdir") == 0 ||
           strcmp(name, "rmdir") == 0 ||
           strcmp(name, "history") == 0 ||
           strcmp(name, "exit") == 0;
}

/* This function changes current folder. */
static int builtin_cd(char *args[], int argc) {
    char *target;
    char cwd[PATH_MAX];

    if (argc > 2) {
        fprintf(stderr, "cd: too many arguments\n");
        return 1;
    }

    if (argc == 1) {
        target = getenv("HOME");
        if (target == NULL) {
            fprintf(stderr, "cd: HOME is not set\n");
            return 1;
        }
    } else {
        target = args[1];
    }

    if (chdir(target) != 0) {
        perror("cd");
        return 1;
    }

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        if (setenv("PWD", cwd, 1) != 0) {
            perror("setenv");
        }
    }

    return 0;
}

/* This function prints current folder. */
static int builtin_pwd(int argc) {
    char cwd[PATH_MAX];

    if (argc != 1) {
        fprintf(stderr, "pwd: too many arguments\n");
        return 1;
    }

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("pwd");
        return 1;
    }

    printf("%s\n", cwd);
    return 0;
}

/* This function creates empty folders. */
static int builtin_mkdir(char *args[], int argc) {
    int i;

    if (argc < 2) {
        fprintf(stderr, "mkdir: missing operand\n");
        return 1;
    }

    if (argc > 11) {
        fprintf(stderr, "mkdir: you can create at most 10 directories\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (mkdir(args[i], 0777) != 0) {
            perror(args[i]);
        }
    }

    return 0;
}

/* This function removes one empty folder. */
static int builtin_rmdir(char *args[], int argc) {
    if (argc != 2) {
        fprintf(stderr, "rmdir: give one directory name\n");
        return 1;
    }

    if (rmdir(args[1]) != 0) {
        perror(args[1]);
        return 1;
    }

    return 0;
}

/* This function runs built-in commands. */
static int run_builtin(char *args[], int argc) {
    if (argc == 0) {
        return 0;
    }

    if (strcmp(args[0], "cd") == 0) {
        return builtin_cd(args, argc);
    }

    if (strcmp(args[0], "pwd") == 0) {
        return builtin_pwd(argc);
    }

    if (strcmp(args[0], "mkdir") == 0) {
        return builtin_mkdir(args, argc);
    }

    if (strcmp(args[0], "rmdir") == 0) {
        return builtin_rmdir(args, argc);
    }

    if (strcmp(args[0], "history") == 0) {
        print_history();
        return 0;
    }

    if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }

    return -1;
}

/* 
 * This function runs simple system and builtin commands.
 * For system commands, fork() duplicates the process. The child uses execvp() to completely
 * overwrite its memory space and execute the given binary. If background mode is enabled (param=1),
 * the parent prints the child's PID and does not wait. If foreground, parent blocks via waitpid().
 */
static int execute_simple_command(char *command, int background) {
    char *args[MAX_ARGS];
    int argc;
    int builtin_result;
    pid_t pid;
    int status;

    trim_spaces(command);
    argc = parse_args(command, args);
    if (argc <= 0) {
        return 1;
    }

    builtin_result = run_builtin(args, argc);
    if (builtin_result != -1) {
        if (background) {
            fprintf(stderr, "Background mode is only for system commands.\n");
        }
        return builtin_result;
    }

    /* ---------- MULTI COMMAND START ---------- */
    /* Check for -multi-N: remove the token, run N times. */
    int n = 1;
    for (int i = 0; args[i] != NULL; i++) {
        if (strncmp(args[i], "-multi-", 7) == 0) {
            n = atoi(args[i] + 7);
            for (int j = i; args[j] != NULL; j++)
                args[j] = args[j + 1];
            break;
        }
    }
    if (n < 1) {
        fprintf(stderr, "Invalid multi command.\n");
        return 1;
    }
    for (int i = 0; i < n; i++)
        exec_single(args, background);
    /* ---------- MULTI COMMAND END ---------- */
    return 0;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        execvp(args[0], args);
        perror(args[0]);
        exit(1);
    }

    if (background) {
        printf("%d\n", pid);
        return 0;
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return 1;
}

/* 
 * This function handles pipe commands (e.g. ps aux | grep "root").
 * It creates a pipe channel with fd[0] for read and fd[1] for write operations.
 * Then it forks two distinct child processes:
 * 1st child (left_pid): Redirects its stdout to the write end of the pipe using dup2, closing its read end.
 * 2nd child (right_pid): Redirects its stdin to the read end of the pipe using dup2, closing the write end.
 * Parent process closes both ends to allow EOF propagation and waits for both childs.
 */
static int execute_pipe_command(char *line) {
    char *pipe_pos = strchr(line, '|');
    char *left_args[MAX_ARGS];
    char *right_args[MAX_ARGS];
    int left_argc;
    int right_argc;
    int fd[2];
    pid_t left_pid;
    pid_t right_pid;
    int status_left;
    int status_right;

    if (pipe_pos == NULL) {
        return 1;
    }

    *pipe_pos = '\0';
    pipe_pos++;
    trim_spaces(line);
    trim_spaces(pipe_pos);

    left_argc = parse_args(line, left_args);
    right_argc = parse_args(pipe_pos, right_args);

    if (left_argc <= 0 || right_argc <= 0) {
        fprintf(stderr, "Invalid pipe command.\n");
        return 1;
    }

    if (is_builtin_command(left_args[0]) || is_builtin_command(right_args[0])) {
        fprintf(stderr, "Built-in commands are not supported in pipe.\n");
        return 1;
    }

    if (pipe(fd) < 0) {
        perror("pipe");
        return 1;
    }

    left_pid = fork();
    if (left_pid < 0) {
        perror("fork");
        close(fd[0]);
        close(fd[1]);
        return 1;
    }

    if (left_pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(left_args[0], left_args);
        perror(left_args[0]);
        exit(1);
    }

    right_pid = fork();
    if (right_pid < 0) {
        perror("fork");
        close(fd[0]);
        close(fd[1]);
        waitpid(left_pid, NULL, 0);
        return 1;
    }

    if (right_pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(right_args[0], right_args);
        perror(right_args[0]);
        exit(1);
    }

    close(fd[0]);
    close(fd[1]);

    waitpid(left_pid, &status_left, 0);
    waitpid(right_pid, &status_right, 0);

    if (WIFEXITED(status_right)) {
        return WEXITSTATUS(status_right);
    }

    return 1;
}

/* 
 * This function handles logical AND (&&) operations (e.g. gcc main.c && ./a.out).
 * It acts identically to shell behavior. If the left side (first command) returns 
 * an exit status of exactly 0 (WEXITSTATUS == 0 = success), it proceeds
 * to evaluate and execute the right side.
 */
static int execute_and_command(char *line) {
    char *and_pos = strstr(line, "&&");
    char left[MAX_LINE];
    char right[MAX_LINE];
    int left_status;

    if (and_pos == NULL) {
        return 1;
    }

    *and_pos = '\0';
    and_pos += 2;

    strncpy(left, line, sizeof(left) - 1);
    left[sizeof(left) - 1] = '\0';
    strncpy(right, and_pos, sizeof(right) - 1);
    right[sizeof(right) - 1] = '\0';

    trim_spaces(left);
    trim_spaces(right);

    if (strlen(left) == 0 || strlen(right) == 0) {
        fprintf(stderr, "Invalid && command.\n");
        return 1;
    }

    left_status = execute_simple_command(left, 0);
    if (left_status == 0) {
        return execute_simple_command(right, 0);
    }

    return left_status;
}

int main(void) {
    char line[MAX_LINE];

    while (1) {
        /* 
         * WNOHANG bayrağı ile çalışan background (zombie) process'lerin kaynaklarını 
         * parent process'i bloklamadan sisteme iade ediyoruz.
         * (Checklist: 17. Process ve Zombie Kontrolü için mükemmel pratik)
         */
        while (waitpid(-1, NULL, WNOHANG) > 0);

        /* Empty line before prompt is in homework. */
        printf("\nshell322> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        trim_newline(line);

        if (is_empty_line(line)) {
            continue;
        }

        add_history(line);

        if (strstr(line, "&&") != NULL) {
            execute_and_command(line);
            continue;
        }

        if (strchr(line, '|') != NULL) {
            execute_pipe_command(line);
            continue;
        }

        trim_spaces(line);
        if (strlen(line) > 0 && line[strlen(line) - 1] == '&') {
            line[strlen(line) - 1] = '\0';
            trim_spaces(line);
            if (strlen(line) == 0) {
                fprintf(stderr, "Invalid background command.\n");
                continue;
            }
            execute_simple_command(line, 1);
            continue;
        }

        execute_simple_command(line, 0);
    }

    return 0;
}

static int exec_single(char *args[], int background) {
    int status;
    pid_t pid = fork();

    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) { execvp(args[0], args); perror(args[0]); exit(1); }
    if (background) { printf("%d\n", pid); return 0; }
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/*
 * Compile:
 *   gcc -Wall -Wextra -std=c99 main.c -o shell322
 *
 * Run:
 *   ./shell322
 *   shell322> echo hello -multi-3
 */
