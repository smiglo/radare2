/* radare - LGPL - Copyright 2019 - GustavoLCR */

#include <r_core.h>
#include <TlHelp32.h>
#include <windows_heap.h>
#include "..\..\debug\p\native\maps\windows_maps.h"

/*
*	Viewer discretion advised: Spaghetti code ahead
*	Some Code references:
*	https://securityxploded.com/enumheaps.php
*	https://bitbucket.org/evolution536/crysearch-memory-scanner/
*	https://processhacker.sourceforge.io
*	http://www.tssc.de/winint
*	https://www.nirsoft.net/kernel_struct/vista/
*	https://github.com/yoichi/HeapStat/blob/master/heapstat.cpp
*	https://doxygen.reactos.org/
*
*	References:
*	Windows NT(2000) Native API Reference (Book)
*	Papers: 
*	http://illmatics.com/Understanding_the_LFH.pdf
*	http://illmatics.com/Windows%208%20Heap%20Internals.pdf
*	https://www.blackhat.com/docs/us-16/materials/us-16-Yason-Windows-10-Segment-Heap-Internals-wp.pdf
*
*	This code has 2 different approaches to getting the heap info:
*		1) Calling InitHeapInfo with both PDI_HEAPS and PDI_HEAP_BLOCKS.
*			This will fill a buffer with HeapBlockBasicInfo like structures which
*			is then walked through by calling GetFirstHeapBlock and subsequently GetNextHeapBlock
*			(see 1st link). This approach is the more generic one as it uses Windows functions.
*			Unfortunately it fails to offer more detailed information about each block (although it is possible to get this info later) and
*			also fails misteriously once the count of allocated blocks reach a certain threshold (1mil or so) or if segment heap is active for the
*			program (in this case everything locks in the next call for the function)
*		2) In case 1 fails, Calling GetHeapBlocks, which will manually read and parse (poorly :[ ) each block.
*			First it calls InitHeapInfo	with only the PDI_HEAPS flag, with the only objective of getting a list of heap header addresses. It will then
*			do the job that InitHeapInfo would do if it was called with PDI_HEAP_BLOCKS as well, filling a buffer with HeapBlockBasicInfo structures that
*			can also be walked with GetFirstHeapBlock and GetNextHeapBlock (and HeapBlockExtraInfo when needed).
*
*	TODO:
*		Var to select algorithm?
*		x86 vs x64 vs WOW64
*		Graphs
*		Print structures
*		Make sure GetHeapBlocks actually works
*		Maybe instead of using hardcoded structs we can get the offsets from ntdll.pdb
*/

#define PDI_MODULES         0x01
#define PDI_HEAPS           0x04
#define PDI_HEAP_TAGS       0x08
#define PDI_HEAP_BLOCKS     0x10
#define PDI_HEAP_ENTRIES_EX 0x200

#define CHECK_INFO(heapInfo)\
	if (!heapInfo) {\
		eprintf ("It wasn't possible to get the heap information\n");\
		return;\
	}\
	if (!heapInfo->count) {\
		r_cons_print ("No heaps for this process\n");\
		return;\
	}

#define UPDATE_FLAGS(hb, flags)\
	if (((flags) & 0xf1) || ((flags) & 0x0200)) {\
		hb->dwFlags = LF32_FIXED;\
	} else if ((flags) & 0x20) {\
		hb->dwFlags = LF32_MOVEABLE;\
	} else if ((flags) & 0x0100) {\
		hb->dwFlags = LF32_FREE;\
	}\
	hb->dwFlags |= ((flags) >> SHIFT) << SHIFT;

static char *get_type(WPARAM flags) {
	char *state = "";
	switch (flags & 0xFFFF) {
	case LF32_FIXED:
		state = "(FIXED)";
		break;
	case LF32_FREE:
		state = "(FREE)";
		break;
	case LF32_MOVEABLE:
		state = "(MOVEABLE)";
		break;
	}
	char *heaptype = "";
	if (flags & SEGMENT_HEAP_BLOCK) {
		heaptype = "Segment";
	} else if (flags & NT_BLOCK) {
		heaptype =  "NT";
	}
	char *type = "";
	if (flags & LFH_BLOCK) {
		type = "/LFH";
	} else if (flags & LARGE_BLOCK) {
		type = "/LARGE";
	} else if (flags & BACKEND_BLOCK) {
		type = "/BACKEND";
	} else if (flags & VS_BLOCK) {
		type = "/VS";
	}
	return r_str_newf ("%s %s%s", state, heaptype, type);
}

static bool init_func() {
	HANDLE ntdll = LoadLibrary (TEXT ("ntdll.dll"));
	if (!ntdll) {
		return false;
	}
	if (!RtlCreateQueryDebugBuffer) {
		RtlCreateQueryDebugBuffer = (PDEBUG_BUFFER (NTAPI *)(DWORD, BOOLEAN))GetProcAddress (ntdll, "RtlCreateQueryDebugBuffer");
	}
	if (!RtlQueryProcessDebugInformation) {
		RtlQueryProcessDebugInformation = (NTSTATUS (NTAPI *)(DWORD, DWORD, PDEBUG_BUFFER))GetProcAddress (ntdll, "RtlQueryProcessDebugInformation");
	}
	if (!RtlDestroyQueryDebugBuffer) {
		RtlDestroyQueryDebugBuffer = (NTSTATUS (NTAPI *)(PDEBUG_BUFFER))GetProcAddress (ntdll, "RtlDestroyQueryDebugBuffer");
	}
	return true;
}

static bool is_segment_heap(HANDLE h_proc, PVOID heapBase) {
	HEAP heap;
	if (ReadProcessMemory (h_proc, heapBase, &heap, sizeof (HEAP), NULL)) {
		if (heap.SegmentSignature == 0xddeeddee) {
			return true;
		}
	}
	return false;
}

// These functions are basically Heap32First and Heap32Next but faster
static bool GetFirstHeapBlock(PDEBUG_HEAP_INFORMATION heapInfo, PHeapBlock hb) {
	r_return_val_if_fail (heapInfo && hb, false);
	PHeapBlockBasicInfo block;

	hb->index = 0;
	hb->dwAddress = 0;
	hb->dwFlags = 0;
	hb->extraInfo = NULL;

	block = (PHeapBlockBasicInfo)heapInfo->Blocks;
	if (!block) {
		return false;
	}

	SIZE_T index = hb->index;
	do {
		if (index > heapInfo->BlockCount) {
			return false;
		}
		hb->dwAddress = (void *)block[index].address;
		hb->dwSize = block->size;
		if (block[index].extra & EXTRA_FLAG) {
			PHeapBlockExtraInfo extra = (PHeapBlockExtraInfo)(block[index].extra & ~EXTRA_FLAG);
			hb->dwSize -= extra->unusedBytes;
			hb->extraInfo = extra;
			(WPARAM)hb->dwAddress += extra->granularity;
		} else {
			(WPARAM)hb->dwAddress += heapInfo->Granularity;
			hb->extraInfo = NULL;
		}
		index++;
	} while (block[index].flags & 2);

	hb->index = index;

	WPARAM flags = block[hb->index].flags;
	UPDATE_FLAGS (hb, flags);
	return true;
}

static bool GetNextHeapBlock(PDEBUG_HEAP_INFORMATION heapInfo, PHeapBlock hb) {
	r_return_val_if_fail (heapInfo && hb, false);
	PHeapBlockBasicInfo block;

	block = (PHeapBlockBasicInfo)heapInfo->Blocks;
	SIZE_T index = hb->index;

	if (index > heapInfo->BlockCount) {
		return false;
	}

	if (block[index].flags & 2) {
		do {
			if (index > heapInfo->BlockCount) {
				return false;
			}

			// new address = curBlockAddress + Granularity;
			hb->dwAddress = (void *)(block[index].address + heapInfo->Granularity);

			index++;
			hb->dwSize = block->size;
		} while (block[index].flags & 2);
		hb->index = index;
	} else {
		hb->dwSize = block[index].size;
		if (block[index].extra & EXTRA_FLAG) {
			PHeapBlockExtraInfo extra = (PHeapBlockExtraInfo)(block[index].extra & ~EXTRA_FLAG);
			hb->extraInfo = extra;
			hb->dwSize -= extra->unusedBytes;
			hb->dwAddress = (void *)(block[index].address + extra->granularity);
		} else {
			hb->extraInfo = NULL;
			(WPARAM)hb->dwAddress += hb->dwSize;
		}
		hb->index++;
	}

	WPARAM flags;
	if (block[index].extra & EXTRA_FLAG) {
		flags = block[index].flags;
	} else {
		flags = (USHORT)block[index].flags;
	}
	UPDATE_FLAGS (hb, flags);

	return true;
}

static void free_extra_info(PDEBUG_HEAP_INFORMATION heap) {
	r_return_if_fail (heap);
	HeapBlock hb;
	if (GetFirstHeapBlock (heap, &hb)) {
		do {
			R_FREE (hb.extraInfo);
		} while (GetNextHeapBlock (heap, &hb));
	}
}

static WPARAM GetNtDllOffset(RDebug *dbg) {
	r_return_val_if_fail (dbg, 0);
	RList *map_list = r_w32_dbg_maps (dbg);
	RListIter *iter;
	RDebugMap *map;
	WPARAM ntdllOffset = 0;
	// Get ntdll .data location
	r_list_foreach (map_list, iter, map) {
		if (strstr (map->name, "ntdll.dll | .data")) {
			ntdllOffset = map->addr;
			break;
		}
	}
	r_list_free (map_list);
	return ntdllOffset;
}

static WPARAM GetLFHKey(RDebug *dbg, HANDLE h_proc, bool segment) {
	r_return_val_if_fail (dbg, 0);
	WPARAM lfhKey = 0;
	WPARAM lfhKeyLocation;
	WPARAM ntdllOffset = GetNtDllOffset (dbg);

	if (ntdllOffset) {
		if (segment) {
			WPARAM RtlpHpHeapGlobalsOffset = ntdllOffset + 0x44A0; // ntdll!RtlpHpHeapGlobals
			lfhKeyLocation = RtlpHpHeapGlobalsOffset + sizeof (WPARAM);
		} else {
			lfhKeyLocation = ntdllOffset + 0x7508; // ntdll!RtlpLFHKey
		}
		if (!ReadProcessMemory (h_proc, (PVOID)lfhKeyLocation, &lfhKey, sizeof (WPARAM), NULL)) {
			r_sys_perror ("ReadProcessMemory");
			eprintf ("LFH key not found.\n");
		}
	}
	return lfhKey;
}

static bool DecodeHeapEntry(RDebug *dbg, PHEAP heap, PHEAP_ENTRY entry) {
	r_return_val_if_fail (heap && entry, false);
	if (dbg->bits == R_SYS_BITS_64) {
		(WPARAM)entry += dbg->bits;
	}
	if (heap->EncodeFlagMask && (*(UINT32 *)entry & heap->EncodeFlagMask)) {
		if (dbg->bits == R_SYS_BITS_64) {
			(WPARAM)heap += dbg->bits;
		}
		*(WPARAM *)entry ^= *(WPARAM *)&heap->Encoding;
	}
	return !(((BYTE *)entry)[0] ^ ((BYTE *)entry)[1] ^ ((BYTE *)entry)[2] ^ ((BYTE *)entry)[3]);
}

static bool DecodeLFHEntry (RDebug *dbg, PHEAP heap, PHEAP_ENTRY entry, PHEAP_USERDATA_HEADER userBlocks, WPARAM key, WPARAM addr) {
	r_return_val_if_fail (heap && entry, false);
	if (dbg->bits == R_SYS_BITS_64) {
		(WPARAM)entry += dbg->bits;
	}

	if (heap->EncodeFlagMask) {
		*(DWORD *)entry ^= PtrToInt (heap->BaseAddress) ^ (DWORD)(((DWORD)addr - PtrToInt (userBlocks)) << 0xC) ^ (DWORD)key ^ (addr >> 4);
	}
	return !(((BYTE *)entry)[0] ^ ((BYTE *)entry)[1] ^ ((BYTE *)entry)[2] ^ ((BYTE *)entry)[3]);
}

/*
*	This function may fail with PDI_HEAP_BLOCKS if:
*		There's too many allocations
*		The Segment Heap is activated (will block next time called)
*		Notes:
*			Some LFH allocations seem misaligned
*/
static PDEBUG_BUFFER InitHeapInfo(DWORD pid, DWORD mask) {
	// Make sure it is not segment heap to avoid lockups
	if (mask & PDI_HEAP_BLOCKS) {
		PDEBUG_BUFFER db = InitHeapInfo (pid, PDI_HEAPS);
		if (db) {
			HANDLE h_proc = OpenProcess (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
			PHeapInformation heaps = db->HeapInformation;
			for (int i = 0; i < heaps->count; i++) {
				DEBUG_HEAP_INFORMATION heap = heaps->heaps[i];
				if (is_segment_heap (h_proc, heap.Base)) {
					RtlDestroyQueryDebugBuffer (db);
					return NULL;
				}
			}
			RtlDestroyQueryDebugBuffer (db);
		} else {
			return NULL;
		}
	}
	int res;
	PDEBUG_BUFFER db = RtlCreateQueryDebugBuffer (0, FALSE);
	res = RtlQueryProcessDebugInformation (pid, mask, db);
	if (res) {
		// why after it fails the first time it blocks on the second? Thats annoying
		// It stops blocking if i pause radare in the debugger. is it a race?
		// why it fails with 1000000 allocs? also with processes with segment heap enabled?
		RtlDestroyQueryDebugBuffer (db);
		r_sys_perror ("InitHeapInfo");
		return NULL;
	}
	return db;
}

#define GROW_BLOCKS()\
	if (allocated <= count * sizeof (HeapBlockBasicInfo)) {\
		SIZE_T old_alloc = allocated;\
		allocated *= 2;\
		PVOID tmp = blocks;\
		blocks = realloc (blocks, allocated);\
		if (!blocks) {\
			blocks = tmp;\
			goto err;\
		}\
		memset ((BYTE *)blocks + old_alloc, 0, old_alloc);\
	}

#define GROW_PBLOCKS()\
	if (*allocated <= *count * sizeof (HeapBlockBasicInfo)) {\
		SIZE_T old_alloc = *allocated;\
		*allocated *= 2;\
		PVOID tmp = *blocks;\
		tmp = realloc (*blocks, *allocated);\
		if (!tmp) {\
			return false;\
		}\
		*blocks = tmp;\
		memset ((BYTE *)(*blocks) + old_alloc, 0, old_alloc);\
	}

static bool __lfh_segment_loop(HANDLE h_proc, PHeapBlockBasicInfo *blocks, SIZE_T *allocated, WPARAM lfhKey, WPARAM *count, WPARAM first, WPARAM next) {
	while ((first != next) && next) {
		HEAP_LFH_SUBSEGMENT subsegment;
		ReadProcessMemory (h_proc, (void *)next, &subsegment, sizeof (HEAP_LFH_SUBSEGMENT), NULL);
		subsegment.BlockOffsets.EncodedData ^= (DWORD)lfhKey ^ ((DWORD)next >> 0xC);
		WPARAM mask = 1, offset = 0;
		for (int l = 0; l < subsegment.BlockCount; l++) {
			if (!mask) {
				mask = 1;
				offset++;
				ReadProcessMemory (h_proc, (WPARAM *)(next + offsetof (HEAP_LFH_SUBSEGMENT, BlockBitmap)) + offset,
					&subsegment.BlockBitmap, sizeof (WPARAM), NULL);
			}
			if (subsegment.BlockBitmap[0] & mask) {
				GROW_PBLOCKS ();
				WPARAM off = (WPARAM)subsegment.BlockOffsets.FirstBlockOffset + l * (WPARAM)subsegment.BlockOffsets.BlockSize;
				(*blocks)[*count].address = next + off;
				(*blocks)[*count].size = subsegment.BlockOffsets.BlockSize;
				(*blocks)[*count].flags = 1 | SEGMENT_HEAP_BLOCK | LFH_BLOCK;
				PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
				if (!extra) {
					return false;
				}
				extra->segment = next;
				extra->granularity = sizeof (HEAP_ENTRY);
				(*blocks)[*count].extra = EXTRA_FLAG | (WPARAM)extra;
				*count += 1;
			}
			mask <<= 2;
		}
		next = (WPARAM)subsegment.ListEntry.Flink;
	}
	return true;
}

static bool GetSegmentHeapBlocks(RDebug *dbg, HANDLE h_proc, PVOID heapBase, PHeapBlockBasicInfo *blocks, WPARAM *count, SIZE_T *allocated) {
	r_return_val_if_fail (h_proc && blocks && count && allocated, false);
	WPARAM bytesRead;
	SEGMENT_HEAP segheapHeader;
	ReadProcessMemory (h_proc, heapBase, &segheapHeader, sizeof (SEGMENT_HEAP), &bytesRead);

	if (segheapHeader.Signature != 0xddeeddee) {
		return false;
	}

	WPARAM RtlpHpHeapGlobalsOffset = GetNtDllOffset (dbg) + 0x44A0; // ntdll!RtlpHpHeapGlobals

	WPARAM lfhKey;
	WPARAM lfhKeyLocation = RtlpHpHeapGlobalsOffset + sizeof (WPARAM);
	if (!ReadProcessMemory (h_proc, (PVOID)lfhKeyLocation, &lfhKey, sizeof (WPARAM), &bytesRead)) {
		r_sys_perror ("ReadProcessMemory");
		eprintf ("LFH key not found.\n");
		return false;
	}

	// LFH
	byte numBuckets = _countof (segheapHeader.LfhContext.Buckets);
	for (int j = 0; j < numBuckets; j++) {
		if ((WPARAM)segheapHeader.LfhContext.Buckets[j] & 1) {
			continue;
		}
		HEAP_LFH_BUCKET bucket;
		ReadProcessMemory (h_proc, segheapHeader.LfhContext.Buckets[j], &bucket, sizeof (HEAP_LFH_BUCKET), &bytesRead);
		HEAP_LFH_AFFINITY_SLOT affinitySlot, *paffinitySlot;
		ReadProcessMemory (h_proc, bucket.AffinitySlots, &paffinitySlot, sizeof (PHEAP_LFH_AFFINITY_SLOT), &bytesRead);
		bucket.AffinitySlots++;
		ReadProcessMemory (h_proc, paffinitySlot, &affinitySlot, sizeof (HEAP_LFH_AFFINITY_SLOT), &bytesRead);
		WPARAM first = (WPARAM)paffinitySlot + offsetof (HEAP_LFH_SUBSEGMENT_OWNER, AvailableSubsegmentList);
		WPARAM next = (WPARAM)affinitySlot.State.AvailableSubsegmentList.Flink;
		if (!__lfh_segment_loop (h_proc, blocks, allocated, lfhKey, count, first, next)) {
			return false;
		}
		first = (WPARAM)paffinitySlot + offsetof (HEAP_LFH_SUBSEGMENT_OWNER, FullSubsegmentList);
		next = (WPARAM)affinitySlot.State.FullSubsegmentList.Flink;
		if (!__lfh_segment_loop (h_proc, blocks, allocated, lfhKey, count, first, next)) {
			return false;
		}
	}

	// Large Blocks
	if (segheapHeader.LargeAllocMetadata.Root) {
		PRTL_BALANCED_NODE node = malloc (sizeof (RTL_BALANCED_NODE));
		RStack *s = r_stack_new (segheapHeader.LargeReservedPages);
		PRTL_BALANCED_NODE curr = segheapHeader.LargeAllocMetadata.Root;
		do { // while (!r_stack_is_empty(s));
			GROW_PBLOCKS ();
			while (curr) {
				r_stack_push (s, curr);
				ReadProcessMemory (h_proc, curr, node, sizeof (RTL_BALANCED_NODE), &bytesRead);
				curr = node->Left;
			};
			curr = (PRTL_BALANCED_NODE)r_stack_pop (s);
			HEAP_LARGE_ALLOC_DATA entry;
			ReadProcessMemory (h_proc, curr, &entry, sizeof (HEAP_LARGE_ALLOC_DATA), &bytesRead);
			(*blocks)[*count].address = entry.VirtualAddess - entry.UnusedBytes; // This is a union
			(*blocks)[*count].flags = 1 | SEGMENT_HEAP_BLOCK | LARGE_BLOCK;
			(*blocks)[*count].size = ((entry.AllocatedPages >> 12) << 12);
			PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
			if (!extra) {
				return false;
			}
			extra->unusedBytes = entry.UnusedBytes;
			ReadProcessMemory (h_proc, (void *)(*blocks)[*count].address, &extra->granularity, sizeof (USHORT), &bytesRead);
			(*blocks)[*count].extra = EXTRA_FLAG | (WPARAM)extra;
			curr = entry.TreeNode.Right;
			*count += 1;
		} while (curr || !r_stack_is_empty (s));
		r_stack_free (s);
		free (node);
	}

	WPARAM RtlpHpHeapGlobal;
	ReadProcessMemory (h_proc, (PVOID)RtlpHpHeapGlobalsOffset, &RtlpHpHeapGlobal, sizeof (WPARAM), &bytesRead);
	// Backend Blocks (And VS)
	for (int i = 0; i < 2; i++) {
		HEAP_SEG_CONTEXT ctx = segheapHeader.SegContexts[i];
		WPARAM ctxFirstEntry = (WPARAM)heapBase + offsetof (SEGMENT_HEAP, SegContexts) + sizeof (HEAP_SEG_CONTEXT) * i + offsetof (HEAP_SEG_CONTEXT, SegmentListHead);
		HEAP_PAGE_SEGMENT pageSegment;
		WPARAM currPageSegment = (WPARAM)ctx.SegmentListHead.Flink;
		do {
			if (!ReadProcessMemory (h_proc, (PVOID)currPageSegment, &pageSegment, sizeof (HEAP_PAGE_SEGMENT), &bytesRead)) {
				break;
			}
			for (WPARAM j = 2; j < 256; j++) {
				if ((pageSegment.DescArray[j].RangeFlags &
					(PAGE_RANGE_FLAGS_FIRST | PAGE_RANGE_FLAGS_ALLOCATED)) ==
					(PAGE_RANGE_FLAGS_FIRST | PAGE_RANGE_FLAGS_ALLOCATED)) {
					GROW_PBLOCKS ();
					(*blocks)[*count].address = currPageSegment + j * 0x1000;
					(*blocks)[*count].size = (WPARAM)pageSegment.DescArray[j].UnitSize * 0x1000;
					(*blocks)[*count].flags = SEGMENT_HEAP_BLOCK | BACKEND_BLOCK | 1;
					PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
					if (!extra) {
						return false;
					}
					extra->segment = currPageSegment;
					extra->unusedBytes = pageSegment.DescArray[j].UnusedBytes;
					(*blocks)[*count].extra = EXTRA_FLAG | (WPARAM)extra;
					*count += 1;
				}
				// Hack (i dont know if all blocks like this are VS or not)
				if (pageSegment.DescArray[j].RangeFlags & 0xF && pageSegment.DescArray[j].UnusedBytes == 0x1000) {
					HEAP_VS_SUBSEGMENT vsSubsegment;
					WPARAM start, from = currPageSegment + j * 0x1000;
					ReadProcessMemory (h_proc, (PVOID)from, &vsSubsegment, sizeof (HEAP_VS_SUBSEGMENT), &bytesRead);
					// Walk through subsegment
					start = from += sizeof (HEAP_VS_SUBSEGMENT);
					while (from < (WPARAM)start + vsSubsegment.Size * sizeof (HEAP_VS_CHUNK_HEADER)) {
						HEAP_VS_CHUNK_HEADER vsChunk;
						ReadProcessMemory (h_proc, (PVOID)from, &vsChunk, sizeof (HEAP_VS_CHUNK_HEADER), &bytesRead);
						vsChunk.Sizes.HeaderBits ^= from ^ RtlpHpHeapGlobal;
						WPARAM sz = vsChunk.Sizes.UnsafeSize * sizeof (HEAP_VS_CHUNK_HEADER);
						if (vsChunk.Sizes.Allocated) {
							GROW_PBLOCKS ();
							(*blocks)[*count].address = from;
							(*blocks)[*count].size = sz;
							(*blocks)[*count].flags = VS_BLOCK | SEGMENT_HEAP_BLOCK | 1;
							PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
							if (!extra) {
								return false;
							}
							extra->granularity = sizeof (HEAP_VS_CHUNK_HEADER) * 2;
							(*blocks)[*count].extra = EXTRA_FLAG | (WPARAM)extra;
							*count += 1;
						}
						from += sz;
					}
				}
			}
			currPageSegment = (WPARAM)pageSegment.ListEntry.Flink;
		} while (currPageSegment && currPageSegment != ctxFirstEntry);
	}
	return true;
}

static PDEBUG_BUFFER GetHeapBlocks(DWORD pid, RDebug *dbg) {
	/*
		TODO:
			Break this behemoth
			x86 vs x64 vs WOW64	(use dbg->bits or new structs or just a big union with both versions)
	*/
	if (_M_X64 && dbg->bits == R_SYS_BITS_32) {
		return NULL; // Nope nope nope
	}
	WPARAM bytesRead;
	HANDLE h_proc = NULL;
	PDEBUG_BUFFER db = InitHeapInfo (pid, PDI_HEAPS);
	if (!db || !db->HeapInformation) {
		R_LOG_ERROR ("InitHeapInfo Failed\n");
		goto err;
	}
	h_proc = OpenProcess (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!h_proc) {
		R_LOG_ERROR ("OpenProcess failed\n");
		goto err;
	}

	WPARAM lfhKey = GetLFHKey (dbg, h_proc, false);

	if (!lfhKey) {
		eprintf ("GetHeapBlocks: Failed to get LFH key.\n");
		goto err;
	}

	PHeapInformation heapInfo = db->HeapInformation;
	int i;
	for (i = 0; i < heapInfo->count; i++) {
		WPARAM from = 0;
		ut64 count = 0;
		PDEBUG_HEAP_INFORMATION heap = &heapInfo->heaps[i];
		HEAP_ENTRY heapEntry;
		HEAP heapHeader;
		const SIZE_T sz_entry = sizeof (HEAP_ENTRY);
		ReadProcessMemory (h_proc, heap->Base, &heapHeader, sizeof (HEAP), &bytesRead);

		SIZE_T allocated = 128 * sizeof (HeapBlockBasicInfo);
		PHeapBlockBasicInfo blocks = calloc (allocated, 1);
		if (!blocks) {
			R_LOG_ERROR ("Memory Allocation failed\n");
			goto err;
		}

		// SEGMENT_HEAP
		if (heapHeader.SegmentSignature == 0xddeeddee) {
			bool ret = GetSegmentHeapBlocks (dbg, h_proc, heap->Base, &blocks, &count, &allocated);
			heap->Blocks = blocks;
			heap->BlockCount = count;
			if (!ret) {
				goto err;
			}
			continue;
		}

		// VirtualAlloc'd blocks
		PLIST_ENTRY fentry = (PVOID)((WPARAM)heapHeader.BaseAddress + offsetof (HEAP, VirtualAllocdBlocks));
		PLIST_ENTRY entry = heapHeader.VirtualAllocdBlocks.Flink;
		while (entry && (entry != fentry)) {
			HEAP_VIRTUAL_ALLOC_ENTRY vAlloc;
			ReadProcessMemory (h_proc, entry, &vAlloc, sizeof (HEAP_VIRTUAL_ALLOC_ENTRY), &bytesRead);
			DecodeHeapEntry (dbg, &heapHeader, &vAlloc.BusyBlock);
			GROW_BLOCKS ();
			blocks[count].address = (WPARAM)entry;
			blocks[count].flags = 1 | (vAlloc.BusyBlock.Flags | NT_BLOCK | LARGE_BLOCK) & ~2ULL;
			blocks[count].size = vAlloc.ReserveSize;
			PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
			if (!extra) {
				goto err;
			}
			extra->granularity = sizeof (HEAP_VIRTUAL_ALLOC_ENTRY);
			extra->unusedBytes = vAlloc.ReserveSize - vAlloc.CommitSize;
			blocks[count].extra = EXTRA_FLAG | (WPARAM)extra;
			count++;
			entry = vAlloc.Entry.Flink;
		}

		// LFH Activated
		if (heapHeader.FrontEndHeap && heapHeader.FrontEndHeapType == 0x2) {
			LFH_HEAP lfhHeader;
			if (!ReadProcessMemory (h_proc, heapHeader.FrontEndHeap, &lfhHeader, sizeof (LFH_HEAP), &bytesRead)) {
				r_sys_perror ("ReadProcessMemory");
				goto err;
			}

			PLIST_ENTRY curEntry, firstEntry = (PVOID)((WPARAM)heapHeader.FrontEndHeap + offsetof (LFH_HEAP, SubSegmentZones));
			curEntry = lfhHeader.SubSegmentZones.Flink;

			// Loops through all _HEAP_SUBSEGMENTs
			do { // (curEntry != firstEntry)
				HEAP_LOCAL_SEGMENT_INFO info;
				HEAP_LOCAL_DATA localData;
				HEAP_SUBSEGMENT subsegment;
				HEAP_USERDATA_HEADER userdata;
				LFH_BLOCK_ZONE blockZone;

				WPARAM curSubsegment = (WPARAM)(curEntry + 2);
				int next = 0;
				do { // (next < blockZone.NextIndex)
					if (!ReadProcessMemory (h_proc, (PVOID)curSubsegment, &subsegment, sizeof (HEAP_SUBSEGMENT), &bytesRead)
						|| !subsegment.BlockSize
						|| !ReadProcessMemory (h_proc, subsegment.LocalInfo, &info, sizeof (HEAP_LOCAL_SEGMENT_INFO), &bytesRead)
						|| !ReadProcessMemory (h_proc, info.LocalData, &localData, sizeof (HEAP_LOCAL_DATA), &bytesRead)
						|| !ReadProcessMemory (h_proc, localData.CrtZone, &blockZone, sizeof (LFH_BLOCK_ZONE), &bytesRead)) {
						break;
					}

					size_t sz = subsegment.BlockSize * sizeof (HEAP_ENTRY);
					ReadProcessMemory (h_proc, subsegment.UserBlocks, &userdata, sizeof (HEAP_USERDATA_HEADER), &bytesRead);
					userdata.EncodedOffsets.StrideAndOffset ^= PtrToInt (subsegment.UserBlocks) ^ PtrToInt (heapHeader.FrontEndHeap) ^ (WPARAM)lfhKey;
					size_t bitmapsz = (userdata.BusyBitmap.SizeOfBitMap + 8 - userdata.BusyBitmap.SizeOfBitMap % 8) / 8;
					WPARAM *bitmap = calloc (bitmapsz > sizeof (WPARAM) ? bitmapsz : sizeof (WPARAM), 1);
					if (!bitmap) {
						goto err;
					}
					ReadProcessMemory (h_proc, userdata.BusyBitmap.Buffer, bitmap, bitmapsz, &bytesRead);
					WPARAM mask = 1;
					// Walk through the busy bitmap
					for (int j = 0, offset = 0; j < userdata.BusyBitmap.SizeOfBitMap; j++) {
						if (!mask) {
							mask = 1;
							offset++;
						}
						// Only if block is busy
						if (*(bitmap + offset) & mask) {
							GROW_BLOCKS ();
							WPARAM off = userdata.EncodedOffsets.FirstAllocationOffset + sz * j;
							from = (WPARAM)subsegment.UserBlocks + off;
							ReadProcessMemory (h_proc, (PVOID)from, &heapEntry, sz_entry, &bytesRead);
							DecodeLFHEntry (dbg, &heapHeader, &heapEntry, subsegment.UserBlocks, lfhKey, from);
							blocks[count].address = from;
							blocks[count].flags = 1 | NT_BLOCK | LFH_BLOCK;
							blocks[count].size = sz;
							PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
							if (!extra) {
								goto err;
							}
							extra->granularity = sizeof (HEAP_ENTRY);
							extra->segment = curSubsegment;
							blocks[count].extra = EXTRA_FLAG | (WPARAM)extra;
							count++;
						}
						mask <<= 1;
					}
					free (bitmap);
					curSubsegment += sizeof (HEAP_SUBSEGMENT);
					next++;
				} while (next < blockZone.NextIndex || subsegment.BlockSize);

				LIST_ENTRY entry;
				ReadProcessMemory (h_proc, curEntry, &entry, sizeof (entry), &bytesRead);
				curEntry = entry.Flink;
			} while (curEntry != firstEntry);
		}

		HEAP_SEGMENT oldSegment, segment;
		WPARAM firstSegment = (WPARAM)heapHeader.SegmentList.Flink;
		ReadProcessMemory (h_proc, (PVOID)(firstSegment - offsetof (HEAP_SEGMENT, SegmentListEntry)), &segment, sizeof (HEAP_SEGMENT), &bytesRead);
		// NT Blocks (Loops through all _HEAP_SEGMENTs)
		do {
			from = (WPARAM)segment.FirstEntry;
			if (!from) {
				goto next;
			}
			do {
				if (!ReadProcessMemory (h_proc, (PVOID)from, &heapEntry, sz_entry, &bytesRead)) {
					break;
				}
				DecodeHeapEntry (dbg, &heapHeader, &heapEntry);
				if (!heapEntry.Size) {
					// Last Heap block
					count--;
					break;
				}

				SIZE_T real_sz = heapEntry.Size * sz_entry;

				GROW_BLOCKS ();
				PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
				if (!extra) {
					goto err;
				}
				extra->granularity = sizeof (HEAP_ENTRY);
				extra->segment = (WPARAM)segment.BaseAddress;
				blocks[count].extra = EXTRA_FLAG | (WPARAM)extra;
				blocks[count].address = from;
				blocks[count].flags = heapEntry.Flags | NT_BLOCK | BACKEND_BLOCK;
				blocks[count].size = real_sz;
				from += real_sz;
				count++;
			} while (from <= (WPARAM)segment.LastValidEntry);
next:
			oldSegment = segment;
			from = (WPARAM)segment.SegmentListEntry.Flink - offsetof (HEAP_SEGMENT, SegmentListEntry);
			ReadProcessMemory (h_proc, (PVOID)from, &segment, sizeof (HEAP_SEGMENT), &bytesRead);
		} while ((WPARAM)oldSegment.SegmentListEntry.Flink != firstSegment);
		heap->Blocks = blocks;
		heap->BlockCount = count;
	}
	CloseHandle (h_proc);
	return db;
err:
	if (h_proc) {
		CloseHandle (h_proc);
	}
	if (db) {
		for (int i = 0; i < heapInfo->count; i++) {
			PDEBUG_HEAP_INFORMATION heap = &heapInfo->heaps[i];
			free_extra_info (heap);
			R_FREE (heap->Blocks);
		}
		RtlDestroyQueryDebugBuffer (db);
	}
	return NULL;
}

static PHeapBlock GetSingleSegmentBlock(RDebug *dbg, HANDLE h_proc, PSEGMENT_HEAP heapBase, WPARAM offset) {
	/*
	*	TODO:
	*		- LFH
	*		- VS
	*		- Backend
	*/
	PHeapBlock hb = R_NEW0 (HeapBlock);
	if (!hb) {
		R_LOG_ERROR ("GetSingleSegmentBlock: Allocation failed.\n");
		return NULL;
	}
	PHeapBlockExtraInfo extra = R_NEW0 (HeapBlockExtraInfo);
	if (!extra) {
		R_LOG_ERROR ("GetSingleSegmentBlock: Allocation failed.\n");
		goto err;
	}
	hb->extraInfo = extra;
	SEGMENT_HEAP heap;
	ReadProcessMemory (h_proc, heapBase, &heap, sizeof (SEGMENT_HEAP), NULL);
	// Try Large Blocks
	if ((offset & 0xFFFF) < 0x100) {
		if (!heap.LargeAllocMetadata.Root) {
			goto err;
		}
		RTL_BALANCED_NODE node;
		WPARAM curr = (WPARAM)heap.LargeAllocMetadata.Root;
		ReadProcessMemory (h_proc, (PVOID)curr, &node, sizeof (RTL_BALANCED_NODE), NULL);

		while (curr) {
			HEAP_LARGE_ALLOC_DATA entry;
			ReadProcessMemory (h_proc, (PVOID)curr, &entry, sizeof (HEAP_LARGE_ALLOC_DATA), NULL);
			WPARAM VirtualAddess = entry.VirtualAddess - entry.UnusedBytes;
			if ((offset & ~0xFFFFULL) > VirtualAddess) {
				curr = (WPARAM)node.Right;
			} else if ((offset & ~0xFFFFULL) < VirtualAddess) {
				curr = (WPARAM)node.Left;
			} else {
				hb->dwAddress = (PVOID)VirtualAddess;
				hb->dwSize = ((entry.AllocatedPages >> 12) << 12) - entry.UnusedBytes;
				hb->dwFlags = SEGMENT_HEAP_BLOCK | LARGE_BLOCK | 1;
				extra->unusedBytes = entry.UnusedBytes;
				ReadProcessMemory (h_proc, hb->dwAddress, &extra->granularity, sizeof (USHORT), NULL);
				return hb;
			}
			if (curr) {
				ReadProcessMemory (h_proc, (PVOID)curr, &node, sizeof (RTL_BALANCED_NODE), NULL);
			}
		}
	}
err:
	free (hb);
	free (extra);
	return NULL;
}

static PHeapBlock GetSingleBlock(RDebug *dbg, ut64 offset) {
	PHeapBlock hb = R_NEW0 (HeapBlock);
	PDEBUG_BUFFER db = NULL;
	PHeapBlockExtraInfo extra = NULL;

	if (!hb) {
		R_LOG_ERROR ("GetSingleBlock: Allocation failed.\n");
		return NULL;
	}
	HANDLE h_proc = OpenProcess (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dbg->pid);
	if (!h_proc) {
		r_sys_perror ("GetSingleBlock/OpenProcess");
		goto err;
	}
	db = InitHeapInfo (dbg->pid, PDI_HEAPS);
	if (!db) {
		goto err;
	}
	extra = R_NEW0 (HeapBlockExtraInfo);
	if (!extra) {
		R_LOG_ERROR ("GetSingleBlock: Allocation failed.\n");
		goto err;
	}
	WPARAM NtLFHKey = GetLFHKey (dbg, h_proc, false);
	PHeapInformation heapInfo = db->HeapInformation;
	for (int i = 0; i < heapInfo->count; i++) {
		DEBUG_HEAP_INFORMATION heap = heapInfo->heaps[i];
		if (is_segment_heap (h_proc, heap.Base)) {
			free (hb);
			R_FREE (extra);
			hb = GetSingleSegmentBlock (dbg, h_proc, heap.Base, offset);
			if (!hb) {
				goto err;
			}
			break;
		} else {
			HEAP h;
			HEAP_ENTRY entry;
			WPARAM entryOffset = offset - heap.Granularity;
			if (!ReadProcessMemory (h_proc, heap.Base, &h, sizeof (HEAP), NULL) ||
				!ReadProcessMemory (h_proc, (PVOID)entryOffset, &entry, sizeof (HEAP_ENTRY), NULL)) {
				goto err;
			}
			extra->granularity = heap.Granularity;
			hb->extraInfo = extra;
			HEAP_ENTRY tmpEntry = entry;
			if (DecodeHeapEntry (dbg, &h, &tmpEntry)) {
				entry = tmpEntry;
				hb->dwAddress = (PVOID)offset;
				UPDATE_FLAGS (hb, (DWORD)entry.Flags | NT_BLOCK);
				if (entry.UnusedBytes == 0x4) {
					HEAP_VIRTUAL_ALLOC_ENTRY largeEntry;
					if (ReadProcessMemory (h_proc, (PVOID)(offset - sizeof (HEAP_VIRTUAL_ALLOC_ENTRY)), &largeEntry, sizeof (HEAP_VIRTUAL_ALLOC_ENTRY), NULL)) {
						hb->dwSize = largeEntry.CommitSize;
						hb->dwFlags |= LARGE_BLOCK;
						extra->unusedBytes = largeEntry.ReserveSize - largeEntry.CommitSize;
						extra->granularity = sizeof (HEAP_VIRTUAL_ALLOC_ENTRY);
					}
				} else {
					hb->dwSize = (WPARAM)entry.Size * heap.Granularity;
					hb->dwFlags |= BACKEND_BLOCK;
				}
				break;
			}
			// LFH
			if (entry.UnusedBytes & 0x80) {
				tmpEntry = entry;
				WPARAM userBlocksOffset;
				if (dbg->bits == R_SYS_BITS_64) {
					*(((WPARAM *)&tmpEntry) + 1) ^= PtrToInt (h.BaseAddress) ^ (entryOffset >> 0x4) ^ (DWORD)NtLFHKey;
					userBlocksOffset = entryOffset - (USHORT)((*(((WPARAM *)&tmpEntry) + 1)) >> 0xC);
				} else {
					*((WPARAM *)&tmpEntry) ^= PtrToInt (h.BaseAddress) ^ ((DWORD)(entryOffset) >> 0x4) ^ (DWORD)NtLFHKey;
					userBlocksOffset = entryOffset - (USHORT)(*((WPARAM *)&tmpEntry) >> 0xC);
				}
				// Confirm it is LFH
				if (DecodeLFHEntry (dbg, &h, &entry, (PVOID)userBlocksOffset, NtLFHKey, entryOffset)) {
					HEAP_USERDATA_HEADER UserBlocks;
					HEAP_SUBSEGMENT subsegment;
					if (!ReadProcessMemory (h_proc, (PVOID)userBlocksOffset, &UserBlocks, sizeof (HEAP_USERDATA_HEADER), NULL)) {
						r_sys_perror ("GetSingleBlock/ReadProcessMemory");
						continue;
					}
					if (!ReadProcessMemory (h_proc, (PVOID)UserBlocks.SubSegment, &subsegment, sizeof (HEAP_SUBSEGMENT), NULL)) {
						continue;
					}
					hb->dwAddress = (PVOID)offset;
					hb->dwSize = (WPARAM)subsegment.BlockSize * heap.Granularity;
					hb->dwFlags = 1 | LFH_BLOCK | NT_BLOCK;
					break;
				}
			}
		}
	}
	if (!hb->dwSize) {
		goto err;
	}
	RtlDestroyQueryDebugBuffer (db);
	CloseHandle (h_proc);
	return hb;
err:
	if (h_proc) {
		CloseHandle (h_proc);
	}
	if (db) {
		RtlDestroyQueryDebugBuffer (db);
	}
	free (hb);
	free (extra);
	return NULL;
}

static void w32_list_heaps(RCore *core, const char format) {
	ULONG pid = core->dbg->pid;
	PDEBUG_BUFFER db = InitHeapInfo (pid, PDI_HEAPS | PDI_HEAP_BLOCKS);
	if (!db) {
		os_info *info = r_sys_get_osinfo ();
		if (info->major >= 10) {
			db = GetHeapBlocks (pid, core->dbg);
		}
		free (info);
		if (!db) {
			eprintf ("Couldn't get heap info.\n");
			return;
		}
	}
	PHeapInformation heapInfo = db->HeapInformation;
	CHECK_INFO (heapInfo);
	int i;
	PJ *pj = pj_new ();
	pj_a (pj);
	for (i = 0; i < heapInfo->count; i++) {
		DEBUG_HEAP_INFORMATION heap = heapInfo->heaps[i];
		switch (format) {
		case 'j':
			pj_o (pj);
			pj_kN (pj, "address", (WPARAM)heap.Base);
			pj_kN (pj, "count", (WPARAM)heap.BlockCount);
			pj_kN (pj, "allocated", (WPARAM)heap.Allocated);
			pj_kN (pj, "commited", (WPARAM)heap.Committed);
			pj_end (pj);
			break;
		default:
			r_cons_printf ("Heap @ 0x%08"PFMT64x":\n", (WPARAM)heap.Base);
			r_cons_printf ("\tBlocks: %"PFMT64u"\n", (WPARAM)heap.BlockCount);
			r_cons_printf ("\tAllocated: %"PFMT64u"\n", (WPARAM)heap.Allocated);
			r_cons_printf ("\tCommited: %"PFMT64u"\n", (WPARAM)heap.Committed);
			break;
		}
		if (!(db->InfoClassMask & PDI_HEAP_BLOCKS)) {
			free_extra_info (&heap);
			R_FREE (heap.Blocks);
		}
	}
	if (format == 'j') {
		pj_end (pj);
		r_cons_println (pj_string (pj));
	}
	pj_free (pj);
	RtlDestroyQueryDebugBuffer (db);
}

static void w32_list_heaps_blocks(RCore *core, const char format) {
	DWORD pid = core->dbg->pid;
	os_info *info = r_sys_get_osinfo ();
	PDEBUG_BUFFER db;
	if (info->major >= 10) {
		db = GetHeapBlocks (pid, core->dbg);
	} else {
		db = InitHeapInfo (pid, PDI_HEAPS | PDI_HEAP_BLOCKS);
	}
	free (info);
	if (!db) {
		eprintf ("Couldn't get heap info.\n");
		return;
	}
	PHeapInformation heapInfo = db->HeapInformation;
	HeapBlock *block = malloc (sizeof (HeapBlock));
	CHECK_INFO (heapInfo);
	int i;
	PJ *pj = pj_new ();
	pj_a (pj);
	for (i = 0; i < heapInfo->count; i++) {
		bool go = true;
		switch (format) {
		case 'f':
			if (heapInfo->heaps[i].BlockCount > 50000) {
				go = r_cons_yesno ('n', "Are you sure you want to add %"PFMT64u" flags? (y/N)", heapInfo->heaps[i].BlockCount);
			}
			break;
		case 'j':
			pj_o (pj);
			pj_kN (pj, "heap", (WPARAM)heapInfo->heaps[i].Base);
			pj_k (pj, "blocks");
			pj_a (pj);
			break;
		default:
			r_cons_printf ("Heap @ 0x%"PFMT64x":\n", heapInfo->heaps[i].Base);
		}
		char *type;
		if (GetFirstHeapBlock (&heapInfo->heaps[i], block) & go) {
			do {
				type = get_type (block->dwFlags);
				if (!type) {
					type = "";
				}
				unsigned short granularity = block->extraInfo ? block->extraInfo->granularity : heapInfo->heaps[i].Granularity;
				switch (format) {
				case 'f':
				{
					ut64 addr = (ut64)block->dwAddress - granularity;
					char *name = r_str_newf ("alloc.%"PFMT64x"", addr);
					r_flag_set (core->flags, name, addr, block->dwSize);
					free (name);
					break;
				}
				case 'j':
					pj_o (pj);
					pj_kN (pj, "address", (ut64)block->dwAddress - granularity);
					pj_kN (pj, "data_address", (ut64)block->dwAddress);
					pj_kN (pj, "size", block->dwSize);
					pj_ks (pj, "type", type);
					pj_end (pj);
					break;
				default:
					r_cons_printf ("\tBlock @ 0x%"PFMT64x" %s:\n", (ut64)block->dwAddress - granularity, type);
					r_cons_printf ("\t\tSize 0x%"PFMT64x"\n", (ut64)block->dwSize);
					r_cons_printf ("\t\tData address @ 0x%"PFMT64x"\n", (ut64)block->dwAddress);
					break;
				}
			} while (GetNextHeapBlock (&heapInfo->heaps[i], block));
		}
		if (format == 'j') {
			pj_end (pj);
			pj_end (pj);
		}
		if (!(db->InfoClassMask & PDI_HEAP_BLOCKS)) {
			// RtlDestroyQueryDebugBuffer wont free this for some reason
			free_extra_info (&heapInfo->heaps[i]);
			R_FREE (heapInfo->heaps[i].Blocks);
		}
	}
	if (format == 'j') {
		pj_end (pj);
		r_cons_println (pj_string (pj));
	}
	pj_free (pj);
	RtlDestroyQueryDebugBuffer (db);
}

static const char *help_msg[] = {
	"Usage:", " dmh[?|b][f|j]", " # Memory map heap",
	"dmh[j]", "", "List process heaps",
	"dmhb[?] [addr]", "", "List process heap blocks",
	NULL
};

static const char *help_msg_block[] = {
	"Usage:", " dmhb[f|j]", " # Memory map heap",
	"dmhb [addr]", "", "List allocated heap blocks",
	"dmhbf", "", "Create flags for each allocated block",
	"dmhbj [addr]", "", "Print output in JSON format",
	NULL
};

static void cmd_debug_map_heap_block_win(RCore *core, const char *input) {
	char *space = strchr (input, ' ');
	ut64 off = 0;
	PHeapBlock hb = NULL;
	if (space) {
		off = r_num_math (core->num, space + 1);
		PHeapBlock hb = GetSingleBlock (core->dbg, off);
		if (hb) {
			ut64 granularity = hb->extraInfo->granularity;
			char *type = get_type (hb->dwFlags);
			if (!type) {
				type = "";
			}
			PJ *pj = pj_new ();
			switch (input[0]) {
			case ' ':
				r_cons_printf ("Block @ 0x%"PFMT64x" %s\n", off - granularity, type);
				r_cons_printf ("\tSize: 0x%"PFMT64x"\n", hb->dwSize);
				if (hb->extraInfo->unusedBytes) {
					r_cons_printf ("\tUnused: 0x%"PFMT64x"\n", hb->extraInfo->unusedBytes);
				}
				r_cons_printf ("\tGranularity: 0x%"PFMT64x"\n", granularity);
				break;
			case 'j':
				pj_o (pj);
				pj_kN (pj, "address", off - granularity);
				pj_ks (pj, "type", type);
				pj_kN (pj, "size", hb->dwSize);
				if (hb->extraInfo->unusedBytes) {
					pj_kN (pj, "unused", hb->extraInfo->unusedBytes);
				}
				pj_end (pj);
				r_cons_println (pj_string (pj));
			}
			free (hb->extraInfo);
			free (hb);
			pj_free (pj);
		}
		return;
	}
	switch (input[0]) {
	case '\0':
	case 'f':
	case 'j':
		w32_list_heaps_blocks (core, input[0]);
		break;
	default:
		r_core_cmd_help (core, help_msg_block);
	}
}

static int cmd_debug_map_heap_win(RCore *core, const char *input) {
	init_func ();
	switch (input[0]) {
	case '?': // dmh?
		r_core_cmd_help (core, help_msg);
		break;
	case 'b': // dmhb
		cmd_debug_map_heap_block_win (core, input + 1);
		break;
	default:
		w32_list_heaps (core, input[0]);
		break;
	}
	return true;
}