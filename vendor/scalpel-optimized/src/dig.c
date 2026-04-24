// Scalpel Copyright (C) 2005-13 by Golden G. Richard III and 
// 2007-13 by Vico Marziale.
// Written by Golden G. Richard III and Vico Marziale.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
// 02110-1301, USA.
//
// Thanks to Kris Kendall, Jesse Kornblum, et al for their work 
// on Foremost.  Foremost 0.69 was used as the starting point for 
// Scalpel, in 2005.


#include "scalpel.h"


/////////// GLOBALS ////////////

static char *readbuffer;	// Read buffer--process image files in 
                                // SIZE_OF_BUFFER-size chunks.

// Info needed for each of above SIZE_OF_BUFFER chunks.
typedef struct readbuf_info {
  long long bytesread;		// number of bytes in this buf
  long long beginreadpos;	// position in the image
  char *readbuf;		// pointer SIZE_OF_BUFFER array
} readbuf_info;


// queues to facilitiate async reads, concurrent cpu activities
syncqueue_t *full_readbuf;	// que of full buffers read from image
syncqueue_t *empty_readbuf;	// que of empty buffers 

sig_atomic_t reads_finished;
//pthread_cond_t reader_done = PTHREAD_COND_INITIALIZER;


#ifdef MULTICORE_THREADING
// Multi-core only threading globals

// Parameters to threads under the MULTICORE_THREADING model, to allow
// efficient header/footer searches.
typedef struct ThreadFindAllParams {
  int id;
  char *str;
  size_t length;
  char *startpos;
  long offset;
  char **foundat;
  size_t *foundatlens;
  int strisRE;
  union {
    size_t *table;
    regex_t *regex;
  };
  int casesensitive;
  int nosearchoverlap;
  struct scalpelState *state;
} ThreadFindAllParams;

// TODO:  These structures could be released after the dig phase; they aren't needed in the carving phase since it's not
// threaded.  Look into this in the future.

static pthread_t *searchthreads;	// thread pool for header/footer searches
static ThreadFindAllParams *threadargs;	// argument structures for threads
static char ***foundat;		// locations of headers/footers discovered by threads    
static size_t **foundatlens;	// lengths of discovered headers/footers
//static sem_t *workavailable;	// semaphores that block threads until 
// header/footer searches are required
static pthread_mutex_t *workavailable;
//static sem_t *workcomplete;	// semaphores that allow main thread to wait
// for all search threads to complete current job
static pthread_mutex_t *workcomplete;

#endif

// prototypes for private dig.c functions
static unsigned long long adjustForEmbedding(struct SearchSpecLine
					     *currentneedle,
					     unsigned long long headerindex,
					     unsigned long long *prevstopindex);
static int writeHeaderFooterDatabase(struct scalpelState *state);
static int setupCoverageMaps(struct scalpelState *state,
			     unsigned long long filesize);
static int auditUpdateCoverageBlockmap(struct scalpelState *state,
				       struct CarveInfo *carve);
static int updateCoverageBlockmap(struct scalpelState *state,
				  unsigned long long block);
static void generateFragments(struct scalpelState *state, Queue * fragments,
			      struct CarveInfo *carve);
static unsigned long long positionUseCoverageBlockmap(struct scalpelState
						      *state,
						      unsigned long long
						      position);
static void destroyCoverageMaps(struct scalpelState *state);
static int fseeko_use_coverage_map(struct scalpelState *state, FILE * fp,
				   off64_t offset);
static off64_t ftello_use_coverage_map(struct scalpelState *state, FILE * fp);
static size_t fread_use_coverage_map(struct scalpelState *state, void *ptr,
				     size_t size, size_t nmemb, FILE * stream);
static void printhex(char *s, int len);
static void clean_up(struct scalpelState *state, int signum);
static int displayPosition(int *units,
			   unsigned long long pos,
			   unsigned long long size, char *fn);
static int setupAuditFile(struct scalpelState *state);
static int digBuffer(struct scalpelState *state,
		     unsigned long long lengthofbuf,
		     unsigned long long offset);
static void ensureHeaderStorageCapacity(struct scalpelState *state,
					struct SearchSpecLine *currentneedle);
static void ensureFooterStorageCapacity(struct scalpelState *state,
					struct SearchSpecLine *currentneedle);
static double currentTimeSeconds(void);
static void recordReadTime(double startedAt);
static void recordWriteTime(double startedAt);
static void recordQueueTime(double startedAt);
static void recordSearchTime(double startedAt);
static void printPerformanceMetrics(double pass2TotalSec,
				    double pass2OpenCloseSec,
				    unsigned long long handleOpenCount,
				    unsigned long long handleReopenCount,
				    unsigned long long handleCloseCount,
				    unsigned long long handleEvictionCount,
				    unsigned long long peakOpenHandles,
				    unsigned long long peakActiveCarves,
				    unsigned long long bytesWritten);
#ifdef MULTICORE_THREADING
static void *threadedFindAll(void *args);
#endif

#define CARVE_IO_BUFFER_SIZE (1 * MEGABYTE)

typedef struct BlockCarveEvents {
  struct CarveInfo *start_head;
  struct CarveInfo *stop_head;
  struct CarveInfo *same_head;
  unsigned char has_work;
} BlockCarveEvents;

typedef struct FileHandleCache {
  struct CarveInfo *lru_head;
  struct CarveInfo *lru_tail;
  unsigned long long open_count;
  unsigned long long peak_open_count;
  unsigned long long open_ops;
  unsigned long long reopen_ops;
  unsigned long long close_ops;
  unsigned long long eviction_ops;
  unsigned long long max_open;
  double open_close_sec;
  unsigned long long bytes_written;
} FileHandleCache;

static double
currentTimeSeconds(void) {
#ifdef _WIN32
  LARGE_INTEGER now;
  LARGE_INTEGER freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return ((double) now.QuadPart) / ((double) freq.QuadPart);
#else
  struct timeval now;
  gettimeofday(&now, 0);
  return ((double) now.tv_sec) + (((double) now.tv_usec) / 1000000.0);
#endif
}

static void
recordReadTime(double startedAt) {
  totalreads += currentTimeSeconds() - startedAt;
}

static void
recordWriteTime(double startedAt) {
  totalwrites += currentTimeSeconds() - startedAt;
}

static void
recordQueueTime(double startedAt) {
  totalqueues += currentTimeSeconds() - startedAt;
}

static void
recordSearchTime(double startedAt) {
  totalsearch += currentTimeSeconds() - startedAt;
}

static void
printPerformanceMetrics(double pass2TotalSec,
			 double pass2OpenCloseSec,
			 unsigned long long handleOpenCount,
			 unsigned long long handleReopenCount,
			 unsigned long long handleCloseCount,
			 unsigned long long handleEvictionCount,
			 unsigned long long peakOpenHandles,
			 unsigned long long peakActiveCarves,
			 unsigned long long bytesWritten) {
  fprintf(stdout,
	  "Performance metrics: pass1_scan_sec=%.6f queue_build_sec=%.6f pass2_read_sec=%.6f pass2_write_sec=%.6f pass2_open_close_sec=%.6f pass2_total_sec=%.6f\n",
	  totalsearch, totalqueues, totalreads, totalwrites,
	  pass2OpenCloseSec, pass2TotalSec);
#ifdef _WIN32
  fprintf(stdout,
	  "Performance counters: handle_open_count=%I64u handle_reopen_count=%I64u handle_close_count=%I64u handle_eviction_count=%I64u peak_open_handles=%I64u peak_active_carves=%I64u pass2_bytes_written=%I64u\n",
	  handleOpenCount, handleReopenCount, handleCloseCount,
	  handleEvictionCount, peakOpenHandles, peakActiveCarves, bytesWritten);
#else
  fprintf(stdout,
	  "Performance counters: handle_open_count=%llu handle_reopen_count=%llu handle_close_count=%llu handle_eviction_count=%llu peak_open_handles=%llu peak_active_carves=%llu pass2_bytes_written=%llu\n",
	  handleOpenCount, handleReopenCount, handleCloseCount,
	  handleEvictionCount, peakOpenHandles, peakActiveCarves, bytesWritten);
#endif
}

static void
ensureHeaderStorageCapacity(struct scalpelState *state,
			    struct SearchSpecLine *currentneedle) {

  unsigned long long newcapacity;

  if(currentneedle->offsets.headerstorage > currentneedle->offsets.numheaders) {
    return;
  }

  // 작은 고정 증가보다 doubling이 대량 match에서 더 유리하다.
  newcapacity = currentneedle->offsets.headerstorage ?
    currentneedle->offsets.headerstorage * 2 : 128;
  if(newcapacity <= currentneedle->offsets.numheaders) {
    newcapacity = currentneedle->offsets.numheaders + 1;
  }

  currentneedle->offsets.headers = (unsigned long long *)
    realloc(currentneedle->offsets.headers,
	    sizeof(unsigned long long) * newcapacity);
  checkMemoryAllocation(state, currentneedle->offsets.headers,
			__LINE__, __FILE__, "header array");

  currentneedle->offsets.headerlens =
    (size_t *) realloc(currentneedle->offsets.headerlens,
		       sizeof(size_t) * newcapacity);
  checkMemoryAllocation(state, currentneedle->offsets.headerlens,
			__LINE__, __FILE__, "header array");

  currentneedle->offsets.headerstorage = newcapacity;

  if(state->modeVerbose) {
#ifdef _WIN32
    fprintf(stdout,
	    "Memory reallocation performed, total header storage = %I64u\n",
	    currentneedle->offsets.headerstorage);
#else
    fprintf(stdout,
	    "Memory reallocation performed, total header storage = %llu\n",
	    currentneedle->offsets.headerstorage);
#endif
  }
}


static void
ensureFooterStorageCapacity(struct scalpelState *state,
			    struct SearchSpecLine *currentneedle) {

  unsigned long long newcapacity;

  if(currentneedle->offsets.footerstorage > currentneedle->offsets.numfooters) {
    return;
  }

  // footer도 header와 같은 성장 정책으로 맞춰 메모리 재할당 패턴을 단순화한다.
  newcapacity = currentneedle->offsets.footerstorage ?
    currentneedle->offsets.footerstorage * 2 : 128;
  if(newcapacity <= currentneedle->offsets.numfooters) {
    newcapacity = currentneedle->offsets.numfooters + 1;
  }

  currentneedle->offsets.footers = (unsigned long long *)
    realloc(currentneedle->offsets.footers,
	    sizeof(unsigned long long) * newcapacity);
  checkMemoryAllocation(state, currentneedle->offsets.footers,
			__LINE__, __FILE__, "footer array");

  currentneedle->offsets.footerlens =
    (size_t *) realloc(currentneedle->offsets.footerlens,
		       sizeof(size_t) * newcapacity);
  checkMemoryAllocation(state, currentneedle->offsets.footerlens,
			__LINE__, __FILE__, "footer array");

  currentneedle->offsets.footerstorage = newcapacity;

  if(state->modeVerbose) {
#ifdef _WIN32
    fprintf(stdout,
	    "Memory reallocation performed, total footer storage = %I64u\n",
	    currentneedle->offsets.footerstorage);
#else
    fprintf(stdout,
	    "Memory reallocation performed, total footer storage = %llu\n",
	    currentneedle->offsets.footerstorage);
#endif
  }
}

static void
touchCarveHandle(FileHandleCache *cache, struct CarveInfo *carve) {
  if(carve->lru_prev) {
    carve->lru_prev->lru_next = carve->lru_next;
  }
  else if(cache->lru_head == carve) {
    cache->lru_head = carve->lru_next;
  }

  if(carve->lru_next) {
    carve->lru_next->lru_prev = carve->lru_prev;
  }
  else if(cache->lru_tail == carve) {
    cache->lru_tail = carve->lru_prev;
  }

  carve->lru_prev = 0;
  carve->lru_next = cache->lru_head;
  if(cache->lru_head) {
    cache->lru_head->lru_prev = carve;
  }
  else {
    cache->lru_tail = carve;
  }
  cache->lru_head = carve;
}

static int
closeCarveFile(struct scalpelState *state,
		 FileHandleCache *cache,
		 struct CarveInfo *carve,
		 int finalClose) {
  double closeStartedAt = 0.0;
  int err = 0;

  if(!carve->fp || state->previewMode) {
    if(finalClose) {
      carve->completed = 1;
    }
    return SCALPEL_OK;
  }

  closeStartedAt = currentTimeSeconds();
  err = fclose(carve->fp);
  cache->open_close_sec += currentTimeSeconds() - closeStartedAt;
  if(err) {
    fprintf(stderr, "Error closing file: %s -- %s\n\n",
	    carve->filename, strerror(errno));
    fprintf(state->auditFile,
	    "Error closing file: %s -- %s\n\n",
	    carve->filename, strerror(errno));
    return SCALPEL_ERROR_FILE_WRITE;
  }

  cache->close_ops++;
  if(cache->open_count > 0) {
    cache->open_count--;
  }
  if(carve->lru_prev) {
    carve->lru_prev->lru_next = carve->lru_next;
  }
  else if(cache->lru_head == carve) {
    cache->lru_head = carve->lru_next;
  }
  if(carve->lru_next) {
    carve->lru_next->lru_prev = carve->lru_prev;
  }
  else if(cache->lru_tail == carve) {
    cache->lru_tail = carve->lru_prev;
  }
  carve->lru_prev = 0;
  carve->lru_next = 0;
  carve->fp = 0;
  if(carve->iobuf) {
    free(carve->iobuf);
    carve->iobuf = 0;
  }

  if(finalClose) {
    carve->completed = 1;
  }
  return SCALPEL_OK;
}

static int
ensureCarveFileOpen(struct scalpelState *state,
		    FileHandleCache *cache,
		    struct CarveInfo *carve) {
  double openStartedAt = 0.0;
  int reopening = carve->opened_once;

  if(state->previewMode) {
    return SCALPEL_OK;
  }

  if(carve->fp) {
    touchCarveHandle(cache, carve);
    return SCALPEL_OK;
  }

  while(cache->open_count >= cache->max_open && cache->lru_tail) {
    struct CarveInfo *evicted = cache->lru_tail;
    int err;
    cache->eviction_ops++;
    err = closeCarveFile(state, cache, evicted, FALSE);
    if(err != SCALPEL_OK) {
      return err;
    }
  }

  openStartedAt = currentTimeSeconds();
  carve->fp = fopen(carve->filename, carve->opened_once ? "ab" : "wb");
  cache->open_close_sec += currentTimeSeconds() - openStartedAt;
  if(!carve->fp) {
    fprintf(stderr, "Error opening file: %s -- %s\n",
	    carve->filename, strerror(errno));
    fprintf(state->auditFile, "Error opening file: %s -- %s\n",
	    carve->filename, strerror(errno));
    return SCALPEL_ERROR_FILE_WRITE;
  }

  carve->iobuf = (char *) malloc(CARVE_IO_BUFFER_SIZE);
  if(carve->iobuf) {
    setvbuf(carve->fp, carve->iobuf, _IOFBF, CARVE_IO_BUFFER_SIZE);
  }

  carve->opened_once = 1;
  cache->open_count++;
  if(cache->open_count > cache->peak_open_count) {
    cache->peak_open_count = cache->open_count;
  }
  cache->open_ops++;
  if(reopening) {
    cache->reopen_ops++;
  }
  touchCarveHandle(cache, carve);
  return SCALPEL_OK;
}

static void
addActiveCarve(struct CarveInfo **activeHead,
		 struct CarveInfo *carve,
		 unsigned long long *activeCount,
		 unsigned long long *peakActiveCount) {
  if(carve->is_active) {
    return;
  }

  carve->active_prev = 0;
  carve->active_next = *activeHead;
  if(*activeHead) {
    (*activeHead)->active_prev = carve;
  }
  *activeHead = carve;
  carve->is_active = 1;
  (*activeCount)++;
  if(*activeCount > *peakActiveCount) {
    *peakActiveCount = *activeCount;
  }
}

static void
removeActiveCarve(struct CarveInfo **activeHead,
		    struct CarveInfo *carve,
		    unsigned long long *activeCount) {
  if(!carve->is_active) {
    return;
  }

  if(carve->active_prev) {
    carve->active_prev->active_next = carve->active_next;
  }
  else {
    *activeHead = carve->active_next;
  }
  if(carve->active_next) {
    carve->active_next->active_prev = carve->active_prev;
  }
  carve->active_prev = 0;
  carve->active_next = 0;
  carve->is_active = 0;
  if(*activeCount > 0) {
    (*activeCount)--;
  }
}

static int
writeCarveBlock(struct scalpelState *state,
		  FileHandleCache *cache,
		  struct CarveInfo *carve,
		  unsigned long long blockStart,
		  unsigned long long bytesread,
		  int sameBlockOnly) {
  unsigned long long carveStart;
  unsigned long long carveStop;
  unsigned long long offset;
  unsigned long long bytestowrite;
  size_t byteswritten;
  int err;
  double writeStartedAt;

  carveStart = carve->start > blockStart ? carve->start : blockStart;
  carveStop = carve->stop < (blockStart + bytesread - 1) ?
    carve->stop : (blockStart + bytesread - 1);

  if(carveStop < carveStart) {
    return SCALPEL_OK;
  }

  offset = carveStart - blockStart;
  bytestowrite = carveStop - carveStart + 1;

  if(!state->previewMode) {
    err = ensureCarveFileOpen(state, cache, carve);
    if(err != SCALPEL_OK) {
      return err;
    }

    writeStartedAt = currentTimeSeconds();
    byteswritten = fwrite(readbuffer + offset, sizeof(char),
			  (size_t) bytestowrite, carve->fp);
    recordWriteTime(writeStartedAt);
    if(byteswritten != (size_t) bytestowrite) {
      fprintf(stderr, "Error writing to file: %s -- %s\n",
	      carve->filename, strerror(errno));
      fprintf(state->auditFile,
	      "Error writing to file: %s -- %s\n",
	      carve->filename, strerror(errno));
      return SCALPEL_ERROR_FILE_WRITE;
    }
    cache->bytes_written += bytestowrite;
  }

  if(sameBlockOnly || carve->stopblock == (blockStart / SIZE_OF_BUFFER)) {
    err = closeCarveFile(state, cache, carve, TRUE);
    if(err != SCALPEL_OK) {
      return err;
    }
    auditUpdateCoverageBlockmap(state, carve);
    free(carve->filename);
    carve->filename = 0;
    return SCALPEL_OK;
  }

  return SCALPEL_OK;
}


// force header/footer matching to deal with embedded headers/footers
static unsigned long long
adjustForEmbedding(struct SearchSpecLine *currentneedle,
		   unsigned long long headerindex,
		   unsigned long long *prevstopindex) {

  unsigned long long h = headerindex + 1;
  unsigned long long f = 0;
  int header = 0;
  int footer = 0;
  long long headerstack = 0;
  unsigned long long start = currentneedle->offsets.headers[headerindex] + 1;
  unsigned long long candidatefooter;
  int morecandidates = 1;
  int moretests = 1;

  // skip footers which precede header
  while (*prevstopindex < currentneedle->offsets.numfooters &&
	 currentneedle->offsets.footers[*prevstopindex] < start) {
    (*prevstopindex)++;
  }

  f = *prevstopindex;
  candidatefooter = *prevstopindex;

  // skip footers until header/footer count balances
  while (candidatefooter < currentneedle->offsets.numfooters && morecandidates) {
    // see if this footer is viable
    morecandidates = 0;		// assumption is yes, viable
    moretests = 1;
    while (moretests && start < currentneedle->offsets.footers[candidatefooter]) {
      header = 0;
      footer = 0;
      if(h < currentneedle->offsets.numheaders
	 && currentneedle->offsets.headers[h] >= start
	 && currentneedle->offsets.headers[h] <
	 currentneedle->offsets.footers[candidatefooter]) {
	header = 1;
      }
      if(f < currentneedle->offsets.numfooters
	 && currentneedle->offsets.footers[f] >= start
	 && currentneedle->offsets.footers[f] <
	 currentneedle->offsets.footers[candidatefooter]) {
	footer = 1;
      }

      if(header && (!footer ||
		    currentneedle->offsets.headers[h] <
		    currentneedle->offsets.footers[f])) {
	h++;
	headerstack++;
	start = (h < currentneedle->offsets.numheaders)
	  ? currentneedle->offsets.headers[h] : start + 1;
      }
      else if(footer) {
	f++;
	headerstack--;
	if(headerstack < 0) {
	  headerstack = 0;
	}
	start = (f < currentneedle->offsets.numfooters)
	  ? currentneedle->offsets.footers[f] : start + 1;
      }
      else {
	moretests = 0;
      }
    }

    if(headerstack > 0) {
      // header/footer imbalance, need to try next footer
      candidatefooter++;
      // this footer counts against footer deficit!
      headerstack--;
      morecandidates = 1;
    }
  }

  return (headerstack > 0) ? 999999999999999999ULL : candidatefooter;
}


// output hex notation for chars in 's'
static void printhex(char *s, int len) {

  int i;
  for(i = 0; i < len; i++) {
    printf("\\x%.2x", (unsigned char)s[i]);
  }
}


static void clean_up(struct scalpelState *state, int signum) {

  scalpelLog(state, "Cleaning up...\n");
  scalpelLog(state,
	     "\nCaught signal: %s. Program is terminating early\n",
	     (char *)strsignal(signum));
  closeAuditFile(state->auditFile);
  exit(1);
}



// display progress bar
static int
displayPosition(int *units,
		unsigned long long pos, unsigned long long size, char *fn) {

  double percentDone = (((double)pos) / (double)(size) * 100);
  double position = (double)pos;
  int count;
  int barlength, i, len;
  double elapsed;
  long remaining;

  char buf[MAX_STRING_LENGTH];
  char line[MAX_STRING_LENGTH];

#ifdef _WIN32
  static LARGE_INTEGER start;
  LARGE_INTEGER now;
  static LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
#else
  static struct timeval start;
  struct timeval now, td;
#endif

  // get current time and remember start time when first chunk of 
  // an image file is read

  if(pos <= SIZE_OF_BUFFER) {
    gettimeofday(&start, (struct timezone *)0);
  }
  gettimeofday(&now, (struct timezone *)0);

  // First, reduce the position to the right units 
  for(count = 0; count < *units; count++) {
    position = position / 1024;
  }

  // Now check if we've hit the next type of units 
  while (position > 1023) {
    position = position / 1024;
    (*units)++;
  }

  switch (*units) {

  case UNITS_BYTES:
    sprintf(buf, "bytes");
    break;
  case UNITS_KILOB:
    sprintf(buf, "KB");
    break;
  case UNITS_MEGAB:
    sprintf(buf, "MB");
    break;
  case UNITS_GIGAB:
    sprintf(buf, "GB");
    break;
  case UNITS_TERAB:
    sprintf(buf, "TB");
    break;
  case UNITS_PETAB:
    sprintf(buf, "PB");
    break;
  case UNITS_EXAB:
    sprintf(buf, "EB");
    break;

  default:
    fprintf(stdout, "Unable to compute progress.\n");
    return SCALPEL_OK;
  }

  len = 0;
  len +=
    snprintf(line + len, sizeof(line) - len, "\r%s: %5.1f%% ", fn, percentDone);
  barlength = ttywidth - strlen(fn) - strlen(buf) - 32;
  if(barlength > 0) {
    i = barlength * (int)percentDone / 100;
    len += snprintf(line + len, sizeof(line) - len,
		    "|%.*s%*s|", i,
		    "****************************************************************************************************************************************************************",
		    barlength - i, "");
  }

  len += snprintf(line + len, sizeof(line) - len, " %6.1f %s", position, buf);

#ifdef _WIN32
  elapsed =
    ((double)now.QuadPart - (double)start.QuadPart) / ((double)freq.QuadPart);
  //printf("elapsed: %f\n",elapsed);
#else
  timersub(&now, &start, &td);
  elapsed = td.tv_sec + (td.tv_usec / 1000000.0);
#endif
  remaining = (long)((100 - percentDone) / percentDone * elapsed);
  //printf("Ratio remaining: %f\n",(100-percentDone)/percentDone);
  //printf("Elapsed time: %f\n",elapsed);
  if(remaining >= 100 * (60 * 60)) {	//60*60 is seconds per hour
    len += snprintf(line + len, sizeof(line) - len, " --:--ETA");
  }
  else {
    i = remaining / (60 * 60);
    if(i)
      len += snprintf(line + len, sizeof(line) - len, " %2d:", i);
    else
      len += snprintf(line + len, sizeof(line) - len, "    ");
    i = remaining % (60 * 60);
    len +=
      snprintf(line + len, sizeof(line) - len, "%02d:%02d ETA", i / 60, i % 60);
  }

  fprintf(stdout, "%s", line);
  fflush(stdout);

  return SCALPEL_OK;
}



// create initial entries in audit for each image file processed
static int setupAuditFile(struct scalpelState *state) {

  char imageFile[MAX_STRING_LENGTH];

  if(realpath(state->imagefile, imageFile)) {
    scalpelLog(state, "\nOpening target \"%s\"\n\n", imageFile);
  }
  else {
    //handleError(state, SCALPEL_ERROR_FILE_OPEN);
    return SCALPEL_ERROR_FILE_OPEN;
  }
  
#ifdef _WIN32
  if(state->skip) {
    fprintf(state->auditFile, "Skipped the first %I64u bytes of %s...\n",
	    state->skip, state->imagefile);
    if(state->modeVerbose) {
      fprintf(stdout, "Skipped the first %I64u bytes of %s...\n",
	      state->skip, state->imagefile);
    }
  }
#else
  if(state->skip) {
    fprintf(state->auditFile, "Skipped the first %llu bytes of %s...\n",
	    state->skip, state->imagefile);
    if(state->modeVerbose) {
      fprintf(stdout, "Skipped the first %llu bytes of %s...\n",
	      state->skip, state->imagefile);
    }
  }
#endif

  fprintf(state->auditFile, "The following files were carved:\n");
  fprintf(state->auditFile,
	  "File\t\t  Start\t\t\tChop\t\tLength\t\tExtracted From\n");
	  
	  return SCALPEL_OK;
}



static int
digBuffer(struct scalpelState *state, unsigned long long lengthofbuf,
	  unsigned long long offset) {

  unsigned long long startLocation = 0;
  int needlenum, i = 0;
  struct SearchSpecLine *currentneedle = 0;
//  gettimeofday_t srchnow, srchthen;

  // for each file type, find all headers and some (or all) footers

  // signal check
  if(signal_caught == SIGTERM || signal_caught == SIGINT) {
    clean_up(state, signal_caught);
  }


  ///////////////////////////////////////////////////
  ///////////////////////////////////////////////////
  ///////////////MULTI-THREADED SEARCH //////////////
  ///////////////////////////////////////////////////
  ///////////////////////////////////////////////////
#ifdef MULTICORE_THREADING

  // as of v1.9, this is now the lowest common denominator mode
  
  // ---------------- threaded header search ------------------ //
  // ---------------- threaded header search ------------------ //
  
  //  gettimeofday(&srchthen, 0);
  if(state->modeVerbose) {
    printf("Waking up threads for header searches.\n");
  }
  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);
    // # of matches in last element of foundat array
    foundat[needlenum][MAX_MATCHES_PER_BUFFER] = 0;
    threadargs[needlenum].id = needlenum;
    threadargs[needlenum].str = currentneedle->begin;
    threadargs[needlenum].length = currentneedle->beginlength;
    threadargs[needlenum].startpos = readbuffer;
    threadargs[needlenum].offset = (long)readbuffer + lengthofbuf;
    threadargs[needlenum].foundat = foundat[needlenum];
    threadargs[needlenum].foundatlens = foundatlens[needlenum];
    threadargs[needlenum].strisRE = currentneedle->beginisRE;
    if(currentneedle->beginisRE) {
      threadargs[needlenum].regex = &(currentneedle->beginstate.re);
    }
    else {
      threadargs[needlenum].table = currentneedle->beginstate.bm_table;
    }
    threadargs[needlenum].casesensitive = currentneedle->casesensitive;
    threadargs[needlenum].nosearchoverlap = state->noSearchOverlap;
    threadargs[needlenum].state = state;

    // unblock thread
    //    sem_post(&workavailable[needlenum]);

    pthread_mutex_unlock(&workavailable[needlenum]);

  }

  // ---------- thread group synchronization point ----------- //
  // ---------- thread group synchronization point ----------- //

  if(state->modeVerbose) {
    printf("Waiting for thread group synchronization.\n");
  }

  // wait for all threads to complete header search before proceeding
  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    //    sem_wait(&workcomplete[needlenum]);

    pthread_mutex_lock(&workcomplete[needlenum]);

  }

  if(state->modeVerbose) {
    printf("Thread group synchronization complete.\n");
  }

  // digest header locations discovered by the thread group

  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);

    // number of matches stored in last element of vector
    for(i = 0; i < (long)foundat[needlenum][MAX_MATCHES_PER_BUFFER]; i++) {
      startLocation = offset + (foundat[needlenum][i] - readbuffer);
      // found a header--record location in header offsets database
      if(state->modeVerbose) {
#ifdef _WIN32
	fprintf(stdout, "A %s header was found at : %I64u\n",
		currentneedle->suffix,
		positionUseCoverageBlockmap(state, startLocation));
#else
	fprintf(stdout, "A %s header was found at : %llu\n",
		currentneedle->suffix,
		positionUseCoverageBlockmap(state, startLocation));
#endif
      }

      currentneedle->offsets.numheaders++;
      // 헤더 탐지 수가 많을 때 realloc 병목을 줄이기 위한 용량 보장.
      ensureHeaderStorageCapacity(state, currentneedle);
      currentneedle->offsets.headers[currentneedle->offsets.numheaders -
				     1] = startLocation;
      currentneedle->offsets.headerlens[currentneedle->offsets.
					numheaders - 1] =
	foundatlens[needlenum][i];
    }
  }


  // ---------------- threaded footer search ------------------ //
  // ---------------- threaded footer search ------------------ //

  // now footer search, if:
  //
  // there's a footer for the file type AND
  //
  // (at least one header for that type has been previously seen and 
  // at least one header is viable--that is, it was found in the current
  // buffer, or it's less than the max carve distance behind the current
  // file offset
  //
  // OR
  // 
  // a header/footer database is being created.  In this case, ALL headers and
  // footers must be discovered)

  if(state->modeVerbose) {
    printf("Waking up threads for footer searches.\n");
  }

  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);
    if(
       // regular case--want to search for only "viable" (in the sense that they are
       // useful for carving unfragmented files) footers, to save time
       (currentneedle->offsets.numheaders > 0 &&
	currentneedle->endlength &&
	(currentneedle->offsets.
	 headers[currentneedle->offsets.numheaders - 1] > offset
	 || (offset -
	     currentneedle->offsets.headers[currentneedle->offsets.
					    numheaders - 1] <
	     currentneedle->length))) ||
       // generating header/footer database, need to find all footers
       // BUG:  ALSO need to do this for discovery of fragmented files--document this
       (currentneedle->endlength && state->generateHeaderFooterDatabase)) {
      // # of matches in last element of foundat array
      foundat[needlenum][MAX_MATCHES_PER_BUFFER] = 0;
      threadargs[needlenum].id = needlenum;
      threadargs[needlenum].str = currentneedle->end;
      threadargs[needlenum].length = currentneedle->endlength;
      threadargs[needlenum].startpos = readbuffer;
      threadargs[needlenum].offset = (long)readbuffer + lengthofbuf;
      threadargs[needlenum].foundat = foundat[needlenum];
      threadargs[needlenum].foundatlens = foundatlens[needlenum];
      threadargs[needlenum].strisRE = currentneedle->endisRE;
      if(currentneedle->endisRE) {
	threadargs[needlenum].regex = &(currentneedle->endstate.re);
      }
      else {
	threadargs[needlenum].table = currentneedle->endstate.bm_table;
      }
      threadargs[needlenum].casesensitive = currentneedle->casesensitive;
      threadargs[needlenum].nosearchoverlap = state->noSearchOverlap;
      threadargs[needlenum].state = state;

      // unblock thread
      //      sem_post(&workavailable[needlenum]);
      pthread_mutex_unlock(&workavailable[needlenum]);

    }
    else {
      threadargs[needlenum].length = 0;
    }
  }

  if(state->modeVerbose) {
    printf("Waiting for thread group synchronization.\n");
  }

  // ---------- thread group synchronization point ----------- //
  // ---------- thread group synchronization point ----------- //

  // wait for all threads to complete footer search before proceeding
  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    if(threadargs[needlenum].length > 0) {
      //      sem_wait(&workcomplete[needlenum]);
      pthread_mutex_lock(&workcomplete[needlenum]);
    }
  }

  if(state->modeVerbose) {
    printf("Thread group synchronization complete.\n");
  }

  // digest footer locations discovered by the thread group

  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);
    // number of matches stored in last element of vector
    for(i = 0; i < (long)foundat[needlenum][MAX_MATCHES_PER_BUFFER]; i++) {
      startLocation = offset + (foundat[needlenum][i] - readbuffer);
      if(state->modeVerbose) {
#ifdef _WIN32
	fprintf(stdout, "A %s footer was found at : %I64u\n",
		currentneedle->suffix,
		positionUseCoverageBlockmap(state, startLocation));
#else
	fprintf(stdout, "A %s footer was found at : %llu\n",
		currentneedle->suffix,
		positionUseCoverageBlockmap(state, startLocation));
#endif
      }

      currentneedle->offsets.numfooters++;
      // 푸터 탐지 수 증가도 동일한 방식으로 처리한다.
      ensureFooterStorageCapacity(state, currentneedle);
      currentneedle->offsets.footers[currentneedle->offsets.numfooters -
				     1] = startLocation;
      currentneedle->offsets.footerlens[currentneedle->offsets.
					numfooters - 1] =
	foundatlens[needlenum][i];
    }
  }

#endif // multi-core CPU code
  ///////////////////////////////////////////////////
  ///////////////////////////////////////////////////
  ///////////END MULTI-THREADED SEARCH///////////////
  ///////////////////////////////////////////////////
  ///////////////////////////////////////////////////

  return SCALPEL_OK;
}




////////////////////////////////////////////////////////////////////////////////
/////////////////////// LMIII //////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

/* In order to facilitate asynchronous reads, we will set up a reader
   pthread, whose only job is to read the image in SIZE_OF_BUFFER
   chunks using the coverage map functions.

   This thread will buffer QUEUELEN reads.
*/


// Streaming reader gets empty buffers from the empty_readbuf queue, reads 
// SIZE_OF_BUFFER chunks of the input image into the buffers and puts them into
// the full_readbuf queue for processing.
void *streaming_reader(void *sss) {

  struct scalpelState *state = (struct scalpelState *)sss;

  long long filesize = 0, bytesread = 0, filebegin = 0,
    fileposition = 0, beginreadpos = 0;
  long err = SCALPEL_OK;
  int displayUnits = UNITS_BYTES;
  int longestneedle = findLongestNeedle(state->SearchSpec);

	reads_finished = FALSE;

  filebegin = ftello(state->infile);
  if((filesize = measureOpenFile(state->infile, state)) == -1) {
    fprintf(stderr,
	    "ERROR: Couldn't measure size of image file %s\n",
	    state->imagefile);
    err=SCALPEL_ERROR_FILE_READ;
    goto exit_reader_thread;
  }

  // Get empty buffer from empty_readbuf queue
  readbuf_info *rinfo = (readbuf_info *)get(empty_readbuf);

  // Read chunk of image into empty buffer.
  while (1) {
    double readStartedAt = currentTimeSeconds();
    bytesread =
      fread_use_coverage_map(state, rinfo->readbuf, 1, SIZE_OF_BUFFER,
			     state->infile);
    recordReadTime(readStartedAt);
    if(bytesread <= longestneedle - 1) {
      break;
    }

    if(state->modeVerbose) {
#ifdef _WIN32
      fprintf(stdout, "Read %I64u bytes from image file.\n", bytesread);
#else
      fprintf(stdout, "Read %llu bytes from image file.\n", bytesread);
#endif
    }

    if((err = ferror(state->infile))) {
      err = SCALPEL_ERROR_FILE_READ;      
      goto exit_reader_thread;
    }

    // progress report needs a fileposition that doesn't depend on coverage map
    fileposition = ftello(state->infile);
    displayPosition(&displayUnits, fileposition - filebegin,
		    filesize, state->imagefile);

    // if carving is dependent on coverage map, need adjusted fileposition
    fileposition = ftello_use_coverage_map(state, state->infile);
    beginreadpos = fileposition - bytesread;

    //signal check
    if(signal_caught == SIGTERM || signal_caught == SIGINT) {
      clean_up(state, signal_caught);
    }

    // Put now-full buffer into full_readbuf queue.
    // Note that if the -s option was used we need to adjust the relative begin
    // position 
    rinfo->bytesread = bytesread;
    rinfo->beginreadpos = beginreadpos - state->skip;
    put(full_readbuf, (void *)rinfo);

    // Get another empty buffer.
    rinfo = (readbuf_info *)get(empty_readbuf);

    // move file position back a bit so headers and footers that fall
    // across SIZE_OF_BUFFER boundaries in the image file aren't
    // missed
    fseeko_use_coverage_map(state, state->infile, -1 * (longestneedle - 1));
  }

 exit_reader_thread:
  if (err != SCALPEL_OK) {
    handleError(state, err);
  }
  // Done reading image.
  reads_finished = TRUE;
  if (state->infile) {
    fclose(state->infile);
  }
  pthread_exit(0);
  return NULL;
}



// Scalpel's approach dictates that this function digAllFiles an image
// file, building the header/footer offset database.  The task of
// extracting files from the image has been moved to carveImageFile(),
// which operates in a second pass over the image.  Digging for
// header/footer values proceeds in SIZE_OF_BUFFER sized chunks of the
// image file.  This buffer is now global and named "readbuffer".
int digImageFile(struct scalpelState *state) {

  int status, err;
  int longestneedle = findLongestNeedle(state->SearchSpec);
  long long filebegin, filesize;


  if ((err = setupAuditFile(state)) != SCALPEL_OK) {
    return err;
  }

  if(state->SearchSpec[0].suffix == NULL) {
    return SCALPEL_ERROR_NO_SEARCH_SPEC;
  }

  // open current image file
  if((state->infile = fopen(state->imagefile, "rb")) == NULL) {
    return SCALPEL_ERROR_FILE_OPEN;
  }

#ifdef _WIN32
  // set binary mode for Win32
  setmode(fileno(state->infile), O_BINARY);
#endif
#ifdef __linux
  fcntl(fileno(state->infile), F_SETFL, O_LARGEFILE);
#endif

  // skip initial portion of input file, if that cmd line option
  // was set
  if(state->skip > 0) {
    if(!skipInFile(state, state->infile)) {
      return SCALPEL_ERROR_FILE_READ;
    }

    // BUG/QUESTION:  ***GGRIII: want to update coverage bitmap when skip is specified????

    // ANSWER:  ***GGRIII: Don't worry about this:  when coverage bitmaps are reintroduced, skip can
    // be built on top, so it won't matter

  }

  filebegin = ftello(state->infile);
  if((filesize = measureOpenFile(state->infile, state)) == -1) {
    fprintf(stderr,
	    "ERROR: Couldn't measure size of image file %s\n",
	    state->imagefile);
    return SCALPEL_ERROR_FILE_READ;
  }

  // can't process an image file smaller than the longest needle
  if(filesize <= longestneedle * 2) {
    return SCALPEL_ERROR_FILE_TOO_SMALL;
  }

#ifdef _WIN32
  if(state->modeVerbose) {
    fprintf(stdout, "Total file size is %I64u bytes\n", filesize);
  }
#else
  if(state->modeVerbose) {
    fprintf(stdout, "Total file size is %llu bytes\n", filesize);
  }
#endif

  // allocate and initialize coverage bitmap and blockmap, if appropriate
  if((err = setupCoverageMaps(state, filesize)) != SCALPEL_OK) {
    return err;
  }

  // process SIZE_OF_BUFFER-sized chunks of the current image
  // file and look for both headers and footers, recording their
  // offsets for use in the 2nd scalpel phase, when file data will 
  // be extracted.

  fprintf(stdout, "Image file pass 1/2.\n");

  // Create and start the streaming reader thread for this image file.
  reads_finished = FALSE;
  pthread_t reader;
  if(pthread_create(&reader, NULL, streaming_reader, (void *)state) != 0) {
    return SCALPEL_ERROR_PTHREAD_FAILURE;
  }

  // wait on reader mutex "reads_begun"
  

#ifdef MULTICORE_THREADING

  // The reader is now reading in chunks of the image. We call digbuffer on
  // these chunks for multi-threaded search. 

  while (!reads_finished || !full_readbuf->empty) {

    readbuf_info *rinfo = (readbuf_info *)get(full_readbuf);
    readbuffer = rinfo->readbuf;
    {
      double searchStartedAt = currentTimeSeconds();
      status = digBuffer(state, rinfo->bytesread, rinfo->beginreadpos);
      recordSearchTime(searchStartedAt);
    }
    if(status != SCALPEL_OK) {
      return status;
    }
    put(empty_readbuf, (void *)rinfo);
  }

#endif

  pthread_join(reader, NULL);

  return SCALPEL_OK;
}




// carveImageFile() uses the header/footer offsets database
// created by digImageFile() to build a list of files to carve.  These
// files are then carved during a single, sequential pass over the
// image file.  The global 'readbuffer' is used as a buffer in this
// function.

int carveImageFile(struct scalpelState *state) {

  FILE *infile;
  struct SearchSpecLine *currentneedle;
  struct CarveInfo *carveinfo;
  char fn[MAX_STRING_LENGTH];	// temp buffer for output filename
  char orgdir[MAX_STRING_LENGTH];	// buffer for name of organizing subdirectory
  long long start, stop;	// temp begin/end bytes for file to carve
  unsigned long long prevstopindex;	// tracks index of first 'reasonable' 
  // footer
  int needlenum;
  long long filesize = 0, bytesread = 0, fileposition = 0, filebegin = 0;
  long err = 0;
  int displayUnits = UNITS_BYTES;
  int success = 0;
  long long i, j;
  int halt;
  unsigned long long blockcount;
  unsigned long long initializedBlocks = 0;
  char chopped;			// file chopped because it exceeds
  // max carve size for type?
  unsigned long long firstcandidatefooter=0;
  unsigned long long headerblockindex, footerblockindex;
  unsigned long long continueCarveSpans = 0;
  unsigned long long activeCarveCount = 0;
  unsigned long long peakActiveCarves = 0;
  double queueStartedAt;
  double pass2StartedAt;
  BlockCarveEvents *eventsByBlock;
  FileHandleCache handleCache;
  struct CarveInfo *activeCarves = 0;

  // open image file and get size so carvelists can be allocated
  if((infile = fopen(state->imagefile, "rb")) == NULL) {
    fprintf(stderr, "ERROR: Couldn't open input file: %s -- %s\n",
	    (*(state->imagefile) == '\0') ? "<blank>" : state->imagefile,
	    strerror(errno));
    return SCALPEL_ERROR_FILE_OPEN;
  }

#ifdef _WIN32
  // explicit binary option for Win32
  setmode(fileno(infile), O_BINARY);
#endif
#ifdef __linux
  fcntl(fileno(infile), F_SETFL, O_LARGEFILE);
#endif

  // If skip was activated, then there's no way headers/footers were
  // found there, so skip during the carve operations, too

  if(state->skip > 0) {
    if(!skipInFile(state, infile)) {
      return SCALPEL_ERROR_FILE_READ;
    }
  }

  filebegin = ftello(infile);
  if((filesize = measureOpenFile(infile, state)) == -1) {
    fprintf(stderr,
	    "ERROR: Couldn't measure size of image file %s\n",
	    state->imagefile);
    return SCALPEL_ERROR_FILE_READ;
  }

  totalreads = 0.0;
  totalwrites = 0.0;

  blockcount = 2 + (filesize / SIZE_OF_BUFFER);
  eventsByBlock =
    (BlockCarveEvents *) calloc(blockcount, sizeof(BlockCarveEvents));
  checkMemoryAllocation(state, eventsByBlock, __LINE__, __FILE__,
			"carve event table");

  memset(&handleCache, 0, sizeof(handleCache));
  handleCache.max_open = MAX_FILES_TO_OPEN;

  fprintf(stdout, "Allocating carve event table...\n");
  fprintf(stdout, "Carve event table ready. Building start/stop events...\n");

  queueStartedAt = currentTimeSeconds();

  for(needlenum = 0; needlenum < state->specLines; needlenum++) {

    currentneedle = &(state->SearchSpec[needlenum]);

    // handle each discovered header independently

    prevstopindex = 0;
    for(i = 0; i < (long long)currentneedle->offsets.numheaders; i++) {
      start = currentneedle->offsets.headers[i];

			////////////// DEBUG ////////////////////////
			//fprintf(stdout, "start: %lu\n", start);
			
      // block aligned test for "-q"

      if(state->blockAlignedOnly && start % state->alignedblocksize != 0) {
	continue;
      }

      stop = 0;
      chopped = 0;

      // case 1: no footer defined for this file type
      if(!currentneedle->endlength) {

	// this is the unfortunate case--if file type doesn't have a footer,
	// all we can done is carve a block between header position and
	// maximum carve size.
	stop = start + currentneedle->length - 1;
	// these are always considered chopped, because we don't really
	// know the actual size
	chopped = 1;
      }
      else if(currentneedle->searchtype == SEARCHTYPE_FORWARD ||
	      currentneedle->searchtype == SEARCHTYPE_FORWARD_NEXT) {
	// footer defined: use FORWARD or FORWARD_NEXT semantics.
	// Stop at first occurrence of footer, but for FORWARD,
	// include the header in the carved file; for FORWARD_NEXT,
	// don't include footer in carved file.  For FORWARD_NEXT, if
	// no footer is found, then the maximum carve size for this
	// file type will be used and carving will proceed.  For
	// FORWARD, if no footer is found then no carving will be
	// performed unless -b was specified on the command line.

	halt = 0;

	if (state->handleEmbedded && 
	    (currentneedle->searchtype == SEARCHTYPE_FORWARD ||
	     currentneedle->searchtype == SEARCHTYPE_FORWARD_NEXT)) {
	  firstcandidatefooter=adjustForEmbedding(currentneedle, i, &prevstopindex);
	}
	else {
	  firstcandidatefooter=prevstopindex;
	}

	for (j=firstcandidatefooter;
	     j < (long long)currentneedle->offsets.numfooters && ! halt; j++) {

	  if((long long)currentneedle->offsets.footers[j] <= start) {
	    if (! state->handleEmbedded) {
	      prevstopindex=j;
	    }

	  }
	  else {
	    halt = 1;
	    stop = currentneedle->offsets.footers[j];

	    if(currentneedle->searchtype == SEARCHTYPE_FORWARD) {
	      // include footer in carved file
	      stop += currentneedle->endlength - 1;
	      // 	BUG? this or above?		    stop += currentneedle->offsets.footerlens[j] - 1;
	    }
	    else {
	      // FORWARD_NEXT--don't include footer in carved file
	      stop--;
	    }
	    // sanity check on size of potential file to carve--different
	    // actions depending on FORWARD or FORWARD_NEXT semantics
	    if(stop - start + 1 > (long long)currentneedle->length) {
	      if(currentneedle->searchtype == SEARCHTYPE_FORWARD) {
		// if the user specified -b, then foremost 0.69
		// compatibility is desired: carve this file even 
		// though the footer wasn't found and indicate
		// the file was chopped, in the log.  Otherwise, 
		// carve nothing and move on.
		if(state->carveWithMissingFooters) {
		  stop = start + currentneedle->length - 1;
		  chopped = 1;
		}
		else {
		  stop = 0;
		}
	      }
	      else {
		// footer found for FORWARD_NEXT, but distance exceeds
		// max carve size for this file type, so use max carve
		// size as stop
		stop = start + currentneedle->length - 1;
		chopped = 1;
	      }
	    }
	  }
	}
	if(!halt &&
	   (currentneedle->searchtype == SEARCHTYPE_FORWARD_NEXT ||
	    (currentneedle->searchtype == SEARCHTYPE_FORWARD &&
	     state->carveWithMissingFooters))) {
	  // no footer found for SEARCHTYPE_FORWARD_NEXT, or no footer
	  // found for SEARCHTYPE_FORWARD and user specified -b, so just use
	  // max carve size for this file type as stop
	  stop = start + currentneedle->length - 1;
	}
      }
      else {
	// footer defined: use REVERSE semantics: want matching footer
	// as far away from header as possible, within maximum carving
	// size for this file type.  Don't bother to look at footers
	// that can't possibly match a header and remember this info
	// in prevstopindex, as the next headers will be even deeper
	// into the image file.  Footer is included in carved file for
	// this type of carve.
	halt = 0;
	for(j = prevstopindex; j < (long long)currentneedle->offsets.numfooters &&
	      !halt; j++) {
	  if((long long)currentneedle->offsets.footers[j] <= start) {
	    prevstopindex = j;
	  }
	  else if(currentneedle->offsets.footers[j] - start <=
		  currentneedle->length) {
	    stop = currentneedle->offsets.footers[j]
	      + currentneedle->endlength - 1;
	  }
	  else {
	    halt = 1;
	  }
	}
      }

      // if stop <> 0, then we have enough information to set up a
      // file carving operation.  It must pass the minimum carve size
      // test, if currentneedle->minLength != 0.
      //     if(stop) {
      if (stop && (stop - start + 1) >= (long long)currentneedle->minlength) {

	// don't carve past end of image file...
	stop = stop > filesize ? filesize : stop;

	// find indices (in SIZE_OF_BUFFER units) of header and
	// footer, so the carveinfo can be placed into the right
	// queues.  The priority of each element in a queue allows the
	// appropriate thing to be done (e.g., STARTSTOPCARVE,
	// STARTCARVE, STOPCARVE, CONTINUECARVE).

	headerblockindex = start / SIZE_OF_BUFFER;
	footerblockindex = stop / SIZE_OF_BUFFER;

	// set up a struct CarveInfo for inclusion into the
	// appropriate carvelists

	// generate unique filename for file to carve

	if(state->organizeSubdirectories) {
	  snprintf(orgdir, MAX_STRING_LENGTH, "%s/%s-%d-%1lu",
		   state->outputdirectory,
		   currentneedle->suffix,
		   needlenum, currentneedle->organizeDirNum);
	  if(!state->previewMode) {
#ifdef _WIN32
	    mkdir(orgdir);
#else
	    mkdir(orgdir, 0777);
#endif
	  }
	}
	else {
	  snprintf(orgdir, MAX_STRING_LENGTH, "%s", state->outputdirectory);
	}

	if(state->modeNoSuffix || currentneedle->suffix[0] ==
	   SCALPEL_NOEXTENSION) {
#ifdef _WIN32
	  snprintf(fn, MAX_STRING_LENGTH, "%s/%08I64u",
		   orgdir, state->fileswritten);
#else
	  snprintf(fn, MAX_STRING_LENGTH, "%s/%08llu",
		   orgdir, state->fileswritten);
#endif

	}
	else {
#ifdef _WIN32
	  snprintf(fn, MAX_STRING_LENGTH, "%s/%08I64u.%s",
		   orgdir, state->fileswritten, currentneedle->suffix);
#else
	  snprintf(fn, MAX_STRING_LENGTH, "%s/%08llu.%s",
		   orgdir, state->fileswritten, currentneedle->suffix);
#endif
	}
	state->fileswritten++;
	currentneedle->numfilestocarve++;
	if(currentneedle->numfilestocarve % state->organizeMaxFilesPerSub == 0) {
	  currentneedle->organizeDirNum++;
	}

	carveinfo = (struct CarveInfo *)malloc(sizeof(struct CarveInfo));
	checkMemoryAllocation(state, carveinfo, __LINE__, __FILE__,
			      "carveinfo");

	// remember filename
	carveinfo->filename = (char *)malloc(strlen(fn) + 1);
	checkMemoryAllocation(state, carveinfo->filename, __LINE__,
			      __FILE__, "carveinfo");
	strcpy(carveinfo->filename, fn);
	carveinfo->start = start;
	carveinfo->stop = stop;
	carveinfo->startblock = headerblockindex;
	carveinfo->stopblock = footerblockindex;
	carveinfo->chopped = chopped;
	carveinfo->fp = 0;
	carveinfo->completed = 0;
	carveinfo->opened_once = 0;
	carveinfo->is_active = 0;
	carveinfo->iobuf = 0;
	carveinfo->active_prev = 0;
	carveinfo->active_next = 0;
	carveinfo->lru_prev = 0;
	carveinfo->lru_next = 0;
	carveinfo->start_next = 0;
	carveinfo->stop_next = 0;
	carveinfo->same_next = 0;

	if(!eventsByBlock[headerblockindex].has_work) {
	  initializedBlocks++;
	  eventsByBlock[headerblockindex].has_work = TRUE;
	}
	if(headerblockindex == footerblockindex) {
	  carveinfo->same_next = eventsByBlock[headerblockindex].same_head;
	  eventsByBlock[headerblockindex].same_head = carveinfo;
	}
	else {
	  carveinfo->start_next = eventsByBlock[headerblockindex].start_head;
	  eventsByBlock[headerblockindex].start_head = carveinfo;
	  carveinfo->stop_next = eventsByBlock[footerblockindex].stop_head;
	  eventsByBlock[footerblockindex].stop_head = carveinfo;
	  for(j = (long long)headerblockindex + 1;
	      j <= (long long)footerblockindex; j++) {
	    if(!eventsByBlock[j].has_work) {
	      initializedBlocks++;
	      eventsByBlock[j].has_work = TRUE;
	    }
	  }
	  continueCarveSpans += footerblockindex - headerblockindex - 1;
	}
      }
    }
  }
  recordQueueTime(queueStartedAt);

#ifdef _WIN32
  fprintf(stdout,
	  "Event schedule built using %I64u/%I64u initialized blocks, CONTINUE spans avoided = %I64u.\n",
	  initializedBlocks, blockcount, continueCarveSpans);
#else
  fprintf(stdout,
	  "Event schedule built using %llu/%llu initialized blocks, CONTINUE spans avoided = %llu.\n",
	  initializedBlocks, blockcount, continueCarveSpans);
#endif

  fprintf(stdout, "Event schedule built.  Workload:\n");
  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);
    fprintf(stdout, "%s with header \"", currentneedle->suffix);
    fprintf(stdout, "%s", currentneedle->begintext);
    fprintf(stdout, "\" and footer \"");
    if(currentneedle->end == 0) {
      fprintf(stdout, "NONE");
    }
    else {
      fprintf(stdout, "%s", currentneedle->endtext);
    }
#ifdef _WIN32
    fprintf(stdout, "\" --> %I64u files\n", currentneedle->numfilestocarve);
#else
    fprintf(stdout, "\" --> %llu files\n", currentneedle->numfilestocarve);
#endif

  }

  if(state->previewMode) {
    fprintf(stdout, "** PREVIEW MODE: GENERATING AUDIT LOG ONLY **\n");
    fprintf(stdout, "** NO CARVED FILES WILL BE WRITTEN **\n");
  }

  fprintf(stdout, "Carving files from image.\n");
  fprintf(stdout, "Image file pass 2/2.\n");

  pass2StartedAt = currentTimeSeconds();
  success = 1;
  while (success) {
    unsigned long long blockindex;
    unsigned long long biglseek = 0L;
    fileposition = ftello_use_coverage_map(state, infile);
    blockindex = fileposition / SIZE_OF_BUFFER;

    while (success && blockindex < blockcount &&
	   !eventsByBlock[blockindex].has_work) {
      biglseek += SIZE_OF_BUFFER;
      fileposition += SIZE_OF_BUFFER;
      success = fileposition <= filesize;
      blockindex = fileposition / SIZE_OF_BUFFER;
    }

    if(success && biglseek) {
      fseeko_use_coverage_map(state, infile, biglseek);
    }

    if(!success) {
      // not an error--just means we've exhausted the image file--show
      // progress report then quit carving
      displayPosition(&displayUnits, filesize, filesize, state->imagefile);

      continue;
    }

    if(!state->previewMode) {
      double readStartedAt = currentTimeSeconds();
      bytesread =
	fread_use_coverage_map(state, readbuffer, 1, SIZE_OF_BUFFER, infile);
      recordReadTime(readStartedAt);
      // Check for read errors
      if((err = ferror(infile))) {
	return SCALPEL_ERROR_FILE_READ;
      }
      else if(bytesread == 0) {
	// no error, but image file exhausted
	success = 0;
	continue;
      }
    }
    else {
      // in preview mode, seeks are used in the 2nd pass instead of
      // reads.  This isn't optimal, but it's fast enough and avoids
      // complicating the file carving code further.

      fileposition = ftello_use_coverage_map(state, infile);
      fseeko_use_coverage_map(state, infile, SIZE_OF_BUFFER);
      bytesread = ftello_use_coverage_map(state, infile) - fileposition;

      // Check for errors
      if((err = ferror(infile))) {
	return SCALPEL_ERROR_FILE_READ;
      }
      else if(bytesread == 0) {
	// no error, but image file exhausted
	success = 0;
	continue;
      }
    }

    success = 1;

    // progress report needs real file position
    fileposition = ftello(infile);
    displayPosition(&displayUnits, fileposition - filebegin,
		    filesize, state->imagefile);

    // if using coverage map for carving, need adjusted file position
    fileposition = ftello_use_coverage_map(state, infile);
    blockindex = (fileposition - bytesread) / SIZE_OF_BUFFER;

    // signal check
    if(signal_caught == SIGTERM || signal_caught == SIGINT) {
      clean_up(state, signal_caught);
    }

    {
      BlockCarveEvents *blockEvents = &eventsByBlock[blockindex];
      struct CarveInfo *sameCarve = 0;
      struct CarveInfo *startCarve = 0;
      struct CarveInfo *activeCarve = 0;
      struct CarveInfo *nextActive = 0;
      struct CarveInfo *stopCarve = 0;
      unsigned long long blockStart = fileposition - bytesread;

      for(sameCarve = blockEvents->same_head; sameCarve;
	  sameCarve = sameCarve->same_next) {
	err = writeCarveBlock(state, &handleCache, sameCarve,
			      blockStart, bytesread, TRUE);
	if(err != SCALPEL_OK) {
	  return err;
	}
      }

      for(startCarve = blockEvents->start_head; startCarve;
	  startCarve = startCarve->start_next) {
	addActiveCarve(&activeCarves, startCarve, &activeCarveCount,
		       &peakActiveCarves);
      }

      activeCarve = activeCarves;
      while(activeCarve) {
	nextActive = activeCarve->active_next;
	err = writeCarveBlock(state, &handleCache, activeCarve,
			      blockStart, bytesread, FALSE);
	if(err != SCALPEL_OK) {
	  return err;
	}
	if(activeCarve->completed) {
	  removeActiveCarve(&activeCarves, activeCarve, &activeCarveCount);
	}
	activeCarve = nextActive;
      }

      for(stopCarve = blockEvents->stop_head; stopCarve;
	  stopCarve = stopCarve->stop_next) {
	if(stopCarve->completed) {
	  removeActiveCarve(&activeCarves, stopCarve, &activeCarveCount);
	}
      }
    }
  }

  while(handleCache.lru_tail) {
    err = closeCarveFile(state, &handleCache, handleCache.lru_tail, FALSE);
    if(err != SCALPEL_OK) {
      return err;
    }
  }

  //  closeFile(infile);
  fclose(infile);

  // write header/footer database, if necessary, before 
  // cleanup for current image file.  

  if(state->generateHeaderFooterDatabase) {
    if((err = writeHeaderFooterDatabase(state)) != SCALPEL_OK) {
      return err;
    }
  }

  // tear down coverage maps, if necessary
  destroyCoverageMaps(state);

  printf("Processing of image file complete. Cleaning up...\n");
  printPerformanceMetrics(currentTimeSeconds() - pass2StartedAt,
			  handleCache.open_close_sec,
			  handleCache.open_ops,
			  handleCache.reopen_ops,
			  handleCache.close_ops,
			  handleCache.eviction_ops,
			  handleCache.peak_open_count,
			  peakActiveCarves,
			  handleCache.bytes_written);

  // tear down header/footer databases

  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);
    if(currentneedle->offsets.headers) {
      free(currentneedle->offsets.headers);
    }
    if(currentneedle->offsets.footers) {
      free(currentneedle->offsets.footers);
    }
    currentneedle->offsets.headers = 0;
    currentneedle->offsets.footers = 0;
    currentneedle->offsets.numheaders = 0;
    currentneedle->offsets.numfooters = 0;
    currentneedle->offsets.headerstorage = 0;
    currentneedle->offsets.footerstorage = 0;
    currentneedle->numfilestocarve = 0;
  }

  free(eventsByBlock);

  printf("Done.");
  return SCALPEL_OK;
}






// The coverage blockmap marks which blocks (of a user-specified size)
// have been "covered" by a carved file.  If the coverage blockmap
// is to be modified, check to see if it exists.  If it does, then
// open it and set the file handle in the Scalpel state.  If it
// doesn't, create a zeroed copy and set the file handle in the
// Scalpel state.  If the coverage blockmap is guiding carving, then
// create the coverage bitmap and initialize it using the coverage
// blockmap file.  The difference between the coverage blockmap (on
// disk) and the coverage bitmap (in memory) is that the blockmap
// counts carved files that cover a block.  The coverage bitmap only
// indicates if ANY carved file covers a block.  'filesize' is the
// size of the image file being examined.

static int
setupCoverageMaps(struct scalpelState *state, unsigned long long filesize) {

  char fn[MAX_STRING_LENGTH];	// filename for coverage blockmap
  unsigned long long i, k;
  int empty;
  unsigned int blocksize, entry;


  state->coveragebitmap = 0;
  state->coverageblockmap = 0;

  if(!state->useCoverageBlockmap && !state->updateCoverageBlockmap) {
    return SCALPEL_OK;
  }

  fprintf(stdout, "Setting up coverage blockmap.\n");
  // generate pathname for coverage blockmap
  snprintf(fn, MAX_STRING_LENGTH, "%s", state->coveragefile);

  fprintf(stdout, "Coverage blockmap is \"%s\".\n", fn);

  empty = ((state->coverageblockmap = fopen(fn, "rb")) == NULL);
  fprintf(stdout, "Coverage blockmap file is %s.\n",
	  (empty ? "EMPTY" : "NOT EMPTY"));

  if(!empty) {
#ifdef _WIN32
    // set binary mode for Win32
    setmode(fileno(state->coverageblockmap), O_BINARY);
#endif
#ifdef __linux
    fcntl(fileno(state->coverageblockmap), F_SETFL, O_LARGEFILE);
#endif

    fprintf(stdout, "Reading blocksize from coverage blockmap file.\n");

    // read block size and make sure it matches user-specified block size
    if(fread(&blocksize, sizeof(unsigned int), 1, state->coverageblockmap) != 1) {
      fprintf(stderr,
	      "Error reading coverage blockmap blocksize in\ncoverage blockmap file: %s\n",
	      fn);
      fprintf(state->auditFile,
	      "Error reading coverage blockmap blocksize in\ncoverage blockmap file: %s\n",
	      fn);
      return SCALPEL_ERROR_FATAL_READ;
    }

    if(state->coverageblocksize != 0 && blocksize != state->coverageblocksize) {
      fprintf(stderr,
	      "User-specified blocksize does not match blocksize in\ncoverage blockmap file: %s; aborting.\n",
	      fn);
      fprintf(state->auditFile,
	      "User-specified blocksize does not match blocksize in\ncoverage blockmap file: %s\n",
	      fn);
      return SCALPEL_GENERAL_ABORT;
    }

    state->coverageblocksize = blocksize;
    fprintf(stdout, "Blocksize for coverage blockmap is %u.\n",
	    state->coverageblocksize);

    state->coveragenumblocks =
      (unsigned long
       long)(ceil((double)filesize / (double)state->coverageblocksize));
#ifdef _WIN32
    fprintf(stdout, "# of blocks in coverage blockmap is %I64u.\n",
	    state->coveragenumblocks);
#else
    fprintf(stdout, "# of blocks in coverage blockmap is %llu.\n",
	    state->coveragenumblocks);
#endif

    fprintf(stdout, "Allocating and clearing in-core coverage bitmap.\n");
    // for bitmap, 8 bits per unsigned char, with each bit representing one
    // block
    state->coveragebitmap =
      (unsigned char *)malloc((state->coveragenumblocks / 8) *
			      sizeof(unsigned char));
    checkMemoryAllocation(state, state->coveragebitmap, __LINE__, __FILE__,
			  "coveragebitmap");

    // zap coverage bitmap 
    for(k = 0; k < state->coveragenumblocks / 8; k++) {
      state->coveragebitmap[k] = 0;
    }

    fprintf(stdout,
	    "Reading existing coverage blockmap...this may take a while.\n");

    for(i = 0; i < state->coveragenumblocks; i++) {
      fseeko(state->coverageblockmap, (i + 1) * sizeof(unsigned int), SEEK_SET);
      if(fread(&entry, sizeof(unsigned int), 1, state->coverageblockmap) != 1) {
	fprintf(stderr,
		"Error reading coverage blockmap entry (blockmap truncated?): %s\n",
		fn);
	fprintf(state->auditFile,
		"Error reading coverage blockmap entry (blockmap truncated?): %s\n",
		fn);
	return SCALPEL_ERROR_FATAL_READ;
      }
      if(entry) {
	state->coveragebitmap[i / 8] |= 1 << (i % 8);
      }
    }
  }
  else if(empty && state->useCoverageBlockmap && !state->updateCoverageBlockmap) {
    fprintf(stderr,
	    "-u option requires that the blockmap file %s exist.\n", fn);
    fprintf(state->auditFile,
	    "-u option requires that the blockmap file %s exist.\n", fn);
    return SCALPEL_GENERAL_ABORT;
  }
  else {
    // empty coverage blockmap
    if(state->coverageblocksize == 0) {	// user didn't override default
      state->coverageblocksize = 512;
    }
    fprintf(stdout, "Blocksize for coverage blockmap is %u.\n",
	    state->coverageblocksize);
    state->coveragenumblocks =
      (unsigned long long)ceil((double)filesize /
			       (double)state->coverageblocksize);
#ifdef _WIN32
    fprintf(stdout, "# of blocks in coverage blockmap is %I64u.\n",
	    state->coveragenumblocks);
#else
    fprintf(stdout, "# of blocks in coverage blockmap is %llu.\n",
	    state->coveragenumblocks);
#endif

    fprintf(stdout, "Allocating and clearing in-core coverage bitmap.\n");
    // for bitmap, 8 bits per unsigned char, with each bit representing one
    // block
    state->coveragebitmap =
      (unsigned char *)malloc((state->coveragenumblocks / 8) *
			      sizeof(unsigned char));
    checkMemoryAllocation(state, state->coveragebitmap, __LINE__, __FILE__,
			  "coveragebitmap");

    // zap coverage bitmap 
    for(k = 0; k < state->coveragenumblocks / 8; k++) {
      state->coveragebitmap[k] = 0;
    }
  }

  // change mode to read/write for future updates if coverage blockmap will be updated
  if(state->updateCoverageBlockmap) {
    if(state->modeVerbose) {
      fprintf(stdout, "Changing mode of coverage blockmap file to R/W.\n");
    }

    if(!empty) {
      fclose(state->coverageblockmap);
    }
    if((state->coverageblockmap = fopen(fn, (empty ? "w+b" : "r+b"))) == NULL) {
      fprintf(stderr, "Error writing to coverage blockmap file: %s\n", fn);
      fprintf(state->auditFile,
	      "Error writing to coverage blockmap file: %s\n", fn);
      return SCALPEL_ERROR_FILE_WRITE;
    }

#ifdef _WIN32
    // set binary mode for Win32
    setmode(fileno(state->coverageblockmap), O_BINARY);
#endif
#ifdef __linux
    fcntl(fileno(state->coverageblockmap), F_SETFL, O_LARGEFILE);
#endif

    if(empty) {
      // create entries in empty coverage blockmap file
      fprintf(stdout,
	      "Writing empty coverage blockmap...this may take a while.\n");
      entry = 0;
      if(fwrite
	 (&(state->coverageblocksize), sizeof(unsigned int), 1,
	  state->coverageblockmap) != 1) {
	fprintf(stderr,
		"Error writing initial entry in coverage blockmap file!\n");
	fprintf(state->auditFile,
		"Error writing initial entry in coverage blockmap file!\n");
	return SCALPEL_ERROR_FILE_WRITE;
      }
      for(k = 0; k < state->coveragenumblocks; k++) {
	if(fwrite
	   (&entry, sizeof(unsigned int), 1, state->coverageblockmap) != 1) {
	  fprintf(stderr, "Error writing to coverage blockmap file!\n");
	  fprintf(state->auditFile,
		  "Error writing to coverage blockmap file!\n");
	  return SCALPEL_ERROR_FILE_WRITE;
	}
      }
    }
  }

  fprintf(stdout, "Finished setting up coverage blockmap.\n");

  return SCALPEL_OK;

}

// map carve->start ... carve->stop into a queue of 'fragments' that
// define a carved file in the disk image.  
static void
generateFragments(struct scalpelState *state, Queue * fragments,
		  CarveInfo * carve) {

  unsigned long long curblock, neededbytes =
    carve->stop - carve->start + 1, bytestoskip, morebytes, totalbytes =
    0, curpos;

  Fragment frag;


  init_queue(fragments, sizeof(struct Fragment), TRUE, 0, TRUE);

  if(!state->useCoverageBlockmap) {
    // no translation necessary
    frag.start = carve->start;
    frag.stop = carve->stop;
    add_to_queue(fragments, &frag, 0);
    return;
  }
  else {
    curpos = positionUseCoverageBlockmap(state, carve->start);
    curblock = curpos / state->coverageblocksize;

    while (totalbytes < neededbytes && curblock < state->coveragenumblocks) {

      morebytes = 0;
      bytestoskip = 0;

      // skip covered blocks
      while (curblock < state->coveragenumblocks &&
	     (state->coveragebitmap[curblock / 8] & (1 << (curblock % 8)))) {
	bytestoskip += state->coverageblocksize -
	  curpos % state->coverageblocksize;
	curblock++;
      }

      curpos += bytestoskip;

      // accumulate uncovered blocks in fragment
      while (curblock < state->coveragenumblocks &&
	     ((state->
	       coveragebitmap[curblock / 8] & (1 << (curblock % 8))) == 0)
	     && totalbytes + morebytes < neededbytes) {

	morebytes += state->coverageblocksize -
	  curpos % state->coverageblocksize;

	curblock++;
      }

      // cap size
      if(totalbytes + morebytes > neededbytes) {
	morebytes = neededbytes - totalbytes;
      }

      frag.start = curpos;
      curpos += morebytes;
      frag.stop = curpos - 1;
      totalbytes += morebytes;

      add_to_queue(fragments, &frag, 0);
    }
  }
}


// If the coverage blockmap is used to guide carving, then use the
// coverage blockmap to map a logical index in the disk image (i.e.,
// the index skips covered blocks) to an actual disk image index.  If
// the coverage blockmap isn't being used, just returns the second
// argument.  
//
// ***This function assumes that the 'position' does NOT lie
// within a covered block! ***
static unsigned long long
positionUseCoverageBlockmap(struct scalpelState *state,
			    unsigned long long position) {


  unsigned long long totalbytes = 0, neededbytes = position,
    morebytes, curblock = 0, curpos = 0, bytestoskip;

  if(!state->useCoverageBlockmap) {
    return position;
  }
  else {
    while (totalbytes < neededbytes && curblock < state->coveragenumblocks) {
      morebytes = 0;
      bytestoskip = 0;

      // skip covered blocks
      while (curblock < state->coveragenumblocks &&
	     (state->coveragebitmap[curblock / 8] & (1 << (curblock % 8)))) {
	bytestoskip += state->coverageblocksize -
	  curpos % state->coverageblocksize;
	curblock++;
      }

      curpos += bytestoskip;

      // accumulate uncovered blocks
      while (curblock < state->coveragenumblocks &&
	     ((state->
	       coveragebitmap[curblock / 8] & (1 << (curblock % 8))) == 0)
	     && totalbytes + morebytes < neededbytes) {

	morebytes += state->coverageblocksize -
	  curpos % state->coverageblocksize;
	curblock++;
      }

      // cap size
      if(totalbytes + morebytes > neededbytes) {
	morebytes = neededbytes - totalbytes;
      }

      curpos += morebytes;
      totalbytes += morebytes;
    }

    return curpos;
  }
}



// update the coverage blockmap for a carved file (if appropriate) and write entries into
// the audit log describing the carved file.  If the file is fragmented, then multiple
// lines are written to indicate where the fragments occur. 
static int
auditUpdateCoverageBlockmap(struct scalpelState *state,
			    struct CarveInfo *carve) {

  struct Queue fragments;
  Fragment *frag;
  int err;
  unsigned long long k;

  // If the coverage blockmap used to guide carving, then carve->start and
  // carve->stop may not correspond to addresses in the disk image--the coverage blockmap
  // processing layer in Scalpel may have skipped "in use" blocks.  Transform carve->start
  // and carve->stop into a list of fragments that contain real disk image offsets.
  generateFragments(state, &fragments, carve);

  rewind_queue(&fragments);
  while (!end_of_queue(&fragments)) {
    frag = (Fragment *) pointer_to_current(&fragments);
    fprintf(state->auditFile, "%s", base_name(carve->filename));
#ifdef _WIN32
    fprintf(state->auditFile, "%13I64u\t\t", frag->start);
#else
    fprintf(state->auditFile, "%13llu\t\t", frag->start);
#endif

    fprintf(state->auditFile, "%3s", carve->chopped ? "YES   " : "NO    ");

#ifdef _WIN32
    fprintf(state->auditFile, "%13I64u\t\t", frag->stop - frag->start + 1);
#else
    fprintf(state->auditFile, "%13llu\t\t", frag->stop - frag->start + 1);
#endif

    fprintf(state->auditFile, "%s\n", base_name(state->imagefile));

    fflush(state->auditFile);

    // update coverage blockmap, if appropriate
    if(state->updateCoverageBlockmap) {
      for(k = frag->start / state->coverageblocksize;
	  k <= frag->stop / state->coverageblocksize; k++) {
	if((err = updateCoverageBlockmap(state, k)) != SCALPEL_OK) {
	  destroy_queue(&fragments);
	  return err;
	}
      }
    }
    next_element(&fragments);
  }

  destroy_queue(&fragments);

  return SCALPEL_OK;
}



static int
updateCoverageBlockmap(struct scalpelState *state, unsigned long long block) {

  unsigned int entry;

  if(state->updateCoverageBlockmap) {
    // first entry in file is block size, so seek one unsigned int further
    fseeko(state->coverageblockmap, (block + 1) * sizeof(unsigned int),
	   SEEK_SET);
    if(fread(&entry, sizeof(unsigned int), 1, state->coverageblockmap) != 1) {
      fprintf(stderr, "Error reading coverage blockmap entry!\n");
      fprintf(state->auditFile, "Error reading coverage blockmap entry!\n");
      return SCALPEL_ERROR_FATAL_READ;
    }
    entry++;
    // first entry in file is block size, so seek one unsigned int further 
    fseeko(state->coverageblockmap, (block + 1) * sizeof(unsigned int),
	   SEEK_SET);
    if(fwrite(&entry, sizeof(unsigned int), 1, state->coverageblockmap)
       != 1) {
      fprintf(stderr, "Error writing to coverage blockmap file!\n");
      fprintf(state->auditFile, "Error writing to coverage blockmap file!\n");
      return SCALPEL_ERROR_FILE_WRITE;
    }
  }

  return SCALPEL_OK;
}



static void destroyCoverageMaps(struct scalpelState *state) {

  // free memory associated with coverage bitmap, close coverage blockmap file

  if(state->coveragebitmap) {
    free(state->coveragebitmap);
  }

  if(state->useCoverageBlockmap || state->updateCoverageBlockmap) {
    fclose(state->coverageblockmap);
  }
}


// simple wrapper for fseeko with SEEK_CUR semantics that uses the
// coverage bitmap to skip over covered blocks, IF the coverage
// blockmap is being used.  The offset is adjusted so that covered
// blocks are silently skipped when seeking if the coverage blockmap
// is used, otherwise an fseeko() with an umodified offset is
// performed.
static int
fseeko_use_coverage_map(struct scalpelState *state, FILE * fp, off64_t offset) {

  off64_t currentpos;

  // BUG TEST: revision 5 changed curbloc to unsigned, removed all tests for 
  // curblock >= 0 from each of next three while loops
  // we should put back guards to make sure we don't underflow
  // what did I break? anything? maybe not?

  unsigned long long curblock, bytestoskip, bytestokeep, totalbytes = 0;
  int sign;

  if(state->useCoverageBlockmap) {
    currentpos = ftello(fp);
    sign = (offset > 0 ? 1 : -1);

    curblock = currentpos / state->coverageblocksize;

    while (totalbytes < (offset > 0 ? offset : offset * -1) &&
	   curblock < state->coveragenumblocks) {
      bytestoskip = 0;

      // covered blocks increase offset
      while (curblock < state->coveragenumblocks &&
	     (state->coveragebitmap[curblock / 8] & (1 << (curblock % 8)))) {

	bytestoskip += (state->coverageblocksize -
			currentpos % state->coverageblocksize);
	curblock += sign;
      }

      offset += (bytestoskip * sign);
      currentpos += (bytestoskip * sign);

      bytestokeep = 0;

      // uncovered blocks don't increase offset
      while (curblock < state->coveragenumblocks &&
	     ((state->
	       coveragebitmap[curblock / 8] & (1 << (curblock % 8))) == 0)
	     && totalbytes < (offset > 0 ? offset : offset * -1)) {

	bytestokeep += (state->coverageblocksize -
			currentpos % state->coverageblocksize);

	curblock += sign;
      }

      totalbytes += bytestokeep;
      currentpos += (bytestokeep * sign);
    }
  }

  return fseeko(fp, offset, SEEK_CUR);
}



// simple wrapper for ftello() that uses the coverage bitmap to
// report the current file position *minus* the contribution of
// marked blocks, IF the coverage blockmap is being used.  If a
// coverage blockmap isn't in use, just performs a standard ftello()
// call.
//
// GGRIII:  *** This could use optimization, e.g., use of a pre-computed
// table to avoid walking the coverage bitmap on each call.

static off64_t ftello_use_coverage_map(struct scalpelState *state, FILE * fp) {

  off64_t currentpos, decrease = 0;
  unsigned long long endblock, k;

  currentpos = ftello(fp);

  if(state->useCoverageBlockmap) {
    endblock = currentpos / state->coverageblocksize;

    // covered blocks don't contribute to current file position
    for(k = 0; k <= endblock; k++) {
      if(state->coveragebitmap[k / 8] & (1 << (k % 8))) {
	decrease += state->coverageblocksize;
      }
    }

    if(state->coveragebitmap[endblock / 8] & (1 << (endblock % 8))) {
      decrease += (state->coverageblocksize -
		   currentpos % state->coverageblocksize);
    }

    if(state->modeVerbose && state->useCoverageBlockmap) {
#ifdef _WIN32
      fprintf(stdout,
	      "Coverage map decreased current file position by %I64u bytes.\n",
	      (unsigned long long)decrease);
#else
      fprintf(stdout,
	      "Coverage map decreased current file position by %llu bytes.\n",
	      (unsigned long long)decrease);
#endif
    }
  }

  return currentpos - decrease;
}



// simple wrapper for fread() that uses the coverage bitmap--the read silently
// skips blocks that are marked covered (corresponding bit in coverage
// bitmap is 1)
static size_t
fread_use_coverage_map(struct scalpelState *state, void *ptr,
		       size_t size, size_t nmemb, FILE * stream) {

  unsigned long long curblock, neededbytes = nmemb * size, bytestoskip,
    bytestoread, bytesread, totalbytesread = 0, curpos;
  int shortread;
//  gettimeofday_t readnow, readthen;

//  gettimeofday(&readthen, 0);

  if(state->useCoverageBlockmap) {
    if(state->modeVerbose) {
#ifdef _WIN32
      fprintf(stdout,
	      "Issuing coverage map-based READ, wants %I64u bytes.\n",
	      neededbytes);
#else
      fprintf(stdout,
	      "Issuing coverage map-based READ, wants %llu bytes.\n",
	      neededbytes);
#endif
    }

    curpos = ftello(stream);
    curblock = curpos / state->coverageblocksize;
    shortread = 0;

    while (totalbytesread < neededbytes
	   && curblock < state->coveragenumblocks && !shortread) {
      bytestoread = 0;
      bytestoskip = 0;

      // skip covered blocks
      while (curblock < state->coveragenumblocks &&
	     (state->coveragebitmap[curblock / 8] & (1 << (curblock % 8)))) {
	bytestoskip += (state->coverageblocksize -
			curpos % state->coverageblocksize);
	curblock++;
      }

      curpos += bytestoskip;


      if(state->modeVerbose) {
#ifdef _WIN32
	fprintf(stdout,
		"fread using coverage map to skip %I64u bytes.\n", bytestoskip);
#else
	fprintf(stdout,
		"fread using coverage map to skip %llu bytes.\n", bytestoskip);
#endif
      }

      fseeko(stream, (off64_t) bytestoskip, SEEK_CUR);

      // accumulate uncovered blocks for read
      while (curblock < state->coveragenumblocks &&
	     ((state->
	       coveragebitmap[curblock / 8] & (1 << (curblock % 8))) == 0)
	     && totalbytesread + bytestoread <= neededbytes) {

	bytestoread += (state->coverageblocksize -
			curpos % state->coverageblocksize);

	curblock++;
      }

      // cap read size
      if(totalbytesread + bytestoread > neededbytes) {
	bytestoread = neededbytes - totalbytesread;
      }


      if(state->modeVerbose) {
#ifdef _WIN32
	fprintf(stdout,
		"fread using coverage map found %I64u consecutive bytes.\n",
		bytestoread);
#else
	fprintf(stdout,
		"fread using coverage map found %llu consecutive bytes.\n",
		bytestoread);
#endif
      }

      if((bytesread =
	  fread((char *)ptr + totalbytesread, 1, (size_t) bytestoread,
		stream)) < bytestoread) {
	shortread = 1;
      }

      totalbytesread += bytesread;
      curpos += bytestoread;

      if(state->modeVerbose) {
#ifdef _WIN32
	fprintf(stdout, "fread using coverage map read %I64u bytes.\n",
		bytesread);
#else
	fprintf(stdout, "fread using coverage map read %llu bytes.\n",
		bytesread);
#endif
      }
    }

    if(state->modeVerbose) {
      fprintf(stdout, "Coverage map-based READ complete.\n");
    }

    // conform with fread() semantics by returning # of items read
    return totalbytesread / size;
  }
  else {
    size_t ret = fread(ptr, size, nmemb, stream);
    return ret;
  }
}

#ifdef MULTICORE_THREADING

// threaded header/footer search
static void *threadedFindAll(void *args) {

  int id = ((ThreadFindAllParams *) args)->id;
  char *str;
  size_t length;
  char *startpos;
  long offset;
  char **foundat;
  size_t *foundatlens;
  size_t *table = 0;
  regex_t *regexp = 0;
  int strisRE;
  int casesensitive;
  int nosearchoverlap;
  struct scalpelState *state;

  regmatch_t *match;

  // wait for work
  //  sem_wait(&workavailable[id]);
	
  // need to be holding the workcomplete mutex initially
  pthread_mutex_lock(&workcomplete[id]);
  pthread_mutex_lock(&workavailable[id]);

  while (1) {
    // get args that define current workload
    str = ((ThreadFindAllParams *) args)->str;
    length = ((ThreadFindAllParams *) args)->length;
    startpos = ((ThreadFindAllParams *) args)->startpos;
    offset = ((ThreadFindAllParams *) args)->offset;
    foundat = ((ThreadFindAllParams *) args)->foundat;
    foundatlens = ((ThreadFindAllParams *) args)->foundatlens;
    strisRE = ((ThreadFindAllParams *) args)->strisRE;
    if(strisRE) {
      regexp = ((ThreadFindAllParams *) args)->regex;
    }
    else {
      table = ((ThreadFindAllParams *) args)->table;
    }
    casesensitive = ((ThreadFindAllParams *) args)->casesensitive;
    nosearchoverlap = ((ThreadFindAllParams *) args)->nosearchoverlap;
    state = ((ThreadFindAllParams *) args)->state;

    if(state->modeVerbose) {
      printf("needle search thread # %d awake.\n", id);
    }

    while (startpos) {
      if(!strisRE) {
	startpos = bm_needleinhaystack(str,
				       length,
				       startpos,
				       offset - (long)startpos,
				       table, casesensitive);
      }
      else {
	//printf("Before regexp search, startpos = %p\n", startpos);
	match = re_needleinhaystack(regexp, startpos, offset - (long)startpos);
	if(!match) {
	  startpos = 0;
	}
	else {
	  startpos = match->rm_so + startpos;
	  length = match->rm_eo - match->rm_so;
	  free(match);
	  //printf("After regexp search, startpos = %p\n", startpos);
	}
      }

      if (startpos) {
	// make sure we can accomodate storage of another match...
	if (foundat[MAX_MATCHES_PER_BUFFER] == MAX_MATCHES_PER_BUFFER) {
	  // out of space to store matches--abort with a descriptive error message
	  // can't propagate error up to digImageFile because we're in a thread, 
	  // so just handle it here
	  handleError(state, SCALPEL_ERROR_TOO_MANY_MATCHES);
	}

	// remember match location
	foundat[(long)(foundat[MAX_MATCHES_PER_BUFFER])] = startpos;
	foundatlens[(long)(foundat[MAX_MATCHES_PER_BUFFER])] = length;
	foundat[MAX_MATCHES_PER_BUFFER]++;

	// move past match position.  Foremost 0.69 didn't find overlapping
	// headers/footers.  If you need that behavior, specify "-r" on the
	// command line.  Scalpel's default behavior is to find overlapping
	// headers/footers.

	if(nosearchoverlap) {
	  startpos += length;
	}
	else {
	  startpos++;
	}
      }
    }

    if(state->modeVerbose) {
      printf("needle search thread # %d asleep.\n", id);
    }

    // signal completion of work
    //    sem_post(&workcomplete[id]);
    pthread_mutex_unlock(&workcomplete[id]);

    // wait for more work
    //    sem_wait(&workavailable[id]);
    pthread_mutex_lock(&workavailable[id]);

  }

  return 0;

}

#endif


// Buffers for reading image 
void init_store() {

  // the queues hold pointers to full and empty SIZE_OF_BUFFER read buffers 
  full_readbuf = syncqueue_init("full_readbuf", QUEUELEN);
  empty_readbuf = syncqueue_init("empty_readbuf", QUEUELEN);

  // backing store of actual buffers pointed to above, along with some  
  // necessary bookeeping info
  readbuf_info *readbuf_store;
  if((readbuf_store =
      (readbuf_info *)malloc(QUEUELEN * sizeof(readbuf_info))) == 0) {
    fprintf(stderr, (char *)"malloc %lu failed in streaming reader\n",
	    (unsigned long)QUEUELEN * sizeof(readbuf_info));
  }

  // initialization
  int g;
  for(g = 0; g < QUEUELEN; g++) {
    readbuf_store[g].bytesread = 0;
    readbuf_store[g].beginreadpos = 0;
    readbuf_store[g].readbuf = (char *)malloc(SIZE_OF_BUFFER);


    // put pointer to empty but initialized readbuf in the empty que        
    put(empty_readbuf, (void *)(&readbuf_store[g]));
  }
}


// initialize thread-related data structures for 
// MULTICORE_THREADING model
int init_threading_model(struct scalpelState *state) {

  int i;


#ifdef MULTICORE_THREADING

  printf("Multi-core CPU threading model enabled.\n");
  printf("Initializing thread group data structures.\n");

  // initialize global data structures for threads
  searchthreads = (pthread_t *) malloc(state->specLines * sizeof(pthread_t));
  checkMemoryAllocation(state, searchthreads, __LINE__, __FILE__,
			"searchthreads");
  threadargs =
    (ThreadFindAllParams *) malloc(state->specLines *
				   sizeof(ThreadFindAllParams));
  checkMemoryAllocation(state, threadargs, __LINE__, __FILE__, "args");
  foundat = (char ***)malloc(state->specLines * sizeof(char *));
  checkMemoryAllocation(state, foundat, __LINE__, __FILE__, "foundat");
  foundatlens = (size_t **) malloc(state->specLines * sizeof(size_t));
  checkMemoryAllocation(state, foundatlens, __LINE__, __FILE__, "foundatlens");
  workavailable = (pthread_mutex_t *)malloc(state->specLines * sizeof(pthread_mutex_t));

  checkMemoryAllocation(state, workavailable, __LINE__, __FILE__,
			"workavailable");
  workcomplete = (pthread_mutex_t *)malloc(state->specLines * sizeof(pthread_mutex_t));

  checkMemoryAllocation(state, workcomplete, __LINE__, __FILE__,
			"workcomplete");

  printf("Creating threads...\n");
  for(i = 0; i < state->specLines; i++) {
    foundat[i] = (char **)malloc((MAX_MATCHES_PER_BUFFER + 1) * sizeof(char *));
    checkMemoryAllocation(state, foundat[i], __LINE__, __FILE__, "foundat");
    foundatlens[i] = (size_t *) malloc(MAX_MATCHES_PER_BUFFER * sizeof(size_t));
    checkMemoryAllocation(state, foundatlens[i], __LINE__, __FILE__,
			  "foundatlens");
    foundat[i][MAX_MATCHES_PER_BUFFER] = 0;

    if(pthread_mutex_init(&workavailable[i], 0)) {
      return SCALPEL_ERROR_MUTEX_FAILURE;
    }

    pthread_mutex_lock(&workavailable[i]);

    if(pthread_mutex_init(&workcomplete[i], 0)) {
      return SCALPEL_ERROR_MUTEX_FAILURE;
    }		

    // create a thread in the thread pool; thread will block on workavailable[] 
    // semaphore until there's work to do

    // FIX:  NEED PROPER ERROR CODE/MESSAGE FOR THREAD CREATION FAILURE
    threadargs[i].id = i;	// thread needs to read id before blocking
    if(pthread_create
       (&searchthreads[i], NULL, &threadedFindAll, &threadargs[i])) {
      return SCALPEL_ERROR_PTHREAD_FAILURE;
    }
  }
  printf("Thread creation completed.\n");

#endif

  return 0;
}


// write header/footer database for current image file into the
// Scalpel output directory. No information is written into the
// database for file types without a suffix.  The filename used
// is the current image filename with ".hfd" appended.  The 
// format of the database file is straightforward:
//
// suffix_#1 (string)
// number_of_headers (unsigned long long)
// header_pos_#1 (unsigned long long)
// header_pos_#2 (unsigned long long) 
// ...
// number_of_footers (unsigned long long)
// footer_pos_#1 (unsigned long long)
// footer_pos_#2 (unsigned long long) 
// ...
// suffix_#2 (string)
// number_of_headers (unsigned long long)
// header_pos_#1 (unsigned long long)
// header_pos_#2 (unsigned long long) 
// ...
// number_of_footers (unsigned long long)
// footer_pos_#1 (unsigned long long)
// footer_pos_#2 (unsigned long long) 
// ...
// ...
//
// If state->useCoverageBlockmap, then translation is required to
// produce real disk image addresses for the generated header/footer
// database file, because the Scalpel carving engine isn't aware of
// gaps created by blocks that are covered by previously carved files.

static int writeHeaderFooterDatabase(struct scalpelState *state) {

  FILE *dbfile;
  char fn[MAX_STRING_LENGTH];	// filename for header/footer database
  int needlenum;
  struct SearchSpecLine *currentneedle;
  unsigned long long i;

  // generate unique name for header/footer database
  snprintf(fn, MAX_STRING_LENGTH, "%s/%s.hfd",
	   state->outputdirectory, base_name(state->imagefile));

  if((dbfile = fopen(fn, "w")) == NULL) {
    fprintf(stderr, "Error writing to header/footer database file: %s\n", fn);
    fprintf(state->auditFile,
	    "Error writing to header/footer database file: %s\n", fn);
    return SCALPEL_ERROR_FILE_WRITE;
  }

#ifdef _WIN32
  // set binary mode for Win32
  setmode(fileno(dbfile), O_BINARY);
#endif
#ifdef __linux
  fcntl(fileno(dbfile), F_SETFL, O_LARGEFILE);
#endif

  for(needlenum = 0; needlenum < state->specLines; needlenum++) {
    currentneedle = &(state->SearchSpec[needlenum]);

    if(currentneedle->suffix[0] != SCALPEL_NOEXTENSION) {
      // output current suffix
      if(fprintf(dbfile, "%s\n", currentneedle->suffix) <= 0) {
	fprintf(stderr,
		"Error writing to header/footer database file: %s\n", fn);
	fprintf(state->auditFile,
		"Error writing to header/footer database file: %s\n", fn);
	return SCALPEL_ERROR_FILE_WRITE;
      }

      // # of headers
#ifdef _WIN32
      if(fprintf(dbfile, "%I64u\n", currentneedle->offsets.numheaders)
	 <= 0) {
#else
	if(fprintf(dbfile, "%llu\n", currentneedle->offsets.numheaders) <= 0) {
#endif
	  fprintf(stderr,
		  "Error writing to header/footer database file: %s\n", fn);
	  fprintf(state->auditFile,
		  "Error writing to header/footer database file: %s\n", fn);
	  return SCALPEL_ERROR_FILE_WRITE;
	}

	// all header positions for current suffix
	for(i = 0; i < currentneedle->offsets.numheaders; i++) {
#ifdef _WIN32
	  if(fprintf
	     (dbfile, "%I64u\n",
	      positionUseCoverageBlockmap(state,
					  currentneedle->offsets.
					  headers[i])) <= 0) {
#else
	    if(fprintf
	       (dbfile, "%llu\n",
		positionUseCoverageBlockmap(state,
					    currentneedle->offsets.
					    headers[i])) <= 0) {
#endif
	      fprintf(stderr,
		      "Error writing to header/footer database file: %s\n", fn);
	      fprintf(state->auditFile,
		      "Error writing to header/footer database file: %s\n", fn);
	      return SCALPEL_ERROR_FILE_WRITE;
	    }
	  }

	  // # of footers
#ifdef _WIN32
	  if(fprintf(dbfile, "%I64u\n", currentneedle->offsets.numfooters)
	     <= 0) {
#else
	    if(fprintf(dbfile, "%llu\n", currentneedle->offsets.numfooters) <= 0) {
#endif
	      fprintf(stderr,
		      "Error writing to header/footer database file: %s\n", fn);
	      fprintf(state->auditFile,
		      "Error writing to header/footer database file: %s\n", fn);
	      return SCALPEL_ERROR_FILE_WRITE;
	    }

	    // all footer positions for current suffix
	    for(i = 0; i < currentneedle->offsets.numfooters; i++) {
#ifdef _WIN32
	      if(fprintf
		 (dbfile, "%I64u\n",
		  positionUseCoverageBlockmap(state,
					      currentneedle->offsets.
					      footers[i])) <= 0) {
#else
		if(fprintf
		   (dbfile, "%llu\n",
		    positionUseCoverageBlockmap(state,
						currentneedle->offsets.
						footers[i])) <= 0) {
#endif
		  fprintf(stderr,
			  "Error writing to header/footer database file: %s\n", fn);
		  fprintf(state->auditFile,
			  "Error writing to header/footer database file: %s\n", fn);
		  return SCALPEL_ERROR_FILE_WRITE;
		}
	      }
	    }
	  }
	  fclose(dbfile);
	  return SCALPEL_OK;
	}
	
