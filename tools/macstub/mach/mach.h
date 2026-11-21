/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * mach.h - a stand-in for the part of <mach/mach.h> of macOS the POSIX
 * back end uses (the VM statistics of src/host/posix/machine.c) for `make
 * check-macos`; the layout and the constants follow the SDK.  Never used
 * by the real build.
 */
#ifndef MACSTUB_MACH_H_
#define MACSTUB_MACH_H_

#include <stdint.h>

typedef int kern_return_t;
typedef unsigned int natural_t;
typedef int integer_t;
typedef natural_t mach_port_t;
typedef mach_port_t host_t;
typedef integer_t host_flavor_t;
typedef natural_t mach_msg_type_number_t;
typedef integer_t* host_info64_t;

#define KERN_SUCCESS   0
#define HOST_VM_INFO64 4

struct vm_statistics64
{
	natural_t free_count;
	natural_t active_count;
	natural_t inactive_count;
	natural_t wire_count;
	uint64_t zero_fill_count;
	uint64_t reactivations;
	uint64_t pageins;
	uint64_t pageouts;
	uint64_t faults;
	uint64_t cow_faults;
	uint64_t lookups;
	uint64_t hits;
	uint64_t purges;
	natural_t purgeable_count;
	natural_t speculative_count;
	uint64_t decompressions;
	uint64_t compressions;
	uint64_t swapins;
	uint64_t swapouts;
	natural_t compressor_page_count;
	natural_t throttled_count;
	natural_t external_page_count;
	natural_t internal_page_count;
	uint64_t total_uncompressed_pages_in_compressor;
};
typedef struct vm_statistics64 vm_statistics64_data_t;
#define HOST_VM_INFO64_COUNT ((mach_msg_type_number_t)(sizeof(vm_statistics64_data_t) / sizeof(integer_t)))

mach_port_t mach_host_self(void);
kern_return_t host_statistics64(host_t host_priv, host_flavor_t flavor, host_info64_t host_info64_out,
	mach_msg_type_number_t* host_info64_outCnt);

#endif // MACSTUB_MACH_H_
