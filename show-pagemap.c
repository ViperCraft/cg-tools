#define _GNU_SOURCE
#define _XOPEN_SOURCE 600

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <grp.h>
#include <sysexits.h>

static uint64_t     const PAGE_SIZE = 4096;

char const DEFAULT_CGROUP_MNT[] = { "/sys/fs/cgroup/" };


static char const *cgroup_mount = DEFAULT_CGROUP_MNT;

static uint64_t read_pagecount( uint64_t pfn );
static uint64_t read_pagecgroup( uint64_t pfn );

static int o_show_details = 0, o_show_cgroup = 0, o_print_refs = 0, 
    o_print_map_name = 0, o_dir_mode = 0;

struct summary_t {
    uint64_t total_pages;
    uint64_t total_active_pages;
    uint64_t total_shared_pages;

} g_summary;


/*  since we cannot use here C++ std::unordered_map 
    we shall use just huge arrays for calculation
*/


typedef struct var_array_t {
    uint64_t *ptr;
    uint64_t size;
} var_array_t;

static var_array_t a_per_cgroup_stats;

void init_array( var_array_t *a )
{
    a->ptr = NULL;
    a->size = 0;
}

void free_array( var_array_t *a )
{
    if( a->ptr )
        free(a->ptr);

    a->size = 0;
}

static size_t get_npages( size_t fsize )
{
    size_t npages = (fsize + PAGE_SIZE - 1) / PAGE_SIZE;
    return npages;
}

static double percent( size_t share, size_t total )
{
    return total ? (double)share / total * 100. : 0.;
}

static char const* human_bytes( size_t bytes )
{
    static size_t const KB = 1024UL, MB = KB * KB, GB = MB * KB, TB = GB * KB;
    size_t kbytes = bytes / KB;
    size_t mbytes = bytes / MB;
    size_t gbytes = bytes / GB;
    size_t tbytes = bytes / TB;
    static char buf[64];
    if( tbytes ) {
        snprintf(buf, sizeof(buf), "%.4f TB", (double)(bytes) / TB);
    } else if( gbytes ) {
        snprintf(buf, sizeof(buf), "%.3f GB", (double)(bytes) / GB);
    } else if( mbytes ) {
        snprintf(buf, sizeof(buf), "%.2f MB", (double)(bytes) / MB);
    } else if( kbytes ) {
        snprintf(buf, sizeof(buf), "%.2f KB", (double)(bytes) / KB);
    } else {
        snprintf(buf, sizeof(buf), "%lu B", bytes);
    }

    return buf;
}

void put_or_append( var_array_t *a, uint64_t idx, uint64_t value )
{
    if( a->size < idx )
    {
        // realloc
        uint64_t new_size = idx * 1.5;
        a->ptr = realloc(a->ptr, new_size * sizeof(uint64_t));
        if( NULL == a->ptr )
            perror("failed to allocate buffer");
        // fill tail with zeroes
        memset(a->ptr + a->size, 0, (new_size - a->size) * sizeof(uint64_t));
        a->size = new_size;
    }
    a->ptr[idx] += value;
}

static int parse_cgroup_mnt( char const *dirname, uint64_t target_inode, char **fname_out )
{
    DIR *pDir = opendir(dirname);
    if (NULL == pDir)
    {
        perror("[parse_cgroup_mnt] opendir() failed");
        exit(1);
    }

    int exit_flag = 1;
    struct dirent *dp;
    struct stat st;
    int err;
    while ((dp = readdir(pDir)) != NULL)
    {
        if (dp->d_name[0] == '.')
            continue;

        int dir_slen = strlen(dirname), total_sz = 
            strlen(dp->d_name) + dir_slen + 2;
        char *fname = malloc(total_sz);

        if( dirname[dir_slen-1] != '/' )
            snprintf(fname, total_sz, "%s/%s", dirname, dp->d_name);
        else
            snprintf(fname, total_sz, "%s%s", dirname, dp->d_name);


        int isDir = 0;
        err = -2;
        if (dp->d_type == DT_UNKNOWN)
        {
            err = stat(fname, &st);
            if (err != -1) {
                isDir = S_ISDIR(st.st_mode);
            }
        }
        else
        {
            isDir = (dp->d_type == DT_DIR);
        }

        if (isDir)
        {
            if( err != 0 )
                err = stat(fname, &st);
            
            if( err == 0 )
            {
                if( st.st_ino == target_inode )
                {
                    // found
                    *fname_out = fname;
                    exit_flag = 1;
                    break;
                }
            }
            if( !(exit_flag = parse_cgroup_mnt(fname, target_inode, fname_out)) )
                break;
        }

        free(fname);
    }

    closedir(pDir);

    return exit_flag;
}


static char const* get_groupid_name( uint64_t gid )
{
    // WARN: scan fs each time!
    char *cg_name = NULL;
    parse_cgroup_mnt(cgroup_mount, gid, &cg_name);

    if( cg_name )
    {
        static char buf[1024];
        memcpy(buf, cg_name, strlen(cg_name) + 1);
        free(cg_name);
        return buf;
    }

    return "[ERROR]";
}


static void usage()
{
    fprintf( stderr, "Usage: show-pagemap [options] <PID/DIR>\n" );
    fprintf( stderr, "options are following:\n" );
    fprintf( stderr, "\t-D|--dir:                      treat PID as DIR name and traverse files as page-cache.\n" );
    fprintf( stderr, "\t-d|--details:                  show per page details.\n" );
    fprintf( stderr, "\t-g|--cgroup:                   show cgroup refs from /proc/kpagecgroup.\n" );
    fprintf( stderr, "\t-r|--refs:                     show sharing refs from /proc/kpagecount.\n" );
    fprintf( stderr, "\t-n|--names:                    show map name if found.\n" );
    fprintf( stderr, "\t-m|--mount:                    override cgroup mount, default is /sys/fs/cgroup/.\n" );
}

static void dump_page(uint64_t address, uint64_t data, const char *map_name) 
{
    uint64_t pfn = data & 0x7fffffffffffff;
    uint64_t cnt = o_print_refs ? (pfn ? read_pagecount(pfn) : 0) : 0;

    uint64_t page_is_present = (data >> 63) & 1;

    int64_t cgroup_id = o_show_cgroup ? read_pagecgroup(pfn) : -1;

    if( o_show_details )
    {
        printf("0x%-16lx : PFN %-16lx refs: %ld soft-dirty %ld ex-map: %ld shared %ld "
            "swapped %ld present %ld", address, pfn, cnt,
            (data >> 55) & 1,
            (data >> 56) & 1,
            (data >> 61) & 1,
            (data >> 62) & 1,
            page_is_present);

        if( cgroup_id != -1 )
            printf(" cgroup: %ld", cgroup_id);

        if( map_name )
            printf(" name: %s", map_name);
        
        printf("\n");
    }

    g_summary.total_pages += 1;
    g_summary.total_active_pages += page_is_present ? 1 : 0;
    g_summary.total_shared_pages += cnt > 1 ? 1 : 0;

    if( o_show_cgroup && cgroup_id > 0 && page_is_present )
    {
        put_or_append(&a_per_cgroup_stats, cgroup_id, 1);
    }
}

void print_summary()
{
    printf("Summary:\n");
    printf("total pages:       %16ld = %s\n", g_summary.total_pages, human_bytes(g_summary.total_pages * PAGE_SIZE));
    printf("total active(RSS): %16ld = %s\n", g_summary.total_active_pages, human_bytes(g_summary.total_active_pages * PAGE_SIZE));
    printf("total shared:      %16ld = %s\n", g_summary.total_shared_pages, human_bytes(g_summary.total_shared_pages * PAGE_SIZE));

    if( o_show_cgroup && a_per_cgroup_stats.size )
    {
        printf("cgroup(s) active pages:\n");
        for( uint64_t i = 0; i < a_per_cgroup_stats.size; ++i )
        {
            if( a_per_cgroup_stats.ptr[i] )
            {
                printf("{%ld}%-128s %-8ld = %s\n", i, get_groupid_name(i), a_per_cgroup_stats.ptr[i], human_bytes(a_per_cgroup_stats.ptr[i] * PAGE_SIZE));
            }
        }
    }
}

void read_vma(int fd, uint64_t start, uint64_t end, const char *map_name) 
{
    for(uint64_t i, val; start < end; start += PAGE_SIZE) 
    {
        i = (start / PAGE_SIZE) * sizeof(uint64_t);
        if(pread(fd, &val, sizeof(uint64_t), i) != sizeof(uint64_t)) 
        {
            if(errno) perror("vma pread");
            break;
        }
        dump_page(i, val, map_name);
    }
}

void parse_maps( const char *maps_file, const char *pagemap_file ) 
{
    int maps = open(maps_file, O_RDONLY);
    if(maps < 0)
        perror("open /proc/maps failed");

    int pagemap = open(pagemap_file, O_RDONLY);
    if(pagemap < 0) {
        close(maps);
        perror("open /proc/pagemap failed");
        return;
    }

    char buffer[BUFSIZ];
    int offset = 0;
    size_t y;

    for(;;) {
        ssize_t length = read(maps, buffer + offset, sizeof buffer - offset);
        if(length <= 0) break;

        length += offset;

        for(size_t i = offset; i < (size_t)length; i ++) 
        {
            uint64_t low = 0, high = 0;
            if(buffer[i] == '\n' && i) 
            {
                size_t x = i - 1;
                while(x && buffer[x] != '\n') x --;
                if(buffer[x] == '\n') x ++;
                size_t beginning = x;

                while(buffer[x] != '-' && x+1 < sizeof buffer) {
                    char c = buffer[x ++];
                    low *= 16;
                    if(c >= '0' && c <= '9') {
                        low += c - '0';
                    }
                    else if(c >= 'a' && c <= 'f') {
                        low += c - 'a' + 10;
                    }
                    else break;
                }

                while(buffer[x] != '-' && x+1 < sizeof buffer) x ++;
                if(buffer[x] == '-') x ++;

                while(buffer[x] != ' ' && x+1 < sizeof buffer) 
                {
                    char c = buffer[x ++];
                    high *= 16;
                    if(c >= '0' && c <= '9') {
                        high += c - '0';
                    }
                    else if(c >= 'a' && c <= 'f') {
                        high += c - 'a' + 10;
                    }
                    else break;
                }

                const char *lib_name = 0;
                if( o_print_map_name )
                {
                    for(int field = 0; field < 4; field ++) {
                        x ++;  // skip space
                        while(buffer[x] != ' ' && x+1 < sizeof buffer) x ++;
                    }
                    while(buffer[x] == ' ' && x+1 < sizeof buffer) x ++;

                    y = x;
                    while(buffer[y] != '\n' && y+1 < sizeof buffer) y ++;
                    buffer[y] = 0;

                    lib_name = buffer + x;
                }

                read_vma(pagemap, low, high, lib_name);

                if( o_print_map_name )
                    buffer[y] = '\n';
            }
        }
    }

    close(maps);
    close(pagemap);
}

static void process_file( char const *fname, int pagemap )
{

    int f = open(fname, O_RDONLY | O_NOATIME);
    if( -1 == f )
    {
        fprintf( stderr, "failed to open file: %s, errno: %d\n", fname, errno );
        exit( EXIT_FAILURE );
    }

    struct stat st;

    if( fstat(f, &st) )
    {
        fprintf( stderr, "failed to stat file: %s, errno: %d\n", fname, errno );
        exit( EXIT_FAILURE );
    }

    size_t npages = get_npages(st.st_size);

    void *mm = mmap(0, st.st_size, PROT_READ, MAP_SHARED, f, 0);

    if( MAP_FAILED == mm )
    {
        fprintf( stderr, "failed to mmap file: %s, errno: %d\n", fname, errno );
        close(f);
        exit( EXIT_FAILURE );
    }

    close(f);

    uint8_t *mcore_map = malloc(npages), *glue_map = NULL;

    if( mincore(mm, st.st_size, mcore_map) )
    {
        fprintf( stderr, "failed to mincore-mapping file: %s, errno: %d\n", fname, errno );
        munmap(mm, st.st_size);
        exit( EXIT_FAILURE );
    }

    size_t marked = 0;
    uint64_t volatile sum = 0;

    for( size_t i = 0; i < npages; ++i )
    {
        if( mcore_map[i] & 0x1 )
        {
            ++marked;
            // HACK: need to read this page to get hit into local VMA
            uint64_t *mem = (uint64_t *)((char*)mm + PAGE_SIZE * i);
            sum += *mem;
        }
    }

    // now we ready read vma
    size_t addr = (size_t)mm;
    read_vma(pagemap, addr, addr + st.st_size, fname);

    munmap(mm, st.st_size);
    free(mcore_map);

    printf("%s: Pages %lu/%lu %.2f%%\n", fname, npages, marked, percent(marked, npages));
}

static int process_dir( char const *dirname, int pagemap )
{
    DIR *pDir = opendir(dirname);
    if (NULL == pDir)
    {
        perror("opendir() failed");
        exit(1);
    }

    int exit_flag = 1;
    struct dirent *dp;
    while ((dp = readdir(pDir)) != NULL)
    {
        if (dp->d_name[0] == '.')
            continue;

        int dir_slen = strlen(dirname), total_sz = 
            strlen(dp->d_name) + dir_slen + 2;
        char *fname = malloc(total_sz);

        if( dirname[dir_slen-1] != '/' )
            snprintf(fname, total_sz, "%s/%s", dirname, dp->d_name);
        else
            snprintf(fname, total_sz, "%s%s", dirname, dp->d_name);


        int isDir = 0, isReg = 0;
        if (dp->d_type == DT_UNKNOWN)
        {
            struct stat st;
            int err = stat(fname, &st);
            if (err != -1) {
                isDir = S_ISDIR(st.st_mode);
                isReg = S_ISREG(st.st_mode);
            }
        }
        else
        {
            isDir = (dp->d_type == DT_DIR);
            isReg = (dp->d_type == DT_REG);
        }

        if (isDir)
        {
            if( !(exit_flag = process_dir(fname, pagemap)) )
                break;
        } 
        else if (isReg) process_file(fname, pagemap);

        free(fname);
    }

    closedir(pDir);

    return exit_flag;
}

int main( int argc, char *argv[] ) 
{
    while( 1 )
    {

        static struct option long_options[] = {
            { "dir",                  no_argument,       NULL, 'D' },
            { "details",              no_argument,       NULL, 'd' },
            { "cgroup",               no_argument,       NULL, 'g' },
            { "refs",                 no_argument,       NULL, 'r' },
            { "names",                no_argument,       NULL, 'n' },
            { "mount",                required_argument, NULL, 'm' },
            { NULL,                             0,       NULL, 0   }
        };

        int op_c = getopt_long( argc, argv, "dgrnDm:", long_options, NULL );
        if( op_c == -1 )
            break;

        switch( op_c )
        {
            case 'D':
                o_dir_mode = 1;
                break;
            case 'd':
                o_show_details = 1;
                break;
            case 'g':
                o_show_cgroup = 1;
                break;
            case 'r':
                o_print_refs = 1;
                break;
            case 'n':
                o_print_map_name = 1;
                break;
            case 'm':
                cgroup_mount = optarg;
                break;
            default:
                fprintf( stderr, "invalid argument\n");
                exit( EXIT_FAILURE );
        }
    }

    argc -= optind;
    argv += optind;

    if( argc < 1 )
    {
        usage();
        exit( EX_USAGE );
    }

    if( o_dir_mode && argc > 1 )
    {
        usage();
        exit( EX_USAGE );
    }

    char pagemap_file[BUFSIZ];

    init_array(&a_per_cgroup_stats);

    if( o_dir_mode )
    {
        snprintf(pagemap_file, sizeof(pagemap_file), "/proc/self/pagemap");
        int pagemap = open(pagemap_file, O_RDONLY);
        if(pagemap < 0) 
        {
            perror("open /proc/pagemap failed");
            return 1;
        }
        process_dir(*argv, pagemap);
    }
    else
    {        
        errno = 0;
        char maps_file[BUFSIZ];
        for( int p = 0; p < argc; ++p )
        {
            int pid = (int)strtol(argv[p], NULL, 0);
            if( errno ) 
            {
                perror("failed to parse PID");
                return 1;
            }

            snprintf(maps_file, sizeof(maps_file), "/proc/%lu/maps", (uint64_t)pid);
            snprintf(pagemap_file, sizeof(pagemap_file), "/proc/%lu/pagemap", (uint64_t)pid);

            parse_maps(maps_file, pagemap_file);
        }
    }

    print_summary();

    free_array(&a_per_cgroup_stats);

    return 0;
}

static int fd_pagecount = -1;

uint64_t read_pagecount( uint64_t pfn ) 
{
   /* This file contains a 64-bit count of the number of
   times each page is mapped, indexed by PFN.
   */
   if( -1 == fd_pagecount )
   {
      fd_pagecount = open("/proc/kpagecount", O_RDONLY);
      if( fd_pagecount < 0 ) 
      {
         perror("open kpagecount");
         return 0;
      }
   }

   uint64_t data, index = pfn * sizeof(uint64_t);

   if(pread(fd_pagecount, &data, sizeof(data), index) != sizeof(data)) {
      perror("pread kpagecount");
      return 0;
   }

   return data;
}

static int fd_pagecgroup = -1;

uint64_t read_pagecgroup( uint64_t pfn )
{
    /* This file contains a 64-bit inode number of the memory
              cgroup each page is charged to, indexed by page frame
              number (see the discussion of /proc/pid/pagemap).
   */

   if( -1 == fd_pagecgroup )
   {
      fd_pagecgroup = open("/proc/kpagecgroup", O_RDONLY);
      if( fd_pagecgroup < 0 ) 
      {
         perror("open kpagecgroup");
         exit(1);
      }
   }

   uint64_t data, index = pfn * sizeof(uint64_t);

   if(pread(fd_pagecgroup, &data, sizeof(data), index) != sizeof(data)) {
      perror("pread kpagecgroup");
      return 0;
   }

   return data;
}

