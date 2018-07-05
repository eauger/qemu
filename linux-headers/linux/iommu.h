/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * IOMMU user API definitions
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _UAPI_IOMMU_H
#define _UAPI_IOMMU_H

#include <linux/types.h>

/**
 * SMMUv3 Stream Table Entry stage 1 related information
 * @abort: shall the STE lead to abort
 * @s1fmt: STE s1fmt field as set by the guest
 * @s1dss: STE s1dss as set by the guest
 * All field names match the smmu 3.0/3.1 spec (ARM IHI 0070A)
 */
struct iommu_pasid_smmuv3 {
	__u8 abort;
	__u8 s1fmt;
	__u8 s1dss;
};

/**
 * PASID table data used to bind guest PASID table to the host IOMMU
 * Note PASID table corresponds to the Context Table on ARM SMMUv3.
 *
 * @version: API version to prepare for future extensions
 * @format: format of the PASID table
 *
 */
struct iommu_pasid_table_config {
#define PASID_TABLE_CFG_VERSION_1 1
	__u32	version;
#define IOMMU_PASID_FORMAT_SMMUV3	(1 << 0)
	__u32	format;
	__u64	base_ptr;
	__u8	pasid_bits;
	__u8	bypass;
	union {
		struct iommu_pasid_smmuv3 smmuv3;
	};
};

/**
 * enum iommu_inv_granularity - Generic invalidation granularity
 * @IOMMU_INV_GRANU_DOMAIN_ALL_PASID:	TLB entries or PASID caches of all
 *					PASIDs associated with a domain ID
 * @IOMMU_INV_GRANU_PASID_SEL:		TLB entries or PASID cache associated
 *					with a PASID and a domain
 * @IOMMU_INV_GRANU_PAGE_PASID:		TLB entries of selected page range
 *					within a PASID
 *
 * When an invalidation request is passed down to IOMMU to flush translation
 * caches, it may carry different granularity levels, which can be specific
 * to certain types of translation caches.
 * This enum is a collection of granularities for all types of translation
 * caches. The idea is to make it easy for IOMMU model specific driver to
 * convert from generic to model specific value. Each IOMMU driver
 * can enforce check based on its own conversion table. The conversion is
 * based on 2D look-up with inputs as follows:
 * - translation cache types
 * - granularity
 *
 *             type |   DTLB    |    TLB    |   PASID   |
 *  granule         |           |           |   cache   |
 * -----------------+-----------+-----------+-----------+
 *  DN_ALL_PASID    |   Y       |   Y       |   Y       |
 *  PASID_SEL       |   Y       |   Y       |   Y       |
 *  PAGE_PASID      |   Y       |   Y       |   N/A     |
 *
 */
enum iommu_inv_granularity {
	IOMMU_INV_GRANU_DOMAIN_ALL_PASID,
	IOMMU_INV_GRANU_PASID_SEL,
	IOMMU_INV_GRANU_PAGE_PASID,
	IOMMU_INV_NR_GRANU,
};

/**
 * enum iommu_inv_type - Generic translation cache types for invalidation
 *
 * @IOMMU_INV_TYPE_DTLB:	device IOTLB
 * @IOMMU_INV_TYPE_TLB:		IOMMU paging structure cache
 * @IOMMU_INV_TYPE_PASID:	PASID cache
 * Invalidation requests sent to IOMMU for a given device need to indicate
 * which type of translation cache to be operated on. Combined with enum
 * iommu_inv_granularity, model specific driver can do a simple lookup to
 * convert from generic to model specific value.
 */
enum iommu_inv_type {
	IOMMU_INV_TYPE_DTLB,
	IOMMU_INV_TYPE_TLB,
	IOMMU_INV_TYPE_PASID,
	IOMMU_INV_NR_TYPE
};

/**
 * Translation cache invalidation header that contains mandatory meta data.
 * @version:	info format version, expecting future extesions
 * @type:	type of translation cache to be invalidated
 */
struct iommu_cache_invalidate_hdr {
	__u32 version;
#define TLB_INV_HDR_VERSION_1 1
	enum iommu_inv_type type;
};

/**
 * Translation cache invalidation information, contains generic IOMMU
 * data which can be parsed based on model ID by model specific drivers.
 * Since the invalidation of second level page tables are included in the
 * unmap operation, this info is only applicable to the first level
 * translation caches, i.e. DMA request with PASID.
 *
 * @granularity:	requested invalidation granularity, type dependent
 * @size:		2^size of 4K pages, 0 for 4k, 9 for 2MB, etc.
 * @nr_pages:		number of pages to invalidate
 * @pasid:		processor address space ID value per PCI spec.
 * @arch_id:		architecture dependent id characterizing a context
 *			and tagging the caches, ie. domain Identfier on VTD,
 *			asid on ARM SMMU
 * @addr:		page address to be invalidated
 * @flags		IOMMU_INVALIDATE_ADDR_LEAF: leaf paging entries
 *			IOMMU_INVALIDATE_GLOBAL_PAGE: global pages
 *
 */
struct iommu_cache_invalidate_info {
	struct iommu_cache_invalidate_hdr	hdr;
	enum iommu_inv_granularity	granularity;
	__u32		flags;
#define IOMMU_INVALIDATE_ADDR_LEAF	(1 << 0)
#define IOMMU_INVALIDATE_GLOBAL_PAGE	(1 << 1)
	__u8		size;
	__u64		nr_pages;
	__u32		pasid;
	__u64		arch_id;
	__u64		addr;
};

struct iommu_guest_msi_binding {
	__u64		iova;
	__u64		gpa;
	__u32		granule;
};

/*  Generic fault types, can be expanded IRQ remapping fault */
enum iommu_fault_type {
	IOMMU_FAULT_DMA_UNRECOV = 1,	/* unrecoverable fault */
	IOMMU_FAULT_PAGE_REQ,		/* page request fault */
};

enum iommu_fault_reason {
	IOMMU_FAULT_REASON_UNKNOWN = 0,

	/* IOMMU internal error, no specific reason to report out */
	IOMMU_FAULT_REASON_INTERNAL,

	/* Could not access the PASID table (fetch caused external abort) */
	IOMMU_FAULT_REASON_PASID_FETCH,

	/* could not access the device context (fetch caused external abort) */
	IOMMU_FAULT_REASON_DEVICE_CONTEXT_FETCH,

	/* pasid entry is invalid or has configuration errors */
	IOMMU_FAULT_REASON_BAD_PASID_ENTRY,

	/* device context entry is invalid or has configuration errors */
	IOMMU_FAULT_REASON_BAD_DEVICE_CONTEXT_ENTRY,
	/*
	 * PASID is out of range (e.g. exceeds the maximum PASID
	 * supported by the IOMMU) or disabled.
	 */
	IOMMU_FAULT_REASON_PASID_INVALID,

	/* source id is out of range */
	IOMMU_FAULT_REASON_SOURCEID_INVALID,

	/*
	 * An external abort occurred fetching (or updating) a translation
	 * table descriptor
	 */
	IOMMU_FAULT_REASON_WALK_EABT,

	/*
	 * Could not access the page table entry (Bad address),
	 * actual translation fault
	 */
	IOMMU_FAULT_REASON_PTE_FETCH,

	/* Protection flag check failed */
	IOMMU_FAULT_REASON_PERMISSION,

	/* access flag check failed */
	IOMMU_FAULT_REASON_ACCESS,

	/* Output address of a translation stage caused Address Size fault */
	IOMMU_FAULT_REASON_OOR_ADDRESS
};

/**
 * struct iommu_fault - Generic fault data
 *
 * @type contains fault type
 * @reason fault reasons if relevant outside IOMMU driver.
 * IOMMU driver internal faults are not reported.
 * @addr: tells the offending page address
 * @fetch_addr: tells the address that caused an abort, if any
 * @pasid: contains process address space ID, used in shared virtual memory
 * @page_req_group_id: page request group index
 * @last_req: last request in a page request group
 * @pasid_valid: indicates if the PRQ has a valid PASID
 * @prot: page access protection flag:
 *	IOMMU_FAULT_READ, IOMMU_FAULT_WRITE
 */

struct iommu_fault {
	__u32	type;   /* enum iommu_fault_type */
	__u32	reason; /* enum iommu_fault_reason */
	__u64	addr;
	__u64	fetch_addr;
	__u32	pasid;
	__u32	page_req_group_id;
	__u32	last_req;
	__u32	pasid_valid;
	__u32	prot;
	__u32	access;
};
#endif /* _UAPI_IOMMU_H */
