/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Memory handling and MMU.
 *
 * Authors:	Sarah Walker, <tommowalker@tommowalker.co.uk>
 *		Miran Grca, <mgrca8@gmail.com>
 *		Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *		Copyright 2008-2020 Sarah Walker.
 *		Copyright 2016-2020 Miran Grca.
 *		Copyright 2017-2020 Fred N. van Kempen.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/version.h>
#include "cpu.h"
#include "x86_ops.h"
#include "x86.h"
#include <86box/machine.h>
#include <86box/m_xt_xi8088.h>
#include <86box/config.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/plat.h>
#include <86box/rom.h>
#include <86box/gdbstub.h>
#ifdef USE_DYNAREC
# include "codegen_public.h"
#else
#ifdef USE_NEW_DYNAREC
# define PAGE_MASK_SHIFT	6
#else
# define PAGE_MASK_INDEX_MASK	3
# define PAGE_MASK_INDEX_SHIFT	10
# define PAGE_MASK_SHIFT	4
#endif
# define PAGE_MASK_MASK		63
#endif
#if (!defined(USE_DYNAREC) && defined(USE_NEW_DYNAREC))
#define BLOCK_PC_INVALID 0xffffffff
#define BLOCK_INVALID 0
#endif


#define mmutranslate_read_2386(addr) mmutranslatereal_2386(addr,0)
#define mmutranslate_write_2386(addr) mmutranslatereal_2386(addr,1)


uint64_t
mmutranslatereal_2386(uint32_t addr, int rw)
{
    uint32_t temp, temp2, temp3;
    uint32_t addr2;

    if (cpu_state.abrt)
	return 0xffffffffffffffffULL;

    addr2 = ((cr3 & ~0xfff) + ((addr >> 20) & 0xffc));
    temp = temp2 = mem_readl_phys(addr2);
    if (!(temp & 1)) {
	cr2 = addr;
	temp &= 1;
	if (CPL == 3) temp |= 4;
	if (rw) temp |= 2;
	cpu_state.abrt = ABRT_PF;
	abrt_error = temp;
	return 0xffffffffffffffffULL;
    }

    temp = mem_readl_phys((temp & ~0xfff) + ((addr >> 10) & 0xffc));
    temp3 = temp & temp2;
    if (!(temp & 1) || ((CPL == 3) && !(temp3 & 4) && !cpl_override) || (rw && !(temp3 & 2) && (((CPL == 3) && !cpl_override) || (is486 && (cr0 & WP_FLAG))))) {
	cr2 = addr;
	temp &= 1;
	if (CPL == 3)
		temp |= 4;
	if (rw)
		temp |= 2;
	cpu_state.abrt = ABRT_PF;
	abrt_error = temp;
	return 0xffffffffffffffffULL;
    }

    mmu_perm = temp & 4;
    mem_writel_phys(addr2, mem_readl_phys(addr) | 0x20);    
    mem_writel_phys((temp2 & ~0xfff) + ((addr >> 10) & 0xffc), mem_readl_phys((temp2 & ~0xfff) + ((addr >> 10) & 0xffc)) | (rw ? 0x60 : 0x20));

    return (uint64_t) ((temp & ~0xfff) + (addr & 0xfff));
}


uint64_t
mmutranslate_noabrt_2386(uint32_t addr, int rw)
{
    uint32_t temp,temp2,temp3;
    uint32_t addr2;

    if (cpu_state.abrt)
	return 0xffffffffffffffffULL;

    addr2 = ((cr3 & ~0xfff) + ((addr >> 20) & 0xffc));
    temp = temp2 = mem_readl_phys(addr2);

    if (! (temp & 1))
	return 0xffffffffffffffffULL;

    temp = mem_readl_phys((temp & ~0xfff) + ((addr >> 10) & 0xffc));
    temp3 = temp & temp2;

    if (!(temp & 1) || ((CPL == 3) && !(temp3 & 4) && !cpl_override) || (rw && !(temp3 & 2) && ((CPL == 3) || (cr0 & WP_FLAG))))
	return 0xffffffffffffffffULL;

    return (uint64_t) ((temp & ~0xfff) + (addr & 0xfff));
}


uint8_t
readmembl_2386(uint32_t addr)
{
    mem_mapping_t *map;
    uint64_t a;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 1);

    addr64 = (uint64_t) addr;
    mem_logical_addr = addr;

    high_page = 0;

    if (cr0 >> 31) {
	a = mmutranslate_read_2386(addr);
	addr64 = (uint32_t) a;

	if (a > 0xffffffffULL)
		return 0xff;
    }
    addr = (uint32_t) (addr64 & rammask);

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_b)
	return map->read_b(addr, map->priv);

    return 0xff;
}


void
writemembl_2386(uint32_t addr, uint8_t val)
{
    mem_mapping_t *map;
    uint64_t a;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 1);

    addr64 = (uint64_t) addr;
    mem_logical_addr = addr;

    high_page = 0;

    if (cr0 >> 31) {
	a = mmutranslate_write_2386(addr);
	addr64 = (uint32_t) a;

	if (a > 0xffffffffULL)
		return;
    }
    addr = (uint32_t) (addr64 & rammask);

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->write_b)
	map->write_b(addr, val, map->priv);
}


/* Read a byte from memory without MMU translation - result of previous MMU translation passed as value. */
uint8_t
readmembl_no_mmut_2386(uint32_t addr, uint32_t a64)
{
    mem_mapping_t *map;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 1);

    mem_logical_addr = addr;

    if (cr0 >> 31) {
	if (cpu_state.abrt || high_page)
		return 0xff;

	addr = a64 & rammask;
    } else
	addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_b)
	return map->read_b(addr, map->priv);

    return 0xff;
}


/* Write a byte to memory without MMU translation - result of previous MMU translation passed as value. */
void
writemembl_no_mmut_2386(uint32_t addr, uint32_t a64, uint8_t val)
{
    mem_mapping_t *map;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 1);

    mem_logical_addr = addr;

    if (cr0 >> 31) {
	if (cpu_state.abrt || high_page)
		return;

	addr = a64 & rammask;
    } else
	addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->write_b)
	map->write_b(addr, val, map->priv);
}


uint16_t
readmemwl_2386(uint32_t addr)
{
    mem_mapping_t *map;
    int i;
    uint64_t a;

    addr64a[0] = addr;
    addr64a[1] = addr + 1;
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_READ, 2);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 1) {
	if (!cpu_cyrix_alignment || (addr & 7) == 7)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffe) {
		if (cr0 >> 31) {
			for (i = 0; i < 2; i++) {
				a = mmutranslate_read_2386(addr + i);
				addr64a[i] = (uint32_t) a;

				if (a > 0xffffffffULL)
					return 0xffff;
			}
		}

		return readmembl_no_mmut(addr, addr64a[0]) |
		       (((uint16_t) readmembl_no_mmut(addr + 1, addr64a[1])) << 8);
	}
    }

    if (cr0 >> 31) {
	a = mmutranslate_read_2386(addr);
	addr64a[0] = (uint32_t) a;

	if (a > 0xffffffffULL)
		return 0xffff;
    } else
	addr64a[0] = (uint64_t) addr;

    addr = addr64a[0] & rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_w)
	return map->read_w(addr, map->priv);

    if (map && map->read_b) {
	return map->read_b(addr, map->priv) |
	       ((uint16_t) (map->read_b(addr + 1, map->priv)) << 8);
    }

    return 0xffff;
}


void
writememwl_2386(uint32_t addr, uint16_t val)
{
    mem_mapping_t *map;
    int i;
    uint64_t a;

    addr64a[0] = addr;
    addr64a[1] = addr + 1;
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_WRITE, 2);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 1) {
	if (!cpu_cyrix_alignment || (addr & 7) == 7)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffe) {
		if (cr0 >> 31) {
			for (i = 0; i < 2; i++) {
				a = mmutranslate_write_2386(addr + i);
				addr64a[i] = (uint32_t) a;

				if (a > 0xffffffffULL)
					return;
			}
		}

		/* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
		   their result as a parameter to be used if needed. */
		writemembl_no_mmut(addr, addr64a[0], val);
		writemembl_no_mmut(addr + 1, addr64a[1], val >> 8);
		return;
	}
    }

    if (cr0 >> 31) {
	a = mmutranslate_write_2386(addr);
	addr64a[0] = (uint32_t) a;

	if (a > 0xffffffffULL)
		return;
    }

    addr = addr64a[0] & rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_w) {
	map->write_w(addr, val, map->priv);
	return;
    }

    if (map && map->write_b) {
	map->write_b(addr, val, map->priv);
	map->write_b(addr + 1, val >> 8, map->priv);
	return;
    }
}


/* Read a word from memory without MMU translation - results of previous MMU translation passed as array. */
uint16_t
readmemwl_no_mmut_2386(uint32_t addr, uint32_t *a64)
{
    mem_mapping_t *map;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 2);

    mem_logical_addr = addr;

    if (addr & 1) {
	if (!cpu_cyrix_alignment || (addr & 7) == 7)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffe) {
		if (cr0 >> 31) {
			if (cpu_state.abrt || high_page)
				return 0xffff;
		}

		return readmembl_no_mmut(addr, a64[0]) |
		       (((uint16_t) readmembl_no_mmut(addr + 1, a64[1])) << 8);
	}
    }

    if (cr0 >> 31) {
	if (cpu_state.abrt || high_page)
		return 0xffff;

	addr = (uint32_t) (a64[0] & rammask);
    } else
	addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_w)
	return map->read_w(addr, map->priv);

    if (map && map->read_b) {
	return map->read_b(addr, map->priv) |
	       ((uint16_t) (map->read_b(addr + 1, map->priv)) << 8);
    }

    return 0xffff;
}


/* Write a word to memory without MMU translation - results of previous MMU translation passed as array. */
void
writememwl_no_mmut_2386(uint32_t addr, uint32_t *a64, uint16_t val)
{
    mem_mapping_t *map;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 2);

    mem_logical_addr = addr;

    if (addr & 1) {
	if (!cpu_cyrix_alignment || (addr & 7) == 7)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffe) {
		if (cr0 >> 31) {
			if (cpu_state.abrt || high_page)
				return;
		}

		writemembl_no_mmut(addr, a64[0], val);
		writemembl_no_mmut(addr + 1, a64[1], val >> 8);
		return;
	}
    }

    if (cr0 >> 31) {
	if (cpu_state.abrt || high_page)
		return;

	addr = (uint32_t) (a64[0] & rammask);
    } else
	addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_w) {
	map->write_w(addr, val, map->priv);
	return;
    }

    if (map && map->write_b) {
	map->write_b(addr, val, map->priv);
	map->write_b(addr + 1, val >> 8, map->priv);
	return;
    }
}


uint32_t
readmemll_2386(uint32_t addr)
{
    mem_mapping_t *map;
    int i;
    uint64_t a = 0x0000000000000000ULL;

    for (i = 0; i < 4; i++)
	addr64a[i] = (uint64_t) (addr + i);
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_READ, 4);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 3) {
	if (!cpu_cyrix_alignment || (addr & 7) > 4)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffc) {
		if (cr0 >> 31) {
			for (i = 0; i < 4; i++) {
				if (i == 0) {
					a = mmutranslate_read_2386(addr + i);
					addr64a[i] = (uint32_t) a;
				} else if (!((addr + i) & 0xfff)) {
					a = mmutranslate_read_2386(addr + 3);
					addr64a[i] = (uint32_t) a;
					if (!cpu_state.abrt) {
						a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
						addr64a[i] = (uint32_t) a;
					}
				} else {
					a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
					addr64a[i] = (uint32_t) a;
				}

				if (a > 0xffffffffULL)
					return 0xffff;
			}
		}

		/* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
		   their result as a parameter to be used if needed. */
		return readmemwl_no_mmut(addr, addr64a) |
		       (((uint32_t) readmemwl_no_mmut(addr + 2, &(addr64a[2]))) << 16);
	}
    }

    if (cr0 >> 31) {
	a = mmutranslate_read_2386(addr);
	addr64a[0] = (uint32_t) a;

	if (a > 0xffffffffULL)
		return 0xffffffff;
    }

    addr = addr64a[0] & rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_l)
	return map->read_l(addr, map->priv);

    if (map && map->read_w)
	return map->read_w(addr, map->priv) |
	       ((uint32_t) (map->read_w(addr + 2, map->priv)) << 16);

    if (map && map->read_b)
	return map->read_b(addr, map->priv) |
	       ((uint32_t) (map->read_b(addr + 1, map->priv)) << 8) |
	       ((uint32_t) (map->read_b(addr + 2, map->priv)) << 16) |
	       ((uint32_t) (map->read_b(addr + 3, map->priv)) << 24);

    return 0xffffffff;
}


void
writememll_2386(uint32_t addr, uint32_t val)
{
    mem_mapping_t *map;
    int i;
    uint64_t a = 0x0000000000000000ULL;

    for (i = 0; i < 4; i++)
	addr64a[i] = (uint64_t) (addr + i);
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_WRITE, 4);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 3) {
	if (!cpu_cyrix_alignment || (addr & 7) > 4)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffc) {
		if (cr0 >> 31) {
			for (i = 0; i < 4; i++) {
				if (i == 0) {
					a = mmutranslate_write_2386(addr + i);
					addr64a[i] = (uint32_t) a;
				} else if (!((addr + i) & 0xfff)) {
					a = mmutranslate_write_2386(addr + 3);
					addr64a[i] = (uint32_t) a;
					if (!cpu_state.abrt) {
						a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
						addr64a[i] = (uint32_t) a;
					}
				} else {
					a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
					addr64a[i] = (uint32_t) a;
				}

				if (a > 0xffffffffULL)
					return;
			}
		}

		/* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
		   their result as a parameter to be used if needed. */
		writememwl_no_mmut(addr, &(addr64a[0]), val);
		writememwl_no_mmut(addr + 2, &(addr64a[2]), val >> 16);
		return;
	}
    }

    if (cr0 >> 31) {
	a = mmutranslate_write_2386(addr);
	addr64a[0] = (uint32_t) a;

	if (a > 0xffffffffULL)
		return;
    }

    addr = addr64a[0] & rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_l) {
	map->write_l(addr, val,	   map->priv);
	return;
    }
    if (map && map->write_w) {
	map->write_w(addr,     val,       map->priv);
	map->write_w(addr + 2, val >> 16, map->priv);
	return;
    }
    if (map && map->write_b) {
	map->write_b(addr,     val,       map->priv);
	map->write_b(addr + 1, val >> 8,  map->priv);
	map->write_b(addr + 2, val >> 16, map->priv);
	map->write_b(addr + 3, val >> 24, map->priv);
	return;
    }
}


/* Read a long from memory without MMU translation - results of previous MMU translation passed as array. */
uint32_t
readmemll_no_mmut_2386(uint32_t addr, uint32_t *a64)
{
    mem_mapping_t *map;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 4);

    mem_logical_addr = addr;

    if (addr & 3) {
	if (!cpu_cyrix_alignment || (addr & 7) > 4)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffc) {
		if (cr0 >> 31) {
			if (cpu_state.abrt || high_page)
				return 0xffffffff;
		}

		return readmemwl_no_mmut(addr, a64) |
		       ((uint32_t) (readmemwl_no_mmut(addr + 2, &(a64[2]))) << 16);
	}
    }

    if (cr0 >> 31) {
	if (cpu_state.abrt || high_page)
		return 0xffffffff;

	addr = (uint32_t) (a64[0] & rammask);
    } else
	addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_l)
	return map->read_l(addr, map->priv);

    if (map && map->read_w)
	return map->read_w(addr, map->priv) |
	       ((uint32_t) (map->read_w(addr + 2, map->priv)) << 16);

    if (map && map->read_b)
	return map->read_b(addr, map->priv) |
	       ((uint32_t) (map->read_b(addr + 1, map->priv)) << 8) |
	       ((uint32_t) (map->read_b(addr + 2, map->priv)) << 16) |
	       ((uint32_t) (map->read_b(addr + 3, map->priv)) << 24);

    return 0xffffffff;
}


/* Write a long to memory without MMU translation - results of previous MMU translation passed as array. */
void
writememll_no_mmut_2386(uint32_t addr, uint32_t *a64, uint32_t val)
{
    mem_mapping_t *map;

    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 4);

    mem_logical_addr = addr;

    if (addr & 3) {
	if (!cpu_cyrix_alignment || (addr & 7) > 4)
		cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xffc) {
		if (cr0 >> 31) {
			if (cpu_state.abrt || high_page)
				return;
		}

		writememwl_no_mmut(addr, &(a64[0]), val);
		writememwl_no_mmut(addr + 2, &(a64[2]), val >> 16);
		return;
	}
    }

    if (cr0 >> 31) {
	if (cpu_state.abrt || high_page)
		return;

	addr = (uint32_t) (a64[0] & rammask);
    } else
	addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_l) {
	map->write_l(addr, val,	   map->priv);
	return;
    }
    if (map && map->write_w) {
	map->write_w(addr,     val,       map->priv);
	map->write_w(addr + 2, val >> 16, map->priv);
	return;
    }
    if (map && map->write_b) {
	map->write_b(addr,     val,       map->priv);
	map->write_b(addr + 1, val >> 8,  map->priv);
	map->write_b(addr + 2, val >> 16, map->priv);
	map->write_b(addr + 3, val >> 24, map->priv);
	return;
    }
}


uint64_t
readmemql_2386(uint32_t addr)
{
    mem_mapping_t *map;
    int i;
    uint64_t a = 0x0000000000000000ULL;

    for (i = 0; i < 8; i++)
	addr64a[i] = (uint64_t) (addr + i);
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_READ, 8);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 7) {
	cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xff8) {
		if (cr0 >> 31) {
			for (i = 0; i < 8; i++) {
				if (i == 0) {
					a = mmutranslate_read_2386(addr + i);
					addr64a[i] = (uint32_t) a;
				} else if (!((addr + i) & 0xfff)) {
					a = mmutranslate_read_2386(addr + 7);
					addr64a[i] = (uint32_t) a;
					if (!cpu_state.abrt) {
						a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
						addr64a[i] = (uint32_t) a;
					}
				} else {
					a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
					addr64a[i] = (uint32_t) a;
				}

				if (a > 0xffffffffULL)
					return 0xffff;
			}
		}

		/* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
		   their result as a parameter to be used if needed. */
		return readmemll_no_mmut(addr, addr64a) |
		       (((uint64_t) readmemll_no_mmut(addr + 4, &(addr64a[4]))) << 32);
	}
    }

    if (cr0 >> 31) {
	a = mmutranslate_read_2386(addr);
        addr64a[0] = (uint32_t) a;

	if (a > 0xffffffffULL)
		return 0xffffffffffffffffULL;
    }

    addr = addr64a[0] & rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_l)
	return map->read_l(addr, map->priv) | ((uint64_t)map->read_l(addr + 4, map->priv) << 32);

    return readmemll(addr) | ((uint64_t) readmemll(addr + 4) << 32);
}


void
writememql_2386(uint32_t addr, uint64_t val)
{
    mem_mapping_t *map;
    int i;
    uint64_t a = 0x0000000000000000ULL;

    for (i = 0; i < 8; i++)
	addr64a[i] = (uint64_t) (addr + i);
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_WRITE, 8);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 7) {
	cycles -= timing_misaligned;
	if ((addr & 0xfff) > 0xff8) {
		if (cr0 >> 31) {
			for (i = 0; i < 8; i++) {
				if (i == 0) {
					a = mmutranslate_write_2386(addr + i);
					addr64a[i] = (uint32_t) a;
				} else if (!((addr + i) & 0xfff)) {
					a = mmutranslate_write_2386(addr + 7);
					addr64a[i] = (uint32_t) a;
					if (!cpu_state.abrt) {
						a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
						addr64a[i] = (uint32_t) a;
					}
				} else {
					a = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
					addr64a[i] = (uint32_t) a;
				}

				if (addr64a[i] > 0xffffffffULL)
					return;
			}
		}

		/* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
		   their result as a parameter to be used if needed. */
		writememll_no_mmut(addr, addr64a, val);
		writememll_no_mmut(addr + 4, &(addr64a[4]), val >> 32);
		return;
	}
    }

    if (cr0 >> 31) {
	addr64a[0] = mmutranslate_write_2386(addr);
	if (addr64a[0] > 0xffffffffULL)
		return;
    }

    addr = addr64a[0] & rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_l) {
	map->write_l(addr,     val,       map->priv);
	map->write_l(addr + 4, val >> 32, map->priv);
	return;
    }
    if (map && map->write_w) {
	map->write_w(addr,     val,       map->priv);
	map->write_w(addr + 2, val >> 16, map->priv);
	map->write_w(addr + 4, val >> 32, map->priv);
	map->write_w(addr + 6, val >> 48, map->priv);
	return;
    }
    if (map && map->write_b) {
	map->write_b(addr,     val,       map->priv);
	map->write_b(addr + 1, val >> 8,  map->priv);
	map->write_b(addr + 2, val >> 16, map->priv);
	map->write_b(addr + 3, val >> 24, map->priv);
	map->write_b(addr + 4, val >> 32, map->priv);
	map->write_b(addr + 5, val >> 40, map->priv);
	map->write_b(addr + 6, val >> 48, map->priv);
	map->write_b(addr + 7, val >> 56, map->priv);
	return;
    }
}


void
do_mmutranslate_2386(uint32_t addr, uint32_t *a64, int num, int write)
{
    int i;
    uint32_t last_addr = addr + (num - 1);
    uint64_t a = 0x0000000000000000ULL;

    for (i = 0; i < num; i++)
	a64[i] = (uint64_t) addr;

    for (i = 0; i < num; i++) {
	if (cr0 >> 31) {
		/* If we have encountered at least one page fault, mark all subsequent addresses as
		   having page faulted, prevents false negatives in readmem*l_no_mmut. */
		if ((i > 0) && cpu_state.abrt && !high_page)
			a64[i] = a64[i - 1];
		/* If we are on the same page, there is no need to translate again, as we can just
		   reuse the previous result. */
		else if (i == 0) {
			a = mmutranslatereal_2386(addr, write);
			a64[i] = (uint32_t) a;
		} else if (!(addr & 0xfff)) {
			a = mmutranslatereal_2386(last_addr, write);
			a64[i] = (uint32_t) a;

			if (!cpu_state.abrt) {
				a = (a & 0xfffffffffffff000ULL) | ((uint64_t) (addr & 0xfff));
				a64[i] = (uint32_t) a;
			}
		} else {
			a = (a & 0xfffffffffffff000ULL) | ((uint64_t) (addr & 0xfff));
			a64[i] = (uint32_t) a;
		}
	}

	addr++;
    }
}
