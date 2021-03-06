#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <elf.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <errno.h>
#include <link.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <sys/user.h>
#include <sys/procfs.h> /* struct elf_prstatus */
#include "dwarf.h"
#include "libdwarf.h"

#define MAX_TID 256
#define PT_ATTACHED 1
#define PT_DETACHED 2

#define PARTIAL_SNAPSHOT 1
#define COMPLETE_SNAPSHOT 0

#define MAXFD 255

#define PS_TRACED 1
#define PS_STOPPED 2
#define PS_SLEEP_UNINTER 4
#define PS_SLEEP_INTER 8
#define PS_DEFUNCT 16
#define PS_RUNNING 32
#define PS_UNKNOWN 64

#define MAX_THREADS 256 // for each threads prstatus

#define ELFNOTE_NAME(_n_) ((char*)(_n_) + sizeof(*(_n_)))
#define ELFNOTE_ALIGN(_n_) (((_n_)+3)&~3)
#define ELFNOTE_NAME(_n_) ((char*)(_n_) + sizeof(*(_n_)))
#define ELFNOTE_DESC(_n_) (ELFNOTE_NAME(_n_) + ELFNOTE_ALIGN((_n_)->namesz))
#define ELFNOTE_NEXT(_n_) ((ElfW(Note) *)(ELFNOTE_DESC(_n_) + ELFNOTE_ALIGN((_n_)->descsz)))

struct opts {
        int coretype;
        int all;
        int pid;
        char *snapdir;
};

typedef struct {
        Elf64_Word namesz;
        Elf64_Word descsz;
        Elf64_Word type;
} ElfW(Note);

struct fde_func_data { /* For eh_frame.c */ 
        uint64_t addr;
        size_t size;
};


struct memelfnote
{
        const char *name;
        int type;
        unsigned int datasz;
        void *data;
};

struct elf_thread_core_info {
        struct elf_thread_core_info *next;
        struct elf_prstatus prstatus;
        struct memelfnote notes[0];
};

struct elf_note_info {
        struct memelfnote *notes;
        struct elf_prstatus *prstatus;  /* NT_PRSTATUS */
        struct elf_prpsinfo *psinfo;    /* NT_PRPSINFO */
        elf_fpregset_t *fpu;
        int thread_status_size;
        int numnote;
};

struct coredump_params {
        siginfo_t *siginfo;
	struct pt_regs *regs;
        unsigned long limit;
        unsigned long mm_flags;
};

typedef struct elfdesc {
	uint8_t *mem;
	ElfW(Ehdr) *ehdr;
	ElfW(Phdr) *phdr;
	ElfW(Shdr) *shdr;
	ElfW(Addr) textVaddr;
	ElfW(Addr) dataVaddr;
	ElfW(Off) textOffset;
	ElfW(Off) dataOffset;
	ElfW(Off) dynamicOffset;
	char *StringTable;
} elfdesc_t;

typedef struct mappings {
	uint8_t *mem;
	char *filename;
	unsigned long base;
	size_t size;
	int elfmap;
	int stack;
	int thread_stack;
	int heap;
	int shlib;
	int padding;
	int special;
	int anonmap_exe;
	int filemap;
	int filemap_exe;
	int vdso;
	int vsyscall;
	int stack_tid;
	size_t sh_offset;
	uint32_t p_flags;
} mappings_t;

typedef struct memdesc {
	pid_t pid;	
	uint8_t *exe; /* Points to /proc/<pid>/exe */
	char *path;   // path to executable
	char *comm; //name of executable
	int mapcount; // overall # of memory maps
	int type; // ET_EXEC or ET_DYN
	ElfW(Addr) base, data_base;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} stack;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} vdso;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} vsyscall;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} heap;
	struct { 
		int fds[MAXFD];
		int pid;
		int uid, gid; 
		int tidcount;
		pid_t tid[MAX_TID];
		pid_t leader;
		pid_t tracer; // the pid of the tracer
		unsigned int state;
	} task;
	mappings_t *maps;
	struct user_regs_struct pt_regs;
	char *stack_args;
	size_t stack_args_len;
	uint8_t *saved_auxv;
} memdesc_t;
	
		
	
typedef struct descriptor {
	elfdesc_t binary;
	memdesc_t memory;
	struct elf_note_info info[MAX_THREADS];
	int exe_type;
	int dynlinking;
	char *snapdir;
} desc_t;


typedef struct node {
        struct node *next;
        struct node *prev;
        desc_t *desc;
} node_t;

typedef struct list {
        node_t *head;
        node_t *tail;
} list_t;



memdesc_t * take_process_snapshot(pid_t);
void * heapAlloc(size_t);
char * xstrdup(const char *);
char * xfmtstrdup(char *fmt, ...);
int get_all_functions(const char *filepath, struct fde_func_data **funcs);

