/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef NVPVA_VPU_APP_H
#define NVPVA_VPU_APP_H
#include <uapi/linux/nvpva_ioctl.h>
#include "pva-ucode-header.h"
#include "pva-sys-params.h"
#include "pva-task.h"
#include <linux/types.h>
#include <linux/mutex.h>
#include "pva-bit.h"

#define ELF_MAXIMUM_SYMBOL_LENGTH 64
#define MAX_NUM_VPU_EXE 32U

/**
 *  enum to identify different segments of VPU ELF
 */
enum pva_elf_seg_type {
	/**< Code segment in VPU ELF */
	PVA_SEG_VPU_CODE = 0U,
	/**< DATA segment in VPU ELF */
	PVA_SEG_VPU_DATA,
	/**< DATA segment in VPU ELF containing symbol information*/
	PVA_SEG_VPU_IN_PARAMS,
	/**< Not a valid segment in VPU ELF */
	PVA_SEG_VPU_MAX_TYPE
};

/**
 * Structure that holds buffer and handles shared with FW
 */
struct pva_elf_buffer {
	/**< Aligned size of allocated buffer */
	size_t size;
	/**< IOVA address if allocated buffer */
	dma_addr_t pa;
	/**< Virtual address of allocated buffer */
	void *va;

	/*original value came out of allocator*/
	size_t alloc_size;
	dma_addr_t alloc_pa;
	void *alloc_va;

	/*< Local buffer holding data to be copied to allocated buffer.
	 *  May undergo resizing
	 */
	uint8_t *localbuffer;
	/**< Unaligned size of local buffer */
	uint32_t localsize;
	/**< Number of segments */
	uint32_t num_segments;
};

/*
 * Store elf symbols information
 */
struct pva_elf_symbolId {
	/**< Symbol name */
	char *symbol_name;
	/**< Symbol ID */
	uint16_t symbolID;
	/**< Symbol Size */
	size_t size;
	/**< VMEM address of Symbol */
	uint32_t addr;
	/**<IOVA address offset in symbol buffer */
	uint64_t offset;
};

/**
 * elf image details
 */
struct pva_elf_image {
	/**< buffer storing vpu_bin_info */
	struct pva_elf_buffer vpu_bin_buffer;
	/**< Buffers containing information about vpu segments */
	struct pva_elf_buffer vpu_segments_buffer[PVA_SEG_VPU_MAX_TYPE];
	/**<ID of the VPU app */
	uint16_t elf_id;
	/**<True if user has successfully registered a VPU ELF */
	bool user_registered;
	/**<Count of how many tasks submitted to FW use this ELF image */
	atomic_t submit_refcount;
	/**< Number of symbols in the VPU app */
	uint32_t num_symbols;
	/**< Stores symbol information */
	struct pva_elf_symbolId sym[NVPVA_TASK_MAX_SYMBOLS];
	/**< Total size of all the symbols in VPU app */
	uint32_t symbol_size_total;
	/**< Bin info which stores information about different vpu segments */
	struct pva_bin_info_s info;
};

/**
 * Store multiple of elf image
 */
struct pva_elf_images {
	/* TODO: Implement hashtable and remove size restrictions. JIRA
	 * PVAAS-4115
	 */
	/**< Stores information about all VPU APPs */
	struct pva_elf_image elf_img[MAX_NUM_VPU_EXE];
	/**< Alloctable keeping track of VPU APPs */
	unsigned int alloctable;
};

struct nvpva_elf_context {
	struct pva *dev;
	/* Contains context for all elf images */
	struct pva_elf_images *elf_images;
	/* Mutex for atomic access */
	struct mutex elf_mutex;
};

/* following functions to deal with UMD request */
/**
 * Get Symbol ID given the symbol name from a vpu app
 *
 * @param d		Pointer to Elf Context
 * @param vpu_exe_id	ID of the VPU app
 * @param *sym_name	String containing Name of the symbol
 * @param *id		ID of the symbol filled by this function
 * @param *sym_size	Size of the symbol filled by this function
 *
 * @return		EOK for symbol found. -EINVAL for symbol not found
 *			When -EINVAL is returned, ignore values in id and
 *			sym_size
 */
int32_t pva_get_sym_id(struct nvpva_elf_context *d, uint16_t vpu_exe_id,
		       const char *sym_name, uint16_t *id, uint32_t *sym_size);

/**
 * Get IOVA address of bin_info to passed to FW
 *
 * @param d		Pointer to Elf Context
 * @param exe_id	ID of the VPU app for which IOVA address of bin_info is
 *			required
 *
 * @return		IOVA address of bin_info
 *
 * Must be called with registered exe_id or exe_id = NVPVA_NOOP_EXE_ID
 */
dma_addr_t phys_get_bin_info(struct nvpva_elf_context *d, uint16_t exe_id);

/* following functions to serve firmware requirement */
/**
 * Assign unique ID to VPU APP
 *
 * @param d		Pointer to Elf Context
 * @param *exe_id	Unique ID assigned to VPU APP.
 *			Filled by this function
 *
 * @return		EOK if valid ID is assigned else ERROR
 */
int32_t pva_get_vpu_exe_id(struct nvpva_elf_context *d, uint16_t *exe_id);

/**
 * Get VMEM address of symbol
 *
 * @param d		Pointer to Elf Context
 * @param exe_id	Unique VPU APP ID
 * @param sym_id	Unique Symbol ID
 * @param addr		Symbol offset
 *
 * @return		EOK if valid offset is found else ERROR
 *
 * Must be called with registered exe_id or exe_id = NVPVA_NOOP_EXE_ID
 */
int32_t pva_get_sym_offset(struct nvpva_elf_context *d, uint16_t exe_id,
			   uint32_t sym_id, uint32_t *addr);

/**
 * Check if vpu id is registered in given context
 *
 * @param d		Pointer to Elf Context
 * @param exe_id	Unique VPU APP ID
 *
 * @return		TRUE if registered else FALSE
 */
static inline bool pva_vpu_elf_is_registered(struct nvpva_elf_context *d,
					     uint16_t exe_id)
{
	return (exe_id < MAX_NUM_VPU_EXE) &&
	       ((d->elf_images->alloctable >> exe_id) & 1U);
}

/**
 * Load VPU APP elf file
 *
 * @param d		Pointer to the Elf Context
 * @param *buffer	Buffer containing the VPU APP elf file
 * @param size		Size of the VPU APP elf file
 * @param *exe_id	ID assigned to the VPU APP in KMD filled
 *			by this function
 * @param hwid		HWID associated with the VPU APP used for
 *			allocation
 *
 * @return		EOK if everything is valid and VPU APP is
 *			loaded successfully
 */
int32_t pva_load_vpu_app(struct nvpva_elf_context *d, uint8_t *buffer,
			 size_t size, uint16_t *exe_id);

/**
 * Unload VPU APP elf file
 *
 * @param d		Pointer to the Elf Context
 * @param exe_id	Unique ID of VPU APP to be unloaded
 *
 * @return		EOK if successful
 */
int32_t pva_unload_vpu_app(struct nvpva_elf_context *d, uint16_t exe_id);

/**
 * Unload all VPU APP elf files associated with the given ELF context
 *
 * @param d		Pointer to the Elf Context
 *
 */
void pva_unload_all_apps(struct nvpva_elf_context *d);

/**
 * Get reference to a vpu app for task
 *
 * @param d		Pointer to the Elf Context
 * @param exe_id	Unique ID of VPU APP to be referenced
 *
 * @return		EOK if successful
 */
int32_t pva_task_acquire_ref_vpu_app(struct nvpva_elf_context *d,
				     uint16_t exe_id);

/**
 * Unref VPU APP elf file from user side
 *
 * @param d		Pointer to the Elf Context
 * @param exe_id	Unique ID of VPU APP to be unreferenced
 *
 * @return		EOK if successful
 */
int32_t pva_release_vpu_app(struct nvpva_elf_context *d, uint16_t exe_id);

/**
 * Unref VPU APP elf file from task side
 *
 * @param d		Pointer to the Elf Context
 * @param exe_id	Unique ID of VPU APP to be unreferenced
 *
 * @return		EOK if successful
 */
int32_t pva_task_release_ref_vpu_app(struct nvpva_elf_context *d,
				     uint16_t exe_id);

/**
 * Deinitialize and Deallocate memory for VPU parsing
 *
 * @param		Pointer to the Elf Context
 *
 * @return		Void
 */
void pva_vpu_deinit(struct nvpva_elf_context *d);

/**
 * Initialize memory for VPU Parsing
 *
 * @param		Pointer to the Elf Context
 *
 * @return		EOK if no errors encountered
 */
int32_t pva_vpu_init(struct pva *dev, struct nvpva_elf_context *d);

void print_segments_info(struct pva_elf_image *elf_img);
#endif
