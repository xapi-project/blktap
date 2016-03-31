#define CUM_PATH "/cpu_util_monitor"

typedef struct {
    float curr;     // total usage
    float idle;     // idle
} cum_t;

typedef struct {
    int    fd;
    char  *path;
    cum_t *mm;
} cum_entry_t;
