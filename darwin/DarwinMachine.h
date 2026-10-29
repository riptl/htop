#ifndef HEADER_DarwinMachine
#define HEADER_DarwinMachine
/*
htop - DarwinMachine.h
(C) 2014 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include <stddef.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>

#include "Machine.h"
#include "zfs/ZfsArcStats.h"


typedef enum DarwinCPUCluster_ {
   DARWIN_CPU_CLUSTER_E,
   DARWIN_CPU_CLUSTER_P,
   DARWIN_CPU_CLUSTER_COUNT
} DarwinCPUCluster;

typedef struct DarwinCPUFreqState_ {
   /* Opaque IOReport subscription (private API, see DarwinMachine.c) */
   CFTypeRef subscription;
   CFMutableDictionaryRef channels;
   CFDictionaryRef prevSample;

   /* DVFS frequency table in MHz per cluster type */
   double* table[DARWIN_CPU_CLUSTER_COUNT];
   size_t tableLen[DARWIN_CPU_CLUSTER_COUNT];

   /* Logical CPU ids per cluster type, in ascending order */
   unsigned int* cpus[DARWIN_CPU_CLUSTER_COUNT];
   unsigned int cpuCount[DARWIN_CPU_CLUSTER_COUNT];
} DarwinCPUFreqState;

typedef struct DarwinMachine_ {
   Machine super;

   host_basic_info_data_t host_info;
   vm_statistics64_data_t vm_stats;
   processor_cpu_load_info_t prev_load;
   processor_cpu_load_info_t curr_load;

   /* Current frequency in MHz per logical CPU, NAN if unknown */
   double* cpuFrequency;
   DarwinCPUFreqState freqState;

   io_service_t GPUService;

   ZfsArcStats zfs;
} DarwinMachine;

#endif
