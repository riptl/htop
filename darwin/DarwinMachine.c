/*
htop - DarwinMachine.c
(C) 2014 Hisham H. Muhammad
(C) 2023 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "darwin/DarwinMachine.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <Availability.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitKeys.h>

#include "CRT.h"
#include "Machine.h"
#include "XUtils.h"
#include "darwin/Platform.h"
#include "darwin/PlatformHelpers.h"
#include "generic/openzfs_sysctl.h"
#include "zfs/ZfsArcStats.h"

// Compatibility for older SDKs.
#if !defined(HAVE_DECL_KIOMAINPORTDEFAULT) || !HAVE_DECL_KIOMAINPORTDEFAULT
#define kIOMainPortDefault kIOMasterPortDefault
#endif

#ifdef HAVE_LIBIOREPORT
/* Private API of libIOReport (no public header available) */
CFMutableDictionaryRef IOReportCopyChannelsInGroup(CFStringRef group, CFStringRef subgroup, uint64_t a, uint64_t b, uint64_t c);
CFTypeRef IOReportCreateSubscription(const void* a, CFMutableDictionaryRef desiredChannels, CFMutableDictionaryRef* subbedChannels, uint64_t channelId, CFTypeRef b);
CFDictionaryRef IOReportCreateSamples(CFTypeRef subscription, CFMutableDictionaryRef subbedChannels, CFTypeRef a);
CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef prev, CFDictionaryRef curr, CFTypeRef a);
CFStringRef IOReportChannelGetChannelName(CFDictionaryRef channel);
int32_t IOReportStateGetCount(CFDictionaryRef channel);
CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef channel, int32_t index);
int64_t IOReportStateGetResidency(CFDictionaryRef channel, int32_t index);
#endif

static void DarwinMachine_getHostInfo(host_basic_info_data_t* p) {
   mach_msg_type_number_t info_size = HOST_BASIC_INFO_COUNT;

   if (0 != host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t)p, &info_size)) {
      CRT_fatalError("Unable to retrieve host info");
   }
}

static void DarwinMachine_freeCPULoadInfo(processor_cpu_load_info_t* p) {
   if (!p)
      return;

   if (!*p)
      return;

   if (0 != munmap(*p, vm_page_size)) {
      CRT_fatalError("Unable to free old CPU load information");
   }

   *p = NULL;
}

static unsigned int DarwinMachine_allocateCPULoadInfo(processor_cpu_load_info_t* p) {
   mach_msg_type_number_t info_size = sizeof(processor_cpu_load_info_t);
   natural_t cpu_count;

   // TODO Improving the accuracy of the load counts would help a lot.
   if (0 != host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpu_count, (processor_info_array_t*)p, &info_size)) {
      CRT_fatalError("Unable to retrieve CPU info");
   }

   return cpu_count;
}

static void DarwinMachine_getVMStats(DarwinMachine* this) {
#ifdef HAVE_STRUCT_VM_STATISTICS64
   mach_msg_type_number_t info_size = HOST_VM_INFO64_COUNT;

   if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info_t)&this->vm_stats, &info_size) != 0) {
      CRT_fatalError("Unable to retrieve VM statistics64");
   }
#else
   mach_msg_type_number_t info_size = HOST_VM_INFO_COUNT;

   if (host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&this->vm_stats, &info_size) != 0) {
      CRT_fatalError("Unable to retrieve VM statistics");
   }
#endif
}

#ifdef HAVE_LIBIOREPORT
/* Assign the logical CPUs to their cluster type (E or P) using the device tree */
static bool DarwinMachine_readCPUClusters(DarwinCPUFreqState* fs, unsigned int existingCPUs) {
   io_registry_entry_t cpusNode = IORegistryEntryFromPath(kIOMainPortDefault, kIODeviceTreePlane ":/cpus");
   if (!cpusNode)
      return false;

   io_iterator_t iter;
   if (IORegistryEntryGetChildIterator(cpusNode, kIODeviceTreePlane, &iter) != KERN_SUCCESS) {
      IOObjectRelease(cpusNode);
      return false;
   }

   int* cluster = xMallocArray(existingCPUs, sizeof(int));
   for (unsigned int i = 0; i < existingCPUs; i++)
      cluster[i] = -1;

   io_registry_entry_t cpu;
   while ((cpu = IOIteratorNext(iter))) {
      CFTypeRef idRef = IORegistryEntryCreateCFProperty(cpu, CFSTR("logical-cpu-id"), kCFAllocatorDefault, kNilOptions);
      CFTypeRef typeRef = IORegistryEntryCreateCFProperty(cpu, CFSTR("cluster-type"), kCFAllocatorDefault, kNilOptions);

      int id = -1;
      if (idRef && CFGetTypeID(idRef) == CFNumberGetTypeID())
         CFNumberGetValue(idRef, kCFNumberIntType, &id);

      char type = '\0';
      if (typeRef && CFGetTypeID(typeRef) == CFDataGetTypeID() && CFDataGetLength(typeRef) > 0)
         type = (char)CFDataGetBytePtr(typeRef)[0];

      if (id >= 0 && (unsigned int)id < existingCPUs) {
         if (type == 'E') {
            cluster[id] = DARWIN_CPU_CLUSTER_E;
         } else if (type == 'P') {
            cluster[id] = DARWIN_CPU_CLUSTER_P;
         }
      }

      if (idRef)
         CFRelease(idRef);
      if (typeRef)
         CFRelease(typeRef);
      IOObjectRelease(cpu);
   }

   IOObjectRelease(iter);
   IOObjectRelease(cpusNode);

   for (int c = 0; c < DARWIN_CPU_CLUSTER_COUNT; c++) {
      fs->cpus[c] = xMallocArray(existingCPUs, sizeof(unsigned int));
      fs->cpuCount[c] = 0;
      for (unsigned int i = 0; i < existingCPUs; i++) {
         if (cluster[i] == c)
            fs->cpus[c][fs->cpuCount[c]++] = i;
      }
   }

   free(cluster);

   return fs->cpuCount[DARWIN_CPU_CLUSTER_E] + fs->cpuCount[DARWIN_CPU_CLUSTER_P] > 0;
}

/* Read a DVFS table: an array of (frequency, voltage) pairs of 32-bit integers */
static void DarwinMachine_readFreqTable(io_registry_entry_t pmgr, CFStringRef key, double** table, size_t* len) {
   CFTypeRef data = IORegistryEntryCreateCFProperty(pmgr, key, kCFAllocatorDefault, kNilOptions);
   if (!data)
      return;

   size_t n = 0;
   if (CFGetTypeID(data) == CFDataGetTypeID())
      n = (size_t)CFDataGetLength(data) / (2 * sizeof(uint32_t));

   if (n > 0) {
      const UInt8* bytes = CFDataGetBytePtr(data);
      *table = xMallocArray(n, sizeof(double));
      *len = n;
      for (size_t i = 0; i < n; i++) {
         uint32_t freq;
         memcpy(&freq, bytes + i * 2 * sizeof(uint32_t), sizeof(freq));
         /* M1 to M3 report Hz, M4 and later report kHz */
         (*table)[i] = freq >= 100000000 ? freq / 1e6 : freq / 1e3;
      }
   }

   CFRelease(data);
}

static bool DarwinMachine_readFreqTables(DarwinCPUFreqState* fs) {
   io_registry_entry_t pmgr = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("pmgr"));
   if (!pmgr)
      return false;

   DarwinMachine_readFreqTable(pmgr, CFSTR("voltage-states1-sram"), &fs->table[DARWIN_CPU_CLUSTER_E], &fs->tableLen[DARWIN_CPU_CLUSTER_E]);
   DarwinMachine_readFreqTable(pmgr, CFSTR("voltage-states5-sram"), &fs->table[DARWIN_CPU_CLUSTER_P], &fs->tableLen[DARWIN_CPU_CLUSTER_P]);

   IOObjectRelease(pmgr);

   return fs->tableLen[DARWIN_CPU_CLUSTER_E] + fs->tableLen[DARWIN_CPU_CLUSTER_P] > 0;
}

static void DarwinMachine_initCPUFrequency(DarwinMachine* this) {
   DarwinCPUFreqState* fs = &this->freqState;

   if (!DarwinMachine_readFreqTables(fs))
      return;

   if (!DarwinMachine_readCPUClusters(fs, this->super.existingCPUs))
      return;

   CFMutableDictionaryRef channels = IOReportCopyChannelsInGroup(CFSTR("CPU Stats"), CFSTR("CPU Core Performance States"), 0, 0, 0);
   if (!channels)
      return;

   CFMutableDictionaryRef desired = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, channels);
   CFRelease(channels);

   CFMutableDictionaryRef subbed = NULL;
   fs->subscription = IOReportCreateSubscription(NULL, desired, &subbed, 0, NULL);
   CFRelease(desired);

   if (!fs->subscription || !subbed) {
      CRT_debug("Cannot subscribe to IOReport CPU performance states");
      if (fs->subscription)
         CFRelease(fs->subscription);
      fs->subscription = NULL;
      return;
   }

   fs->channels = subbed;
   fs->prevSample = IOReportCreateSamples(fs->subscription, fs->channels, NULL);
}

/* Average frequency over the active (non-idle) performance states of a channel */
static double DarwinMachine_channelFrequency(CFDictionaryRef channel, const double* table, size_t tableLen) {
   int32_t count = IOReportStateGetCount(channel);
   double weighted = 0.0;
   double total = 0.0;
   double minFreq = NAN;
   size_t level = 0;

   for (int32_t i = 0; i < count && level < tableLen; i++) {
      CFStringRef name = IOReportStateGetNameForIndex(channel, i);
      if (name && (CFEqual(name, CFSTR("IDLE")) || CFEqual(name, CFSTR("DOWN")) || CFEqual(name, CFSTR("OFF"))))
         continue;

      double freq = table[level++];
      if (freq > 0 && (isnan(minFreq) || freq < minFreq))
         minFreq = freq;

      double residency = (double)IOReportStateGetResidency(channel, i);
      weighted += residency * freq;
      total += residency;
   }

   /* Report the lowest frequency for cores that were idle the whole interval */
   return total > 0 ? weighted / total : minFreq;
}

static void DarwinMachine_scanCPUFrequency(DarwinMachine* this) {
   DarwinCPUFreqState* fs = &this->freqState;

   if (!fs->subscription || !fs->prevSample)
      return;

   CFDictionaryRef sample = IOReportCreateSamples(fs->subscription, fs->channels, NULL);
   if (!sample)
      return;

   CFDictionaryRef delta = IOReportCreateSamplesDelta(fs->prevSample, sample, NULL);
   CFRelease(fs->prevSample);
   fs->prevSample = sample;
   if (!delta)
      return;

   /* Channels are named e.g. "ECPU0" or "PCPU12"; they are assigned to the
      logical CPUs of the matching cluster type in order of appearance */
   unsigned int seen[DARWIN_CPU_CLUSTER_COUNT] = {0};

   CFArrayRef items = CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
   if (!items || CFGetTypeID(items) != CFArrayGetTypeID())
      goto cleanup;

   for (CFIndex i = 0; i < CFArrayGetCount(items); i++) {
      CFDictionaryRef channel = CFArrayGetValueAtIndex(items, i);
      CFStringRef name = IOReportChannelGetChannelName(channel);
      if (!name)
         continue;

      int c;
      if (CFStringFind(name, CFSTR("ECPU"), 0).location != kCFNotFound) {
         c = DARWIN_CPU_CLUSTER_E;
      } else if (CFStringFind(name, CFSTR("PCPU"), 0).location != kCFNotFound) {
         c = DARWIN_CPU_CLUSTER_P;
      } else {
         continue;
      }

      if (seen[c] >= fs->cpuCount[c] || !fs->tableLen[c])
         continue;

      unsigned int cpu = fs->cpus[c][seen[c]++];
      this->cpuFrequency[cpu] = DarwinMachine_channelFrequency(channel, fs->table[c], fs->tableLen[c]);
   }

cleanup:
   CFRelease(delta);
}

static void DarwinMachine_doneCPUFrequency(DarwinMachine* this) {
   DarwinCPUFreqState* fs = &this->freqState;

   if (fs->prevSample)
      CFRelease(fs->prevSample);
   if (fs->channels)
      CFRelease(fs->channels);
   if (fs->subscription)
      CFRelease(fs->subscription);

   for (int c = 0; c < DARWIN_CPU_CLUSTER_COUNT; c++) {
      free(fs->table[c]);
      free(fs->cpus[c]);
   }
}
#endif /* HAVE_LIBIOREPORT */

/* Nominal frequency on Intel Macs; not available on Apple Silicon */
static void DarwinMachine_initNominalCPUFrequency(DarwinMachine* this) {
   uint64_t freq = 0;
   size_t size = sizeof(freq);
   if (sysctlbyname("hw.cpufrequency", &freq, &size, NULL, 0) != 0 || freq == 0)
      return;

   for (unsigned int i = 0; i < this->super.existingCPUs; i++)
      this->cpuFrequency[i] = (double)freq / 1e6;
}

void Machine_scan(Machine* super) {
   DarwinMachine* host = (DarwinMachine*) super;

   /* Update the global data (CPU times and VM stats) */
   DarwinMachine_freeCPULoadInfo(&host->prev_load);
   host->prev_load = host->curr_load;
   DarwinMachine_allocateCPULoadInfo(&host->curr_load);
   DarwinMachine_getVMStats(host);
   openzfs_sysctl_updateArcStats(&host->zfs);

#ifdef HAVE_LIBIOREPORT
   DarwinMachine_scanCPUFrequency(host);
#endif
}

Machine* Machine_new(UsersTable* usersTable, uid_t userId) {
   DarwinMachine* this = xCalloc(1, sizeof(DarwinMachine));
   Machine* super = &this->super;

   Machine_init(super, usersTable, userId);

   /* Initialize the CPU information */
   super->activeCPUs = DarwinMachine_allocateCPULoadInfo(&this->prev_load);
   super->existingCPUs = super->activeCPUs;
   DarwinMachine_getHostInfo(&this->host_info);
   DarwinMachine_allocateCPULoadInfo(&this->curr_load);

   /* Initialize the VM statistics */
   DarwinMachine_getVMStats(this);

   /* Initialize the ZFS kstats, if zfs.kext loaded */
   openzfs_sysctl_init(&this->zfs);
   openzfs_sysctl_updateArcStats(&this->zfs);

   this->GPUService = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOGPU"));
   if (!this->GPUService) {
      CRT_debug("Cannot initialize IOGPU service");
   }

   /* Initialize the CPU frequencies */
   this->cpuFrequency = xMallocArray(super->existingCPUs, sizeof(double));
   for (unsigned int i = 0; i < super->existingCPUs; i++)
      this->cpuFrequency[i] = NAN;

#ifdef HAVE_LIBIOREPORT
   DarwinMachine_initCPUFrequency(this);
   if (!this->freqState.subscription)
      DarwinMachine_initNominalCPUFrequency(this);
#else
   DarwinMachine_initNominalCPUFrequency(this);
#endif

   return super;
}

void Machine_delete(Machine* super) {
   DarwinMachine* this = (DarwinMachine*) super;

   IOObjectRelease(this->GPUService);

#ifdef HAVE_LIBIOREPORT
   DarwinMachine_doneCPUFrequency(this);
#endif
   free(this->cpuFrequency);

   DarwinMachine_freeCPULoadInfo(&this->prev_load);

   Machine_done(super);
   free(this);
}

bool Machine_isCPUonline(const Machine* host, unsigned int id) {
   assert(id < host->existingCPUs);

   // TODO: support offline CPUs and hot swapping
   (void) host; (void) id;

   return true;
}

int Machine_getCPUPhysicalCoreID(const Machine* host, unsigned int id) {
   assert(id < host->existingCPUs);
   (void) host;
   return (int)id;
}

int Machine_getCPUThreadIndex(const Machine* host, unsigned int id) {
   assert(id < host->existingCPUs);
   (void) host; (void) id;
   return 0;
}
