#define CUM_PATH "/cpu_util_monitor"

typedef struct {
    float curr;     // total usage
    float idle;     // idle
} cpumond_t;

typedef struct {
    int    fd;
    char  *path;
    cpumond_t *mm;
} cpumond_entry_t;
