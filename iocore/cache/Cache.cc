/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */


#include "P_Cache.h"

// Cache Inspector and State Pages
#ifdef NON_MODULAR
#include "P_CacheTest.h"
#include "StatPages.h"
#endif

#include "I_Layout.h"

#ifdef HTTP_CACHE
#include "HttpTransactCache.h"
#include "HttpSM.h"
#include "HttpCacheSM.h"
#include "InkAPIInternal.h"
#endif

// Compilation Options

#define USELESS_REENABLES       // allow them for now
// #define VERIFY_JTEST_DATA

#define DOCACHE_CLEAR_DYN_STAT(x) \
do { \
	RecSetRawStatSum(rsb, x, 0); \
	RecSetRawStatCount(rsb, x, 0); \
} while (0);

// Configuration

int64_t cache_config_ram_cache_size = AUTO_SIZE_RAM_CACHE;
int cache_config_ram_cache_algorithm = 0;
int cache_config_ram_cache_compress = 0;
int cache_config_ram_cache_compress_percent = 90;
int cache_config_http_max_alts = 3;
int cache_config_dir_sync_frequency = 60;
int cache_config_permit_pinning = 0;
int cache_config_vary_on_user_agent = 0;
int cache_config_select_alternate = 1;
int cache_config_max_doc_size = 0;
int cache_config_min_average_object_size = ESTIMATED_OBJECT_SIZE;
int64_t cache_config_ram_cache_cutoff = AGG_SIZE;
int cache_config_max_disk_errors = 5;
#ifdef HIT_EVACUATE
int cache_config_hit_evacuate_percent = 10;
int cache_config_hit_evacuate_size_limit = 0;
#endif
int cache_config_force_sector_size = 0;
int cache_config_target_fragment_size = DEFAULT_TARGET_FRAGMENT_SIZE;
int cache_config_agg_write_backlog = AGG_SIZE * 2;
int cache_config_enable_checksum = 0;
int cache_config_alt_rewrite_max_size = 4096;
int cache_config_read_while_writer = 0;
char cache_system_config_directory[PATH_NAME_MAX + 1];
int cache_config_mutex_retry_delay = 2;

// Globals

RecRawStatBlock *cache_rsb = NULL;
Cache *theStreamCache = 0;
Cache *theCache = 0;
CacheDisk **gdisks = NULL;
int gndisks = 0;
static volatile int initialize_disk = 0;
Cache *caches[NUM_CACHE_FRAG_TYPES] = { 0 };
CacheSync *cacheDirSync = 0;
Store theCacheStore;
volatile int CacheProcessor::initialized = CACHE_INITIALIZING;
volatile uint32_t CacheProcessor::cache_ready = 0;
volatile int CacheProcessor::start_done = 0;
int CacheProcessor::clear = 0;
int CacheProcessor::fix = 0;
int CacheProcessor::start_internal_flags = 0;
int CacheProcessor::auto_clear_flag = 0;
CacheProcessor cacheProcessor;
Vol **gvol = NULL;
volatile int gnvol = 0;
ClassAllocator<CacheVC> cacheVConnectionAllocator("cacheVConnection");
ClassAllocator<EvacuationBlock> evacuationBlockAllocator("evacuationBlock");
ClassAllocator<CacheRemoveCont> cacheRemoveContAllocator("cacheRemoveCont");
ClassAllocator<EvacuationKey> evacuationKeyAllocator("evacuationKey");
int CacheVC::size_to_init = -1;
CacheKey zero_key(0, 0);

void verify_cache_api() {
  ink_assert((int)TS_EVENT_CACHE_OPEN_READ == (int)CACHE_EVENT_OPEN_READ);
  ink_assert((int)TS_EVENT_CACHE_OPEN_READ_FAILED == (int)CACHE_EVENT_OPEN_READ_FAILED);
  ink_assert((int)TS_EVENT_CACHE_OPEN_WRITE == (int)CACHE_EVENT_OPEN_WRITE);
  ink_assert((int)TS_EVENT_CACHE_OPEN_WRITE_FAILED == (int)CACHE_EVENT_OPEN_WRITE_FAILED);
  ink_assert((int)TS_EVENT_CACHE_REMOVE == (int)CACHE_EVENT_REMOVE);
  ink_assert((int)TS_EVENT_CACHE_REMOVE_FAILED == (int)CACHE_EVENT_REMOVE_FAILED);
  ink_assert((int)TS_EVENT_CACHE_SCAN == (int)CACHE_EVENT_SCAN);
  ink_assert((int)TS_EVENT_CACHE_SCAN_FAILED == (int)CACHE_EVENT_SCAN_FAILED);
  ink_assert((int)TS_EVENT_CACHE_SCAN_OBJECT == (int)CACHE_EVENT_SCAN_OBJECT);
  ink_assert((int)TS_EVENT_CACHE_SCAN_OPERATION_BLOCKED == (int)CACHE_EVENT_SCAN_OPERATION_BLOCKED);
  ink_assert((int)TS_EVENT_CACHE_SCAN_OPERATION_FAILED == (int)CACHE_EVENT_SCAN_OPERATION_FAILED);
  ink_assert((int)TS_EVENT_CACHE_SCAN_DONE == (int)CACHE_EVENT_SCAN_DONE);
}

struct VolInitInfo
{
  off_t recover_pos;
  AIOCallbackInternal vol_aio[4];
  char *vol_h_f;

  VolInitInfo()
  {
    recover_pos = 0;
    if ((vol_h_f = (char *) valloc(4 * STORE_BLOCK_SIZE)) != NULL)
      memset(vol_h_f, 0, 4 * STORE_BLOCK_SIZE);
  }
  ~VolInitInfo()
  {
    for (int i = 0; i < 4; i++) {
      vol_aio[i].action = NULL;
      vol_aio[i].mutex.clear();
    }
    free(vol_h_f);
  }
};

void cplist_init();
static void cplist_update();
int cplist_reconfigure();
static int create_volume(int volume_number, off_t size_in_blocks, int scheme, CacheVol *cp);
static void rebuild_host_table(Cache *cache);
void register_cache_stats(RecRawStatBlock *rsb, const char *prefix);

Queue<CacheVol> cp_list;
int cp_list_len = 0;
ConfigVolumes config_volumes;

#if TS_HAS_TESTS
void force_link_CacheTestCaller() {
  force_link_CacheTest();
}
#endif

int64_t
cache_bytes_used(int volume)
{
  uint64_t used = 0;
  int start = 0; // These defaults are for volume 0, or volume 1 with volume.config
  int end = 1;

  if (-1 == volume) {
    end = gnvol;
  } else if (volume > 1) { // Special case when volume.config is used
    start = volume - 1;
    end = volume;
  }
  
  for (int i = start; i < end; i++) {
    if (!DISK_BAD(gvol[i]->disk)) {
      if (!gvol[i]->header->cycle)
          used += gvol[i]->header->write_pos - gvol[i]->start;
      else
          used += gvol[i]->len - vol_dirlen(gvol[i]) - EVACUATION_SIZE;
    }
  }
  return used;
}

int
cache_stats_bytes_used_cb(const char *name, RecDataT data_type, RecData *data, RecRawStatBlock *rsb, int id)
{
  int volume = -1;

  NOWARN_UNUSED(data_type);
  NOWARN_UNUSED(data);

  // Well, there's no way to pass along the volume ID, so extracting it from the stat name.
  if (0 == strncmp(name+20, "volume_", 10))
    volume = strtol(name+30, NULL, 10);

  if (cacheProcessor.initialized == CACHE_INITIALIZED)
    RecSetGlobalRawStatSum(rsb, id, cache_bytes_used(volume));

  RecRawStatSyncSum(name, data_type, data, rsb, id);

  return 1;
}

static int
validate_rww(int new_value)
{
  if (new_value) {
    float http_bg_fill;

    IOCORE_ReadConfigFloat(http_bg_fill, "proxy.config.http.background_fill_completed_threshold");
    if (http_bg_fill > 0.0) {
      Note("to enable reading while writing a document, %s should be 0.0: read while writing disabled",
           "proxy.config.http.background_fill_completed_threshold");
      return 0;
    }
    if (cache_config_max_doc_size > 0) {
      Note("to enable reading while writing a document, %s should be 0: read while writing disabled",
           "proxy.config.cache.max_doc_size");
      return 0;
    }
    return new_value;
  }
  return 0;
}

static int
update_cache_config(const char *name, RecDataT data_type, RecData data, void *cookie)
{
  NOWARN_UNUSED(name);
  NOWARN_UNUSED(data_type);
  NOWARN_UNUSED(cookie);

  volatile int new_value = validate_rww(data.rec_int);
  cache_config_read_while_writer = new_value;

  return 0;
}

CacheVC::CacheVC():alternate_index(CACHE_ALT_INDEX_DEFAULT)
{
  size_to_init = sizeof(CacheVC) - (size_t) & ((CacheVC *) 0)->vio;
  memset((char *) &vio, 0, size_to_init);
  // the constructor does a memset() on the members that need to be initialized
  //coverity[uninit_member]
}

VIO *
CacheVC::do_io_read(Continuation *c, int64_t nbytes, MIOBuffer *abuf)
{
  ink_assert(vio.op == VIO::READ);
  vio.buffer.writer_for(abuf);
  vio.set_continuation(c);
  vio.ndone = 0;
  vio.nbytes = nbytes;
  vio.vc_server = this;
  ink_assert(c->mutex->thread_holding);
  if (!trigger && !recursive)
    trigger = c->mutex->thread_holding->schedule_imm_local(this);
  return &vio;
}

VIO *
CacheVC::do_io_pread(Continuation *c, int64_t nbytes, MIOBuffer *abuf, int64_t offset)
{
  NOWARN_UNUSED(nbytes);
  ink_assert(vio.op == VIO::READ);
  vio.buffer.writer_for(abuf);
  vio.set_continuation(c);
  vio.ndone = 0;
  vio.nbytes = nbytes;
  vio.vc_server = this;
  seek_to = offset;
  ink_assert(c->mutex->thread_holding);
  if (!trigger && !recursive)
    trigger = c->mutex->thread_holding->schedule_imm_local(this);
  return &vio;
}

VIO *
CacheVC::do_io_write(Continuation *c, int64_t nbytes, IOBufferReader *abuf, bool owner)
{
  ink_assert(vio.op == VIO::WRITE);
  ink_assert(!owner);
  vio.buffer.reader_for(abuf);
  vio.set_continuation(c);
  vio.ndone = 0;
  vio.nbytes = nbytes;
  vio.vc_server = this;
  ink_assert(c->mutex->thread_holding);
  if (!trigger && !recursive)
    trigger = c->mutex->thread_holding->schedule_imm_local(this);
  return &vio;
}

void
CacheVC::do_io_close(int alerrno)
{
  ink_debug_assert(mutex->thread_holding == this_ethread());
  int previous_closed = closed;
  closed = (alerrno == -1) ? 1 : -1;    // Stupid default arguments
  DDebug("cache_close", "do_io_close %p %d %d", this, alerrno, closed);
  if (!previous_closed && !recursive)
    die();
}

void
CacheVC::reenable(VIO *avio)
{
  DDebug("cache_reenable", "reenable %p", this);
  (void) avio;
  ink_assert(avio->mutex->thread_holding);
  if (!trigger) {
#ifndef USELESS_REENABLES
    if (vio.op == VIO::READ) {
      if (vio.buffer.mbuf->max_read_avail() > vio.buffer.writer()->water_mark)
        ink_assert(!"useless reenable of cache read");
    } else if (!vio.buffer.reader()->read_avail())
      ink_assert(!"useless reenable of cache write");
#endif
    trigger = avio->mutex->thread_holding->schedule_imm_local(this);
  }
}

void
CacheVC::reenable_re(VIO *avio)
{
  DDebug("cache_reenable", "reenable_re %p", this);
  (void) avio;
  ink_assert(avio->mutex->thread_holding);
  if (!trigger) {
    if (!is_io_in_progress() && !recursive) {
      handleEvent(EVENT_NONE, (void *) 0);
    } else
      trigger = avio->mutex->thread_holding->schedule_imm_local(this);
  }
}

bool
CacheVC::get_data(int i, void *data)
{
  switch (i) {
  case CACHE_DATA_SIZE:
    *((int *) data) = doc_len;
    return true;
#ifdef HTTP_CACHE
  case CACHE_DATA_HTTP_INFO:
    *((CacheHTTPInfo **) data) = &alternate;
    return true;
#endif
  case CACHE_DATA_RAM_CACHE_HIT_FLAG:
    *((int *) data) = !f.not_from_ram_cache;
    return true;
  default:
    break;
  }
  return false;
}

int64_t
CacheVC::get_object_size()
{
  return ((CacheVC *) this)->doc_len;
}

bool CacheVC::set_data(int i, void *data)
{
  (void) i;
  (void) data;
  ink_debug_assert(!"CacheVC::set_data should not be called!");
  return true;
}

#ifdef HTTP_CACHE
void
CacheVC::get_http_info(CacheHTTPInfo ** ainfo)
{
  *ainfo = &((CacheVC *) this)->alternate;
}

// set_http_info must be called before do_io_write
// cluster vc does an optimization where it calls do_io_write() before
// calling set_http_info(), but it guarantees that the info will
// be set before transferring any bytes
void
CacheVC::set_http_info(CacheHTTPInfo *ainfo)
{
  ink_assert(!total_len);
  if (f.update) {
    ainfo->object_key_set(update_key);
    ainfo->object_size_set(update_len);
  } else {
    ainfo->object_key_set(earliest_key);
    // don't know the total len yet
  }
  alternate.copy_shallow(ainfo);
  ainfo->clear();
}
#endif

bool CacheVC::set_pin_in_cache(time_t time_pin)
{
  if (total_len) {
    ink_assert(!"should Pin the document before writing");
    return false;
  }
  if (vio.op != VIO::WRITE) {
    ink_assert(!"Pinning only allowed while writing objects to the cache");
    return false;
  }
  pin_in_cache = time_pin;
  return true;
}

bool CacheVC::set_disk_io_priority(int priority)
{

  ink_assert(priority >= AIO_LOWEST_PRIORITY);
  io.aiocb.aio_reqprio = priority;
  return true;
}

time_t CacheVC::get_pin_in_cache()
{
  return pin_in_cache;
}

int
CacheVC::get_disk_io_priority()
{
  return io.aiocb.aio_reqprio;
}

int
Vol::begin_read(CacheVC *cont)
{
  ink_debug_assert(cont->mutex->thread_holding == this_ethread());
  ink_debug_assert(mutex->thread_holding == this_ethread());
#ifdef CACHE_STAT_PAGES
  ink_assert(!cont->stat_link.next && !cont->stat_link.prev);
  stat_cache_vcs.enqueue(cont, cont->stat_link);
#endif
  // no need for evacuation as the entire document is already in memory
  if (cont->f.single_fragment)
    return 0;
  int i = dir_evac_bucket(&cont->earliest_dir);
  EvacuationBlock *b;
  for (b = evacuate[i].head; b; b = b->link.next) {
    if (dir_offset(&b->dir) != dir_offset(&cont->earliest_dir))
      continue;
    if (b->readers)
      b->readers = b->readers + 1;
    return 0;
  }
  // we don't actually need to preserve this block as it is already in
  // memory, but this is easier, and evacuations are rare
  EThread *t = cont->mutex->thread_holding;
  b = new_EvacuationBlock(t);
  b->readers = 1;
  b->dir = cont->earliest_dir;
  b->evac_frags.key = cont->earliest_key;
  evacuate[i].push(b);
  return 1;
}

int
Vol::close_read(CacheVC *cont)
{
  EThread *t = cont->mutex->thread_holding;
  ink_debug_assert(t == this_ethread());
  ink_debug_assert(t == mutex->thread_holding);
  if (dir_is_empty(&cont->earliest_dir))
    return 1;
  int i = dir_evac_bucket(&cont->earliest_dir);
  EvacuationBlock *b;
  for (b = evacuate[i].head; b;) {
    EvacuationBlock *next = b->link.next;
    if (dir_offset(&b->dir) != dir_offset(&cont->earliest_dir)) {
      b = next;
      continue;
    }
    if (b->readers && !--b->readers) {
      evacuate[i].remove(b);
      free_EvacuationBlock(b, t);
      break;
    }
    b = next;
  }
#ifdef CACHE_STAT_PAGES
  stat_cache_vcs.remove(cont, cont->stat_link);
  ink_assert(!cont->stat_link.next && !cont->stat_link.prev);
#endif
  return 1;
}

// Cache Processor

int
CacheProcessor::start(int)
{
  return start_internal(0);
}

int
CacheProcessor::start_internal(int flags)
{
#ifdef NON_MODULAR
  verify_cache_api();
#endif

  start_internal_flags = flags;
  clear = !!(flags & PROCESSOR_RECONFIGURE) || auto_clear_flag;
  fix = !!(flags & PROCESSOR_FIX);
  int i;
  start_done = 0;
  int diskok = 1;

  /* read the config file and create the data structures corresponding
     to the file */
  gndisks = theCacheStore.n_disks;
  gdisks = (CacheDisk **) xmalloc(gndisks * sizeof(CacheDisk *));

  gndisks = 0;
  ink_aio_set_callback(new AIO_Callback_handler());
  Span *sd;
  config_volumes.read_config_file();
  for (i = 0; i < theCacheStore.n_disks; i++) {
    sd = theCacheStore.disk[i];
    char path[PATH_MAX];
    int opts = O_RDWR;
    ink_strlcpy(path, sd->pathname, sizeof(path));
    if (!sd->file_pathname) {
#if !defined(_WIN32)
      if (config_volumes.num_http_volumes && config_volumes.num_stream_volumes) {
        Warning("It is suggested that you use raw disks if streaming and http are in the same cache");
      }
#endif
      ink_strlcat(path, "/cache.db", sizeof(path));
      opts |= O_CREAT;
    }
    opts |= _O_ATTRIB_OVERLAPPED;
#ifdef O_DIRECT
    opts |= O_DIRECT;
#endif
#ifdef O_DSYNC
    opts |= O_DSYNC;
#endif

    int fd = open(path, opts, 0644);
    int blocks = sd->blocks;
    if (fd > 0) {
#if defined (_WIN32)
      aio_completion_port.register_handle((void *) fd, 0);
#endif
      if (!sd->file_pathname) {
        if (ftruncate(fd, ((uint64_t) blocks) * STORE_BLOCK_SIZE) < 0) {
          Warning("unable to truncate cache file '%s' to %d blocks", path, blocks);
          diskok = 0;
#if defined(_WIN32)
          /* We can do a specific check for FAT32 systems on NT,
           * to print a specific warning */
          if ((((uint64_t) blocks) * STORE_BLOCK_SIZE) > (1 << 32)) {
            Warning("If you are using a FAT32 file system, please ensure that cachesize"
                    "specified in storage.config, does not exceed 4GB!. ");
          }
#endif
        }
      }
      if (diskok) {
        gdisks[gndisks] = NEW(new CacheDisk());
        Debug("cache_hosting", "Disk: %d, blocks: %d", gndisks, blocks);
        int sector_size = sd->hw_sector_size;
        if (sector_size < cache_config_force_sector_size)
          sector_size = cache_config_force_sector_size;
        if (sd->hw_sector_size <= 0 || sector_size > STORE_BLOCK_SIZE) {
          Warning("bad hardware sector size %d, resetting to %d", sector_size, STORE_BLOCK_SIZE);
          sector_size = STORE_BLOCK_SIZE;
        }
        off_t skip = ROUND_TO_STORE_BLOCK((sd->offset < START_POS ? START_POS + sd->alignment : sd->offset));
        blocks = blocks - ROUND_TO_STORE_BLOCK(sd->offset + skip);
        gdisks[gndisks]->open(path, blocks, skip, sector_size, fd, clear);
        gndisks++;
      }
    } else
      Warning("cache unable to open '%s': %s", path, strerror(errno));
  }

  if (gndisks == 0) {
    Warning("unable to open cache disk(s): Cache Disabled\n");
    return -1;
  }
  start_done = 1;

  return 0;
}

void
CacheProcessor::diskInitialized()
{
  ink_atomic_increment(&initialize_disk, 1);
  int bad_disks = 0;
  int res = 0;
  if (initialize_disk == gndisks) {

    int i;
    for (i = 0; i < gndisks; i++) {
      if (DISK_BAD(gdisks[i]))
        bad_disks++;
    }

    if (bad_disks != 0) {
      // create a new array
      CacheDisk **p_good_disks;
      if ((gndisks - bad_disks) > 0)
        p_good_disks = (CacheDisk **) xmalloc((gndisks - bad_disks) * sizeof(CacheDisk *));
      else
        p_good_disks = 0;

      int insert_at = 0;
      for (i = 0; i < gndisks; i++) {
        if (DISK_BAD(gdisks[i])) {
          delete gdisks[i];
          continue;
        }
        if (p_good_disks != NULL) {
          p_good_disks[insert_at++] = gdisks[i];
        }
      }
      xfree(gdisks);
      gdisks = p_good_disks;
      gndisks = gndisks - bad_disks;
    }

    /* create the cachevol list only if num volumes are greater
       than 0. */
    if (config_volumes.num_volumes == 0) {
      res = cplist_reconfigure();
      /* if no volumes, default to just an http cache */
    } else {
      // else
      /* create the cachevol list. */
      cplist_init();
      /* now change the cachevol list based on the config file */
      res = cplist_reconfigure();
    }

    if (res == -1) {
      /* problems initializing the volume.config. Punt */
      gnvol = 0;
      cacheInitialized();
      return;
    } else {
      CacheVol *cp = cp_list.head;
      for (; cp; cp = cp->link.next) {
        cp->vol_rsb = RecAllocateRawStatBlock((int) cache_stat_count);
        char vol_stat_str_prefix[256];
        snprintf(vol_stat_str_prefix, sizeof(vol_stat_str_prefix), "proxy.process.cache.volume_%d", cp->vol_number);
        register_cache_stats(cp->vol_rsb, vol_stat_str_prefix);
      }
    }

    gvol = (Vol **) xmalloc(gnvol * sizeof(Vol *));
    memset(gvol, 0, gnvol * sizeof(Vol *));
    gnvol = 0;
    for (i = 0; i < gndisks; i++) {
      CacheDisk *d = gdisks[i];
      if (is_debug_tag_set("cache_hosting")) {
        int j;
        Debug("cache_hosting", "Disk: %d: Vol Blocks: %ld: Free space: %ld",
              i, d->header->num_diskvol_blks, d->free_space);
        for (j = 0; j < (int) d->header->num_volumes; j++) {
          Debug("cache_hosting", "\tVol: %d Size: %d", d->disk_vols[j]->vol_number, d->disk_vols[j]->size);
        }
        for (j = 0; j < (int) d->header->num_diskvol_blks; j++) {
          Debug("cache_hosting", "\tBlock No: %d Size: %d Free: %d",
                d->header->vol_info[j].number, d->header->vol_info[j].len, d->header->vol_info[j].free);
        }
      }
      d->sync();
    }
    if (config_volumes.num_volumes == 0) {
      theCache = NEW(new Cache());
      theCache->scheme = CACHE_HTTP_TYPE;
      theCache->open(clear, fix);
      return;
    }
    if (config_volumes.num_http_volumes != 0) {
      theCache = NEW(new Cache());
      theCache->scheme = CACHE_HTTP_TYPE;
      theCache->open(clear, fix);
    }

    if (config_volumes.num_stream_volumes != 0) {
      theStreamCache = NEW(new Cache());
      theStreamCache->scheme = CACHE_RTSP_TYPE;
      theStreamCache->open(clear, fix);
    }

  }
}

void
CacheProcessor::cacheInitialized()
{
  int i;

  if ((theCache && (theCache->ready == CACHE_INITIALIZING)) ||
      (theStreamCache && (theStreamCache->ready == CACHE_INITIALIZING)))
    return;
  int caches_ready = 0;
  int cache_init_ok = 0;
  /* allocate ram size in proportion to the disk space the
     volume accupies */
  int64_t total_size = 0;
  uint64_t total_cache_bytes = 0;
  uint64_t total_direntries = 0;
  uint64_t used_direntries = 0;
  uint64_t vol_total_cache_bytes = 0;
  uint64_t vol_total_direntries = 0;
  uint64_t vol_used_direntries = 0;
  Vol *vol;

  ProxyMutex *mutex = this_ethread()->mutex;

  if (theCache) {
    total_size += theCache->cache_size;
    Debug("cache_init", "CacheProcessor::cacheInitialized - theCache, total_size = %" PRId64 " = %" PRId64 " MB",
          total_size, total_size / ((1024 * 1024) / STORE_BLOCK_SIZE));
  }
  if (theStreamCache) {
    total_size += theStreamCache->cache_size;
    Debug("cache_init", "CacheProcessor::cacheInitialized - theStreamCache, total_size = %" PRId64 " = %" PRId64 " MB",
          total_size, total_size / ((1024 * 1024) / STORE_BLOCK_SIZE));
  }

  if (theCache) {
    if (theCache->ready == CACHE_INIT_FAILED) {
      Debug("cache_init", "CacheProcessor::cacheInitialized - failed to initialize the cache for http: cache disabled");
      Warning("failed to initialize the cache for http: cache disabled\n");
    } else {
      caches_ready = caches_ready | (1 << CACHE_FRAG_TYPE_HTTP);
      caches_ready = caches_ready | (1 << CACHE_FRAG_TYPE_NONE);
      caches[CACHE_FRAG_TYPE_HTTP] = theCache;
      caches[CACHE_FRAG_TYPE_NONE] = theCache;
    }
  }
  if (theStreamCache) {
    if (theStreamCache->ready == CACHE_INIT_FAILED) {
      Debug("cache_init",
            "CacheProcessor::cacheInitialized - failed to initialize the cache for streaming: cache disabled");
      Warning("failed to initialize the cache for streaming: cache disabled\n");
    } else {
      caches_ready = caches_ready | (1 << CACHE_FRAG_TYPE_RTSP);
      caches[CACHE_FRAG_TYPE_RTSP] = theStreamCache;
    }
  }

  if (caches_ready) {
    Debug("cache_init", "CacheProcessor::cacheInitialized - caches_ready=0x%0X, gnvol=%d",
          (unsigned int) caches_ready, gnvol);
    int64_t ram_cache_bytes = 0;
    if (gnvol) {
      for (i = 0; i < gnvol; i++) {
        switch (cache_config_ram_cache_algorithm) {
          default:
          case RAM_CACHE_ALGORITHM_CLFUS:
            gvol[i]->ram_cache = new_RamCacheCLFUS();
            break;
          case RAM_CACHE_ALGORITHM_LRU:
            gvol[i]->ram_cache = new_RamCacheLRU();
            break;
        }
      }
      if (cache_config_ram_cache_size == AUTO_SIZE_RAM_CACHE) {
        Debug("cache_init", "CacheProcessor::cacheInitialized - cache_config_ram_cache_size == AUTO_SIZE_RAM_CACHE");
        for (i = 0; i < gnvol; i++) {
          vol = gvol[i];
          gvol[i]->ram_cache->init(vol_dirlen(vol), vol);
          ram_cache_bytes += vol_dirlen(gvol[i]);
          Debug("cache_init", "CacheProcessor::cacheInitialized - ram_cache_bytes = %" PRId64 " = %" PRId64 "Mb",
                ram_cache_bytes, ram_cache_bytes / (1024 * 1024));
          /*
             CACHE_VOL_SUM_DYN_STAT(cache_ram_cache_bytes_total_stat,
             (int64_t)vol_dirlen(gvol[i]));
           */
          RecSetGlobalRawStatSum(vol->cache_vol->vol_rsb,
                                 cache_ram_cache_bytes_total_stat, (int64_t) vol_dirlen(gvol[i]));
          vol_total_cache_bytes = gvol[i]->len - vol_dirlen(gvol[i]);
          total_cache_bytes += vol_total_cache_bytes;
          Debug("cache_init", "CacheProcessor::cacheInitialized - total_cache_bytes = %" PRId64 " = %" PRId64 "Mb",
                total_cache_bytes, total_cache_bytes / (1024 * 1024));

          CACHE_VOL_SUM_DYN_STAT(cache_bytes_total_stat, vol_total_cache_bytes);


          vol_total_direntries = gvol[i]->buckets * gvol[i]->segments * DIR_DEPTH;
          total_direntries += vol_total_direntries;
          CACHE_VOL_SUM_DYN_STAT(cache_direntries_total_stat, vol_total_direntries);


          vol_used_direntries = dir_entries_used(gvol[i]);
          CACHE_VOL_SUM_DYN_STAT(cache_direntries_used_stat, vol_used_direntries);
          used_direntries += vol_used_direntries;
        }

      } else {
        Debug("cache_init", "CacheProcessor::cacheInitialized - %" PRId64 " != AUTO_SIZE_RAM_CACHE",
              cache_config_ram_cache_size);
        int64_t http_ram_cache_size =
          (theCache) ? (int64_t) (((double) theCache->cache_size / total_size) * cache_config_ram_cache_size) : 0;
        Debug("cache_init", "CacheProcessor::cacheInitialized - http_ram_cache_size = %" PRId64 " = %" PRId64 "Mb",
              http_ram_cache_size, http_ram_cache_size / (1024 * 1024));
        int64_t stream_ram_cache_size = cache_config_ram_cache_size - http_ram_cache_size;
        Debug("cache_init", "CacheProcessor::cacheInitialized - stream_ram_cache_size = %" PRId64 " = %" PRId64 "Mb",
              stream_ram_cache_size, stream_ram_cache_size / (1024 * 1024));

        // Dump some ram_cache size information in debug mode.
        Debug("ram_cache", "config: size = %" PRId64 ", cutoff = %" PRId64 "",
              cache_config_ram_cache_size, cache_config_ram_cache_cutoff);

        for (i = 0; i < gnvol; i++) {
          vol = gvol[i];
          double factor;
          if (gvol[i]->cache == theCache) {
            factor = (double) (int64_t) (gvol[i]->len >> STORE_BLOCK_SHIFT) / (int64_t) theCache->cache_size;
            Debug("cache_init", "CacheProcessor::cacheInitialized - factor = %f", factor);
            gvol[i]->ram_cache->init((int64_t) (http_ram_cache_size * factor), vol);
            ram_cache_bytes += (int64_t) (http_ram_cache_size * factor);
            CACHE_VOL_SUM_DYN_STAT(cache_ram_cache_bytes_total_stat, (int64_t) (http_ram_cache_size * factor));
          } else {
            factor = (double) (int64_t) (gvol[i]->len >> STORE_BLOCK_SHIFT) / (int64_t) theStreamCache->cache_size;
            Debug("cache_init", "CacheProcessor::cacheInitialized - factor = %f", factor);
            gvol[i]->ram_cache->init((int64_t) (stream_ram_cache_size * factor), vol);
            ram_cache_bytes += (int64_t) (stream_ram_cache_size * factor);
            CACHE_VOL_SUM_DYN_STAT(cache_ram_cache_bytes_total_stat, (int64_t) (stream_ram_cache_size * factor));
          }
          Debug("cache_init", "CacheProcessor::cacheInitialized[%d] - ram_cache_bytes = %" PRId64 " = %" PRId64 "Mb",
                i, ram_cache_bytes, ram_cache_bytes / (1024 * 1024));

          vol_total_cache_bytes = gvol[i]->len - vol_dirlen(gvol[i]);
          total_cache_bytes += vol_total_cache_bytes;
          CACHE_VOL_SUM_DYN_STAT(cache_bytes_total_stat, vol_total_cache_bytes);
          Debug("cache_init", "CacheProcessor::cacheInitialized - total_cache_bytes = %" PRId64 " = %" PRId64 "Mb",
                total_cache_bytes, total_cache_bytes / (1024 * 1024));

          vol_total_direntries = gvol[i]->buckets * gvol[i]->segments * DIR_DEPTH;
          total_direntries += vol_total_direntries;
          CACHE_VOL_SUM_DYN_STAT(cache_direntries_total_stat, vol_total_direntries);


          vol_used_direntries = dir_entries_used(gvol[i]);
          CACHE_VOL_SUM_DYN_STAT(cache_direntries_used_stat, vol_used_direntries);
          used_direntries += vol_used_direntries;

        }
      }
      switch (cache_config_ram_cache_compress) {
        default:
          Fatal("unknown RAM cache compression type: %d", cache_config_ram_cache_compress);
        case CACHE_COMPRESSION_NONE: 
        case CACHE_COMPRESSION_FASTLZ:
          break;
        case CACHE_COMPRESSION_LIBZ:
#if ! TS_HAS_LIBZ
          Fatal("libz not available for RAM cache compression");
#endif
          break;
        case CACHE_COMPRESSION_LIBLZMA:
#if ! TS_HAS_LZMA
          Fatal("lzma not available for RAM cache compression");
#endif
          break;
      }

      GLOBAL_CACHE_SET_DYN_STAT(cache_ram_cache_bytes_total_stat, ram_cache_bytes);
      GLOBAL_CACHE_SET_DYN_STAT(cache_bytes_total_stat, total_cache_bytes);
      GLOBAL_CACHE_SET_DYN_STAT(cache_direntries_total_stat, total_direntries);
      GLOBAL_CACHE_SET_DYN_STAT(cache_direntries_used_stat, used_direntries);
      dir_sync_init();
      cache_init_ok = 1;
    } else
      Warning("cache unable to open any vols, disabled");
  }
  if (cache_init_ok) {
    // Initialize virtual cache
    CacheProcessor::initialized = CACHE_INITIALIZED;
    CacheProcessor::cache_ready = caches_ready;
    Note("cache enabled");
#ifdef CLUSTER_CACHE
    if (!(start_internal_flags & PROCESSOR_RECONFIGURE)) {
      CacheContinuation::init();
      clusterProcessor.start();
    }
#endif
  } else {
    CacheProcessor::initialized = CACHE_INIT_FAILED;
    Note("cache disabled");
  }
}

void
CacheProcessor::stop()
{
}

int
CacheProcessor::dir_check(bool afix)
{
  for (int i = 0; i < gnvol; i++)
    gvol[i]->dir_check(afix);
  return 0;
}

int
CacheProcessor::db_check(bool afix)
{
  for (int i = 0; i < gnvol; i++)
    gvol[i]->db_check(afix);
  return 0;
}

int
Vol::db_check(bool fix)
{
  (void) fix;
  char tt[256];
  printf("    Data for [%s]\n", hash_id);
  printf("        Length:          %" PRIu64 "\n", (uint64_t)len);
  printf("        Write Position:  %" PRIu64 "\n", (uint64_t) (header->write_pos - skip));
  printf("        Phase:           %d\n", (int)!!header->phase);
  ink_ctime_r(&header->create_time, tt);
  tt[strlen(tt) - 1] = 0;
  printf("        Create Time:     %s\n", tt);
  printf("        Sync Serial:     %u\n", (unsigned int)header->sync_serial);
  printf("        Write Serial:    %u\n", (unsigned int)header->write_serial);
  printf("\n");

  return 0;
}

static void
vol_init_data_internal(Vol *d)
{
  d->buckets = ((d->len - (d->start - d->skip)) / cache_config_min_average_object_size) / DIR_DEPTH;
  d->segments = (d->buckets + (((1<<16)-1)/DIR_DEPTH)) / ((1<<16)/DIR_DEPTH);
  d->buckets = (d->buckets + d->segments - 1) / d->segments;
  d->start = d->skip + 2 *vol_dirlen(d);
}

static void
vol_init_data(Vol *d) {
  // iteratively calculate start + buckets
  vol_init_data_internal(d);
  vol_init_data_internal(d);
  vol_init_data_internal(d);
}

void
vol_init_dir(Vol *d)
{
  int b, s, l;

  for (s = 0; s < d->segments; s++) {
    d->header->freelist[s] = 0;
    Dir *seg = dir_segment(s, d);
    for (l = 1; l < DIR_DEPTH; l++) {
      for (b = 0; b < d->buckets; b++) {
        Dir *bucket = dir_bucket(b, seg);
        dir_free_entry(dir_bucket_row(bucket, l), s, d);
      }
    }
  }
}

void
vol_clear_init(Vol *d)
{
  size_t dir_len = vol_dirlen(d);
  memset(d->raw_dir, 0, dir_len);
  vol_init_dir(d);
  d->header->magic = VOL_MAGIC;
  d->header->version.ink_major = CACHE_DB_MAJOR_VERSION;
  d->header->version.ink_minor = CACHE_DB_MINOR_VERSION;
  d->scan_pos = d->header->agg_pos = d->header->write_pos = d->start;
  d->header->last_write_pos = d->header->write_pos;
  d->header->phase = 0;
  d->header->cycle = 0;
  d->header->create_time = time(NULL);
  d->header->dirty = 0;
  d->sector_size = d->header->sector_size = d->disk->hw_sector_size;
  *d->footer = *d->header;
}

int
vol_dir_clear(Vol *d)
{
  size_t dir_len = vol_dirlen(d);
  vol_clear_init(d);

  if (pwrite(d->fd, d->raw_dir, dir_len, d->skip) < 0) {
    Warning("unable to clear cache directory '%s'", d->hash_id);
    return -1;
  }
  return 0;
}

int
Vol::clear_dir()
{
  size_t dir_len = vol_dirlen(this);
  vol_clear_init(this);

  SET_HANDLER(&Vol::handle_dir_clear);

  io.aiocb.aio_fildes = fd;
  io.aiocb.aio_buf = raw_dir;
  io.aiocb.aio_nbytes = dir_len;
  io.aiocb.aio_offset = skip;
  io.action = this;
  io.thread = AIO_CALLBACK_THREAD_ANY;
  io.then = 0;
  ink_assert(ink_aio_write(&io));
  return 0;
}

int
Vol::init(char *s, off_t blocks, off_t dir_skip, bool clear)
{
  dir_skip = ROUND_TO_STORE_BLOCK((dir_skip < START_POS ? START_POS : dir_skip));
  path = xstrdup(s);
  const size_t hash_id_size = strlen(s) + 32;
  hash_id = (char *) malloc(hash_id_size);
  ink_strncpy(hash_id, s, hash_id_size);
  const size_t s_size = strlen(s);
  snprintf(hash_id + s_size, (hash_id_size - s_size), " %" PRIu64 ":%" PRIu64 "",
           (uint64_t)dir_skip, (uint64_t)blocks);
  hash_id_md5.encodeBuffer(hash_id, strlen(hash_id));
  len = blocks * STORE_BLOCK_SIZE;
  ink_assert(len <= MAX_VOL_SIZE);
  skip = dir_skip;
  int i;
  prev_recover_pos = 0;

  // successive approximation, directory/meta data eats up some storage
  start = dir_skip;
  vol_init_data(this);
  data_blocks = (len - (start - skip)) / STORE_BLOCK_SIZE;
#ifdef HIT_EVACUATE
  hit_evacuate_window = (data_blocks * cache_config_hit_evacuate_percent) / 100;
#endif

  evacuate_size = (int) (len / EVACUATION_BUCKET_SIZE) + 2;
  int evac_len = (int) evacuate_size * sizeof(DLL<EvacuationBlock>);
  evacuate = (DLL<EvacuationBlock> *)malloc(evac_len);
  memset(evacuate, 0, evac_len);

#if !defined (_WIN32)
  raw_dir = (char *) valloc(vol_dirlen(this));
#else
  /* the directory should be page aligned for raw disk transfers.
     WIN32 does not support valloc
     or memalign, so we need to allocate extra space and then align the
     pointer ourselves.
     Don't need to keep track of the pointer to the original memory since
     we never free this */
  size_t alignment = getpagesize();
  size_t mem_to_alloc = vol_dirlen(this) + (alignment - 1);
  raw_dir = (char *) malloc(mem_to_alloc);
  raw_dir = (char *) align_pointer_forward(raw_dir, alignment);
#endif

  dir = (Dir *) (raw_dir + vol_headerlen(this));
  header = (VolHeaderFooter *) raw_dir;
  footer = (VolHeaderFooter *) (raw_dir + vol_dirlen(this) - ROUND_TO_STORE_BLOCK(sizeof(VolHeaderFooter)));

  if (clear) {
    Note("clearing cache directory '%s'", hash_id);
    return clear_dir();
  }

  init_info = new VolInitInfo();
  int footerlen = ROUND_TO_STORE_BLOCK(sizeof(VolHeaderFooter));
  off_t footer_offset = vol_dirlen(this) - footerlen;
  // try A
  off_t as = skip;
  if (is_debug_tag_set("cache_init"))
    Note("reading directory '%s'", hash_id);
  SET_HANDLER(&Vol::handle_header_read);
  init_info->vol_aio[0].aiocb.aio_offset = as;
  init_info->vol_aio[1].aiocb.aio_offset = as + footer_offset;
  off_t bs = skip + vol_dirlen(this);
  init_info->vol_aio[2].aiocb.aio_offset = bs;
  init_info->vol_aio[3].aiocb.aio_offset = bs + footer_offset;

  for (i = 0; i < 4; i++) {
    AIOCallback *aio = &(init_info->vol_aio[i]);
    aio->aiocb.aio_fildes = fd;
    aio->aiocb.aio_buf = &(init_info->vol_h_f[i * STORE_BLOCK_SIZE]);
    aio->aiocb.aio_nbytes = footerlen;
    aio->action = this;
    aio->thread = this_ethread();
    aio->then = (i < 3) ? &(init_info->vol_aio[i + 1]) : 0;
  }

  eventProcessor.schedule_imm(this, ET_CALL);
  return 0;
}

int
Vol::handle_dir_clear(int event, void *data)
{
  size_t dir_len = vol_dirlen(this);
  AIOCallback *op;

  if (event == AIO_EVENT_DONE) {
    op = (AIOCallback *) data;
    if ((size_t) op->aio_result != (size_t) op->aiocb.aio_nbytes) {
      Warning("unable to clear cache directory '%s'", hash_id);
      fd = -1;
    }

    if (op->aiocb.aio_nbytes == dir_len) {
      /* clear the header for directory B. We don't need to clear the
         whole of directory B. The header for directory B starts at
         skip + len */
      op->aiocb.aio_nbytes = ROUND_TO_STORE_BLOCK(sizeof(VolHeaderFooter));
      op->aiocb.aio_offset = skip + dir_len;
      ink_assert(ink_aio_write(op));
      return EVENT_DONE;
    }
    set_io_not_in_progress();
    SET_HANDLER(&Vol::dir_init_done);
    dir_init_done(EVENT_IMMEDIATE, 0);
    /* mark the volume as bad */
  }
  return EVENT_DONE;
}

int
Vol::handle_dir_read(int event, void *data)
{
  AIOCallback *op = (AIOCallback *) data;

  if (event == AIO_EVENT_DONE) {
    if ((size_t) op->aio_result != (size_t) op->aiocb.aio_nbytes) {
      clear_dir();
      return EVENT_DONE;
    }
  }

  if (header->magic != VOL_MAGIC || header->version.ink_major != CACHE_DB_MAJOR_VERSION || footer->magic != VOL_MAGIC) {
    Warning("bad footer in cache directory for '%s', clearing", hash_id);
    Note("clearing cache directory '%s'", hash_id);
    clear_dir();
    return EVENT_DONE;
  }
  CHECK_DIR(this);
  sector_size = header->sector_size;
  SET_HANDLER(&Vol::handle_recover_from_data);
  return handle_recover_from_data(EVENT_IMMEDIATE, 0);
}

/*
   Philosophy:  The idea is to find the region of disk that could be
   inconsistent and remove all directory entries pointing to that potentially
   inconsistent region.
   Start from a consistent position (the write_pos of the last directory
   synced to disk) and scan forward. Two invariants for docs that were
   written to the disk after the directory was synced:

   1. doc->magic == DOC_MAGIC

   The following two cases happen only when the previous generation
   documents are aligned with the current ones.

   2. All the docs written to the disk
   after the directory was synced will have their sync_serial <=
   header->sync_serial + 1,  because the write aggregation can take
   indeterminate amount of time to sync. The doc->sync_serial can be
   equal to header->sync_serial + 1, because we increment the sync_serial
   before we sync the directory to disk.

   3. The doc->sync_serial will always increase. If doc->sync_serial
   decreases, the document was written in the previous phase

   If either of these conditions fail and we are not too close to the end
   (see the next comment ) then we're done

   We actually start from header->last_write_pos instead of header->write_pos
   to make sure that we haven't wrapped around the whole disk without
   syncing the directory.  Since the sync serial is 60 seconds, it is
   entirely possible to write through the whole cache without
   once syncing the directory. In this case, we need to clear the
   cache.The documents written right before we synced the
   directory to disk should have the write_serial <= header->sync_serial.

      */

int
Vol::handle_recover_from_data(int event, void *data)
{
  (void) data;
  uint32_t got_len = 0;
  uint32_t max_sync_serial = header->sync_serial;
  char *s, *e;
  if (event == EVENT_IMMEDIATE) {
    if (header->sync_serial == 0) {
      io.aiocb.aio_buf = NULL;
      SET_HANDLER(&Vol::handle_recover_write_dir);
      return handle_recover_write_dir(EVENT_IMMEDIATE, 0);
    }
    // initialize
    recover_wrapped = 0;
    last_sync_serial = 0;
    last_write_serial = 0;
    recover_pos = header->last_write_pos;
    if (recover_pos >= skip + len) {
      recover_wrapped = 1;
      recover_pos = start;
    }
#if defined(_WIN32)
    io.aiocb.aio_buf = (char *) malloc(RECOVERY_SIZE);
#else
    io.aiocb.aio_buf = (char *) valloc(RECOVERY_SIZE);
#endif
    io.aiocb.aio_nbytes = RECOVERY_SIZE;
    if ((off_t)(recover_pos + io.aiocb.aio_nbytes) > (off_t)(skip + len))
      io.aiocb.aio_nbytes = (skip + len) - recover_pos;
  } else if (event == AIO_EVENT_DONE) {
    if ((size_t) io.aiocb.aio_nbytes != (size_t) io.aio_result) {
      Warning("disk read error on recover '%s', clearing", hash_id);
      goto Lclear;
    }
    if (io.aiocb.aio_offset == header->last_write_pos) {

      /* check that we haven't wrapped around without syncing
         the directory. Start from last_write_serial (write pos the documents
         were written to just before syncing the directory) and make sure
         that all documents have write_serial <= header->write_serial.
       */
      uint32_t to_check = header->write_pos - header->last_write_pos;
      ink_assert(to_check && to_check < (uint32_t)io.aiocb.aio_nbytes);
      uint32_t done = 0;
      s = (char *) io.aiocb.aio_buf;
      while (done < to_check) {
        Doc *doc = (Doc *) (s + done);
        if (doc->magic != DOC_MAGIC || doc->write_serial > header->write_serial) {
          Warning("no valid directory found while recovering '%s', clearing", hash_id);
          goto Lclear;
        }
        done += round_to_approx_size(doc->len);
        if (doc->sync_serial > last_write_serial)
          last_sync_serial = doc->sync_serial;
      }
      ink_assert(done == to_check);

      got_len = io.aiocb.aio_nbytes - done;
      recover_pos += io.aiocb.aio_nbytes;
      s = (char *) io.aiocb.aio_buf + done;
      e = s + got_len;
    } else {
      got_len = io.aiocb.aio_nbytes;
      recover_pos += io.aiocb.aio_nbytes;
      s = (char *) io.aiocb.aio_buf;
      e = s + got_len;
    }
  }
  // examine what we got
  if (got_len) {

    Doc *doc = NULL;

    if (recover_wrapped && start == io.aiocb.aio_offset) {
      doc = (Doc *) s;
      if (doc->magic != DOC_MAGIC || doc->write_serial < last_write_serial) {
        recover_pos = skip + len - EVACUATION_SIZE;
        goto Ldone;
      }
    }

    while (s < e) {
      doc = (Doc *) s;

      if (doc->magic != DOC_MAGIC || doc->sync_serial != last_sync_serial) {

        if (doc->magic == DOC_MAGIC) {
          if (doc->sync_serial > header->sync_serial)
            max_sync_serial = doc->sync_serial;

          /*
             doc->magic == DOC_MAGIC, but doc->sync_serial != last_sync_serial
             This might happen in the following situations
             1. We are starting off recovery. In this case the
             last_sync_serial == header->sync_serial, but the doc->sync_serial
             can be anywhere in the range (0, header->sync_serial + 1]
             If this is the case, update last_sync_serial and continue;

             2. A dir sync started between writing documents to the
             aggregation buffer and hence the doc->sync_serial went up.
             If the doc->sync_serial is greater than the last
             sync serial and less than (header->sync_serial + 2) then
             continue;

             3. If the position we are recovering from is within AGG_SIZE
             from the disk end, then we can't trust this document. The
             aggregation buffer might have been larger than the remaining space
             at the end and we decided to wrap around instead of writing
             anything at that point. In this case, wrap around and start
             from the beginning.

             If neither of these 3 cases happen, then we are indeed done.

           */

          // case 1
          // case 2
          if (doc->sync_serial > last_sync_serial && doc->sync_serial <= header->sync_serial + 1) {
            last_sync_serial = doc->sync_serial;
            s += round_to_approx_size(doc->len);
            continue;
          }
          // case 3 - we have already recoverd some data and
          // (doc->sync_serial < last_sync_serial) ||
          // (doc->sync_serial > header->sync_serial + 1).
          // if we are too close to the end, wrap around
          else if (recover_pos - (e - s) > (skip + len) - AGG_SIZE) {
            recover_wrapped = 1;
            recover_pos = start;
            io.aiocb.aio_nbytes = RECOVERY_SIZE;

            break;
          }
          // we are done. This doc was written in the earlier phase
          recover_pos -= e - s;
          goto Ldone;
        } else {
          // doc->magic != DOC_MAGIC
          // If we are in the danger zone - recover_pos is within AGG_SIZE
          // from the end, then wrap around
          recover_pos -= e - s;
          if (recover_pos > (skip + len) - AGG_SIZE) {
            recover_wrapped = 1;
            recover_pos = start;
            io.aiocb.aio_nbytes = RECOVERY_SIZE;

            break;
          }
          // we ar not in the danger zone
          goto Ldone;
        }
      }
      // doc->magic == DOC_MAGIC && doc->sync_serial == last_sync_serial
      last_write_serial = doc->write_serial;
      s += round_to_approx_size(doc->len);
    }

    /* if (s > e) then we gone through RECOVERY_SIZE; we need to
       read more data off disk and continue recovering */
    if (s >= e) {
      /* In the last iteration, we increment s by doc->len...need to undo
         that change */
      if (s > e)
        s -= round_to_approx_size(doc->len);
      recover_pos -= e - s;
      if (recover_pos >= skip + len)
        recover_pos = start;
      io.aiocb.aio_nbytes = RECOVERY_SIZE;
      if ((off_t)(recover_pos + io.aiocb.aio_nbytes) > (off_t)(skip + len))
        io.aiocb.aio_nbytes = (skip + len) - recover_pos;
    }
  }
  if (recover_pos == prev_recover_pos) // this should never happen, but if it does break the loop
    goto Lclear;
  prev_recover_pos = recover_pos;
  io.aiocb.aio_offset = recover_pos;
  ink_assert(ink_aio_read(&io));
  return EVENT_CONT;

Ldone:{
    /* if we come back to the starting position, then we don't have to recover anything */
    if (recover_pos == header->write_pos && recover_wrapped) {
      SET_HANDLER(&Vol::handle_recover_write_dir);
      if (is_debug_tag_set("cache_init"))
        Note("recovery wrapped around. nothing to clear\n");
      return handle_recover_write_dir(EVENT_IMMEDIATE, 0);
    }

    recover_pos += EVACUATION_SIZE;   // safely cover the max write size
    if (recover_pos < header->write_pos && (recover_pos + EVACUATION_SIZE >= header->write_pos)) {
      Debug("cache_init", "Head Pos: %" PRIu64 ", Rec Pos: %" PRIu64 ", Wrapped:%d", header->write_pos, recover_pos, recover_wrapped);
      Warning("no valid directory found while recovering '%s', clearing", hash_id);
      goto Lclear;
    }

    if (recover_pos > skip + len)
      recover_pos -= skip + len;
    // bump sync number so it is different from that in the Doc structs
    uint32_t next_sync_serial = max_sync_serial + 1;
    // make that the next sync does not overwrite our good copy!
    if (!(header->sync_serial & 1) == !(next_sync_serial & 1))
      next_sync_serial++;
    // clear effected portion of the cache
    off_t clear_start = offset_to_vol_offset(this, header->write_pos);
    off_t clear_end = offset_to_vol_offset(this, recover_pos);
    if (clear_start <= clear_end)
      dir_clear_range(clear_start, clear_end, this);
    else {
      dir_clear_range(clear_end, DIR_OFFSET_MAX, this);
      dir_clear_range(1, clear_start, this);
    }
    if (is_debug_tag_set("cache_init"))
      Note("recovery clearing offsets [%" PRIu64 ", %" PRIu64 "] sync_serial %d next %d\n",
           header->write_pos, recover_pos, header->sync_serial, next_sync_serial);
    footer->sync_serial = header->sync_serial = next_sync_serial;

    for (int i = 0; i < 3; i++) {
      AIOCallback *aio = &(init_info->vol_aio[i]);
      aio->aiocb.aio_fildes = fd;
      aio->action = this;
      aio->thread = AIO_CALLBACK_THREAD_ANY;
      aio->then = (i < 2) ? &(init_info->vol_aio[i + 1]) : 0;
    }
    int footerlen = ROUND_TO_STORE_BLOCK(sizeof(VolHeaderFooter));
    size_t dirlen = vol_dirlen(this);
    int B = header->sync_serial & 1;
    off_t ss = skip + (B ? dirlen : 0);

    init_info->vol_aio[0].aiocb.aio_buf = raw_dir;
    init_info->vol_aio[0].aiocb.aio_nbytes = footerlen;
    init_info->vol_aio[0].aiocb.aio_offset = ss;
    init_info->vol_aio[1].aiocb.aio_buf = raw_dir + footerlen;
    init_info->vol_aio[1].aiocb.aio_nbytes = dirlen - 2 * footerlen;
    init_info->vol_aio[1].aiocb.aio_offset = ss + footerlen;
    init_info->vol_aio[2].aiocb.aio_buf = raw_dir + dirlen - footerlen;
    init_info->vol_aio[2].aiocb.aio_nbytes = footerlen;
    init_info->vol_aio[2].aiocb.aio_offset = ss + dirlen - footerlen;

    SET_HANDLER(&Vol::handle_recover_write_dir);
    ink_assert(ink_aio_write(init_info->vol_aio));
    return EVENT_CONT;
  }

Lclear:
  free((char *) io.aiocb.aio_buf);
  delete init_info;
  init_info = 0;
  clear_dir();
  return EVENT_CONT;
}

int
Vol::handle_recover_write_dir(int event, void *data)
{
  (void) event;
  (void) data;
  if (io.aiocb.aio_buf)
    free((char *) io.aiocb.aio_buf);
  delete init_info;
  init_info = 0;
  set_io_not_in_progress();
  scan_pos = header->write_pos;
  periodic_scan();
  SET_HANDLER(&Vol::dir_init_done);
  return dir_init_done(EVENT_IMMEDIATE, 0);
}

int
Vol::handle_header_read(int event, void *data)
{
  AIOCallback *op;
  VolHeaderFooter *hf[4];
  switch (event) {
  case EVENT_IMMEDIATE:
  case EVENT_INTERVAL:
    ink_assert(ink_aio_read(init_info->vol_aio));
    return EVENT_CONT;

  case AIO_EVENT_DONE:
    op = (AIOCallback *) data;
    for (int i = 0; i < 4; i++) {
      ink_assert(op != 0);
      hf[i] = (VolHeaderFooter *) (op->aiocb.aio_buf);
      if ((size_t) op->aio_result != (size_t) op->aiocb.aio_nbytes) {
        clear_dir();
        return EVENT_DONE;
      }
      op = op->then;
    }

    io.aiocb.aio_fildes = fd;
    io.aiocb.aio_nbytes = vol_dirlen(this);
    io.aiocb.aio_buf = raw_dir;
    io.action = this;
    io.thread = AIO_CALLBACK_THREAD_ANY;
    io.then = 0;

    if (hf[0]->sync_serial == hf[1]->sync_serial &&
        (hf[0]->sync_serial >= hf[2]->sync_serial || hf[2]->sync_serial != hf[3]->sync_serial)) {
      SET_HANDLER(&Vol::handle_dir_read);
      if (is_debug_tag_set("cache_init"))
        Note("using directory A for '%s'", hash_id);
      io.aiocb.aio_offset = skip;
      ink_assert(ink_aio_read(&io));
    }
    // try B
    else if (hf[2]->sync_serial == hf[3]->sync_serial) {

      SET_HANDLER(&Vol::handle_dir_read);
      if (is_debug_tag_set("cache_init"))
        Note("using directory B for '%s'", hash_id);
      io.aiocb.aio_offset = skip + vol_dirlen(this);
      ink_assert(ink_aio_read(&io));
    } else {
      Note("no good directory, clearing '%s'", hash_id);
      clear_dir();
      delete init_info;
      init_info = 0;
    }

    return EVENT_DONE;
  }
  return EVENT_DONE;
}

int
Vol::dir_init_done(int event, void *data)
{
  (void) event;
  (void) data;
  if (!cache->cache_read_done) {
    eventProcessor.schedule_in(this, HRTIME_MSECONDS(5), ET_CALL);
    return EVENT_CONT;
  } else {
    int vol_no = ink_atomic_increment(&gnvol, 1);
    ink_assert(!gvol[vol_no]);
    gvol[vol_no] = this;
    SET_HANDLER(&Vol::aggWrite);
    if (fd == -1)
      cache->vol_initialized(0);
    else
      cache->vol_initialized(1);
    return EVENT_DONE;
  }
}

void
build_vol_hash_table(CacheHostRecord *cp)
{
  int num_vols = cp->num_vols;
  unsigned int *mapping = (unsigned int *) xmalloc(sizeof(unsigned int) * num_vols);
  Vol **p = (Vol **) xmalloc(sizeof(Vol *) * num_vols);

  memset(mapping, 0, num_vols * sizeof(unsigned int));
  memset(p, 0, num_vols * sizeof(Vol *));
  uint64_t total = 0;
  int i = 0;
  int used = 0;
  int bad_vols = 0;
  int map = 0;
  // initialize number of elements per vol
  for (i = 0; i < num_vols; i++) {
    if (DISK_BAD(cp->vols[i]->disk)) {
      bad_vols++;
      continue;
    }
    mapping[map] = i;
    p[map++] = cp->vols[i];
    total += (cp->vols[i]->len >> STORE_BLOCK_SHIFT);
  }

  num_vols -= bad_vols;

  if (!num_vols) {
    // all the disks are corrupt,
    if (cp->vol_hash_table) {
      new_Freer(cp->vol_hash_table, CACHE_MEM_FREE_TIMEOUT);
    }
    cp->vol_hash_table = NULL;
    xfree(mapping);
    xfree(p);
    return;
  }


  unsigned int *forvol = (unsigned int *) alloca(sizeof(unsigned int) * num_vols);
  unsigned int *rnd = (unsigned int *) alloca(sizeof(unsigned int) * num_vols);
  unsigned short *ttable = (unsigned short *) xmalloc(sizeof(unsigned short) * VOL_HASH_TABLE_SIZE);

  for (i = 0; i < num_vols; i++) {
    forvol[i] = (VOL_HASH_TABLE_SIZE * (p[i]->len >> STORE_BLOCK_SHIFT)) / total;
    used += forvol[i];
  }
  // spread around the excess
  int extra = VOL_HASH_TABLE_SIZE - used;
  for (i = 0; i < extra; i++) {
    forvol[i % num_vols]++;
  }
  // seed random number generator
  for (i = 0; i < num_vols; i++) {
    uint64_t x = p[i]->hash_id_md5.fold();
    rnd[i] = (unsigned int) x;
  }
  // initialize table to "empty"
  for (i = 0; i < VOL_HASH_TABLE_SIZE; i++)
    ttable[i] = VOL_HASH_EMPTY;
  // give each machine it's fav
  int left = VOL_HASH_TABLE_SIZE;
  int d = 0;
  for (; left; d = (d + 1) % num_vols) {
    if (!forvol[d])
      continue;
    do {
      i = next_rand(&rnd[d]) % VOL_HASH_TABLE_SIZE;
    } while (ttable[i] != VOL_HASH_EMPTY);
    ttable[i] = mapping[d];
    forvol[d]--;
    left--;
  }

  // install new table

  if (cp->vol_hash_table) {
    new_Freer(cp->vol_hash_table, CACHE_MEM_FREE_TIMEOUT);
  }
  xfree(mapping);
  xfree(p);
  cp->vol_hash_table = ttable;
}


void
Cache::vol_initialized(bool result)
{
  ink_atomic_increment(&total_initialized_vol, 1);
  if (result)
    ink_atomic_increment(&total_good_nvol, 1);
  if (total_nvol == total_initialized_vol)
    open_done();
}

int
AIO_Callback_handler::handle_disk_failure(int event, void *data)
{
  (void) event;
  /* search for the matching file descriptor */
  if (!CacheProcessor::cache_ready)
    return EVENT_DONE;
  int disk_no = 0;
  int good_disks = 0;
  AIOCallback *cb = (AIOCallback *) data;
  for (; disk_no < gndisks; disk_no++) {
    CacheDisk *d = gdisks[disk_no];

    if (d->fd == cb->aiocb.aio_fildes) {
      d->num_errors++;

      if (!DISK_BAD(d)) {

        char message[128];
        snprintf(message, sizeof(message), "Error accessing Disk %s", d->path);
        Warning(message);
        IOCORE_SignalManager(REC_SIGNAL_CACHE_WARNING, message);
      } else if (!DISK_BAD_SIGNALLED(d)) {

        char message[128];
        snprintf(message, sizeof(message), "too many errors accessing disk %s: declaring disk bad", d->path);
        Warning(message);
        IOCORE_SignalManager(REC_SIGNAL_CACHE_ERROR, message);
        /* subtract the disk space that was being used from  the cache size stat */
        // dir entries stat
        int p;
        uint64_t total_bytes_delete = 0;
        uint64_t total_dir_delete = 0;
        uint64_t used_dir_delete = 0;

        for (p = 0; p < gnvol; p++) {
          if (d->fd == gvol[p]->fd) {
            total_dir_delete += gvol[p]->buckets * gvol[p]->segments * DIR_DEPTH;
            used_dir_delete += dir_entries_used(gvol[p]);
            total_bytes_delete = gvol[p]->len - vol_dirlen(gvol[p]);
          }
        }

        RecIncrGlobalRawStat(cache_rsb, cache_bytes_total_stat, -total_bytes_delete);
        RecIncrGlobalRawStat(cache_rsb, cache_bytes_total_stat, -total_dir_delete);
        RecIncrGlobalRawStat(cache_rsb, cache_bytes_total_stat, -cache_direntries_used_stat);

        if (theCache) {
          rebuild_host_table(theCache);
        }
        if (theStreamCache) {
          rebuild_host_table(theStreamCache);
        }
      }
      if (good_disks)
        return EVENT_DONE;
    }

    if (!DISK_BAD(d))
      good_disks++;

  }
  if (!good_disks) {
    Warning("all disks are bad, cache disabled");
    CacheProcessor::cache_ready = 0;
    delete cb;
    return EVENT_DONE;
  }

  if (theCache && !theCache->hosttable->gen_host_rec.vol_hash_table) {
    unsigned int caches_ready = 0;
    caches_ready = caches_ready | (1 << CACHE_FRAG_TYPE_HTTP);
    caches_ready = caches_ready | (1 << CACHE_FRAG_TYPE_NONE);
    caches_ready = ~caches_ready;
    CacheProcessor::cache_ready &= caches_ready;
    Warning("all volumes for http cache are corrupt, http cache disabled");
  }
  if (theStreamCache && !theStreamCache->hosttable->gen_host_rec.vol_hash_table) {
    unsigned int caches_ready = 0;
    caches_ready = caches_ready | (1 << CACHE_FRAG_TYPE_RTSP);
    caches_ready = ~caches_ready;
    CacheProcessor::cache_ready &= caches_ready;
    Warning("all volumes for mixt cache are corrupt, mixt cache disabled");
  }
  delete cb;
  return EVENT_DONE;
}

int
Cache::open_done()
{
#ifdef NON_MODULAR
  Action *register_ShowCache(Continuation * c, HTTPHdr * h);
  Action *register_ShowCacheInternal(Continuation *c, HTTPHdr *h);
  statPagesManager.register_http("cache", register_ShowCache);
  statPagesManager.register_http("cache-internal", register_ShowCacheInternal);
#endif
  if (total_good_nvol == 0) {
    ready = CACHE_INIT_FAILED;
    cacheProcessor.cacheInitialized();
    return 0;
  }

  hosttable = NEW(new CacheHostTable(this, scheme));
  hosttable->register_config_callback(&hosttable);

  if (hosttable->gen_host_rec.num_cachevols == 0)
    ready = CACHE_INIT_FAILED;
  else
    ready = CACHE_INITIALIZED;
  cacheProcessor.cacheInitialized();

  return 0;
}

int
Cache::open(bool clear, bool fix)
{
  NOWARN_UNUSED(fix);
  int i;
  off_t blocks;
  cache_read_done = 0;
  total_initialized_vol = 0;
  total_nvol = 0;
  total_good_nvol = 0;


  IOCORE_EstablishStaticConfigInt32(cache_config_min_average_object_size, "proxy.config.cache.min_average_object_size");
  Debug("cache_init", "Cache::open - proxy.config.cache.min_average_object_size = %d",
        (int)cache_config_min_average_object_size);

  CacheVol *cp = cp_list.head;
  for (; cp; cp = cp->link.next) {
    if (cp->scheme == scheme) {
      cp->vols = (Vol **) xmalloc(cp->num_vols * sizeof(Vol *));
      int vol_no = 0;
      for (i = 0; i < gndisks; i++) {
        if (cp->disk_vols[i] && !DISK_BAD(cp->disk_vols[i]->disk)) {
          DiskVolBlockQueue *q = cp->disk_vols[i]->dpb_queue.head;
          for (; q; q = q->link.next) {
            cp->vols[vol_no] = NEW(new Vol());
            CacheDisk *d = cp->disk_vols[i]->disk;
            cp->vols[vol_no]->disk = d;
            cp->vols[vol_no]->fd = d->fd;
            cp->vols[vol_no]->cache = this;
            cp->vols[vol_no]->cache_vol = cp;
            blocks = q->b->len;

            bool vol_clear = clear || d->cleared || q->new_block;
            cp->vols[vol_no]->init(d->path, blocks, q->b->offset, vol_clear);
            vol_no++;
            cache_size += blocks;
          }
        }
      }
      total_nvol += vol_no;
    }
  }
  if (total_nvol == 0)
    return open_done();
  cache_read_done = 1;
  return 0;
}


int
Cache::close()
{
  return -1;
}

int
CacheVC::dead(int event, Event *e)
{
  NOWARN_UNUSED(e);
  NOWARN_UNUSED(event);
  ink_assert(0);
  return EVENT_DONE;
}

#define STORE_COLLISION 1

#ifdef HTTP_CACHE
static void unmarshal_helper(Doc *doc, Ptr<IOBufferData> &buf, int &okay) {
  char *tmp = doc->hdr();
  int len = doc->hlen;
  while (len > 0) {
    int r = HTTPInfo::unmarshal(tmp, len, buf._ptr());
    if (r < 0) {
      ink_assert(!"CacheVC::handleReadDone unmarshal failed");
      okay = 0;
      break;
    }
    len -= r;
    tmp += r;
  }
}
#endif

int
CacheVC::handleReadDone(int event, Event *e)
{
  NOWARN_UNUSED(e);
  cancel_trigger();
  ink_debug_assert(this_ethread() == mutex->thread_holding);

  if (event == AIO_EVENT_DONE)
    set_io_not_in_progress();
  else
    if (is_io_in_progress())
      return EVENT_CONT;
  {
    MUTEX_TRY_LOCK(lock, vol->mutex, mutex->thread_holding);
    if (!lock)
      VC_SCHED_LOCK_RETRY();
    if ((!dir_valid(vol, &dir)) || (!io.ok())) {
      if (!io.ok()) {
        Debug("cache_disk_error", "Read error on disk %s\n \
	    read range : [%" PRIu64 " - %" PRIu64 " bytes]  [%" PRIu64 " - %" PRIu64 " blocks] \n", vol->hash_id, io.aiocb.aio_offset, io.aiocb.aio_offset + io.aiocb.aio_nbytes, io.aiocb.aio_offset / 512, (io.aiocb.aio_offset + io.aiocb.aio_nbytes) / 512);
      }
      goto Ldone;
    }

    ink_assert(vol->mutex->nthread_holding < 1000);
    ink_assert(((Doc *) buf->data())->magic == DOC_MAGIC);
#ifdef VERIFY_JTEST_DATA
    char xx[500];
    if (read_key && *read_key == ((Doc *) buf->data())->key && request.valid() && !dir_head(&dir) && !vio.ndone) {
      int ib = 0, xd = 0;
      request.url_get()->print(xx, 500, &ib, &xd);
      char *x = xx;
      for (int q = 0; q < 3; q++)
        x = strchr(x + 1, '/');
      ink_assert(!memcmp(((Doc *) buf->data())->data(), x, ib - (x - xx)));
    }
#endif
    Doc *doc = (Doc *) buf->data();
    // put into ram cache?
    if (io.ok() &&
        ((doc->first_key == *read_key) || (doc->key == *read_key) || STORE_COLLISION) && doc->magic == DOC_MAGIC) {
      int okay = 1;
      if (!f.doc_from_ram_cache)
        f.not_from_ram_cache = 1;
      if (cache_config_enable_checksum && doc->checksum != DOC_NO_CHECKSUM) {
        // verify that the checksum matches
        uint32_t checksum = 0;
        for (char *b = doc->hdr(); b < (char *) doc + doc->len; b++)
          checksum += *b;
        ink_assert(checksum == doc->checksum);
        if (checksum != doc->checksum) {
          Note("cache: checksum error for [%" PRIu64 " %" PRIu64 "] len %d, hlen %d, disk %s, offset %" PRIu64 " size %d",
               doc->first_key.b[0], doc->first_key.b[1],
               doc->len, doc->hlen, vol->path, io.aiocb.aio_offset, io.aiocb.aio_nbytes);
          doc->magic = DOC_CORRUPT;
          okay = 0;
        }
      }
      bool http_copy_hdr = false;
#ifdef HTTP_CACHE
      http_copy_hdr = cache_config_ram_cache_compress && !f.doc_from_ram_cache &&
        doc->ftype == CACHE_FRAG_TYPE_HTTP && doc->hlen;
      // If http doc we need to unmarshal the headers before putting in the ram cache
      // unless it could be compressed
      if (!http_copy_hdr && doc->ftype == CACHE_FRAG_TYPE_HTTP && doc->hlen && okay)
        unmarshal_helper(doc, buf, okay);
#endif
      // Put the request in the ram cache only if its a open_read or lookup
      if (vio.op == VIO::READ && okay) {
        bool cutoff_check;
        // cutoff_check :
        // doc_len == 0 for the first fragment (it is set from the vector)
        //                The decision on the first fragment is based on
        //                doc->total_len
        // After that, the decision is based of doc_len (doc_len != 0)
        // (cache_config_ram_cache_cutoff == 0) : no cutoffs
        cutoff_check = ((!doc_len && (int64_t)doc->total_len < cache_config_ram_cache_cutoff)
                        || (doc_len && (int64_t)doc_len < cache_config_ram_cache_cutoff)
                        || !cache_config_ram_cache_cutoff);
        if (cutoff_check && !f.doc_from_ram_cache) {
          uint64_t o = dir_offset(&dir);
          vol->ram_cache->put(read_key, buf, doc->len, http_copy_hdr, (uint32_t)(o >> 32), (uint32_t)o);
        }
        if (!doc_len) {
          // keep a pointer to it. In case the state machine decides to
          // update this document, we don't have to read it back in memory
          // again
          vol->first_fragment_key = *read_key;
          vol->first_fragment_offset = dir_offset(&dir);
          vol->first_fragment_data = buf;
        }
      }                           // end VIO::READ check
#ifdef HTTP_CACHE
      // If it could be compressed, unmarshal after
      if (http_copy_hdr && doc->ftype == CACHE_FRAG_TYPE_HTTP && doc->hlen && okay)
        unmarshal_helper(doc, buf, okay);
#endif
    }                             // end io.ok() check
  }
Ldone:
  POP_HANDLER;
  return handleEvent(AIO_EVENT_DONE, 0);
}


int
CacheVC::handleRead(int event, Event *e)
{
  NOWARN_UNUSED(event);
  NOWARN_UNUSED(e);
  cancel_trigger();

  f.doc_from_ram_cache = false;

  // check ram cache
  ink_debug_assert(vol->mutex->thread_holding == this_ethread());
  if (vol->ram_cache->get(read_key, &buf, 0, dir_offset(&dir)))
    goto LramHit;

  // check if it was read in the last open_read call
  if (*read_key == vol->first_fragment_key && dir_offset(&dir) == vol->first_fragment_offset) {
    buf = vol->first_fragment_data;
    goto LmemHit;
  }

  // see if its in the aggregation buffer
  if (dir_agg_buf_valid(vol, &dir)) {
    int agg_offset = vol_offset(vol, &dir) - vol->header->write_pos;
    buf = new_IOBufferData(iobuffer_size_to_index(io.aiocb.aio_nbytes, MAX_BUFFER_SIZE_INDEX), MEMALIGNED);
    ink_assert((agg_offset + io.aiocb.aio_nbytes) <= (unsigned) vol->agg_buf_pos);
    char *doc = buf->data();
    char *agg = vol->agg_buffer + agg_offset;
    memcpy(doc, agg, io.aiocb.aio_nbytes);
    io.aio_result = io.aiocb.aio_nbytes;
    SET_HANDLER(&CacheVC::handleReadDone);
    return EVENT_RETURN;
  }

  io.aiocb.aio_fildes = vol->fd;
  io.aiocb.aio_offset = vol_offset(vol, &dir);
  if ((off_t)(io.aiocb.aio_offset + io.aiocb.aio_nbytes) > (off_t)(vol->skip + vol->len))
    io.aiocb.aio_nbytes = vol->skip + vol->len - io.aiocb.aio_offset;
  buf = new_IOBufferData(iobuffer_size_to_index(io.aiocb.aio_nbytes, MAX_BUFFER_SIZE_INDEX), MEMALIGNED);
  io.aiocb.aio_buf = buf->data();
  io.action = this;
  io.thread = mutex->thread_holding;
  SET_HANDLER(&CacheVC::handleReadDone);
  ink_assert(ink_aio_read(&io) >= 0);
  CACHE_DEBUG_INCREMENT_DYN_STAT(cache_pread_count_stat);
  return EVENT_CONT;

LramHit: {
    io.aio_result = io.aiocb.aio_nbytes;
    Doc *doc = (Doc*)buf->data();
    if (cache_config_ram_cache_compress && doc->ftype == CACHE_FRAG_TYPE_HTTP && doc->hlen) {
      SET_HANDLER(&CacheVC::handleReadDone);
      f.doc_from_ram_cache = true;
      return EVENT_RETURN;
    }
  }
LmemHit:
  io.aio_result = io.aiocb.aio_nbytes;
  POP_HANDLER;
  return EVENT_RETURN; // allow the caller to release the volume lock
}

Action *
Cache::lookup(Continuation *cont, CacheKey *key, CacheFragType type, char *hostname, int host_len)
{
  if (!CACHE_READY(type)) {
    cont->handleEvent(CACHE_EVENT_LOOKUP_FAILED, 0);
    return ACTION_RESULT_DONE;
  }

  Vol *vol = key_to_vol(key, hostname, host_len);
  ProxyMutex *mutex = cont->mutex;
  CacheVC *c = new_CacheVC(cont);
  SET_CONTINUATION_HANDLER(c, &CacheVC::openReadStartHead);
  c->vio.op = VIO::READ;
  c->base_stat = cache_lookup_active_stat;
  CACHE_INCREMENT_DYN_STAT(c->base_stat + CACHE_STAT_ACTIVE);
  c->first_key = c->key = *key;
  c->frag_type = type;
  c->f.lookup = 1;
  c->vol = vol;
  c->last_collision = NULL;

  if (c->handleEvent(EVENT_INTERVAL, 0) == EVENT_CONT)
    return &c->_action;
  else
    return ACTION_RESULT_DONE;
}

#ifdef NON_MODULAR
Action *
Cache::lookup(Continuation *cont, CacheURL *url, CacheFragType type)
{
  INK_MD5 md5;

  url->MD5_get(&md5);
  int len = 0;
  const char *hostname = url->host_get(&len);
  return lookup(cont, &md5, type, (char *) hostname, len);
}
#endif

int
CacheVC::removeEvent(int event, Event *e)
{
  NOWARN_UNUSED(e);
  NOWARN_UNUSED(event);

  cancel_trigger();
  set_io_not_in_progress();
  {
    MUTEX_TRY_LOCK(lock, vol->mutex, mutex->thread_holding);
    if (!lock)
      VC_SCHED_LOCK_RETRY();
    if (_action.cancelled) {
      if (od) {
        vol->close_write(this);
        od = 0;
      }
      goto Lfree;
    }
    if (!f.remove_aborted_writers) {
      if (vol->open_write(this, true, 1)) {
        // writer  exists
        ink_assert(od = vol->open_read(&key));
        od->dont_update_directory = 1;
        od = NULL;
      } else {
        od->dont_update_directory = 1;
      }
      f.remove_aborted_writers = 1;
    }
  Lread:
    if (!buf)
      goto Lcollision;
    if (!dir_valid(vol, &dir)) {
      last_collision = NULL;
      goto Lcollision;
    }
    // check read completed correct FIXME: remove bad vols
    if ((size_t) io.aio_result != (size_t) io.aiocb.aio_nbytes)
      goto Ldone;
    {
      // verify that this is our document
      Doc *doc = (Doc *) buf->data();
      /* should be first_key not key..right?? */
      if (doc->first_key == key) {
        ink_assert(doc->magic == DOC_MAGIC);
        if (dir_delete(&key, vol, &dir) > 0) {
          if (od)
            vol->close_write(this);
          od = NULL;
          goto Lremoved;
        }
        goto Ldone;
      }
    }
  Lcollision:
    // check for collision
    if (dir_probe(&key, vol, &dir, &last_collision) > 0) {
      int ret = do_read_call(&key);
      if (ret == EVENT_RETURN)
        goto Lread;
      return ret;
    }
  Ldone:
    CACHE_INCREMENT_DYN_STAT(cache_remove_failure_stat);
    if (od)
      vol->close_write(this);
  }
  ink_debug_assert(!vol || this_ethread() != vol->mutex->thread_holding);
  _action.continuation->handleEvent(CACHE_EVENT_REMOVE_FAILED, (void *) -ECACHE_NO_DOC);
  goto Lfree;
Lremoved:
  _action.continuation->handleEvent(CACHE_EVENT_REMOVE, 0);
Lfree:
  return free_CacheVC(this);
}

Action *
Cache::remove(Continuation *cont, CacheKey *key, CacheFragType type,
              bool user_agents, bool link,
              char *hostname, int host_len)
{
  NOWARN_UNUSED(user_agents);
  NOWARN_UNUSED(link);

  if (!CACHE_READY(type)) {
    if (cont)
      cont->handleEvent(CACHE_EVENT_REMOVE_FAILED, 0);
    return ACTION_RESULT_DONE;
  }

  ink_assert(this);

  ProxyMutexPtr mutex = NULL;
  if (!cont)
    cont = new_CacheRemoveCont();

  CACHE_TRY_LOCK(lock, cont->mutex, this_ethread());
  ink_assert(lock);
  Vol *vol = key_to_vol(key, hostname, host_len);
  // coverity[var_decl]
  Dir result;
  dir_clear(&result);           // initialized here, set result empty so we can recognize missed lock
  mutex = cont->mutex;

  CacheVC *c = new_CacheVC(cont);
  c->vio.op = VIO::NONE;
  c->frag_type = type;
  c->base_stat = cache_remove_active_stat;
  CACHE_INCREMENT_DYN_STAT(c->base_stat + CACHE_STAT_ACTIVE);
  c->first_key = c->key = *key;
  c->vol = vol;
  c->dir = result;
  c->f.remove = 1;

  SET_CONTINUATION_HANDLER(c, &CacheVC::removeEvent);
  int ret = c->removeEvent(EVENT_IMMEDIATE, 0);
  if (ret == EVENT_DONE)
    return ACTION_RESULT_DONE;
  else
    return &c->_action;
}
// CacheVConnection

CacheVConnection::CacheVConnection()
  : VConnection(NULL)
{ }


void
cplist_init()
{
  cp_list_len = 0;
  for (int i = 0; i < gndisks; i++) {
    CacheDisk *d = gdisks[i];
    DiskVol **dp = d->disk_vols;
    for (unsigned int j = 0; j < d->header->num_volumes; j++) {
      ink_assert(dp[j]->dpb_queue.head);
      CacheVol *p = cp_list.head;
      while (p) {
        if (p->vol_number == dp[j]->vol_number) {
          ink_assert(p->scheme == (int) dp[j]->dpb_queue.head->b->type);
          p->size += dp[j]->size;
          p->num_vols += dp[j]->num_volblocks;
          p->disk_vols[i] = dp[j];
          break;
        }
        p = p->link.next;
      }
      if (!p) {
        // did not find a volume in the cache vol list...create
        // a new one
        CacheVol *new_p = NEW(new CacheVol());
        new_p->vol_number = dp[j]->vol_number;
        new_p->num_vols = dp[j]->num_volblocks;
        new_p->size = dp[j]->size;
        new_p->scheme = dp[j]->dpb_queue.head->b->type;
        new_p->disk_vols = (DiskVol **) xmalloc(gndisks * sizeof(DiskVol *));
        memset(new_p->disk_vols, 0, gndisks * sizeof(DiskVol *));
        new_p->disk_vols[i] = dp[j];
        cp_list.enqueue(new_p);
        cp_list_len++;
      }
    }
  }
}


void
cplist_update()
{
  /* go through cplist and delete volumes that are not in the volume.config */
  CacheVol *cp = cp_list.head;

  while (cp) {
    ConfigVol *config_vol = config_volumes.cp_queue.head;
    for (; config_vol; config_vol = config_vol->link.next) {
      if (config_vol->number == cp->vol_number) {
        int size_in_blocks = config_vol->size << (20 - STORE_BLOCK_SHIFT);
        if ((cp->size <= size_in_blocks) && (cp->scheme == config_vol->scheme)) {
          config_vol->cachep = cp;
        } else {
          /* delete this volume from all the disks */
          int d_no;
          for (d_no = 0; d_no < gndisks; d_no++) {
            if (cp->disk_vols[d_no])
              cp->disk_vols[d_no]->disk->delete_volume(cp->vol_number);
          }
          config_vol = NULL;
        }
        break;
      }
    }

    if (!config_vol) {
      // did not find a matching volume in the config file.
      //Delete hte volume from the cache vol list
      int d_no;
      for (d_no = 0; d_no < gndisks; d_no++) {
        if (cp->disk_vols[d_no])
          cp->disk_vols[d_no]->disk->delete_volume(cp->vol_number);
      }
      CacheVol *temp_cp = cp;
      cp = cp->link.next;
      cp_list.remove(temp_cp);
      cp_list_len--;
      delete temp_cp;
      continue;
    } else
      cp = cp->link.next;
  }
}

int
cplist_reconfigure()
{
  int64_t size;
  int volume_number;
  off_t size_in_blocks;

  gnvol = 0;
  if (config_volumes.num_volumes == 0) {
    /* only the http cache */
    CacheVol *cp = NEW(new CacheVol());
    cp->vol_number = 0;
    cp->scheme = CACHE_HTTP_TYPE;
    cp->disk_vols = (DiskVol **) xmalloc(gndisks * sizeof(DiskVol *));
    memset(cp->disk_vols, 0, gndisks * sizeof(DiskVol *));
    cp_list.enqueue(cp);
    cp_list_len++;
    for (int i = 0; i < gndisks; i++) {
      if (gdisks[i]->header->num_volumes != 1 || gdisks[i]->disk_vols[0]->vol_number != 0) {
        /* The user had created several volumes before - clear the disk
           and create one volume for http */
        Note("Clearing Disk: %s", gdisks[i]->path);
        gdisks[i]->delete_all_volumes();
      }
      if (gdisks[i]->cleared) {
        uint64_t free_space = gdisks[i]->free_space * STORE_BLOCK_SIZE;
        int vols = (free_space / MAX_VOL_SIZE) + 1;
        for (int p = 0; p < vols; p++) {
          off_t b = gdisks[i]->free_space / (vols - p);
          Debug("cache_hosting", "blocks = %d\n", b);
          DiskVolBlock *dpb = gdisks[i]->create_volume(0, b, CACHE_HTTP_TYPE);
          ink_assert(dpb && dpb->len == (uint64_t)b);
        }
        ink_assert(gdisks[i]->free_space == 0);
      }

      ink_assert(gdisks[i]->header->num_volumes == 1);
      DiskVol **dp = gdisks[i]->disk_vols;
      gnvol += dp[0]->num_volblocks;
      cp->size += dp[0]->size;
      cp->num_vols += dp[0]->num_volblocks;
      cp->disk_vols[i] = dp[0];
    }

  } else {
    for (int i = 0; i < gndisks; i++) {
      if (gdisks[i]->header->num_volumes == 1 && gdisks[i]->disk_vols[0]->vol_number == 0) {
        /* The user had created several volumes before - clear the disk
           and create one volume for http */
        Note("Clearing Disk: %s", gdisks[i]->path);
        gdisks[i]->delete_all_volumes();
      }
    }

    /* change percentages in the config patitions to absolute value */
    off_t tot_space_in_blks = 0;
    off_t blocks_per_vol = VOL_BLOCK_SIZE / STORE_BLOCK_SIZE;
    /* sum up the total space available on all the disks.
       round down the space to 128 megabytes */
    for (int i = 0; i < gndisks; i++)
      tot_space_in_blks += (gdisks[i]->num_usable_blocks / blocks_per_vol) * blocks_per_vol;

    double percent_remaining = 100.00;
    ConfigVol *config_vol = config_volumes.cp_queue.head;
    for (; config_vol; config_vol = config_vol->link.next) {
      if (config_vol->in_percent) {
        if (config_vol->percent > percent_remaining) {
          Warning("total volume sizes added up to more than 100%%!");
          Warning("no volumes created");
          return -1;
        }
        int64_t space_in_blks = (int64_t) (((double) (config_vol->percent / percent_remaining)) * tot_space_in_blks);

        space_in_blks = space_in_blks >> (20 - STORE_BLOCK_SHIFT);
        /* round down to 128 megabyte multiple */
        space_in_blks = (space_in_blks >> 7) << 7;
        config_vol->size = space_in_blks;
        tot_space_in_blks -= space_in_blks << (20 - STORE_BLOCK_SHIFT);
        percent_remaining -= (config_vol->size < 128) ? 0 : config_vol->percent;
      }
      if (config_vol->size < 128) {
        Warning("the size of volume %d (%d) is less than the minimum required volume size",
                config_vol->number, config_vol->size, 128);
        Warning("volume %d is not created", config_vol->number);
      }
      Debug("cache_hosting", "Volume: %d Size: %d", config_vol->number, config_vol->size);
    }
    cplist_update();
    /* go through volume config and grow and create volumes */

    config_vol = config_volumes.cp_queue.head;

    for (; config_vol; config_vol = config_vol->link.next) {

      size = config_vol->size;
      if (size < 128)
        continue;

      volume_number = config_vol->number;

      size_in_blocks = ((off_t) size * 1024 * 1024) / STORE_BLOCK_SIZE;

      if (!config_vol->cachep) {
        // we did not find a corresponding entry in cache vol...creat one

        CacheVol *new_cp = NEW(new CacheVol());
        new_cp->disk_vols = (DiskVol **) xmalloc(gndisks * sizeof(DiskVol *));
        memset(new_cp->disk_vols, 0, gndisks * sizeof(DiskVol *));
        if (create_volume(config_vol->number, size_in_blocks, config_vol->scheme, new_cp))
          return -1;
        cp_list.enqueue(new_cp);
        cp_list_len++;
        config_vol->cachep = new_cp;
        gnvol += new_cp->num_vols;
        continue;
      }
//    else
      CacheVol *cp = config_vol->cachep;
      ink_assert(cp->size <= size_in_blocks);
      if (cp->size == size_in_blocks) {
        gnvol += cp->num_vols;
        continue;
      }
      // else the size is greater...
      /* search the cp_list */

      int *sorted_vols = new int[gndisks];
      for (int i = 0; i < gndisks; i++)
        sorted_vols[i] = i;
      for (int i = 0; i < gndisks - 1; i++) {
        int smallest = sorted_vols[i];
        int smallest_ndx = i;
        for (int j = i + 1; j < gndisks; j++) {
          int curr = sorted_vols[j];
          DiskVol *dvol = cp->disk_vols[curr];
          if (gdisks[curr]->cleared) {
            ink_assert(!dvol);
            // disks that are cleared should be filled first
            smallest = curr;
            smallest_ndx = j;
          } else if (!dvol && cp->disk_vols[smallest]) {

            smallest = curr;
            smallest_ndx = j;
          } else if (dvol && cp->disk_vols[smallest] && (dvol->size < cp->disk_vols[smallest]->size)) {
            smallest = curr;
            smallest_ndx = j;
          }
        }
        sorted_vols[smallest_ndx] = sorted_vols[i];
        sorted_vols[i] = smallest;
      }

      int64_t size_to_alloc = size_in_blocks - cp->size;
      int disk_full = 0;
      for (int i = 0; (i < gndisks) && size_to_alloc; i++) {

        int disk_no = sorted_vols[i];
        ink_assert(cp->disk_vols[sorted_vols[gndisks - 1]]);
        int largest_vol = cp->disk_vols[sorted_vols[gndisks - 1]]->size;

        /* allocate storage on new disk. Find the difference
           between the biggest volume on any disk and
           the volume on this disk and try to make
           them equal */
        int64_t size_diff = (cp->disk_vols[disk_no]) ? largest_vol - cp->disk_vols[disk_no]->size : largest_vol;
        size_diff = (size_diff < size_to_alloc) ? size_diff : size_to_alloc;
        /* if size_diff == 0, then then the disks have volumes of the
           same sizes, so we don't need to balance the disks */
        if (size_diff == 0)
          break;

        DiskVolBlock *dpb;
        do {
          dpb = gdisks[disk_no]->create_volume(volume_number, size_diff, cp->scheme);
          if (dpb) {
            if (!cp->disk_vols[disk_no]) {
              cp->disk_vols[disk_no] = gdisks[disk_no]->get_diskvol(volume_number);
            }
            size_diff -= dpb->len;
            cp->size += dpb->len;
            cp->num_vols++;
          } else
            break;
        } while ((size_diff > 0));

        if (!dpb)
          disk_full++;

        size_to_alloc = size_in_blocks - cp->size;
      }

      delete[]sorted_vols;

      if (size_to_alloc) {
        if (create_volume(volume_number, size_to_alloc, cp->scheme, cp))
          return -1;
      }
      gnvol += cp->num_vols;
    }
  }
  return 0;
}

// This is some really bad code, and needs to be rewritten!
int
create_volume(int volume_number, off_t size_in_blocks, int scheme, CacheVol *cp)
{
  static int curr_vol = 0;  // FIXME: this will not reinitialize correctly
  off_t to_create = size_in_blocks;
  off_t blocks_per_vol = VOL_BLOCK_SIZE >> STORE_BLOCK_SHIFT;
  int full_disks = 0;

  int *sp = new int[gndisks];
  memset(sp, 0, gndisks * sizeof(int));

  int i = curr_vol;
  while (size_in_blocks > 0) {
    if (gdisks[i]->free_space >= (sp[i] + blocks_per_vol)) {
      sp[i] += blocks_per_vol;
      size_in_blocks -= blocks_per_vol;
      full_disks = 0;
    } else {
      full_disks += 1;
      if (full_disks == gndisks) {
        char config_file[PATH_NAME_MAX];
        IOCORE_ReadConfigString(config_file, "proxy.config.cache.volume_filename", PATH_NAME_MAX);
        if (cp->size)
          Warning("not enough space to increase volume: [%d] to size: [%d]",
                  volume_number, (to_create + cp->size) >> (20 - STORE_BLOCK_SHIFT));
        else
          Warning("not enough space to create volume: [%d], size: [%d]",
                  volume_number, to_create >> (20 - STORE_BLOCK_SHIFT));

        Note("edit the %s file and restart traffic_server", config_file);
        delete[]sp;
        return -1;
      }
    }
    i = (i + 1) % gndisks;
  }
  cp->vol_number = volume_number;
  cp->scheme = scheme;
  curr_vol = i;
  for (i = 0; i < gndisks; i++) {
    if (sp[i] > 0) {
      while (sp[i] > 0) {
        DiskVolBlock *p = gdisks[i]->create_volume(volume_number, sp[i], scheme);
        ink_assert(p && (p->len >= (unsigned int) blocks_per_vol));
        sp[i] -= p->len;
        cp->num_vols++;
        cp->size += p->len;
      }
      if (!cp->disk_vols[i])
        cp->disk_vols[i] = gdisks[i]->get_diskvol(volume_number);
    }
  }
  delete[]sp;
  return 0;
}

void
rebuild_host_table(Cache *cache)
{
  build_vol_hash_table(&cache->hosttable->gen_host_rec);
  if (cache->hosttable->m_numEntries != 0) {
    CacheHostMatcher *hm = cache->hosttable->getHostMatcher();
    CacheHostRecord *h_rec = hm->getDataArray();
    int h_rec_len = hm->getNumElements();
    int i;
    for (i = 0; i < h_rec_len; i++) {
      build_vol_hash_table(&h_rec[i]);
    }
  }
}

// if generic_host_rec.vols == NULL, what do we do???
Vol *
Cache::key_to_vol(CacheKey *key, char *hostname, int host_len)
{
  uint32_t h = (key->word(2) >> DIR_TAG_WIDTH) % VOL_HASH_TABLE_SIZE;
  unsigned short *hash_table = hosttable->gen_host_rec.vol_hash_table;
  CacheHostRecord *host_rec = &hosttable->gen_host_rec;

  if (hosttable->m_numEntries > 0 && host_len) {
    CacheHostResult res;
    hosttable->Match(hostname, host_len, &res);
    if (res.record) {
      unsigned short *host_hash_table = res.record->vol_hash_table;
      if (host_hash_table) {
        if (is_debug_tag_set("cache_hosting")) {
          char format_str[50];
          snprintf(format_str, sizeof(format_str), "Volume: %%xd for host: %%.%ds", host_len);
          Debug("cache_hosting", format_str, res.record, hostname);
        }
        return res.record->vols[host_hash_table[h]];
      }
    }
  }
  if (hash_table) {
    if (is_debug_tag_set("cache_hosting")) {
      char format_str[50];
      snprintf(format_str, sizeof(format_str), "Generic volume: %%xd for host: %%.%ds", host_len);
      Debug("cache_hosting", format_str, host_rec, hostname);
    }
    return host_rec->vols[hash_table[h]];
  } else
    return host_rec->vols[0];
}

static void reg_int(const char *str, int stat, RecRawStatBlock *rsb, const char *prefix, RecRawStatSyncCb sync_cb=RecRawStatSyncSum) {
  char stat_str[256];
  snprintf(stat_str, sizeof(stat_str), "%s.%s", prefix, str);
  RecRegisterRawStat(rsb, RECT_PROCESS, stat_str, RECD_INT, RECP_NON_PERSISTENT, stat, sync_cb);
  DOCACHE_CLEAR_DYN_STAT(stat)
}
#define REG_INT(_str, _stat) reg_int(_str, (int)_stat, rsb, prefix)

// Register Stats
void
register_cache_stats(RecRawStatBlock *rsb, const char *prefix)
{
  char stat_str[256];

  // Special case for this sucker, since it uses its own aggregator.
  reg_int("bytes_used", cache_bytes_used_stat, rsb, prefix, cache_stats_bytes_used_cb);

  REG_INT("bytes_total", cache_bytes_total_stat);
  snprintf(stat_str, sizeof(stat_str), "%s.%s", prefix, "ram_cache.total_bytes");
  RecRegisterRawStat(rsb, RECT_PROCESS, stat_str, RECD_INT, RECP_NULL, (int) cache_ram_cache_bytes_total_stat, RecRawStatSyncSum);
  REG_INT("ram_cache.bytes_used", cache_ram_cache_bytes_stat);
  REG_INT("ram_cache.hits", cache_ram_cache_hits_stat);
  REG_INT("pread_count", cache_pread_count_stat);
  REG_INT("percent_full", cache_percent_full_stat);
  REG_INT("lookup.active", cache_lookup_active_stat);
  REG_INT("lookup.success", cache_lookup_success_stat);
  REG_INT("lookup.failure", cache_lookup_failure_stat);
  REG_INT("read.active", cache_read_active_stat);
  REG_INT("read.success", cache_read_success_stat);
  REG_INT("read.failure", cache_read_failure_stat);
  REG_INT("write.active", cache_write_active_stat);
  REG_INT("write.success", cache_write_success_stat);
  REG_INT("write.failure", cache_write_failure_stat);
  REG_INT("write.backlog.failure", cache_write_backlog_failure_stat);
  REG_INT("update.active", cache_update_active_stat);
  REG_INT("update.success", cache_update_success_stat);
  REG_INT("update.failure", cache_update_failure_stat);
  REG_INT("remove.active", cache_remove_active_stat);
  REG_INT("remove.success", cache_remove_success_stat);
  REG_INT("remove.failure", cache_remove_failure_stat);
  REG_INT("evacuate.active", cache_evacuate_active_stat);
  REG_INT("evacuate.success", cache_evacuate_success_stat);
  REG_INT("evacuate.failure", cache_evacuate_failure_stat);
  REG_INT("scan.active", cache_scan_active_stat);
  REG_INT("scan.success", cache_scan_success_stat);
  REG_INT("scan.failure", cache_scan_failure_stat);
  REG_INT("direntries.total", cache_direntries_total_stat);
  REG_INT("direntries.used", cache_direntries_used_stat);
  REG_INT("directory_collision", cache_directory_collision_count_stat);
  REG_INT("frags_per_doc.1", cache_single_fragment_document_count_stat);
  REG_INT("frags_per_doc.2", cache_two_fragment_document_count_stat);
  REG_INT("frags_per_doc.3+", cache_three_plus_plus_fragment_document_count_stat);
  REG_INT("read_busy.success", cache_read_busy_success_stat);
  REG_INT("read_busy.failure", cache_read_busy_failure_stat);
  REG_INT("write_bytes_stat", cache_write_bytes_stat);
  REG_INT("vector_marshals", cache_hdr_vector_marshal_stat);
  REG_INT("hdr_marshals", cache_hdr_marshal_stat);
  REG_INT("hdr_marshal_bytes", cache_hdr_marshal_bytes_stat);
  REG_INT("gc_bytes_evacuated", cache_gc_bytes_evacuated_stat);
  REG_INT("gc_frags_evacuated", cache_gc_frags_evacuated_stat);
}


void
ink_cache_init(ModuleVersion v)
{
  ink_release_assert(!checkModuleVersion(v, CACHE_MODULE_VERSION));

  cache_rsb = RecAllocateRawStatBlock((int) cache_stat_count);

  IOCORE_EstablishStaticConfigInteger(cache_config_ram_cache_size, "proxy.config.cache.ram_cache.size");
  Debug("cache_init", "proxy.config.cache.ram_cache.size = %" PRId64 " = %" PRId64 "Mb",
        cache_config_ram_cache_size, cache_config_ram_cache_size / (1024 * 1024));

  IOCORE_EstablishStaticConfigInt32(cache_config_ram_cache_algorithm, "proxy.config.cache.ram_cache.algorithm");
  IOCORE_EstablishStaticConfigInt32(cache_config_ram_cache_compress, "proxy.config.cache.ram_cache.compress");
  IOCORE_EstablishStaticConfigInt32(cache_config_ram_cache_compress_percent, "proxy.config.cache.ram_cache.compress_percent");

  IOCORE_EstablishStaticConfigInt32(cache_config_http_max_alts, "proxy.config.cache.limits.http.max_alts");
  Debug("cache_init", "proxy.config.cache.limits.http.max_alts = %d", cache_config_http_max_alts);

  IOCORE_EstablishStaticConfigInteger(cache_config_ram_cache_cutoff, "proxy.config.cache.ram_cache_cutoff");
  Debug("cache_init", "cache_config_ram_cache_cutoff = %" PRId64 " = %" PRId64 "Mb",
        cache_config_ram_cache_cutoff, cache_config_ram_cache_cutoff / (1024 * 1024));

  IOCORE_EstablishStaticConfigInt32(cache_config_permit_pinning, "proxy.config.cache.permit.pinning");
  Debug("cache_init", "proxy.config.cache.permit.pinning = %d", cache_config_permit_pinning);

  IOCORE_EstablishStaticConfigInt32(cache_config_dir_sync_frequency, "proxy.config.cache.dir.sync_frequency");
  Debug("cache_init", "proxy.config.cache.dir.sync_frequency = %d", cache_config_dir_sync_frequency);

  IOCORE_EstablishStaticConfigInt32(cache_config_vary_on_user_agent, "proxy.config.cache.vary_on_user_agent");
  Debug("cache_init", "proxy.config.cache.vary_on_user_agent = %d", cache_config_vary_on_user_agent);

  IOCORE_EstablishStaticConfigInt32(cache_config_select_alternate, "proxy.config.cache.select_alternate");
  Debug("cache_init", "proxy.config.cache.select_alternate = %d", cache_config_select_alternate);

  IOCORE_EstablishStaticConfigInt32(cache_config_max_doc_size, "proxy.config.cache.max_doc_size");
  Debug("cache_init", "proxy.config.cache.max_doc_size = %d = %dMb",
        cache_config_max_doc_size, cache_config_max_doc_size / (1024 * 1024));

  IOCORE_EstablishStaticConfigInt32(cache_config_mutex_retry_delay, "proxy.config.cache.mutex_retry_delay");
  Debug("cache_init", "proxy.config.cache.mutex_retry_delay = %dms", cache_config_mutex_retry_delay);

  // This is just here to make sure IOCORE "standalone" works, it's usually configured in RecordsConfig.cc
  IOCORE_RegisterConfigString(RECT_CONFIG, "proxy.config.config_dir", TS_BUILD_SYSCONFDIR, RECU_DYNAMIC, RECC_NULL, NULL);
  IOCORE_ReadConfigString(cache_system_config_directory, "proxy.config.config_dir", PATH_NAME_MAX);
  if (cache_system_config_directory[0] != '/') {
    // Not an absolute path so use system one
    Layout::get()->relative(cache_system_config_directory, sizeof(cache_system_config_directory), cache_system_config_directory);
  }
  Debug("cache_init", "proxy.config.config_dir = \"%s\"", cache_system_config_directory);
  if (access(cache_system_config_directory, R_OK) == -1) {
    ink_strlcpy(cache_system_config_directory, Layout::get()->sysconfdir,
                sizeof(cache_system_config_directory));
    Debug("cache_init", "proxy.config.config_dir = \"%s\"", cache_system_config_directory);
    if (access(cache_system_config_directory, R_OK) == -1) {
      fprintf(stderr,"unable to access() config dir '%s': %d, %s\n",
              cache_system_config_directory, errno, strerror(errno));
      fprintf(stderr, "please set config path via 'proxy.config.config_dir' \n");
      _exit(1);
    }
  }
  // TODO: These are left here, since they are only registered if HIT_EVACUATE is enabled.
#ifdef HIT_EVACUATE
  IOCORE_RegisterConfigInteger(RECT_CONFIG, "proxy.config.cache.hit_evacuate_percent", 0, RECU_DYNAMIC, RECC_NULL, NULL);
  IOCORE_EstablishStaticConfigInt32(cache_config_hit_evacuate_percent, "proxy.config.cache.hit_evacuate_percent");
  Debug("cache_init", "proxy.config.cache.hit_evacuate_percent = %d", cache_config_hit_evacuate_percent);

  IOCORE_RegisterConfigInteger(RECT_CONFIG, "proxy.config.cache.hit_evacuate_size_limit", 0, RECU_DYNAMIC, RECC_NULL, NULL);
  IOCORE_EstablishStaticConfigInt32(cache_config_hit_evacuate_size_limit, "proxy.config.cache.hit_evacuate_size_limit");
  Debug("cache_init", "proxy.config.cache.hit_evacuate_size_limit = %d", cache_config_hit_evacuate_size_limit);
#endif

  IOCORE_EstablishStaticConfigInt32(cache_config_force_sector_size, "proxy.config.cache.force_sector_size");
  IOCORE_EstablishStaticConfigInt32(cache_config_target_fragment_size, "proxy.config.cache.target_fragment_size");

  if (cache_config_target_fragment_size == 0)
    cache_config_target_fragment_size = DEFAULT_TARGET_FRAGMENT_SIZE;

#ifdef HTTP_CACHE
  extern int url_hash_method;

  //  # 0 - MD5 hash
  //  # 1 - MMH hash
  IOCORE_EstablishStaticConfigInt32(url_hash_method, "proxy.config.cache.url_hash_method");
  Debug("cache_init", "proxy.config.cache.url_hash_method = %d", url_hash_method);
#endif

  IOCORE_EstablishStaticConfigInt32(cache_config_max_disk_errors, "proxy.config.cache.max_disk_errors");
  Debug("cache_init", "proxy.config.cache.max_disk_errors = %d", cache_config_max_disk_errors);

  IOCORE_EstablishStaticConfigInt32(cache_config_agg_write_backlog, "proxy.config.cache.agg_write_backlog");
  Debug("cache_init", "proxy.config.cache.agg_write_backlog = %d", cache_config_agg_write_backlog);

  IOCORE_EstablishStaticConfigInt32(cache_config_enable_checksum, "proxy.config.cache.enable_checksum");
  Debug("cache_init", "proxy.config.cache.enable_checksum = %d", cache_config_enable_checksum);

  IOCORE_EstablishStaticConfigInt32(cache_config_alt_rewrite_max_size, "proxy.config.cache.alt_rewrite_max_size");
  Debug("cache_init", "proxy.config.cache.alt_rewrite_max_size = %d", cache_config_alt_rewrite_max_size);

  IOCORE_EstablishStaticConfigInt32(cache_config_read_while_writer, "proxy.config.cache.enable_read_while_writer");
  cache_config_read_while_writer = validate_rww(cache_config_read_while_writer);
  IOCORE_RegisterConfigUpdateFunc("proxy.config.cache.enable_read_while_writer", update_cache_config, NULL);
  Debug("cache_init", "proxy.config.cache.enable_read_while_writer = %d", cache_config_read_while_writer);

  register_cache_stats(cache_rsb, "proxy.process.cache");

  const char *err = NULL;
  if ((err = theCacheStore.read_config())) {
    printf("%s  failed\n", err);
    exit(1);
  }
  // XXX: The read for proxy.config.cache.storage_filename is unused!
  //
  if (theCacheStore.n_disks == 0) {
    char p[PATH_NAME_MAX + 1];
    snprintf(p, sizeof(p), "%s/", cache_system_config_directory);
    IOCORE_ReadConfigString(p + strlen(p), "proxy.config.cache.storage_filename", PATH_NAME_MAX - strlen(p) - 1);
    if (p[strlen(p) - 1] == '/' || p[strlen(p) - 1] == '\\') {
      ink_strlcat(p, "storage.config", sizeof(p));
    }
    Warning("no cache disks specified in %s: cache disabled\n", p);
    //exit(1);
  }
}

#ifdef NON_MODULAR
//----------------------------------------------------------------------------
Action *
CacheProcessor::open_read(Continuation *cont, URL *url, CacheHTTPHdr *request,
                          CacheLookupHttpConfig *params, time_t pin_in_cache, CacheFragType type)
{
#ifdef CLUSTER_CACHE
  if (cache_clustering_enabled > 0) {
    return open_read_internal(CACHE_OPEN_READ_LONG, cont, (MIOBuffer *) 0,
                              url, request, params, (CacheKey *) 0, pin_in_cache, type, (char *) 0, 0);
  }
#endif
  return caches[type]->open_read(cont, url, request, params, type);
}


//----------------------------------------------------------------------------
Action *
CacheProcessor::open_write(Continuation *cont, int expected_size, URL *url,
                           CacheHTTPHdr *request, CacheHTTPInfo *old_info, time_t pin_in_cache, CacheFragType type)
{
#ifdef CLUSTER_CACHE
  if (cache_clustering_enabled > 0) {
    INK_MD5 url_md5;
    Cache::generate_key(&url_md5, url, request);
    ClusterMachine *m = cluster_machine_at_depth(cache_hash(url_md5));

    if (m) {
      // Do remote open_write()
      INK_MD5 url_only_md5;
      Cache::generate_key(&url_only_md5, url, 0);
      return Cluster_write(cont, expected_size, (MIOBuffer *) 0, m,
                           &url_only_md5, type,
                           false, pin_in_cache, CACHE_OPEN_WRITE_LONG,
                           (CacheKey *) 0, url, request, old_info, (char *) 0, 0);
    }
  }
#endif
  return caches[type]->open_write(cont, url, request, old_info, pin_in_cache, type);
}

//----------------------------------------------------------------------------
// Note: this should not be called from from the cluster processor, or bad
// recursion could occur. This is merely a convenience wrapper.
Action *
CacheProcessor::remove(Continuation *cont, URL *url, CacheFragType frag_type)
{
  INK_MD5 md5;
  int len = 0;
  const char *hostname;

  url->MD5_get(&md5);
  hostname = url->host_get(&len);

  Debug("cache_remove", "[CacheProcessor::remove] Issuing cache delete for %s", url->string_get_ref());
#ifdef CLUSTER_CACHE
  if (cache_clustering_enabled > 0) {
    // Remove from cluster
    return remove(cont, &md5, frag_type, true, false, const_cast<char *>(hostname), len);
  }
#endif

  // Remove from local cache only.
  return caches[frag_type]->remove(cont, &md5, frag_type, true, false, const_cast<char*>(hostname), len);
}

#endif
