#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct fileinfo {
    /* The file name.  */
    char *name;
    /* For symbolic link, name of the file linked to, otherwise zero.  */
    char *linkname;
    struct stat stat;
};

static uintmax_t gobble_file(char const *name, char const *dirname);

/* Enter and remove entries in the table `cwd_file'.  */
/* Empty the table of files.  */
/* The table of files in the current directory:
   `cwd_file' points to a vector of `struct fileinfo', one per file.
   `cwd_n_alloc' is the number of elements space has been allocated for.
   `cwd_n_used' is the number actually in use.  */
/* Address of block containing the files that are described.  */
static struct fileinfo *cwd_file;
/* Length of block that `cwd_file' points to, measured in files.  */
static size_t cwd_n_alloc;
/* Index of first unused slot in `cwd_file'.  */
static size_t cwd_n_used;

#define LINK_BUF_LEN 500
static char *resolve_link(char *filename) {
    char buf[LINK_BUF_LEN];
    memset(buf, 0, LINK_BUF_LEN);
    readlink(filename, buf, LINK_BUF_LEN);
    return strdup(buf);
}

static void file_failure(char const *message, char const *file) {
    fprintf(stderr, "file %s error:%s\n", file,message);
    exit(1);
}

static void format_time(char *buf, int buf_len, struct timespec ts) {
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    strftime(buf, buf_len, "%b %d %H:%M", &tm);
}

static void clear_files(void) {
    size_t i;
    for (i = 0; i < cwd_n_used; i++) {
        struct fileinfo *f = &(cwd_file[i]);
        free(f->name);
        free(f->linkname);
    }
    cwd_n_used = 0;
}

static char ftypelet(mode_t bits) {
    /* These are the most common, so test for them first.  */
    if (S_ISREG(bits)) return '-';
    if (S_ISDIR(bits)) return 'd';

    /* Other letters standardized by POSIX 1003.1-2004.  */
    if (S_ISBLK(bits)) return 'b';
    if (S_ISCHR(bits)) return 'c';
    if (S_ISLNK(bits)) return 'l';
    if (S_ISFIFO(bits)) return 'p';

    /* Other file types (though not letters) standardized by POSIX.  */
    if (S_ISSOCK(bits)) return 's';

    return '?';
}

void strmode(mode_t mode, char *str) {
    str[0] = ftypelet(mode);
    str[1] = mode & S_IRUSR ? 'r' : '-';
    str[2] = mode & S_IWUSR ? 'w' : '-';
    str[3] = (mode & S_ISUID ? (mode & S_IXUSR ? 's' : 'S')
                             : (mode & S_IXUSR ? 'x' : '-'));
    str[4] = mode & S_IRGRP ? 'r' : '-';
    str[5] = mode & S_IWGRP ? 'w' : '-';
    str[6] = (mode & S_ISGID ? (mode & S_IXGRP ? 's' : 'S')
                             : (mode & S_IXGRP ? 'x' : '-'));
    str[7] = mode & S_IROTH ? 'r' : '-';
    str[8] = mode & S_IWOTH ? 'w' : '-';
    str[9] = (mode & S_ISVTX ? (mode & S_IXOTH ? 't' : 'T')
                             : (mode & S_IXOTH ? 'x' : '-'));
    str[10] = ' ';
    str[11] = '\0';
}

#define TIME_STR_BUF_LEN 20
static void print_file(const struct fileinfo *f) {
    char modebuf[12];
    strmode(f->stat.st_mode, modebuf);

    char m_time[TIME_STR_BUF_LEN];
    format_time(m_time, TIME_STR_BUF_LEN, f->stat.st_mtim);
    printf("%s\t%lu\t%s\t%s\t%s\t%s\n", modebuf, f->stat.st_nlink,
           getpwuid(f->stat.st_uid)->pw_name, getgrgid(f->stat.st_gid)->gr_name,
           m_time, f->name);
}

/* Return true if FILE should be ignored.  */
static bool file_ignored(char const *name) { return name[0] == '.'; }

/* Read directory NAME, and list the files in it.
   If REALNAME is nonzero, print its name instead of NAME;
   this is used for symbolic links to directories.
   COMMAND_LINE_ARG means this directory was mentioned on the command line.  */
static void print_dir(char const *name) {
    DIR *dirp;
    struct dirent *next;
    uintmax_t total_blocks = 0;
    static bool first = true;

    dirp = opendir(name);
    if (!dirp) {
        file_failure("cannot open directory %s", name);
        return;
    }

    /* Read the directory entries, and insert the subfiles into the `cwd_file'
       table.  */
    while (1) {
        /* Set errno to zero so we can distinguish between a readdir failure
           and when readdir simply finds that there are no more entries.  */
        errno = 0;
        next = readdir(dirp);
        if (next) {
            if (!file_ignored(next->d_name)) {
                total_blocks += gobble_file(next->d_name, name);
                // printf("%s %ld\n",next->d_name,next->d_ino);
            }
        } else if (errno != 0) {
            file_failure("reading directory %s", name);
            if (errno != EOVERFLOW) break;
        } else
            break;
    }

    if (closedir(dirp) != 0) {
        file_failure("closing directory %s", name);
        /* Don't return; print whatever we got.  */
    }

    // /* Sort the directory contents.  */
    // sort_files();
    printf("total %lu\n", total_blocks);
    if (cwd_n_used) {
        for (int i = 0; i < cwd_n_used; i++) {
            print_file(&(cwd_file[i]));
        }
    }
}

/* Put DIRNAME/NAME into DEST, handling `.' and `/' properly.  */
/* FIXME: maybe remove this function someday.  See about using a
   non-malloc'ing version of file_name_concat.  */
static void attach(char *dest, const char *dirname, const char *name) {
    const char *dirnamep = dirname;

    /* Copy dirname if it is not ".".  */
    if (dirname[0] != '.' || dirname[1] != 0) {
        while (*dirnamep) *dest++ = *dirnamep++;
        /* Add '/' if `dirname' doesn't already end with it.  */
        if (dirnamep > dirname && dirnamep[-1] != '/') *dest++ = '/';
    }
    while (*name) *dest++ = *name++;
    *dest = 0;
}

/* Add a file to the current table of files.
   Verify that the file exists, and print an error message if it does not.
   Return the number of blocks that the file occupies.  */
static uintmax_t gobble_file(char const *name, char const *dirname) {
    uintmax_t blocks = 0;
    struct fileinfo *f;

    if (cwd_n_used == cwd_n_alloc) {
        cwd_file = realloc(cwd_file, cwd_n_alloc * 2 * sizeof *cwd_file);
        cwd_n_alloc *= 2;
    }

    f = &cwd_file[cwd_n_used];
    memset(f, '\0', sizeof *f);

    /* Absolute name of this file.  */
    char *absolute_name;
    int err;

    if (name[0] == '/' || dirname[0] == 0)
        absolute_name = (char *)name;
    else {
        absolute_name = alloca(strlen(name) + strlen(dirname) + 2);
        attach(absolute_name, dirname, name);
    }

    err = lstat(absolute_name, &f->stat);

    if (err != 0) {
        /* Failure to stat a command line argument leads to
           an exit status of 2.  For other files, stat failure
           provokes an exit status of 1.  */
        file_failure((const char *)("cannot access %s"), absolute_name);
        f->name = strdup(name);
        cwd_n_used++;
        return 0;
    }

    if (S_ISLNK(f->stat.st_mode)) {
        f->linkname = resolve_link(absolute_name);
    }

    blocks = f->stat.st_blocks;

    if (S_ISLNK(f->stat.st_mode)) {
        f->name = malloc(strlen(name) + 4 + strlen(f->linkname));
        sprintf(f->name, "%s%s%s", name, " -> ", f->linkname);
    } else {
        f->name = strdup(name);
    }
    cwd_n_used++;
    return blocks;
}

void main(int argc, void *argv[]) {
    const char *dir = argc >= 2 ? argv[1] : ".";
    cwd_n_alloc = 100;
    cwd_file = malloc(cwd_n_alloc * (sizeof *cwd_file));
    cwd_n_used = 0;
    clear_files();
    print_dir(dir);
}