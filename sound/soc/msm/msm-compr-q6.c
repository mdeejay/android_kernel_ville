/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/q6adm.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <linux/android_pmem.h>

#include "msm-compr-q6.h"
#include "msm-pcm-routing.h"

#define Q6_EFFECT_DEBUG 0

static struct audio_locks the_locks;

/*
old setting from qct default value:
.buffer_bytes_max = 	1200 * 1024 * 2,
.period_bytes_min = 60 * 1024,
.period_bytes_max = 	1200 * 1024,
*/
static struct snd_pcm_hardware msm_compr_hardware_playback = {
	.info =		 (SNDRV_PCM_INFO_MMAP |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats =	      SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_8000_48000,
	.rate_min =	     8000,
	.rate_max =	     48000,
	.channels_min =	 1,
	.channels_max =	 2,
	.buffer_bytes_max =     1024 * 1024 * 2,
	.period_bytes_min =	60 * 1024,
	.period_bytes_max =     1024 * 1024,
	.periods_min =	  2,
	.periods_max =	  40,
	.fifo_size =	    0,
};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

struct compr_msm {
	struct msm_audio *prtd;
	unsigned volume;
};
static struct compr_msm compr_msm_global;

int compr_set_volume(unsigned volume)
{
	int rc = 0;
	pr_info("[AUD]%s, vol %d -> %d\n", __func__, compr_msm_global.volume, volume);
	if (compr_msm_global.prtd && compr_msm_global.prtd->audio_client) {
		rc = q6asm_set_volume(compr_msm_global.prtd->audio_client, volume);
		if (rc < 0) {
			pr_err("[AUD]%s: Send Volume command failed"
					" rc=%d\n", __func__, rc);
		}
	}
	compr_msm_global.volume = volume;
	return rc;
}

static void compr_event_handler(uint32_t opcode,
		uint32_t token, uint32_t *payload, void *priv)
{
	struct compr_audio *compr = priv;
	struct msm_audio *prtd = &compr->prtd;
	struct snd_pcm_substream *substream = prtd->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct audio_aio_write_param param;
	struct audio_buffer *buf = NULL;
	int i = 0;

	pr_debug("[AUD]%s opcode =%08x, start %d +++\n", __func__, opcode, ((&prtd!=NULL)?atomic_read(&prtd->start):-1));
	switch (opcode) {
	case ASM_DATA_EVENT_WRITE_DONE: {
		uint32_t *ptrmem = (uint32_t *)&param;
		pr_debug("[AUD]%s ASM_DATA_EVENT_WRITE_DONE\n", __func__);
		pr_debug("[AUD]%s Buffer Consumed = 0x%08x\n", __func__, *ptrmem);
		prtd->pcm_irq_pos += prtd->pcm_count;
		if (atomic_read(&prtd->start))
			snd_pcm_period_elapsed(substream);
		atomic_inc(&prtd->out_count);
		wake_up(&the_locks.write_wait);
		if (!atomic_read(&prtd->start)) {
			atomic_set(&prtd->pending_buffer, 1);
			break;
		} else
			atomic_set(&prtd->pending_buffer, 0);

		if (runtime->status->hw_ptr >= runtime->control->appl_ptr)
			break;
		buf = prtd->audio_client->port[IN].buf;
		pr_debug("[AUD]%s:writing %d bytes of buffer[%d] to dsp 2\n",
				__func__, prtd->pcm_count, prtd->out_head);
		pr_debug("[AUD]%s:writing buffer[%d] from 0x%08x\n",
				__func__, prtd->out_head,
				((unsigned int)buf[0].phys
				+ (prtd->out_head * prtd->pcm_count)));

		param.paddr = (unsigned long)buf[0].phys
				+ (prtd->out_head * prtd->pcm_count);
		param.len = prtd->pcm_count;
		param.msw_ts = 0;
		param.lsw_ts = 0;
		param.flags = NO_TIMESTAMP;
		param.uid =  (unsigned long)buf[0].phys
				+ (prtd->out_head * prtd->pcm_count);
		for (i = 0; i < sizeof(struct audio_aio_write_param)/4;
					i++, ++ptrmem)
			pr_debug("[AUD]cmd[%d]=0x%08x\n", i, *ptrmem);
		if (q6asm_async_write(prtd->audio_client,
					&param) < 0)
			pr_err("[AUD]%s:q6asm_async_write failed\n",
				__func__);
		else
			prtd->out_head =
				(prtd->out_head + 1) & (runtime->periods - 1);
		break;
	}
	case ASM_DATA_CMDRSP_EOS:
		pr_debug("[AUD]%s ASM_DATA_CMDRSP_EOS\n", __func__);
		prtd->cmd_ack = 1;
		wake_up(&the_locks.eos_wait);
		break;
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case ASM_SESSION_CMD_RUN: {
			if (!atomic_read(&prtd->pending_buffer))
				break;
			if (runtime->status->hw_ptr >= runtime->control->appl_ptr)
				break;
			pr_debug("[AUD]%s:writing %d bytes"
				" of buffer[%d] to dsp\n",
				__func__, prtd->pcm_count, prtd->out_head);
			buf = prtd->audio_client->port[IN].buf;
			pr_debug("[AUD]%s:writing buffer[%d] from 0x%08x\n",
				__func__, prtd->out_head,
				((unsigned int)buf[0].phys
				+ (prtd->out_head * prtd->pcm_count)));
			param.paddr = (unsigned long)buf[prtd->out_head].phys;
			param.len = prtd->pcm_count;
			param.msw_ts = 0;
			param.lsw_ts = 0;
			param.flags = NO_TIMESTAMP;
			param.uid =  (unsigned long)buf[prtd->out_head].phys;
			if (q6asm_async_write(prtd->audio_client,
						&param) < 0)
				pr_err("[AUD]%s:q6asm_async_write failed\n",
					__func__);
			else
				prtd->out_head =
					(prtd->out_head + 1)
					& (runtime->periods - 1);
			atomic_set(&prtd->pending_buffer, 0);
		}
			break;
		case ASM_STREAM_CMD_FLUSH:
			pr_debug("[AUD]%s: ASM_STREAM_CMD_FLUSH\n", __func__);
			prtd->cmd_ack = 1;
			wake_up(&the_locks.eos_wait);
			break;
		default:
			break;
		}
		break;
	}
	default:
		pr_debug("[AUD]%s: Not Supported Event opcode[0x%x]\n", __func__, opcode);
		break;
	}
	pr_debug("[AUD]%s opcode =%08x, start %d ---\n", __func__, opcode, ((&prtd!=NULL)?atomic_read(&prtd->start):-1));
}

static int msm_compr_playback_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;
	int ret;

	pr_debug("[AUD]%s\n", __func__);
	prtd->pcm_size = snd_pcm_lib_buffer_bytes(substream);
	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	prtd->pcm_irq_pos = 0;
	/* rate and channels are sent to audio driver */
	prtd->samp_rate = runtime->rate;
	prtd->channel_mode = runtime->channels;
	prtd->out_head = 0;
	if (prtd->enabled)
		return 0;

	ret = q6asm_media_format_block(prtd->audio_client, compr->codec);
	if (ret < 0)
		pr_info("[AUD]%s: CMD Format block failed\n", __func__);

	atomic_set(&prtd->out_count, runtime->periods);

	prtd->enabled = 1;
	prtd->cmd_ack = 0;

	return 0;
}

static int msm_compr_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;

	pr_debug("[AUD]%s , cmd = %d\n", __func__, cmd);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		prtd->pcm_irq_pos = 0;
		atomic_set(&prtd->pending_buffer, 1);
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pr_debug("[AUD]%s: Trigger START/RESUME/PAUSE_RELEASE\n", __func__);
		q6asm_run_nowait(prtd->audio_client, 0, 0, 0);
		atomic_set(&prtd->start, 1);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("[AUD]%s: SNDRV_PCM_TRIGGER_STOP\n", __func__);
		atomic_set(&prtd->start, 0);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("[AUD]%s: SNDRV_PCM_TRIGGER_PAUSE/SUSPEND\n", __func__);
		q6asm_cmd_nowait(prtd->audio_client, CMD_PAUSE);
		atomic_set(&prtd->start, 0);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void populate_codec_list(struct compr_audio *compr,
		struct snd_pcm_runtime *runtime)
{
	pr_debug("[AUD]%s\n", __func__);
	/* MP3 Block */
	compr->info.compr_cap.num_codecs = 1;
	compr->info.compr_cap.min_fragment_size = runtime->hw.period_bytes_min;
	compr->info.compr_cap.max_fragment_size = runtime->hw.period_bytes_max;
	compr->info.compr_cap.min_fragments = runtime->hw.periods_min;
	compr->info.compr_cap.max_fragments = runtime->hw.periods_max;
	compr->info.compr_cap.codecs[0] = SND_AUDIOCODEC_MP3;
	/* Add new codecs here */
}

static int msm_compr_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = substream->private_data;
	struct compr_audio *compr;
	struct msm_audio *prtd;

	struct asm_softpause_params softpause = {
		.enable = SOFT_PAUSE_ENABLE,
		.period = SOFT_PAUSE_PERIOD,
		.step = SOFT_PAUSE_STEP,
		.rampingcurve = SOFT_PAUSE_CURVE_LINEAR,
	};
	struct asm_softvolume_params softvol = {
		.period = SOFT_VOLUME_PERIOD,
		.step = SOFT_VOLUME_STEP,
		.rampingcurve = SOFT_VOLUME_CURVE_LINEAR,
	};

	int ret = 0;

	/* Capture path */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;

	pr_debug("[AUD]%s\n", __func__);
	compr = kzalloc(sizeof(struct compr_audio), GFP_KERNEL);
	if (compr == NULL) {
		pr_err("[AUD]Failed to allocate memory for msm_audio\n");
		return -ENOMEM;
	}
	prtd = &compr->prtd;
	prtd->substream = substream;
	prtd->audio_client = q6asm_audio_client_alloc(
				(app_cb)compr_event_handler, compr);
	if (!prtd->audio_client) {
		pr_info("[AUD]%s: Could not allocate memory\n", __func__);
		kfree(prtd);
		return -ENOMEM;
	}
	runtime->hw = msm_compr_hardware_playback;

	pr_info("[AUD]%s: session ID %d\n", __func__, prtd->audio_client->session);

	prtd->session_id = prtd->audio_client->session;
	msm_pcm_routing_reg_phy_stream(soc_prtd->dai_link->be_id,
			prtd->session_id, substream->stream);

	prtd->cmd_ack = 1;

	ret = snd_pcm_hw_constraint_list(runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE,
			&constraints_sample_rates);
	if (ret < 0)
		pr_info("[AUD]snd_pcm_hw_constraint_list failed\n");
	/* Ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime,
			    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		pr_info("[AUD]snd_pcm_hw_constraint_integer failed\n");

	prtd->dsp_cnt = 0;
	atomic_set(&prtd->pending_buffer, 1);
	compr->codec = FORMAT_MP3;
	populate_codec_list(compr, runtime);
	runtime->private_data = compr;

	compr_msm_global.prtd = prtd;
	compr_set_volume(compr_msm_global.volume);
	ret = q6asm_set_softpause(compr_msm_global.prtd->audio_client, &softpause);
	if (ret < 0)
		pr_err("[AUD]%s: Send SoftPause Param failed ret=%d\n",
			__func__, ret);
	ret = q6asm_set_softvolume(compr_msm_global.prtd->audio_client, &softvol);
	if (ret < 0)
		pr_err("[AUD]%s: Send SoftVolume Param failed ret=%d\n",
			__func__, ret);

	return 0;
}

static int msm_compr_playback_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = substream->private_data;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;
	int dir = 0;
	int rc = 0;
	pr_debug("[AUD] %s +++\n", __func__);

	/*
	If routing is still enabled, we need to issue EOS to
	the DSP
	To issue EOS to dsp, we need to be run state otherwise
	EOS is not honored.
	*/
	if (msm_routing_check_backend_enabled(soc_prtd->dai_link->be_id)) {
		rc = q6asm_run(prtd->audio_client,0,0,0);
		atomic_set(&prtd->pending_buffer, 0);
		prtd->cmd_ack = 0;
		q6asm_cmd_nowait(prtd->audio_client, CMD_EOS);
		pr_debug("%s ++\n", __func__);
		rc = wait_event_timeout(the_locks.eos_wait,
			prtd->cmd_ack, 3 * HZ);
		pr_debug("%s --\n", __func__);
		if (rc <= 0)
			pr_err("EOS cmd timeout\n");
		prtd->pcm_irq_pos = 0;
	}


	dir = IN;
	atomic_set(&prtd->pending_buffer, 0);
	q6asm_cmd(prtd->audio_client, CMD_CLOSE);
	q6asm_audio_client_buf_free_contiguous(dir,
				prtd->audio_client);

	msm_pcm_routing_dereg_phy_stream(soc_prtd->dai_link->be_id,
	SNDRV_PCM_STREAM_PLAYBACK);
	q6asm_audio_client_free(prtd->audio_client);
	kfree(prtd);
	pr_debug("[AUD] %s ---\n", __func__);
	return 0;
}

static int msm_compr_close(struct snd_pcm_substream *substream)
{
	int ret = 0;
	//roger+{
	compr_msm_global.prtd = NULL;
	//roger+}
	pr_debug("[AUD]%s +++\n", __func__);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_compr_playback_close(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = EINVAL;
	pr_debug("[AUD]%s ---\n", __func__);
	return ret;
}
static int msm_compr_prepare(struct snd_pcm_substream *substream)
{
	int ret = 0;

	pr_info("[AUD]%s, vol %d\n", __func__, compr_msm_global.volume);
	compr_set_volume(compr_msm_global.volume);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_compr_playback_prepare(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = EINVAL;
	return ret;
}

static snd_pcm_uframes_t msm_compr_pointer(struct snd_pcm_substream *substream)
{

	struct snd_pcm_runtime *runtime = substream->runtime;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;

	if (prtd->pcm_irq_pos >= prtd->pcm_size)
		prtd->pcm_irq_pos = 0;

	pr_debug("[AUD]pcm_irq_pos = %d\n", prtd->pcm_irq_pos);
	return bytes_to_frames(runtime, (prtd->pcm_irq_pos));
}

static int msm_compr_mmap(struct snd_pcm_substream *substream,
				struct vm_area_struct *vma)
{
	int result = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;

	pr_debug("[AUD]%s\n", __func__);
	prtd->mmap_flag = 1;
	if (runtime->dma_addr && runtime->dma_bytes) {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		result = remap_pfn_range(vma, vma->vm_start,
				runtime->dma_addr >> PAGE_SHIFT,
				runtime->dma_bytes,
				vma->vm_page_prot);
	} else {
		pr_err("[AUD]Physical address or size of buf is NULL");
		return -EINVAL;
	}
	return result;
}

static int msm_compr_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;
	struct snd_dma_buffer *dma_buf = &substream->dma_buffer;
	struct audio_buffer *buf;
	int dir, ret;

	pr_debug("[AUD]%s\n", __func__);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dir = IN;
	else
		return -EINVAL;

	ret = q6asm_open_write(prtd->audio_client, compr->codec);
	if (ret < 0) {
		pr_err("[AUD]%s: Session out open failed\n", __func__);
		return -ENOMEM;
	}
	ret = q6asm_set_io_mode(prtd->audio_client, ASYNC_IO_MODE);
	if (ret < 0) {
		pr_err("[AUD]%s: Set IO mode failed\n", __func__);
		return -ENOMEM;
	}

	ret = q6asm_audio_client_buf_alloc_contiguous(dir,
			prtd->audio_client,
			runtime->hw.period_bytes_min,
			runtime->hw.periods_max);
	if (ret < 0) {
		pr_err("[AUD]Audio Start: Buffer Allocation failed "
					"rc = %d\n", ret);
		return -ENOMEM;
	}
	buf = prtd->audio_client->port[dir].buf;

	pr_debug("[AUD]%s:buf = %p\n", __func__, buf);
	dma_buf->dev.type = SNDRV_DMA_TYPE_DEV;
	dma_buf->dev.dev = substream->pcm->card->dev;
	dma_buf->private_data = NULL;
	dma_buf->area = buf[0].data;
	dma_buf->addr =  buf[0].phys;
	dma_buf->bytes = runtime->hw.buffer_bytes_max;
	if (!dma_buf->area)
		return -ENOMEM;

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
	return 0;
}

static int msm_compr_ioctl(struct snd_pcm_substream *substream,
		unsigned int cmd, void *arg)
{
	int rc = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct compr_audio *compr = runtime->private_data;
	struct msm_audio *prtd = &compr->prtd;

	switch (cmd) {
	case SNDRV_COMPRESS_GET_CAPS:
		pr_debug("[AUD]SNDRV_COMPRESS_GET_CAPS\n");
		if (copy_to_user((void *) arg, &compr->info.compr_cap,
			sizeof(struct snd_compr_caps))) {
			rc = -EFAULT;
			pr_err("[AUD]%s: ERROR: copy to user\n", __func__);
			return rc;
		}
		return 0;
	case SNDRV_COMPRESS_SET_PARAMS:
		pr_debug("[AUD]SNDRV_COMPRESS_SET_PARAMS: ");
		if (copy_from_user(&compr->info.codec_param, (void *) arg,
			sizeof(struct snd_compr_params))) {
			rc = -EFAULT;
			pr_err("[AUD]%s: ERROR: copy from user\n", __func__);
			return rc;
		}
		switch (compr->info.codec_param.codec.id) {
		case SND_AUDIOCODEC_MP3:
			/* For MP3 we dont need any other parameter */
			pr_debug("[AUD]SND_AUDIOCODEC_MP3\n");
			compr->codec = FORMAT_MP3;
			break;
		default:
			pr_debug("[AUD]FORMAT_LINEAR_PCM\n");
			compr->codec = FORMAT_LINEAR_PCM;
			break;
		}
		return 0;
	case SNDRV_PCM_IOCTL1_RESET:
		prtd->cmd_ack = 0;
		rc = q6asm_cmd(prtd->audio_client, CMD_FLUSH);
		if (rc < 0)
			pr_err("[AUD]%s: flush cmd failed rc=%d\n", __func__, rc);
		rc = wait_event_timeout(the_locks.eos_wait,
			prtd->cmd_ack, 5 * HZ);
		if (rc < 0)
			pr_err("[AUD]Flush cmd timeout\n");
		prtd->pcm_irq_pos = 0;
		break;
	case SNDRV_PCM_IOCTL1_ENABLE_EFFECT:
	{
		struct param {
			uint32_t effect_type; /* 0 for POPP, 1 for COPP */
			uint32_t module_id;
			uint32_t param_id;
			uint32_t payload_size;
		} q6_param;
		void *payload;

		pr_info("%s: SNDRV_PCM_IOCTL1_ENABLE_EFFECT\n", __func__);
		if (copy_from_user(&q6_param, (void *) arg,
					sizeof(q6_param))) {
			pr_err("%s: copy param from user failed\n",
				__func__);
			return -EFAULT;
		}

		if (q6_param.payload_size <= 0 ||
		    (q6_param.effect_type != 0 &&
		     q6_param.effect_type != 1)) {
			pr_err("%s: unsupported param: %d, 0x%x, 0x%x, %d\n",
				__func__, q6_param.effect_type,
				q6_param.module_id, q6_param.param_id,
				q6_param.payload_size);
			return -EINVAL;
		}

		payload = kzalloc(q6_param.payload_size, GFP_KERNEL);
		if (!payload) {
			pr_err("%s: failed to allocate memory\n",
				__func__);
			return -ENOMEM;
		}
		if (copy_from_user(payload, (void *) (arg + sizeof(q6_param)),
			q6_param.payload_size)) {
			pr_err("%s: copy payload from user failed\n",
				__func__);
			kfree(payload);
			return -EFAULT;
		}

		if (q6_param.effect_type == 0) { /* POPP */
			if (!prtd->audio_client) {
				pr_debug("%s: audio_client not found\n",
					__func__);
				kfree(payload);
				return -EACCES;
			}
			rc = q6asm_enable_effect(prtd->audio_client,
						q6_param.module_id,
						q6_param.param_id,
						q6_param.payload_size,
						payload);
			pr_info("%s: call q6asm_enable_effect, rc %d\n",
				__func__, rc);
		} else { /* COPP */
			int port_id = msm_pcm_routing_get_port(substream);
			int index = afe_get_port_index(port_id);
			pr_info("%s: use copp topology, port id %d, index %d\n",
				__func__, port_id, index);
			if (port_id < 0) {
				pr_err("%s: invalid port_id %d\n",
					__func__, port_id);
			} else {
				rc = q6adm_enable_effect(index,
						     q6_param.module_id,
						     q6_param.param_id,
						     q6_param.payload_size,
						     payload);
				pr_info("%s: call q6adm_enable_effect, rc %d\n",
					__func__, rc);
			}
		}
#if Q6_EFFECT_DEBUG
		{
			int *ptr;
			int i;
			ptr = (int *)payload;
			for (i = 0; i < (q6_param.payload_size / 4); i++)
				pr_aud_info("0x%08x", *(ptr + i));
		}
#endif
		kfree(payload);
		return rc;
	}

	default:
		break;
	}
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static struct snd_pcm_ops msm_compr_ops = {
	.open	   = msm_compr_open,
	.hw_params	= msm_compr_hw_params,
	.close	  = msm_compr_close,
	.ioctl	  = msm_compr_ioctl,
	.prepare	= msm_compr_prepare,
	.trigger	= msm_compr_trigger,
	.pointer	= msm_compr_pointer,
	.mmap		= msm_compr_mmap,
};

static int msm_asoc_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	int ret = 0;

	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);
	return ret;
}

static struct snd_soc_platform_driver msm_soc_platform = {
	.ops		= &msm_compr_ops,
	.pcm_new	= msm_asoc_pcm_new,
};

static __devinit int msm_compr_probe(struct platform_device *pdev)
{
	pr_info("[AUD]%s: dev name %s\n", __func__, dev_name(&pdev->dev));
	return snd_soc_register_platform(&pdev->dev,
				   &msm_soc_platform);
}

static int msm_compr_remove(struct platform_device *pdev)
{
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static struct platform_driver msm_compr_driver = {
	.driver = {
		.name = "msm-compr-dsp",
		.owner = THIS_MODULE,
	},
	.probe = msm_compr_probe,
	.remove = __devexit_p(msm_compr_remove),
};

static int __init msm_soc_platform_init(void)
{
	init_waitqueue_head(&the_locks.enable_wait);
	init_waitqueue_head(&the_locks.eos_wait);
	init_waitqueue_head(&the_locks.write_wait);
	init_waitqueue_head(&the_locks.read_wait);

	return platform_driver_register(&msm_compr_driver);
}
module_init(msm_soc_platform_init);

static void __exit msm_soc_platform_exit(void)
{
	platform_driver_unregister(&msm_compr_driver);
}
module_exit(msm_soc_platform_exit);

MODULE_DESCRIPTION("PCM module platform driver");
MODULE_LICENSE("GPL v2");
