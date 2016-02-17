/*
 * Copyright (c) 2014-2015, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "priv.h"
#include "gk20a.h"
#include <core/client.h>
#include <core/gpuobj.h>
#include <subdev/bar.h>
#include <subdev/fb.h>
#include <subdev/mc.h>
#include <subdev/timer.h>
#include <subdev/mmu.h>
#include <subdev/pmu.h>
#include <core/object.h>
#include <core/device.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <subdev/clk.h>
#include <subdev/timer.h>
#include <subdev/volt.h>

#define APP_VERSION_GK20A  17997577
#define GK20A_PMU_UCODE_SIZE_MAX  (256 * 1024)

#define GK20A_PMU_DMEM_BLKSIZE2		    8

#define PMU_UNIT_REWIND		(0x00)
#define PMU_UNIT_PG		(0x03)
#define PMU_UNIT_INIT		(0x07)
#define PMU_UNIT_PERFMON	(0x12)
#define PMU_UNIT_THERM		(0x1B)
#define PMU_UNIT_RC		(0x1F)
#define PMU_UNIT_NULL		(0x20)
#define PMU_UNIT_END		(0x23)
#define PMU_UNIT_TEST_START	(0xFE)
#define PMU_UNIT_END_SIM	(0xFF)
#define PMU_UNIT_TEST_END	(0xFF)

#define PMU_UNIT_ID_IS_VALID(id)		\
		(((id) < PMU_UNIT_END) || ((id) >= PMU_UNIT_TEST_START))
#define PMU_DMEM_ALIGNMENT		(4)
#define PMU_DMEM_ALLOC_ALIGNMENT	(32)

#define GK20A_PMU_UCODE_IMAGE	"gpmu_ucode.bin"

#define PMU_CMD_FLAGS_PMU_MASK		(0xF0)
#define PMU_CMD_FLAGS_STATUS		BIT(0)
#define PMU_CMD_FLAGS_INTR		BIT(1)
#define PMU_CMD_FLAGS_EVENT		BIT(2)
#define PMU_CMD_FLAGS_WATERMARK		BIT(3)

/*
 * Worst case wait will be 40*40us i.e 1.6 ms,
 * (see its usage) which is acceptable and sufficient for all
 * busy tasks to finish
 */
#define MAX_RETRIES			40
enum {
	OFLAG_READ = 0,
	OFLAG_WRITE
};

#define PMU_MSG_HDR_SIZE	sizeof(struct pmu_hdr)
#define PMU_CMD_HDR_SIZE	sizeof(struct pmu_hdr)

#define PMU_INIT_MSG_TYPE_PMU_INIT 0

#define PMU_RC_MSG_TYPE_UNHANDLED_CMD	0
/*
 *Struct to contain PMU PERFMON CMD.
 */
struct pmu_perfmon_cmd {
	u8 cmd_type;
};

/*
 * Struct to contain PMU cmd format.
 * hdr     - PMU hdr format.
 * perfmon - Perfmon cmd i.e It's a cmd used to start/init Perfmon TASK in PMU .
 *
 * NOTE:
 * More type of commands (structs) can be added to this generic struct.
 * The command(struct) added should have same format in PMU fw as well.
 * i.e same struct should be present on PMU fw and Nouveau.
 */
struct pmu_cmd {
	struct pmu_hdr hdr;
	struct pmu_perfmon_cmd perfmon;
};

/* Writen by sw, read by Pmu, protected by sw mutex lock High Prio Q. */
#define PMU_COMMAND_QUEUE_HPQ		0
/* Writen by sw, read by Pmu, protected by sw mutex lock Low Prio Q. */
#define PMU_COMMAND_QUEUE_LPQ		1
/* Writen by pmu, read by Sw, accessed by interrupt handler, no lock. */
#define PMU_MESSAGE_QUEUE		4

#define PMU_IS_COMMAND_QUEUE(id)	\
		((id)  < PMU_MESSAGE_QUEUE)

#define PMU_IS_SW_COMMAND_QUEUE(id)	\
		(((id) == PMU_COMMAND_QUEUE_HPQ) || \
		 ((id) == PMU_COMMAND_QUEUE_LPQ))

#define  PMU_IS_MESSAGE_QUEUE(id)	\
		((id) == PMU_MESSAGE_QUEUE)

#define QUEUE_ALIGNMENT			(4)

#define PMU_INVALID_MUTEX_OWNER_ID	(0)

#define PMU_INVALID_SEQ_DESC		(~0)

enum {
	PMU_SEQ_STATE_FREE = 0,
	PMU_SEQ_STATE_PENDING,
	PMU_SEQ_STATE_USED,
	PMU_SEQ_STATE_CANCELLED
};

struct pmu_payload {
	struct {
		void *buf;
		u32 offset;
		u16 size;
	} in, out;
};



struct gk20a_pmu_dvfs_data {
	int p_load_target;
	int p_load_max;
	int p_smooth;
	unsigned int avg_load;
};

struct gk20a_pmu_dvfs_dev_status {
	unsigned long total;
	unsigned long busy;
	int cur_state;
};


void
gk20a_release_firmware(struct nvkm_pmu *ppmu, const struct firmware *pfw)
{
	nv_debug(ppmu, "firmware released\n");
	release_firmware(pfw);
}

int
gk20a_load_firmware(struct nvkm_pmu *ppmu, const struct firmware **pfw, const
char *fw_name)
{
	struct nvkm_device *dev;
	char name[72];
	int ret;

	dev = nv_device(ppmu);
	snprintf(name, sizeof(name), "nouveau/%s", fw_name);
	ret = request_firmware(pfw, name, nv_device_base(dev));
	return ret;
}

/*reads memory allocated to nvgpu object*/
void
gpu_obj_memwr(struct nvkm_gpuobj *ucodeobj, int offset, void *src, int size)
{
	int temp = size;
	u32 *source32;
	u16 *source16;
	u8 *source8;
	int four_bytes_cnt, two_bytes_cnt, one_bytes_cnt;

	four_bytes_cnt = temp / 4;
	temp = temp % 4;
	two_bytes_cnt = temp / 2;
	temp = temp % 2;
	one_bytes_cnt = temp;
	source32 = (u32 *)src;
	for (temp = 0; temp < four_bytes_cnt; temp++) {
		source32 = (u32 *)src + temp;
		nv_wo32(ucodeobj, offset, *source32);
		offset += 4;
	}
	source16 = (u16 *)source32;
	for (temp = 0; temp < two_bytes_cnt; temp++) {
		source16 = (u16 *)source32 + temp;
		nv_wo16(ucodeobj, offset, *source16);
		offset += 2;
	}
	source8 = (u8 *)source16;
	for (temp = 0; temp < one_bytes_cnt; temp++) {
		source8 = (u8 *)source16 + temp;
		nv_wo08(ucodeobj, offset, *source8);
		offset += 1;
	}

}

static void
gk20a_pmu_dump_firmware_info(struct nvkm_pmu *pmu, const struct firmware *fw)
{
	struct pmu_ucode_desc *desc = (struct pmu_ucode_desc *)fw->data;

	nv_debug(pmu, "GK20A PMU firmware information\n");
	nv_debug(pmu, "descriptor size = %u\n", desc->descriptor_size);
	nv_debug(pmu, "image size  = %u\n", desc->image_size);
	nv_debug(pmu, "app_version = 0x%08x\n", desc->app_version);
	nv_debug(pmu, "date = %s\n", desc->date);
	nv_debug(pmu, "bootloader_start_offset = 0x%08x\n",
				desc->bootloader_start_offset);
	nv_debug(pmu, "bootloader_size = 0x%08x\n", desc->bootloader_size);
	nv_debug(pmu, "bootloader_imem_offset = 0x%08x\n",
				desc->bootloader_imem_offset);
	nv_debug(pmu, "bootloader_entry_point = 0x%08x\n",
				desc->bootloader_entry_point);
	nv_debug(pmu, "app_start_offset = 0x%08x\n", desc->app_start_offset);
	nv_debug(pmu, "app_size = 0x%08x\n", desc->app_size);
	nv_debug(pmu, "app_imem_offset = 0x%08x\n", desc->app_imem_offset);
	nv_debug(pmu, "app_imem_entry = 0x%08x\n", desc->app_imem_entry);
	nv_debug(pmu, "app_dmem_offset = 0x%08x\n", desc->app_dmem_offset);
	nv_debug(pmu, "app_resident_code_offset = 0x%08x\n",
			desc->app_resident_code_offset);
	nv_debug(pmu, "app_resident_code_size = 0x%08x\n",
			desc->app_resident_code_size);
	nv_debug(pmu, "app_resident_data_offset = 0x%08x\n",
			desc->app_resident_data_offset);
	nv_debug(pmu, "app_resident_data_size = 0x%08x\n",
			desc->app_resident_data_size);
	nv_debug(pmu, "nb_overlays = %d\n", desc->nb_overlays);

	nv_debug(pmu, "compressed = %u\n", desc->compressed);
}

static int
gk20a_pmu_dvfs_target(struct gk20a_pmu_priv *priv, int *state)
{
	struct nvkm_clk *clk = nvkm_clk(priv);

	return nvkm_clk_astate(clk, *state, 0, false);
}

static int
gk20a_pmu_dvfs_get_cur_state(struct gk20a_pmu_priv *priv, int *state)
{
	struct nvkm_clk *clk = nvkm_clk(priv);

	*state = clk->pstate;
	return 0;
}

static int
gk20a_pmu_dvfs_get_target_state(struct gk20a_pmu_priv *priv,
				int *state, int load)
{
	struct gk20a_pmu_dvfs_data *data = priv->data;
	struct nvkm_clk *clk = nvkm_clk(priv);
	int cur_level, level;

	/* For GK20A, the performance level is directly mapped to pstate */
	level = cur_level = clk->pstate;

	if (load > data->p_load_max) {
		level = min(clk->state_nr - 1, level + (clk->state_nr / 3));
	} else {
		level += ((load - data->p_load_target) * 10 /
				data->p_load_target) / 2;
		level = max(0, level);
		level = min(clk->state_nr - 1, level);
	}

	nv_trace(priv, "cur level = %d, new level = %d\n", cur_level, level);

	*state = level;

	if (level == cur_level)
		return 0;
	else
		return 1;
}

static int
gk20a_pmu_dvfs_get_dev_status(struct gk20a_pmu_priv *priv,
			      struct gk20a_pmu_dvfs_dev_status *status)
{
	status->busy = nv_rd32(priv, 0x10a508 + (BUSY_SLOT * 0x10));
	status->total= nv_rd32(priv, 0x10a508 + (CLK_SLOT * 0x10));
	return 0;
}

static void
gk20a_pmu_dvfs_reset_dev_status(struct gk20a_pmu_priv *priv)
{
	nv_wr32(priv, 0x10a508 + (BUSY_SLOT * 0x10), 0x80000000);
	nv_wr32(priv, 0x10a508 + (CLK_SLOT * 0x10), 0x80000000);
}

void
gk20a_pmu_dvfs_init(struct gk20a_pmu_priv *priv)
{
	nv_wr32(priv, 0x10a504 + (BUSY_SLOT * 0x10), 0x00200001);
	nv_wr32(priv, 0x10a50c + (BUSY_SLOT * 0x10), 0x00000002);
	nv_wr32(priv, 0x10a50c + (CLK_SLOT * 0x10), 0x00000003);
}

void
gk20a_pmu_dvfs_work(struct nvkm_alarm *alarm)
{
	struct gk20a_pmu_priv *priv =
		container_of(alarm, struct gk20a_pmu_priv, alarm);
	struct gk20a_pmu_dvfs_data *data = priv->data;
	struct gk20a_pmu_dvfs_dev_status status;
	struct nvkm_clk *clk = nvkm_clk(priv);
	struct nvkm_volt *volt = nvkm_volt(priv);
	u32 utilization = 0;
	int state, ret;

	/*
	 * The PMU is initialized before CLK and VOLT, so we have to make sure the
	 * CLK and VOLT are ready here.
	 */
	if (!clk || !volt)
		goto resched;

	ret = gk20a_pmu_dvfs_get_dev_status(priv, &status);
	if (ret) {
		nv_warn(priv, "failed to get device status\n");
		goto resched;
	}

	if (status.total)
		utilization = div_u64((u64)status.busy * 100, status.total);

	data->avg_load = (data->p_smooth * data->avg_load) + utilization;
	data->avg_load /= data->p_smooth + 1;
	nv_trace(priv, "utilization = %d %%, avg_load = %d %%\n",
			utilization, data->avg_load);

	ret = gk20a_pmu_dvfs_get_cur_state(priv, &state);
	if (ret) {
		nv_warn(priv, "failed to get current state\n");
		goto resched;
	}

	if (gk20a_pmu_dvfs_get_target_state(priv, &state, data->avg_load)) {
		nv_trace(priv, "set new state to %d\n", state);
		gk20a_pmu_dvfs_target(priv, &state);
	}

resched:
	gk20a_pmu_dvfs_reset_dev_status(priv);
	nvkm_timer_alarm(priv, 10000000, alarm);
}

int
gk20a_pmu_enable_hw(struct gk20a_pmu_priv *priv, struct nvkm_mc *pmc, bool enable)
{
	if (enable) {
		nv_mask(pmc, 0x000200, 0x00002000, 0x00002000);
		nv_rd32(pmc, 0x00000200);
		if (nv_wait(priv, 0x0010a10c, 0x00000006, 0x00000000))
			return 0;
		nv_mask(pmc, 0x00000200, 0x2000, 0x00000000);
		nv_error(priv, "Falcon mem scrubbing timeout\n");
		return -ETIMEDOUT;
	} else {
		nv_mask(pmc, 0x00000200, 0x2000, 0x00000000);
		return 0;
	}
}

void
gk20a_pmu_enable_irq(struct gk20a_pmu_priv *priv, struct nvkm_mc *pmc, bool enable)
{
	if (enable) {
		nv_debug(priv, "enable pmu irq\n");
		nv_wr32(priv, 0x0010a010, 0xff);
		nv_mask(pmc, 0x00000640, 0x1000000, 0x1000000);
		nv_mask(pmc, 0x00000644, 0x1000000, 0x1000000);
	} else {
		nv_debug(priv, "disable pmu irq\n");
		nv_mask(pmc, 0x00000640, 0x1000000, 0x00000000);
		nv_mask(pmc, 0x00000644, 0x1000000, 0x00000000);
		nv_wr32(priv, 0x0010a014, 0xff);
	}

}

int
gk20a_pmu_idle(struct gk20a_pmu_priv *priv)
{
	if (!nv_wait(priv, 0x0010a04c, 0x0000ffff, 0x00000000)) {
		nv_error(priv, "timeout waiting pmu idle\n");
		return -EBUSY;
	}

	return 0;
}

int
gk20a_pmu_enable(struct gk20a_pmu_priv *priv, struct nvkm_mc *pmc, bool enable)
{
	u32 pmc_enable;
	int err;

	if (enable) {
		err = gk20a_pmu_enable_hw(priv, pmc, true);
		if (err)
			return err;

		err = gk20a_pmu_idle(priv);
		if (err)
			return err;

		gk20a_pmu_enable_irq(priv, pmc, true);
	} else {
		pmc_enable = nv_rd32(pmc, 0x200);
		if ((pmc_enable & 0x2000) != 0x0) {
			gk20a_pmu_enable_irq(priv, pmc, false);
			gk20a_pmu_enable_hw(priv, pmc, false);
		}
	}

	return 0;
}

void
gk20a_pmu_copy_to_dmem(struct gk20a_pmu_priv *priv, u32 dst, u8 *src, u32 size,
		       u8 port)
{
	u32 i, words, bytes;
	u32 data, addr_mask;
	u32 *src_u32 = (u32 *)src;

	if (size == 0) {
		nv_error(priv, "size is zero\n");
		goto out;
	}

	if (dst & 0x3) {
		nv_error(priv, "dst (0x%08x) not 4-byte aligned\n", dst);
		goto out;
	}

	mutex_lock(&priv->pmu_copy_lock);
	words = size >> 2;
	bytes = size & 0x3;
	addr_mask = 0xfffc;
	dst &= addr_mask;

	nv_wr32(priv, (0x10a1c0 + (port * 8)), (dst | (0x1 << 24)));

	for (i = 0; i < words; i++) {
		nv_wr32(priv, (0x10a1c4 + (port * 8)), src_u32[i]);
		nv_debug(priv, "0x%08x\n", src_u32[i]);
	}

	if (bytes > 0) {
		data = 0;
		for (i = 0; i < bytes; i++)
			((u8 *)&data)[i] = src[(words << 2) + i];
		nv_wr32(priv, (0x10a1c4 + (port * 8)), data);
		nv_debug(priv, "0x%08x\n", data);
	}

	data = nv_rd32(priv, (0x10a1c0 + (port * 8))) & addr_mask;
	size = ALIGN(size, 4);
	if (data != dst + size) {
		nv_error(priv, "copy failed.... bytes written %d, expected %d",
							      data - dst, size);
	}
	mutex_unlock(&priv->pmu_copy_lock);
out:
	nv_debug(priv, "exit %s\n", __func__);
}

static void
gk20a_copy_from_dmem(struct gk20a_pmu_priv *priv, u32 src, u8 *dst, u32 size,
		     u8 port)
{
	u32 i, words, bytes;
	u32 data, addr_mask;
	u32 *dst_u32 = (u32 *)dst;

	if (size == 0) {
		nv_error(priv, "size is zero\n");
		goto out;
	}

	if (src & 0x3) {
		nv_error(priv, "src (0x%08x) not 4-byte aligned\n", src);
		goto out;
	}

	mutex_lock(&priv->pmu_copy_lock);

	words = size >> 2;
	bytes = size & 0x3;

	addr_mask = 0xfffc;

	src &= addr_mask;

	nv_wr32(priv, (0x10a1c0 + (port * 8)), (src | (0x1 << 25)));

	for (i = 0; i < words; i++) {
		dst_u32[i] = nv_rd32(priv, (0x0010a1c4 + port * 8));
		nv_debug(priv, "0x%08x\n", dst_u32[i]);
	}
	if (bytes > 0) {
		data = nv_rd32(priv, (0x0010a1c4 + port * 8));
		nv_debug(priv, "0x%08x\n", data);

		for (i = 0; i < bytes; i++)
			dst[(words << 2) + i] = ((u8 *)&data)[i];
	}
	mutex_unlock(&priv->pmu_copy_lock);
out:
	nv_debug(priv, "exit %s\n", __func__);
}

void
gk20a_pmu_seq_init(struct gk20a_pmu_priv *pmu)
{
	u32 i;

	memset(pmu->seq, 0,
		sizeof(struct pmu_sequence) * PMU_MAX_NUM_SEQUENCES);
	memset(pmu->pmu_seq_tbl, 0,
		sizeof(pmu->pmu_seq_tbl));

	for (i = 0; i < PMU_MAX_NUM_SEQUENCES; i++)
		pmu->seq[i].id = i;
}

static int
gk20a_pmu_seq_acquire(struct gk20a_pmu_priv *priv,
			struct pmu_sequence **pseq)
{
	struct nvkm_pmu *pmu = &priv->base;
	struct pmu_sequence *seq;
	u32 index;

	mutex_lock(&priv->pmu_seq_lock);
	index = find_first_zero_bit(priv->pmu_seq_tbl,
				PMU_MAX_NUM_SEQUENCES);
	if (index >= PMU_MAX_NUM_SEQUENCES) {
		nv_error(pmu,
			"no free sequence available");
		mutex_unlock(&priv->pmu_seq_lock);
		return -EAGAIN;
	}
	set_bit(index, priv->pmu_seq_tbl);
	mutex_unlock(&priv->pmu_seq_lock);
	seq = &priv->seq[index];
	seq->state = PMU_SEQ_STATE_PENDING;
	nv_debug(pmu, "seq id acquired is = %d index = %d\n", seq->id, index);

	*pseq = seq;
	return 0;
}

static void
gk20a_pmu_seq_release(struct gk20a_pmu_priv *pmu,
			struct pmu_sequence *seq)
{
	seq->state = PMU_SEQ_STATE_FREE;
	seq->desc = PMU_INVALID_SEQ_DESC;
	seq->callback = NULL;
	seq->cb_params = NULL;
	seq->msg = NULL;
	seq->out_payload = NULL;
	seq->in_gk20a.alloc.dmem.size = 0;
	seq->out_gk20a.alloc.dmem.size = 0;
	nv_debug(&pmu->base, "seq released %d\n", seq->id);
	clear_bit(seq->id, pmu->pmu_seq_tbl);
}

static int
gk20a_pmu_queue_init(struct gk20a_pmu_priv *priv,
		u32 id, struct pmu_init_msg_pmu_gk20a *init)
{
	struct nvkm_pmu *pmu = &priv->base;
	struct pmu_queue *queue = &priv->queue[id];

	queue->id = id;
	queue->index = init->queue_info[id].index;
	queue->offset = init->queue_info[id].offset;
	queue->size = init->queue_info[id].size;
	queue->mutex_id = id;
	mutex_init(&queue->mutex);

	nv_debug(pmu, "queue %d: index %d, offset 0x%08x, size 0x%08x",
		id, queue->index, queue->offset, queue->size);

	return 0;
}

static int
gk20a_pmu_queue_head_get(struct gk20a_pmu_priv *priv, struct pmu_queue *queue,
			u32 *head)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (WARN_ON(!head))
		return -EINVAL;

	if (PMU_IS_COMMAND_QUEUE(queue->id)) {
		if (queue->index >= 0x00000004)
			return -EINVAL;

		*head = nv_rd32(pmu, 0x0010a4a0 + (queue->index * 4)) &
				0xffffffff;
	} else {
		*head = nv_rd32(pmu, 0x0010a4c8) & 0xffffffff;
	}

	return 0;
}

static int
gk20a_pmu_queue_head_set(struct gk20a_pmu_priv *priv, struct pmu_queue *queue,
			u32 *head)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (WARN_ON(!head))
		return -EINVAL;

	if (PMU_IS_COMMAND_QUEUE(queue->id)) {
		if (queue->index >= 0x00000004)
			return -EINVAL;

		nv_wr32(pmu, (0x0010a4a0 + (queue->index * 4)),
						(*head & 0xffffffff));
	} else {
		nv_wr32(pmu, 0x0010a4c8, (*head & 0xffffffff));
	}

	return 0;
}

static int
gk20a_pmu_queue_tail_get(struct gk20a_pmu_priv *priv, struct pmu_queue *queue,
			u32 *tail)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (WARN_ON(!tail))
		return -EINVAL;

	if (PMU_IS_COMMAND_QUEUE(queue->id)) {
		if (queue->index >= 0x00000004)
			return -EINVAL;

		*tail = nv_rd32(pmu, 0x0010a4b0 + (queue->index * 4)) &
				0xffffffff;
	} else {
		*tail = nv_rd32(pmu, 0x0010a4cc) & 0xffffffff;
	}

	return 0;
}

static int
gk20a_pmu_queue_tail_set(struct gk20a_pmu_priv *priv, struct pmu_queue *queue,
			u32 *tail)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (WARN_ON(!tail))
		return -EINVAL;

	if (PMU_IS_COMMAND_QUEUE(queue->id)) {
		if (queue->index >= 0x00000004)
			return -EINVAL;

		nv_wr32(pmu, (0x0010a4b0 + (queue->index * 4)),
						(*tail & 0xffffffff));
	} else {
		nv_wr32(pmu, 0x0010a4cc, (*tail & 0xffffffff));
	}

	return 0;
}

static inline void
gk20a_pmu_queue_read(struct gk20a_pmu_priv *priv,
			u32 offset, u8 *dst, u32 size)
{
	gk20a_copy_from_dmem(priv, offset, dst, size, 0);
}

static inline void
gk20a_pmu_queue_write(struct gk20a_pmu_priv *priv,
			u32 offset, u8 *src, u32 size)
{
	gk20a_pmu_copy_to_dmem(priv, offset, src, size, 0);
}

static int
gk20a_pmu_mutex_acquire(struct nvkm_pmu *pmu, u32 id, u32 *token)
{
	struct gk20a_pmu_priv *priv = to_gk20a_priv(pmu);
	struct pmu_mutex *mutex;
	u32 data, owner, max_retry;

	if (!priv->initialized)
		return -EINVAL;

	if (WARN_ON(!token))
		return -EINVAL;

	if (WARN_ON(id > priv->mutex_cnt))
		return -EINVAL;

	mutex = &priv->mutex[id];

	owner = nv_rd32(pmu, 0x0010a580 + (mutex->index * 4)) & 0xff;

	if (*token != PMU_INVALID_MUTEX_OWNER_ID && *token == owner) {
		if (WARN_ON(mutex->ref_cnt == 0))
			return -EINVAL;

		nv_debug(pmu, "already acquired by owner : 0x%08x", *token);
		mutex->ref_cnt++;
		return 0;
	}

	/*
	 * Worst case wait will be 40*40us i.e 1.6 ms,
	 * (see its usage) which is acceptable and sufficient for all
	 * busy tasks to finish.
	 */
	max_retry = MAX_RETRIES;
	do {
		data = nv_rd32(pmu, 0x0010a488) & 0xff;
		if (data == 0 || data == 0xff) {
			nv_warn(pmu,
				"fail to generate mutex token: val 0x%08x",
				owner);
			break; /* Break and returns -EBUSY */
		}

		owner = data;
		nv_wr32(pmu, (0x0010a580 + mutex->index * 4),
			owner & 0xff);

		data = nv_rd32(pmu, 0x0010a580 + mutex->index * 4);

		if (owner == data) {
			mutex->ref_cnt = 1;
			nv_debug(pmu, "mutex acquired: id=%d, token=0x%x",
							mutex->index, *token);
			*token = owner;
			return 0;
		}
		/*
		 * This can happen if the same mutex is used by some other task
		 * in PMU. This time is sufficient/affordable for a task to
		 * release acquired mutex.
		 */
		nv_debug(pmu, "fail to acquire mutex idx=0x%08x",
							mutex->index);
		nv_mask(pmu, 0x0010a48c, 0xff, owner & 0xff);
		usleep_range(20, 40);
		continue;

	} while (max_retry-- > 0);

	return -EBUSY;
}

static int
gk20a_pmu_mutex_release(struct nvkm_pmu *pmu, u32 id, u32 *token)
{
	struct gk20a_pmu_priv *priv = to_gk20a_priv(pmu);
	struct pmu_mutex *mutex;
	u32 owner;

	if (!priv->initialized)
		return -EINVAL;

	if (WARN_ON(!token))
		return -EINVAL;

	if (WARN_ON(id > priv->mutex_cnt))
		return -EINVAL;

	mutex = &priv->mutex[id];

	owner = nv_rd32(pmu, 0x0010a580 + (mutex->index * 4)) & 0xff;

	if (*token != owner) {
		nv_error(pmu,
			"requester 0x%08x NOT match owner 0x%08x",
			*token, owner);
		return -EINVAL;
	}

	if (--mutex->ref_cnt > 0)
		return 0;

	nv_wr32(pmu, 0x0010a580 + (mutex->index * 4), 0x00);

	nv_mask(pmu, 0x0010a48c, 0xff, owner & 0xff);

	nv_debug(pmu, "mutex released: id=%d, token=0x%x",
							  mutex->index, *token);

	return 0;
}

static int
gk20a_pmu_queue_lock(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (PMU_IS_MESSAGE_QUEUE(queue->id))
		return 0;

	if (PMU_IS_SW_COMMAND_QUEUE(queue->id)) {
		mutex_lock(&queue->mutex);
		return 0;
	}

	return gk20a_pmu_mutex_acquire(pmu, queue->mutex_id,
						&queue->mutex_lock);

}

static int
gk20a_pmu_queue_unlock(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (PMU_IS_MESSAGE_QUEUE(queue->id))
		return 0;

	if (PMU_IS_SW_COMMAND_QUEUE(queue->id)) {
		mutex_unlock(&queue->mutex);
		return 0;
	}

	return gk20a_pmu_mutex_release(pmu, queue->mutex_id,
						&queue->mutex_lock);
}

/* called by gk20a_pmu_read_message, no lock */
static bool
gk20a_pmu_queue_is_empty(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue)
{
	u32 head, tail;

	gk20a_pmu_queue_head_get(priv, queue, &head);
	if (queue->opened && queue->oflag == OFLAG_READ)
		tail = queue->position;
	else
		gk20a_pmu_queue_tail_get(priv, queue, &tail);

	return head == tail;
}

static bool
gk20a_pmu_queue_has_room(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue, u32 size, bool *need_rewind)
{
	u32 head, tail, free;
	bool rewind = false;

	size = ALIGN(size, QUEUE_ALIGNMENT);

	gk20a_pmu_queue_head_get(priv, queue, &head);
	gk20a_pmu_queue_tail_get(priv, queue, &tail);

	if (head >= tail) {
		free = queue->offset + queue->size - head;
		free -= PMU_CMD_HDR_SIZE;

		if (size > free) {
			rewind = true;
			head = queue->offset;
		}
	}

	if (head < tail)
		free = tail - head - 1;

	if (need_rewind)
		*need_rewind = rewind;

	return size <= free;
}

static int
gk20a_pmu_queue_push(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue, void *data, u32 size)
{

	struct nvkm_pmu *pmu = &priv->base;

	if (!queue->opened && queue->oflag == OFLAG_WRITE) {
		nv_error(pmu, "queue not opened for write\n");
		return -EINVAL;
	}

	gk20a_pmu_queue_write(priv, queue->position, data, size);
	queue->position += ALIGN(size, QUEUE_ALIGNMENT);
	return 0;
}

static int
gk20a_pmu_queue_pop(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue, void *data, u32 size,
			u32 *bytes_read)
{
	u32 head, tail, used;
	struct nvkm_pmu *pmu = &priv->base;

	*bytes_read = 0;

	if (!queue->opened && queue->oflag == OFLAG_READ) {
		nv_error(pmu, "queue not opened for read\n");
		return -EINVAL;
	}

	gk20a_pmu_queue_head_get(priv, queue, &head);
	tail = queue->position;

	if (head == tail) {
		*bytes_read = 0;
		return 0;
	}

	if (head > tail)
		used = head - tail;
	else
		used = queue->offset + queue->size - tail;

	if (size > used) {
		nv_warn(pmu, "queue size smaller than request read\n");
		size = used;
	}

	gk20a_pmu_queue_read(priv, tail, data, size);
	queue->position += ALIGN(size, QUEUE_ALIGNMENT);
	*bytes_read = size;
	return 0;
}

static void
gk20a_pmu_queue_rewind(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue)
{
	struct pmu_cmd cmd;
	struct nvkm_pmu *pmu = &priv->base;
	int err;

	if (!queue->opened) {
		nv_error(pmu, "queue not opened\n");
		return;
	}

	if (queue->oflag == OFLAG_WRITE) {
		cmd.hdr.unit_id = PMU_UNIT_REWIND;
		cmd.hdr.size = PMU_CMD_HDR_SIZE;
		err = gk20a_pmu_queue_push(priv, queue, &cmd, cmd.hdr.size);
		if (err)
			nv_error(pmu, "gk20a_pmu_queue_push failed\n");
		nv_debug(pmu, "queue %d rewinded\n", queue->id);
	}

	queue->position = queue->offset;
	nv_debug(pmu, "exit %s\n", __func__);
}

/* Open for read and lock the queue */
static int
gk20a_pmu_queue_open_read(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue)
{
	int err;

	err = gk20a_pmu_queue_lock(priv, queue);
	if (err)
		return err;

	if (WARN_ON(queue->opened))
		return -EBUSY;

	gk20a_pmu_queue_tail_get(priv, queue, &queue->position);
	queue->oflag = OFLAG_READ;
	queue->opened = true;

	return 0;
}

/*
 * open for write and lock the queue
 * make sure there's enough free space for the write
 */
static int
gk20a_pmu_queue_open_write(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue, u32 size)
{
	struct nvkm_pmu *pmu = &priv->base;
	bool rewind = false;
	int err;

	err = gk20a_pmu_queue_lock(priv, queue);
	if (err)
		return err;

	if (WARN_ON(queue->opened))
		return -EBUSY;

	if (!gk20a_pmu_queue_has_room(priv, queue, size, &rewind)) {
		nv_error(pmu, "queue full");
		gk20a_pmu_queue_unlock(priv, queue);
		return -EAGAIN;
	}

	gk20a_pmu_queue_head_get(priv, queue, &queue->position);
	queue->oflag = OFLAG_WRITE;
	queue->opened = true;

	if (rewind)
		gk20a_pmu_queue_rewind(priv, queue);

	return 0;
}

/* close and unlock the queue */
static int
gk20a_pmu_queue_close(struct gk20a_pmu_priv *priv,
			struct pmu_queue *queue, bool commit)
{
	struct nvkm_pmu *pmu = &priv->base;

	if (WARN_ON(!queue->opened)) {
		nv_warn(pmu, "queue already closed\n");
		return 0;
	}

	if (commit) {
		if (queue->oflag == OFLAG_READ) {
			gk20a_pmu_queue_tail_set(priv, queue,
				&queue->position);
		} else {
			gk20a_pmu_queue_head_set(priv, queue,
				&queue->position);
		}
	}

	queue->opened = false;
	gk20a_pmu_queue_unlock(priv, queue);

	return 0;
}

static bool
gk20a_check_cmd_params(struct gk20a_pmu_priv *priv, struct pmu_cmd *cmd,
			struct pmu_msg *msg, struct pmu_payload *payload,
			u32 queue_id)
{
	struct nvkm_pmu *pmu = &priv->base;
	struct pmu_queue *queue;
	u32 in_size, out_size;

	nv_debug(pmu, "check cmd params\n");

	if (!PMU_IS_SW_COMMAND_QUEUE(queue_id))
		return false;

	queue = &priv->queue[queue_id];
	if (cmd->hdr.size < PMU_CMD_HDR_SIZE)
		return false;

	if (cmd->hdr.size > (queue->size >> 1))
		return false;

	if (msg != NULL && msg->hdr.size < PMU_MSG_HDR_SIZE)
		return false;

	if (!PMU_UNIT_ID_IS_VALID(cmd->hdr.unit_id))
		return false;

	if (payload == NULL)
		return true;

	if (payload->in.buf == NULL && payload->out.buf == NULL)
		return false;

	if ((payload->in.buf != NULL && payload->in.size == 0) ||
	    (payload->out.buf != NULL && payload->out.size == 0))
		return false;

	in_size = PMU_CMD_HDR_SIZE;
	if (payload->in.buf) {
		in_size += payload->in.offset;
		in_size += sizeof(struct pmu_allocation_gk20a);
	}

	out_size = PMU_CMD_HDR_SIZE;
	if (payload->out.buf) {
		out_size += payload->out.offset;
		out_size += sizeof(struct pmu_allocation_gk20a);
	}

	if (in_size > cmd->hdr.size || out_size > cmd->hdr.size)
		return false;


	if ((payload->in.offset != 0 && payload->in.buf == NULL) ||
	    (payload->out.offset != 0 && payload->out.buf == NULL))
		return false;

	return true;
}

/*
 * PMU DMEM allocator
 * *addr != 0 for fixed address allocation, if *addr = 0,
 * base addr is returned to caller in *addr.
 * Contigous allocation, which allocates one block of
 * contiguous address.
 */
static int
gk20a_pmu_allocator_block_alloc(struct nvkm_pmu_allocator *allocator,
		u32 *addr, u32 len, u32 align)
{
	unsigned long _addr;

	len = ALIGN(len, align);
	if (!len)
		return -ENOMEM;

	_addr = bitmap_find_next_zero_area(allocator->bitmap,
			allocator->size,
			*addr ? (*addr - allocator->base) : 0,
			len,
			align - 1);
	if ((_addr > allocator->size) ||
	    (*addr && *addr != (_addr + allocator->base))) {
		return -ENOMEM;
	}

	bitmap_set(allocator->bitmap, _addr, len);
	*addr = allocator->base + _addr;

	return 0;
}

/* Free all blocks between start and end. */
static int
gk20a_pmu_allocator_block_free(struct nvkm_pmu_allocator *allocator,
		u32 addr, u32 len, u32 align)
{
	len = ALIGN(len, align);
	if (!len)
		return -EINVAL;

	bitmap_clear(allocator->bitmap, addr - allocator->base, len);

	return 0;
}

static int
gk20a_pmu_allocator_init(struct nvkm_pmu_allocator *allocator,
		const char *name, u32 start, u32 len)
{
	memset(allocator, 0, sizeof(struct nvkm_pmu_allocator));

	allocator->base = start;
	allocator->size = len;

	allocator->bitmap = kcalloc(BITS_TO_LONGS(len), sizeof(long),
			GFP_KERNEL);
	if (!allocator->bitmap)
		return -ENOMEM;

	return 0;
}

/* destroy allocator, free all remaining blocks if any */
void
gk20a_pmu_allocator_destroy(struct nvkm_pmu_allocator *allocator)
{
	kfree(allocator->bitmap);
}

static bool
gk20a_pmu_validate_cmd(struct gk20a_pmu_priv *priv, struct pmu_cmd *cmd,
			struct pmu_msg *msg, struct pmu_payload *payload,
			u32 queue_id)
{
	bool params_valid;
	struct nvkm_pmu *pmu = &priv->base;

	params_valid = gk20a_check_cmd_params(priv, cmd, msg,
							payload, queue_id);
	nv_debug(pmu, "pmu validate cmd\n");

	if (!params_valid)
		nv_error(pmu, "invalid pmu cmd :\n"
			"queue_id=%d,\n"
			"cmd_size=%d, cmd_unit_id=%d, msg=%p, msg_size=%d,\n"
			"payload in=%p, in_size=%d, in_offset=%d,\n"
			"payload out=%p, out_size=%d, out_offset=%d",
			queue_id, cmd->hdr.size, cmd->hdr.unit_id,
			msg, msg ? msg->hdr.unit_id : ~0,
			&payload->in, payload->in.size, payload->in.offset,
			&payload->out, payload->out.size, payload->out.offset);

	return params_valid;
}

static int
gk20a_pmu_write_cmd(struct gk20a_pmu_priv *priv, struct pmu_cmd *cmd,
			u32 queue_id, unsigned long timeout)
{
	struct nvkm_pmu *pmu = &priv->base;
	struct pmu_queue *queue;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(timeout);
	int err;

	nv_debug(pmu, "pmu write cmd\n");

	queue = &priv->queue[queue_id];

	do {
		err = gk20a_pmu_queue_open_write(priv, queue, cmd->hdr.size);
		if (err != -EAGAIN || time_after(jiffies, end_jiffies))
			break;
		usleep_range(1000, 2000);

	} while (1);

	if (err) {
		nv_error(pmu, "pmu_queue_open_write failed\n");
		return err;
	}

	err = gk20a_pmu_queue_push(priv, queue, cmd, cmd->hdr.size);
	if (err) {
		nv_error(pmu, "pmu_queue_push failed\n");
		goto clean_up;
	}

	err = gk20a_pmu_queue_close(priv, queue, true);
	if (err)
		nv_error(pmu, "fail to close the queue %d", queue_id);

	nv_debug(pmu, "cmd writing done");

	return 0;

clean_up:
	nv_error(pmu, "%s failed\n", __func__);

	err = gk20a_pmu_queue_close(priv, queue, true);
	if (err)
		nv_error(pmu, "fail to close the queue %d", queue_id);

	return err;
}

static int
gk20a_pmu_cmd_post(struct nvkm_pmu *pmu, struct pmu_cmd *cmd,
		struct pmu_msg *msg, struct pmu_payload *payload,
		u32 queue_id, pmu_callback callback, void *cb_param,
		u32 *seq_desc, unsigned long timeout)
{
	struct gk20a_pmu_priv *priv = to_gk20a_priv(pmu);
	struct pmu_sequence *seq;
	struct pmu_allocation_gk20a *in = NULL, *out = NULL;
	int err;

	if (WARN_ON(!cmd))
		return -EINVAL;
	if (WARN_ON(!seq_desc))
		return -EINVAL;
	if (WARN_ON(!priv->pmu_ready))
		return -EINVAL;

	if (!gk20a_pmu_validate_cmd(priv, cmd, msg, payload, queue_id))
		return -EINVAL;

	err = gk20a_pmu_seq_acquire(priv, &seq);
	if (err)
		return err;

	cmd->hdr.seq_id = seq->id;

	cmd->hdr.ctrl_flags = 0;
	cmd->hdr.ctrl_flags |= PMU_CMD_FLAGS_STATUS;
	cmd->hdr.ctrl_flags |= PMU_CMD_FLAGS_INTR;

	seq->callback = callback;
	seq->cb_params = cb_param;
	seq->msg = msg;
	seq->out_payload = NULL;
	seq->desc = priv->next_seq_desc++;

	if (payload)
		seq->out_payload = payload->out.buf;

	*seq_desc = seq->desc;

	if (payload && payload->in.offset != 0) {
		in = (struct pmu_allocation_gk20a *)((u8 *)&cmd->perfmon +
			payload->in.offset);

		in->alloc.dmem.size = payload->in.size;

		err = gk20a_pmu_allocator_block_alloc(&priv->dmem,
						&in->alloc.dmem.offset,
						in->alloc.dmem.size,
						PMU_DMEM_ALLOC_ALIGNMENT);
		if (err) {
			nv_error(pmu, "gk20a_pmu_allocator alloc failed\n");
			goto clean_up;
		}

		gk20a_pmu_copy_to_dmem(priv, in->alloc.dmem.offset,
			payload->in.buf, payload->in.size, 0);
		seq->in_gk20a.alloc.dmem.size = in->alloc.dmem.size;
		seq->in_gk20a.alloc.dmem.offset = in->alloc.dmem.offset;
	}

	if (payload && payload->out.offset != 0) {
		out = (struct pmu_allocation_gk20a *)((u8 *)&cmd->perfmon +
			payload->out.offset);

		out->alloc.dmem.size = payload->out.size;

		err = gk20a_pmu_allocator_block_alloc(&priv->dmem,
					&out->alloc.dmem.offset,
					out->alloc.dmem.size,
					PMU_DMEM_ALLOC_ALIGNMENT);
		if (err) {
			nv_error(pmu, "gk20a_pmu_allocator alloc failed\n");
			goto clean_up;
		}

		seq->out_gk20a.alloc.dmem.size = out->alloc.dmem.size;
		seq->out_gk20a.alloc.dmem.offset = out->alloc.dmem.offset;
	}

	seq->state = PMU_SEQ_STATE_USED;
	err = gk20a_pmu_write_cmd(priv, cmd, queue_id, timeout);
	if (err)
		seq->state = PMU_SEQ_STATE_PENDING;

	return 0;

clean_up:

	nv_error(pmu, "cmd post failed\n");
	if (in)
		gk20a_pmu_allocator_block_free(&priv->dmem,
					in->alloc.dmem.offset,
					in->alloc.dmem.size,
					PMU_DMEM_ALLOC_ALIGNMENT);
	if (out)
		gk20a_pmu_allocator_block_free(&priv->dmem,
					out->alloc.dmem.offset,
					out->alloc.dmem.size,
					PMU_DMEM_ALLOC_ALIGNMENT);

	gk20a_pmu_seq_release(priv, seq);

	return err;
}

static bool
gk20a_pmu_read_message(struct gk20a_pmu_priv *priv, struct pmu_queue *queue,
			struct pmu_msg *msg, int *status)
{
	struct nvkm_pmu *pmu = &priv->base;
	u32 read_size, bytes_read;
	int err;

	*status = 0;

	if (gk20a_pmu_queue_is_empty(priv, queue))
		return false;

	err = gk20a_pmu_queue_open_read(priv, queue);
	if (err) {
		nv_error(pmu,
			"fail to open queue %d for read", queue->id);
		*status = err;
		return false;
	}

	err = gk20a_pmu_queue_pop(priv, queue, &msg->hdr,
			PMU_MSG_HDR_SIZE, &bytes_read);
	if (err || bytes_read != PMU_MSG_HDR_SIZE) {
		nv_error(pmu,
			"fail to read msg from queue %d", queue->id);
		*status = err | -EINVAL;
		goto clean_up;
	}

	if (msg->hdr.unit_id == PMU_UNIT_REWIND) {
		gk20a_pmu_queue_rewind(priv, queue);
		/* read again after rewind */
		err = gk20a_pmu_queue_pop(priv, queue, &msg->hdr,
				PMU_MSG_HDR_SIZE, &bytes_read);
		if (err || bytes_read != PMU_MSG_HDR_SIZE) {
			nv_error(pmu,
				"fail to read msg from queue %d", queue->id);
			*status = err | -EINVAL;
			goto clean_up;
		}
	}

	if (!PMU_UNIT_ID_IS_VALID(msg->hdr.unit_id)) {
		nv_error(pmu,
			"read invalid unit_id %d from queue %d",
			msg->hdr.unit_id, queue->id);
			*status = -EINVAL;
			goto clean_up;
	}

	if (msg->hdr.size > PMU_MSG_HDR_SIZE) {
		read_size = msg->hdr.size - PMU_MSG_HDR_SIZE;
		err = gk20a_pmu_queue_pop(priv, queue, &msg->msg,
			read_size, &bytes_read);
		if (err || bytes_read != read_size) {
			nv_error(pmu,
				"fail to read msg from queue %d", queue->id);
			*status = err;
			goto clean_up;
		}
	}

	err = gk20a_pmu_queue_close(priv, queue, true);
	if (err) {
		nv_error(pmu,
			"fail to close queue %d", queue->id);
		*status = err;
		return false;
	}

	return true;

clean_up:
	err = gk20a_pmu_queue_close(priv, queue, false);
	if (err)
		nv_error(pmu,
			"fail to close queue %d", queue->id);
	return false;
}

static int
gk20a_pmu_response_handle(struct gk20a_pmu_priv *priv, struct pmu_msg *msg)
{
	struct nvkm_pmu *pmu = &priv->base;
	struct pmu_sequence *seq;
	int ret = 0;

	nv_debug(pmu, "handling pmu response\n");
	seq = &priv->seq[msg->hdr.seq_id];
	if (seq->state != PMU_SEQ_STATE_USED &&
	    seq->state != PMU_SEQ_STATE_CANCELLED) {
		nv_error(pmu, "msg for an unknown sequence %d", seq->id);
		return -EINVAL;
	}

	if (msg->hdr.unit_id == PMU_UNIT_RC &&
	    msg->msg.rc.msg_type == PMU_RC_MSG_TYPE_UNHANDLED_CMD) {
		nv_error(pmu, "unhandled cmd: seq %d", seq->id);
	} else if (seq->state != PMU_SEQ_STATE_CANCELLED) {
		if (seq->msg) {
			if (seq->msg->hdr.size >= msg->hdr.size) {
				memcpy(seq->msg, msg, msg->hdr.size);
				if (seq->out_gk20a.alloc.dmem.size != 0) {
					gk20a_copy_from_dmem(priv,
					seq->out_gk20a.alloc.dmem.offset,
					seq->out_payload,
					seq->out_gk20a.alloc.dmem.size, 0);
				}
			} else {
				nv_error(pmu,
					"sequence %d msg buffer too small",
					seq->id);
			}
		}
	} else {
		seq->callback = NULL;
	}

	if (seq->in_gk20a.alloc.dmem.size != 0)
		gk20a_pmu_allocator_block_free(&priv->dmem,
			seq->in_gk20a.alloc.dmem.offset,
			seq->in_gk20a.alloc.dmem.size,
			PMU_DMEM_ALLOC_ALIGNMENT);
	if (seq->out_gk20a.alloc.dmem.size != 0)
		gk20a_pmu_allocator_block_free(&priv->dmem,
			seq->out_gk20a.alloc.dmem.offset,
			seq->out_gk20a.alloc.dmem.size,
			PMU_DMEM_ALLOC_ALIGNMENT);

	if (seq->callback)
		seq->callback(pmu, msg, seq->cb_params, seq->desc, ret);

	gk20a_pmu_seq_release(priv, seq);

	/* TBD: notify client waiting for available dmem */
	nv_debug(pmu, "pmu response processed\n");

	return 0;
}

static int
gk20a_pmu_process_init_msg(struct gk20a_pmu_priv *priv, struct pmu_msg *msg)
{
	struct pmu_init_msg_pmu_gk20a *init;
	u32 tail, i;

	tail = nv_rd32(priv, 0x0010a4cc);

	gk20a_copy_from_dmem(priv, tail,
				(u8 *)&msg->hdr, PMU_MSG_HDR_SIZE, 0);

	if (msg->hdr.unit_id != PMU_UNIT_INIT) {
		nv_error(priv, "expecting init msg\n");
		return -EINVAL;
	}

	gk20a_copy_from_dmem(priv, tail + PMU_MSG_HDR_SIZE,
		(u8 *)&msg->msg, msg->hdr.size - PMU_MSG_HDR_SIZE, 0);

	if (msg->msg.init.msg_type != PMU_INIT_MSG_TYPE_PMU_INIT) {
		nv_error(priv, "expecting init msg\n");
		return -EINVAL;
	}

	tail += ALIGN(msg->hdr.size, PMU_DMEM_ALIGNMENT);
	nv_wr32(priv, 0x0010a4cc, tail);
	init = &msg->msg.init.pmu_init_gk20a;
	priv->pmu_ready = true;

	for (i = 0; i < PMU_QUEUE_COUNT; i++)
		gk20a_pmu_queue_init(priv, i, init);

	gk20a_pmu_allocator_init(&priv->dmem, "gk20a_pmu_dmem",
					init->sw_managed_area_offset,
					init->sw_managed_area_size);

	priv->pmu_state = PMU_STATE_INIT_RECEIVED;
	nv_debug(priv, "init msg processed\n");

	return 0;
}

void
gk20a_pmu_process_message(struct work_struct *work)
{
	struct nvkm_pmu *pmu = container_of(work, struct nvkm_pmu, recv.work);
	struct gk20a_pmu_priv *priv = to_gk20a_priv(pmu);
	struct pmu_msg msg;
	int status;
	struct nvkm_mc *pmc = nvkm_mc(pmu);

	mutex_lock(&priv->isr_mutex);
	if (unlikely(!priv->pmu_ready)) {
		nv_debug(pmu, "processing init msg\n");
		gk20a_pmu_process_init_msg(priv, &msg);
		goto out;
	}
	while (gk20a_pmu_read_message(priv,
		&priv->queue[PMU_MESSAGE_QUEUE], &msg, &status)) {

		nv_debug(pmu, "read msg hdr:\n"
				"unit_id = 0x%08x, size = 0x%08x,\n"
				"ctrl_flags = 0x%08x, seq_id = 0x%08x\n",
				msg.hdr.unit_id, msg.hdr.size,
				msg.hdr.ctrl_flags, msg.hdr.seq_id);

		msg.hdr.ctrl_flags &= ~PMU_CMD_FLAGS_PMU_MASK;
		gk20a_pmu_response_handle(priv, &msg);
	}
out:
	mutex_unlock(&priv->isr_mutex);
	gk20a_pmu_enable_irq(priv, pmc, true);
	nv_debug(pmu, "exit %s\n", __func__);
}

static int
gk20a_pmu_init_vm(struct gk20a_pmu_priv *priv, const struct firmware *fw)
{
	int ret = 0;
	u32 *ucode_image;
	struct pmu_ucode_desc *desc = (struct pmu_ucode_desc *)fw->data;
	struct nvkm_pmu_priv_vm *pmuvm = &priv->pmuvm;
	struct nvkm_device *device = nv_device(&priv->base);
	struct nvkm_vm *vm;
	const u64 pmu_area_len = 300*1024;

	/* mem for inst blk*/
	ret = nvkm_gpuobj_new(nv_object(priv), NULL, 0x1000, 0, 0, &pmuvm->mem);
	if (ret)
		return ret;

	/* mem for pgd*/
	ret = nvkm_gpuobj_new(nv_object(priv), NULL, 0x8000, 0, 0, &pmuvm->pgd);
	if (ret)
		return ret;

	/*allocate virtual memory range*/
	ret = nvkm_vm_new(device, 0, pmu_area_len, 0, &vm);
	if (ret)
		return ret;

	atomic_inc(&vm->engref[NVDEV_SUBDEV_PMU]);

	/* update VM with pgd */
	ret = nvkm_vm_ref(vm, &pmuvm->vm, pmuvm->pgd);
	if (ret)
		return ret;

	/*update pgd in inst blk */
	nv_wo32(pmuvm->mem, 0x0200, lower_32_bits(pmuvm->pgd->addr));
	nv_wo32(pmuvm->mem, 0x0204, upper_32_bits(pmuvm->pgd->addr));
	nv_wo32(pmuvm->mem, 0x0208, lower_32_bits(pmu_area_len - 1));
	nv_wo32(pmuvm->mem, 0x020c, upper_32_bits(pmu_area_len - 1));

	/* allocate memory for pmu fw to be copied to*/
	ret = nvkm_gpuobj_new(nv_object(priv), NULL, GK20A_PMU_UCODE_SIZE_MAX,
			      0x1000, 0, &priv->ucode.obj);
	if (ret)
		return ret;

	ucode_image = (u32 *)((u8 *)desc + desc->descriptor_size);
	gpu_obj_memwr(priv->ucode.obj, 0,
			ucode_image, desc->app_start_offset + desc->app_size);

	/* map allocated memory into GMMU */
	ret = nvkm_gpuobj_map_vm(priv->ucode.obj, vm, NV_MEM_ACCESS_RW,
				 &priv->ucode.vma);
	if (ret)
		return ret;

	return ret;
}

static int
gk20a_init_pmu_setup_sw(struct gk20a_pmu_priv *priv)
{
	struct nvkm_pmu_priv_vm *pmuvm = &priv->pmuvm;
	struct nvkm_pmu *pmu = &priv->base;
	int ret = 0, i;

	INIT_WORK(&priv->base.recv.work, gk20a_pmu_process_message);
	priv->mutex_cnt = MUTEX_CNT;
	priv->mutex = kzalloc(priv->mutex_cnt *
		sizeof(struct pmu_mutex), GFP_KERNEL);

	if (!priv->mutex) {
		nv_error(pmu, "not enough space ENOMEM\n");
		return -ENOMEM;
	}

	for (i = 0; i < priv->mutex_cnt; i++)
		priv->mutex[i].index = i;

	priv->seq = kzalloc(PMU_MAX_NUM_SEQUENCES *
		sizeof(struct pmu_sequence), GFP_KERNEL);

	if (!priv->seq) {
		nv_error(pmu, "not enough space ENOMEM\n");
		kfree(priv->mutex);
		return -ENOMEM;
	}

	gk20a_pmu_seq_init(priv);

	ret = nvkm_gpuobj_new(nv_object(priv), NULL, GK20A_PMU_TRACE_BUFSIZE,
					    0, 0, &priv->trace_buf.obj);
	if (ret)
		goto error;

	ret = nvkm_gpuobj_map_vm(nv_gpuobj(priv->trace_buf.obj), pmuvm->vm,
					NV_MEM_ACCESS_RW, &priv->trace_buf.vma);
	if (ret)
		goto error;

	return 0;
error:
	kfree(priv->mutex);
	kfree(priv->seq);

	return ret;
}

static int
gk20a_pmu_bootstrap(struct gk20a_pmu_priv *priv)
{
	struct pmu_ucode_desc *desc = priv->desc;
	u32 addr_code, addr_data, addr_load;
	u32 i, blocks, addr_args;
	struct pmu_cmdline_args_gk20a cmdline_args;
	struct nvkm_pmu_priv_vm *pmuvm = &priv->pmuvm;

	nv_mask(priv, 0x0010a048, 0x01, 0x01);
	/*bind the address*/
	nv_wr32(priv, 0x0010a480,
		pmuvm->mem->addr >> 12 |
		0x1 << 30 |
		0x20000000);

	/* TBD: load all other surfaces */
	cmdline_args.falc_trace_size = GK20A_PMU_TRACE_BUFSIZE;
	cmdline_args.falc_trace_dma_base =
			    lower_32_bits(priv->trace_buf.vma.offset >> 8);
	cmdline_args.falc_trace_dma_idx = GK20A_PMU_DMAIDX_VIRT;
	cmdline_args.cpu_freq_hz = 204;
	cmdline_args.secure_mode = 0;

	addr_args = (nv_rd32(priv, 0x0010a108) >> 9) & 0x1ff;
	addr_args = addr_args << GK20A_PMU_DMEM_BLKSIZE2;
	addr_args -= sizeof(struct pmu_cmdline_args_gk20a);
	nv_debug(priv, "initiating copy to dmem\n");
	gk20a_pmu_copy_to_dmem(priv, addr_args,
			(u8 *)&cmdline_args,
			sizeof(struct pmu_cmdline_args_gk20a), 0);

	nv_wr32(priv, 0x0010a1c0, 0x1 << 24);

	addr_code = lower_32_bits((priv->ucode.vma.offset +
			desc->app_start_offset +
			desc->app_resident_code_offset) >> 8);

	addr_data = lower_32_bits((priv->ucode.vma.offset +
			desc->app_start_offset +
			desc->app_resident_data_offset) >> 8);

	addr_load = lower_32_bits((priv->ucode.vma.offset +
			desc->bootloader_start_offset) >> 8);

	nv_wr32(priv, 0x0010a1c4, GK20A_PMU_DMAIDX_UCODE);
	nv_debug(priv, "0x%08x\n", GK20A_PMU_DMAIDX_UCODE);
	nv_wr32(priv, 0x0010a1c4, (addr_code));
	nv_debug(priv, "0x%08x\n", (addr_code));
	nv_wr32(priv, 0x0010a1c4, desc->app_size);
	nv_debug(priv, "0x%08x\n", desc->app_size);
	nv_wr32(priv, 0x0010a1c4, desc->app_resident_code_size);
	nv_debug(priv, "0x%08x\n", desc->app_resident_code_size);
	nv_wr32(priv, 0x0010a1c4, desc->app_imem_entry);
	nv_debug(priv, "0x%08x\n", desc->app_imem_entry);
	nv_wr32(priv, 0x0010a1c4,  (addr_data));
	nv_debug(priv, "0x%08x\n", (addr_data));
	nv_wr32(priv, 0x0010a1c4, desc->app_resident_data_size);
	nv_debug(priv, "0x%08x\n", desc->app_resident_data_size);
	nv_wr32(priv, 0x0010a1c4, (addr_code));
	nv_debug(priv, "0x%08x\n", (addr_code));
	nv_wr32(priv, 0x0010a1c4, 0x1);
	nv_debug(priv, "0x%08x\n", 1);
	nv_wr32(priv, 0x0010a1c4, addr_args);
	nv_debug(priv, "0x%08x\n", addr_args);

	nv_wr32(priv, 0x0010a110,
		(addr_load) - (desc->bootloader_imem_offset >> 8));

	blocks = ((desc->bootloader_size + 0xFF) & ~0xFF) >> 8;

	for (i = 0; i < blocks; i++) {
		nv_wr32(priv, 0x0010a114,
			desc->bootloader_imem_offset + (i << 8));
		nv_wr32(priv, 0x0010a11c,
			desc->bootloader_imem_offset + (i << 8));
		nv_wr32(priv, 0x0010a118,
			0x01 << 4  |
			0x06 << 8  |
			((GK20A_PMU_DMAIDX_UCODE & 0x07) << 12));
	}

	nv_wr32(priv, 0x0010a104, (desc->bootloader_entry_point));
	nv_wr32(priv, 0x0010a100, 0x1 << 1);
	nv_wr32(priv, 0x0010a080, desc->app_version);

	return 0;
}

static int
gk20a_init_pmu_setup_hw1(struct gk20a_pmu_priv *priv, struct nvkm_mc *pmc)
{
	int err;

	mutex_lock(&priv->isr_mutex);
	err = gk20a_pmu_enable(priv, pmc, true);
	priv->isr_enabled = (err == 0);
	mutex_unlock(&priv->isr_mutex);
	if (err)
		return err;

	/* setup apertures - virtual */
	nv_wr32(priv, 0x10a600 + 0 * 4, 0x0);
	nv_wr32(priv, 0x10a600 + 1 * 4, 0x0);
	/* setup apertures - physical */
	nv_wr32(priv, 0x10a600 + 2 * 4, 0x4 | 0x0);
	nv_wr32(priv, 0x10a600 + 3 * 4, 0x4 | 0x1);
	nv_wr32(priv, 0x10a600 + 4 * 4, 0x4 | 0x2);

	/* TBD: load pmu ucode */
	err = gk20a_pmu_bootstrap(priv);
	if (err)
		return err;

	return 0;
}

void
gk20a_pmu_intr(struct nvkm_subdev *subdev)
{
	struct gk20a_pmu_priv *priv = to_gk20a_priv(nvkm_pmu(subdev));
	struct nvkm_mc *pmc = nvkm_mc(priv);
	u32 intr, mask;

	if (!priv->isr_enabled)
		return;

	mask = nv_rd32(priv, 0x0010a018) & nv_rd32(priv, 0x0010a01c);

	intr = nv_rd32(priv, 0x0010a008) & mask;

	nv_debug(priv, "received falcon interrupt: 0x%08x\n", intr);
	gk20a_pmu_enable_irq(priv, pmc, false);

	if (!intr || priv->pmu_state == PMU_STATE_OFF) {
		nv_wr32(priv, 0x0010a004, intr);
		nv_error(priv, "pmu state off\n");
		gk20a_pmu_enable_irq(priv, pmc, true);
	}

	if (intr & 0x10)
		nv_error(priv, "pmu halt intr not implemented\n");

	if (intr & 0x20) {
		nv_error(priv, "exterr interrupt  not impl..Clear interrupt\n");
		nv_mask(priv, 0x0010a16c, (0x1 << 31), 0x00000000);
	}

	if (intr & 0x40) {
		nv_debug(priv, "scheduling work\n");
		schedule_work(&priv->base.recv.work);
		gk20a_pmu_enable_irq(priv, pmc, true);
	}

	nv_wr32(priv, 0x0010a004, intr);
	nv_debug(priv, "irq handled\n");
}

static void
gk20a_pmu_pgob(struct nvkm_pmu *pmu, bool enable)
{
}

static int
gk20a_pmu_init(struct nvkm_object *object)
{
	struct gk20a_pmu_priv *priv = (void *)object;
	struct nvkm_mc *pmc = nvkm_mc(object);
	int ret;

	ret = nvkm_subdev_init(&priv->base.base);
	if (ret)
		return ret;

	priv->pmu_state = PMU_STATE_STARTING;
	ret = gk20a_init_pmu_setup_hw1(priv, pmc);
	if (ret)
		return ret;

	gk20a_pmu_dvfs_init(priv);

	nvkm_timer_alarm(priv, 2000000000, &priv->alarm);

	return ret;
}

static int
gk20a_pmu_fini(struct nvkm_object *object, bool suspend)
{
	struct gk20a_pmu_priv *priv = (void *)object;
	struct nvkm_mc *pmc = nvkm_mc(object);

	nvkm_timer_alarm_cancel(priv, &priv->alarm);

	cancel_work_sync(&priv->base.recv.work);

	mutex_lock(&priv->isr_mutex);
	gk20a_pmu_enable(priv, pmc, false);
	priv->isr_enabled = false;
	mutex_unlock(&priv->isr_mutex);

	priv->pmu_state = PMU_STATE_OFF;
	priv->pmu_ready = false;
	nv_wr32(priv, 0x10a014, 0x00000060);

	return nvkm_subdev_fini(&priv->base.base, suspend);
}

static void
gk20a_pmu_dtor(struct nvkm_object *object)
{
	struct gk20a_pmu_priv *priv = (void *)object;

	nvkm_gpuobj_unmap(&priv->trace_buf.vma);
	nvkm_gpuobj_ref(NULL, &priv->trace_buf.obj);

	nvkm_gpuobj_unmap(&priv->ucode.vma);
	nvkm_gpuobj_ref(NULL, &priv->ucode.obj);
	nvkm_vm_ref(NULL, &priv->pmuvm.vm, priv->pmuvm.pgd);
	nvkm_gpuobj_ref(NULL, &priv->pmuvm.pgd);
	nvkm_gpuobj_ref(NULL, &priv->pmuvm.mem);
	gk20a_pmu_allocator_destroy(&priv->dmem);
}

struct gk20a_pmu_dvfs_data gk20a_dvfs_data = {
	.p_load_target = 70,
	.p_load_max = 78,
	.p_smooth = 0,
};

static int
gk20a_pmu_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
	       struct nvkm_oclass *oclass, void *data, u32 size,
	       struct nvkm_object **pobject)
{
	struct gk20a_pmu_priv *priv;
	struct nvkm_pmu *pmu;
	struct nvkm_mc *pmc;
	const struct firmware *pmufw = NULL;
	int ret;

	ret = nvkm_pmu_create(parent, engine, oclass, &priv);
	*pobject = nv_object(priv);
	if (ret)
		return ret;

	mutex_init(&priv->isr_mutex);
	mutex_init(&priv->pmu_copy_lock);
	mutex_init(&priv->pmu_seq_lock);
	priv->data = &gk20a_dvfs_data;
	pmu = &priv->base;
	pmc = nvkm_mc(pmu);
	nv_subdev(pmu)->intr = gk20a_pmu_intr;

	ret = gk20a_load_firmware(pmu, &pmufw, GK20A_PMU_UCODE_IMAGE);
	if (ret < 0) {
		nv_error(priv, "failed to load pmu fimware\n");
		return ret;
	}

	ret = gk20a_pmu_init_vm(priv, pmufw);
	if (ret < 0) {
		nv_error(priv, "failed to map pmu fw to va space\n");
		goto err;
	}

	priv->desc = (struct pmu_ucode_desc *)pmufw->data;
	gk20a_pmu_dump_firmware_info(pmu, pmufw);

	if (priv->desc->app_version != APP_VERSION_GK20A) {
		nv_error(priv, "PMU version unsupported: %d\n",
						       priv->desc->app_version);
		ret = -EINVAL;
		goto err;
	}

	ret = gk20a_init_pmu_setup_sw(priv);
	if (ret)
		goto err;

	pmu->pgob = nvkm_pmu_pgob;
	nvkm_alarm_init(&priv->alarm, gk20a_pmu_dvfs_work);

	return 0;

err:
	gk20a_release_firmware(pmu, pmufw);
	return ret;
}

struct nvkm_oclass *
gk20a_pmu_oclass = &(struct nvkm_pmu_impl) {
	.base.handle = NV_SUBDEV(PMU, 0xea),
	.base.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = gk20a_pmu_ctor,
		.dtor = gk20a_pmu_dtor,
		.init = gk20a_pmu_init,
		.fini = gk20a_pmu_fini,
	},
	.pgob = gk20a_pmu_pgob,
}.base;

