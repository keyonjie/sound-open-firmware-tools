/*
 * Copyright (c) 2017, Intel Corporation.
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
 *  Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *          Keyon Jie <yang.jie@linux.intel.com>
 */


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sof/manifest.h>

#include "rimage.h"
#include "file_format.h"
#include "css.h"
#include "cse.h"
#include "plat_auth.h"
#include "manifest.h"

static int man_open_rom_file(struct image *image)
{
	sprintf(image->out_rom_file, "%s.rom", image->out_file);
	unlink(image->out_rom_file);

	/* allocate ROM image  */
	image->rom_image = calloc(image->adsp->rom_size, 1);
	if (image->rom_image == NULL)
		return -ENOMEM;

	/* open ROM outfile for writing */
	image->out_rom_fd = fopen(image->out_rom_file, "w");
	if (image->out_rom_fd == NULL) {
		fprintf(stderr, "error: unable to open %s for writing %d\n",
				image->out_rom_file, errno);
	}

	return 0;
}

static int man_open_manifest_file(struct image *image)
{
	/* open manifest outfile for writing */
	sprintf(image->out_man_file, "%s.met", image->out_file);
	unlink(image->out_man_file);

	image->out_man_fd = fopen(image->out_man_file, "w");
	if (image->out_man_fd == NULL) {
		fprintf(stderr, "error: unable to open %s for writing %d\n",
				image->out_man_file, errno);
	}

	return 0;
}

static int man_init_image(struct image *image)
{
	/* allocate image and copy template manifest */
	image->fw_image = calloc(image->adsp->image_size, 1);
	if (image->fw_image == NULL)
		return -ENOMEM;

	memcpy(image->fw_image, image->adsp->man,
		sizeof(struct fw_image_manifest));

	return 0;
}

/* we should call this after all segments size set up via iterate */
static uint32_t elf_to_file_offset(struct image *image,
	struct module *module, struct sof_man_module *man_module,
	Elf32_Shdr *section)
{
	uint32_t elf_addr = section->sh_addr, file_offset = 0;

	if (section->sh_type == SHT_PROGBITS) {
		if (section->sh_flags & SHF_EXECINSTR) {
			/* text segment */
			file_offset = elf_addr - module->text_start +
				module->foffset;
		}
		else {
			/* rodata segment, append to text segment */
			file_offset = elf_addr - module->data_start +
				module->foffset + module->text_file_size;

		}
	} else if (section->sh_type == SHT_NOBITS) {
		/* bss segment */
		file_offset = 0;
	}

	return file_offset;
}

/* write SRAM sections */
static int man_copy_sram(struct image *image, Elf32_Shdr *section,
	struct module *module, struct sof_man_module *man_module,
	int section_idx)
{
	uint32_t offset = elf_to_file_offset(image, module,
		man_module, section);
	uint32_t end = offset + section->sh_size;
	int seg_type = -1;
	void *buffer = image->fw_image + offset;
	size_t count;

	switch (section->sh_type) {
	case SHT_PROGBITS:
		/* text or data */
		if (section->sh_flags & SHF_EXECINSTR)
			seg_type = SOF_MAN_SEGMENT_TEXT;
		else
			seg_type = SOF_MAN_SEGMENT_RODATA;
		break;
	case SHT_NOBITS:
		seg_type = SOF_MAN_SEGMENT_BSS;
	default:
		return 0;
	}

	/* file_offset for segment should not be 0s, we set it to
	  * the smallest offset of its modules ATM.
	  */
	if (man_module->segment[seg_type].file_offset > offset ||
		man_module->segment[seg_type].file_offset == 0)
		man_module->segment[seg_type].file_offset = offset;

	count = fread(buffer, 1, section->sh_size, module->fd);
	if (count != section->sh_size) {
		fprintf(stderr, "error: cant read section %d\n", -errno);
		return -errno;
	}

	/* get module end offset  ? */
	if (end > image->image_end)
		image->image_end = end;

	fprintf(stdout, "\t%d\t0x%x\t0x%x\t0x%x\t%s\n", section_idx,
		section->sh_addr, section->sh_size, offset,
		seg_type == SOF_MAN_SEGMENT_TEXT ? "TEXT" : "DATA");

	return 0;
}

static int man_copy_elf_section(struct image *image, Elf32_Shdr *section,
	struct module *module, struct sof_man_module *man_module, int idx)
{
	int ret;

	/* seek to ELF section */
	ret = fseek(module->fd, section->sh_offset, SEEK_SET);
	if (ret < 0) {
		fprintf(stderr, "error: can't seek to section %d\n", ret);
		return ret;
	}

	/* write data to DRAM or ROM image */
	if (!elf_is_rom(image, section))
		return man_copy_sram(image, section, module, man_module, idx);

	return 0;
}

static int man_get_module_manifest(struct image *image, struct module *module,
		struct sof_man_module *man_module)
{
	Elf32_Shdr *section;
	struct sof_man_segment_desc *segment;
	struct sof_man_module sof_mod;
	size_t count;
	int ret, man_section_idx;

	fprintf(stdout, "Module Write: %s\n", module->elf_file);

	/* find manifest module data */
	man_section_idx = elf_find_section(image, module, ".module");
	if (man_section_idx < 0) {
		return -EINVAL;
	}

	fprintf(stdout, " Manifest module metadata section at index %d\n",
		man_section_idx);
	section = &module->section[man_section_idx];

	/* load in manifest data */
	ret = fseek(module->fd, section->sh_offset, SEEK_SET);
	if (ret < 0) {
		fprintf(stderr, "error: can't seek to section %d\n", ret);
		return ret;
	}
	count = fread(&sof_mod, 1, sizeof(sof_mod), module->fd);
	if (count != sizeof(sof_mod)) {
		fprintf(stderr, "error: can't read section %d\n", -errno);
		return -errno;
	}

	/* configure man_module with sofmod data */
	strncpy(man_module->struct_id, "$AME", 4);
	man_module->entry_point = sof_mod.entry_point;
	strncpy(man_module->name, sof_mod.name, SOF_MAN_MOD_NAME_LEN);
	memcpy(man_module->uuid, sof_mod.uuid, 16);
	man_module->affinity_mask = sof_mod.affinity_mask;
	man_module->type.auto_start = sof_mod.type.auto_start;
	man_module->type.domain_dp = sof_mod.type.domain_dp;
	man_module->type.domain_ll = sof_mod.type.domain_ll;
	man_module->type.load_type = sof_mod.type.load_type;

	/* text segment */
	segment = &man_module->segment[SOF_MAN_SEGMENT_TEXT];
	segment->flags.r.contents = 1;
	segment->flags.r.alloc = 1;
	segment->flags.r.load = 1;
	segment->flags.r.readonly = 1;
	segment->flags.r.code = 1;

	/* data segment */
	segment = &man_module->segment[SOF_MAN_SEGMENT_RODATA];
	segment->flags.r.contents = 1;
	segment->flags.r.alloc = 1;
	segment->flags.r.load = 1;
	segment->flags.r.readonly = 1;
	segment->flags.r.data = 1;
	segment->flags.r.type = 1;

	/* bss segment */
	segment = &man_module->segment[SOF_MAN_SEGMENT_BSS];
	segment->flags.r.alloc = 1;
	segment->flags.r.type = 2;

	fprintf(stdout, " Entry point 0x%8.8x\n", man_module->entry_point);

	return 0;
}

static int man_module_create(struct image *image, struct module *module,
	struct sof_man_module *man_module)
{
	/* create module and segments */
	uint32_t valid = (SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR);
	const struct adsp *adsp = image->adsp;
	Elf32_Shdr *section;
	size_t count;
	int i, err;
	unsigned pages;

	image->image_end = 0;

	err = man_get_module_manifest(image, module, man_module);
	if (err < 0)
		return err;

	/* stack size ??? convert sizes to PAGES */
	man_module->instance_bss_size = 1;

	/* max number of instances of this module ?? */
	man_module->instance_max_count = 1;

	fprintf(stdout, "\n\tTotals\tStart\t\tEnd\t\tSize");

	fprintf(stdout, "\n\tTEXT\t0x%x\t0x%x\t0x%x\n",
			module->text_start, module->text_end,
			module->text_end - module->text_start);
	fprintf(stdout, "\tDATA\t0x%x\t0x%x\t0x%x\n",
			module->data_start, module->data_end,
			module->data_end - module->data_start);
	fprintf(stdout, "\tBSS\t0x%x\t0x%x\t0x%x\n\n ",
			module->bss_start, module->bss_end,
			module->bss_end - module->bss_start);

	/* main module */
	/* text section is first */
	man_module->segment[SOF_MAN_SEGMENT_TEXT].file_offset =
		module->foffset;
	man_module->segment[SOF_MAN_SEGMENT_TEXT].v_base_addr =
		module->text_start;

	/* calculates those padding 0s by the start of next segment */
	pages = module->text_file_size / MAN_PAGE_SIZE;
	if (module->text_file_size % MAN_PAGE_SIZE)
		pages += 1;

	man_module->segment[SOF_MAN_SEGMENT_TEXT].flags.r.length = pages;

	/* data section */
	man_module->segment[SOF_MAN_SEGMENT_RODATA].v_base_addr =
		module->data_start;
	man_module->segment[SOF_MAN_SEGMENT_RODATA].file_offset =
			module->foffset + module->text_file_size;
	pages = module->data_file_size / MAN_PAGE_SIZE;
	if (module->data_file_size % MAN_PAGE_SIZE)
		pages += 1;

	man_module->segment[SOF_MAN_SEGMENT_RODATA].flags.r.length = pages;

	/* bss is last */
	man_module->segment[SOF_MAN_SEGMENT_BSS].file_offset = 0;
	man_module->segment[SOF_MAN_SEGMENT_BSS].v_base_addr = module->bss_start;
	pages = (module->bss_end - module->bss_start) / MAN_PAGE_SIZE;
	if ((module->bss_end - module->bss_start) % MAN_PAGE_SIZE)
		pages += 1;
	man_module->segment[SOF_MAN_SEGMENT_BSS].flags.r.length = pages;

	fprintf(stdout, "\tNo\tAddress\t\tSize\tFile\tType\n");

	/* find all sections and copy to corresponding segments */
	for (i = 0; i < module->hdr.e_shnum; i++) {

		section = &module->section[i];

		/* only check valid sections */
		if (!(section->sh_flags & valid))
			continue;

		if (section->sh_size == 0)
			continue;

		/* text or data section */
		if (!elf_is_rom(image, section))
			err = man_copy_elf_section(image, section, module,
				man_module, i);

		if (err < 0) {
			fprintf(stderr, "error: failed to write section #%d\n", i);
			return err;
		}
	}
	fprintf(stdout, "\n");

	/* round module end upto nearest page */
	if (image->image_end % MAN_PAGE_SIZE) {
		image->image_end = (image->image_end / MAN_PAGE_SIZE) + 1;
		image->image_end *= MAN_PAGE_SIZE;
	}

	fprintf(stdout, " Total pages text %d data %d bss %d module file limit: 0x%x\n\n",
		man_module->segment[SOF_MAN_SEGMENT_TEXT].flags.r.length,
		man_module->segment[SOF_MAN_SEGMENT_RODATA].flags.r.length,
		man_module->segment[SOF_MAN_SEGMENT_BSS].flags.r.length,
		image->image_end);
	return 0;
}

static int man_write_fw_mod(struct image *image)
{
	int count;
	long unsigned int size = 0;

	/* write ROM - for VM use only */
	count = fwrite(image->rom_image, image->adsp->rom_size, 1,
		image->out_rom_fd);
	if (count != 1) {
		fprintf(stderr, "error: failed to write rom %s %d\n",
			image->out_rom_file, -errno);
		return -errno;
	}
	fclose(image->out_rom_fd);

	/* write manifest and signed image */
	count = fwrite(image->fw_image,
			image->image_end,
			1, image->out_fd);

	/* did the image write succeed ? */
	if (count != 1) {
		fprintf(stderr, "error: failed to write signed firmware %s %d\n",
			image->out_file, -errno);
		return -errno;
	}

	return 0;
}

/* used by others */
static int man_write_fw(struct image *image)
{
	struct sof_man_fw_desc *desc;
	struct fw_image_manifest *m;
	struct module *module;
	struct sof_man_module *man_module;
	uint8_t hash[SOF_MAN_MOD_SHA256_LEN];
	int ret, count, i, man_section_idx;

	/* init image */
	ret = man_init_image(image);
	if (ret < 0)
		goto err;

	/* open ROM image */
	ret = man_open_rom_file(image);
	if (ret < 0)
		goto err;

	/* create the manifest */
	ret = man_open_manifest_file(image);
	if (ret < 0)
		goto err;

	/* create the module */
	m = image->fw_image;
	desc = image->fw_image + MAN_DESC_OFFSET;

	/* create each module */
	m->desc.header.num_module_entries = image->num_modules;
	for (i = 0; i < image->num_modules; i++) {

		man_module = sof_man_get_module(desc, i);
		module = &image->module[i];

		/* set module file offset */
		if (i == 0) {
			module->foffset = FILE_TEXT_OFFSET;
		} else {
			module->foffset = image->image_end;
		}

		ret = man_module_create(image, module, man_module);
		if (ret < 0)
			goto err;
	}

	fprintf(stdout, "Firmware completing manifest\n");

	/* create structures from end of file to start of file */
	ri_adsp_meta_data_create(image);
	ri_plat_ext_data_create(image);
	ri_css_hdr_create(image);
	ri_cse_create(image);

	fprintf(stdout, "Firmware file size 0x%x page count %d\n",
		FILE_TEXT_OFFSET - MAN_DESC_OFFSET + image->image_end,
		desc->header.preload_page_count);

	/* calculate hash for each module */
	for (i = 0; i < image->num_modules; i++) {

		module = &image->module[i];
		man_module = sof_man_get_module(desc, i);

		ri_hash(image, man_module->segment[SOF_MAN_SEGMENT_TEXT].file_offset,
			(man_module->segment[SOF_MAN_SEGMENT_TEXT].flags.r.length +
			man_module->segment[SOF_MAN_SEGMENT_RODATA].flags.r.length) *
			MAN_PAGE_SIZE, man_module->hash);
	}

	/* calculate hash for ADSP meta data extension - 0x480 to end */
	ri_hash(image, MAN_FW_DESC_OFFSET, image->image_end
		- MAN_FW_DESC_OFFSET, m->adsp_file_ext.comp_desc[0].hash);

	/* calculate hash for platform auth data - repeated in hash 2 and 4 */
	ri_hash(image, MAN_META_EXT_OFFSET,
		sizeof(struct sof_man_adsp_meta_file_ext), hash);

	/* hash values in reverse order */
	for (i = 0; i < SOF_MAN_MOD_SHA256_LEN; i++) {
		m->signed_pkg.module[0].hash[i] =
		m->partition_info.module[0].hash[i] =
			hash[SOF_MAN_MOD_SHA256_LEN - 1 - i];
	}

	/* sign manifest */
	ret = ri_manifest_sign(image);
	if (ret < 0)
		goto err;

	/* write the firmware */
	ret = man_write_fw_mod(image);
	if (ret < 0)
		goto err;

	fprintf(stdout, "Firmware manifest and signing completed !\n");
	return 0;

err:
	free(image->rom_image);
	free(image->fw_image);
	unlink(image->out_file);
	unlink(image->out_rom_file);
	return ret;
}

#define ADSP_APL_DSP_ROM_BASE	0xBEFE0000
#define ADSP_APL_DSP_ROM_SIZE	0x00002000
#define APL_DSP_BASE_ENTRY	0xa000a000

#define ADSP_CNL_DSP_ROM_BASE	0xBEFE0000
#define ADSP_CNL_DSP_ROM_SIZE	0x00002000
#define CNL_DSP_IMR_BASE_ENTRY	0xb0038000
#define CNL_DSP_HP_BASE_ENTRY	0xbe040000

/* list of supported adsp */
const struct adsp machine_apl = {
	.name = "apl",
	.rom_base = ADSP_APL_DSP_ROM_BASE,
	.rom_size = ADSP_APL_DSP_ROM_SIZE,
	.sram_base = APL_DSP_BASE_ENTRY,
	.sram_size = 0x100000,
	.image_size = 0x100000,
	.dram_offset = 0,
	.machine_id = MACHINE_APOLLOLAKE,
	.write_firmware = man_write_fw,
	.man = &apl_manifest,
	.base_fw_text_size_fixup = 0xa000,
};

const struct adsp machine_cnl = {
	.name = "cnl",
	.rom_base = ADSP_CNL_DSP_ROM_BASE,
	.rom_size = ADSP_CNL_DSP_ROM_SIZE,
	.imr_base = CNL_DSP_IMR_BASE_ENTRY,
	.imr_size = 0x100000,
	.sram_base = CNL_DSP_HP_BASE_ENTRY,
	.sram_size = 0x100000,
	.image_size = 0x100000,
	.dram_offset = 0,
	.machine_id = MACHINE_CANNONLAKE,
	.write_firmware = man_write_fw,
	.man = &cnl_manifest,
};
