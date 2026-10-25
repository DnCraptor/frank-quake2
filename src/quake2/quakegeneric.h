#ifndef QUAKEGENERIC_H
#define QUAKEGENERIC_H


int QG_Milliseconds(void);
void QG_GetMouseDiff(int* dx, int* dy);
void QG_CaptureMouse(void);
void QG_ReleaseMouse(void);

void QG_Mkdir(const char* path);

// ESP32: data stored in its in-memory format in a mapped flash partition
// (models and decoded skins, see tools/mkassets.py), NULL if not there
const void *QG_FindAsset(const char *name, int *size);

// ESP32: runs fn(arg, 0) on the calling core and fn(arg, 1) on the other one,
// returns when both are done (one after the other where there is no second
// core)
void QG_Parallel2(void (*fn)(void *arg, int part), void *arg);

// RP2350: runs fn(arg, 1) on the other core while this one goes on;
// QG_WaitAsync returns once it is done (QG_Parallel2 waits for it first)
void QG_RunAsync(void (*fn)(void *arg, int part), void *arg);
void QG_WaitAsync(void);
int QG_SecondCore(void);			// true when the other core takes jobs
// RP2350: fn runs on the other core after the next QG_RunAsync job, or in
// QG_WaitAsync if none came; QG_WaitAsync returns once it is done too
void QG_DeferAsync(void (*fn)(void *arg, int part), void *arg);

// RP2350: which items (rows, triangles, columns) of QG_Parallel2 work go to
// the second core: item i is part QG_PART_OF(i); qg_split_mask has one bit
// per i % 16, re-balanced by the platform so that both cores finish together
extern volatile unsigned qg_split_mask;
extern volatile int qg_split_k;			// bits set in qg_split_mask (of 16)
#define QG_PART_OF(i)	((qg_split_mask >> ((i) & 15)) & 1)

// RP2350: the address to write [p, p + size) through without the cache
// (PSRAM: a cache line fill per written line saved); the range is first
// written back and dropped from the cache. p itself elsewhere.
void *QG_WriteThrough(void *p, int size);
// RP2350: drops [p, p + size) from the cache (written through, read next)
void QG_Uncache(void *p, int size);

// RP2350: after a page flip the buffer to draw into is still on screen until
// the next vsync; the renderer calls this before its first pixel write, so
// that the frame's other work (game, BSP, edges) overlaps the wait
extern volatile int qg_back_busy;
void QG_WaitBack(void);
#define QG_NEED_BACK()	do { if (qg_back_busy) QG_WaitBack (); } while (0)

#endif