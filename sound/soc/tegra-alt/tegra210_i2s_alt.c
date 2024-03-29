/*
 * tegra210_i2s.c - Tegra210 I2S driver
 *
 * Copyright (c) 2014-2020, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/pinctrl/pinconf-tegra.h>
#include <linux/regulator/consumer.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/tegra-powergate.h>
#include <linux/version.h>
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
#include <soc/tegra/chip-id.h>
#else
#include <soc/tegra/fuse.h>
#endif
#include <linux/pm_domain.h>

#include "tegra210_xbar_alt.h"
#include "tegra210_i2s_alt.h"

#define DRV_NAME "tegra210-i2s"

#define REG_IOVA(reg) (i2s->base_addr + (reg))

static const struct reg_default tegra210_i2s_reg_defaults[] = {
	{ TEGRA210_I2S_AXBAR_RX_INT_MASK, 0x00000003},
	{ TEGRA210_I2S_AXBAR_RX_CIF_CTRL, 0x00007700},
	{ TEGRA210_I2S_AXBAR_TX_INT_MASK, 0x00000003},
	{ TEGRA210_I2S_AXBAR_TX_CIF_CTRL, 0x00007700},
	{ TEGRA210_I2S_CG, 0x1},
	{ TEGRA210_I2S_TIMING, 0x0000001f},
	{ TEGRA210_I2S_ENABLE, 0x1},
	/*
	 * Below update does not have any effect on Tegra186 and Tegra194.
	 * On Tegra210, I2S4 has "i2s4a" and "i2s4b" pins and below update
	 * is required to select i2s4b for it to be functional for I2S
	 * operation.
	 */
	{ TEGRA210_I2S_CYA, 0x1},
};

static void tegra210_i2s_set_slot_ctrl(struct regmap *regmap,
				unsigned int total_slots,
				unsigned int tx_slot_mask,
				unsigned int rx_slot_mask)
{
	regmap_write(regmap, TEGRA210_I2S_SLOT_CTRL, total_slots - 1);
	regmap_write(regmap, TEGRA210_I2S_AXBAR_TX_SLOT_CTRL,
		tx_slot_mask);
	regmap_write(regmap, TEGRA210_I2S_AXBAR_RX_SLOT_CTRL,
		rx_slot_mask);
}

static int tegra210_i2s_set_clock_rate(struct device *dev, int clock_rate)
{
	unsigned int val;
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	int ret;

	regmap_read(i2s->regmap, TEGRA210_I2S_CTRL, &val);
	val &= TEGRA210_I2S_CTRL_MASTER_EN;

	/* no need to set rates if I2S is being operated in slave */
	if (!val)
		return 0;

	/* skip for fpga units */
	if (tegra_platform_is_fpga())
		return 0;

	ret = clk_set_rate(i2s->clk_i2s, clock_rate);
	if (ret) {
		dev_err(dev, "Can't set I2S clock rate: %d\n", ret);
		return ret;
	}

	if (!IS_ERR(i2s->clk_sync_input)) {
		/*
		 * other I/O modules in AHUB can use i2s bclk as reference
		 * clock. Below sets sync input clock rate as per bclk,
		 * which can be used as input to other I/O modules.
		 */
		ret = clk_set_rate(i2s->clk_sync_input, clock_rate);
		if (ret) {
			dev_err(dev, "Can't set I2S sync input clock rate\n");
			return ret;
		}
	}

	return 0;
}

static int tegra210_i2s_sw_reset(struct snd_soc_codec *codec, bool is_playback)
{
	struct device *dev = codec->dev;
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	unsigned int reset_reg, cif_reg, stream_reg;
	unsigned int cif_ctrl, stream_ctrl, i2s_ctrl, val;
	unsigned int reset_mask, reset_en;
	int ret;

	if (is_playback) {
		reset_mask = TEGRA210_I2S_AXBAR_RX_SOFT_RESET_MASK;
		reset_en = TEGRA210_I2S_AXBAR_RX_SOFT_RESET_EN;
		reset_reg = TEGRA210_I2S_AXBAR_RX_SOFT_RESET;
		cif_reg = TEGRA210_I2S_AXBAR_RX_CIF_CTRL;
		stream_reg = TEGRA210_I2S_AXBAR_RX_CTRL;
	} else {
		reset_mask = TEGRA210_I2S_AXBAR_TX_SOFT_RESET_MASK;
		reset_en = TEGRA210_I2S_AXBAR_TX_SOFT_RESET_EN;
		reset_reg = TEGRA210_I2S_AXBAR_TX_SOFT_RESET;
		cif_reg = TEGRA210_I2S_AXBAR_TX_CIF_CTRL;
		stream_reg = TEGRA210_I2S_AXBAR_TX_CTRL;
	}

	/* store */
	regmap_read(i2s->regmap, cif_reg, &cif_ctrl);
	regmap_read(i2s->regmap, stream_reg, &stream_ctrl);
	regmap_read(i2s->regmap, TEGRA210_I2S_CTRL, &i2s_ctrl);

	/* SW reset */
	regmap_update_bits(i2s->regmap, reset_reg, reset_mask, reset_en);

	ret = readl_poll_timeout_atomic(REG_IOVA(reset_reg), val,
					!(val & reset_mask & reset_en),
					10, 10000);
	if (ret < 0) {
		dev_err(dev, "timeout: failed to reset I2S for %s\n",
			is_playback ? "playback" : "capture");
		return ret;
	}

	/* restore */
	regmap_write(i2s->regmap, cif_reg, cif_ctrl);
	regmap_write(i2s->regmap, stream_reg, stream_ctrl);
	regmap_write(i2s->regmap, TEGRA210_I2S_CTRL, i2s_ctrl);

	return 0;
}

static int tegra210_i2s_init(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct device *dev = codec->dev;
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	unsigned int val, status_reg;
	bool is_playback;
	int ret;

	switch (w->reg) {
	case TEGRA210_I2S_AXBAR_RX_ENABLE:
		is_playback = true;
		status_reg = TEGRA210_I2S_AXBAR_RX_STATUS;
		break;
	case TEGRA210_I2S_AXBAR_TX_ENABLE:
		is_playback = false;
		status_reg = TEGRA210_I2S_AXBAR_TX_STATUS;
		break;
	default:
		return -EINVAL;
	}

	/* ensure I2S is in disabled state before new session */
	ret = readl_poll_timeout_atomic(REG_IOVA(status_reg), val,
			!(val & TEGRA210_I2S_EN_MASK & TEGRA210_I2S_EN),
			10, 10000);
	if (ret < 0) {
		dev_err(dev, "timeout: previous I2S %s is still active\n",
			is_playback ? "playback" : "capture");
		return ret;
	}

	/* SW reset */
	ret = tegra210_i2s_sw_reset(codec, is_playback);
	if (ret < 0)
		return ret;

	return 0;
}

static int tegra210_i2s_runtime_suspend(struct device *dev)
{
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);

	regcache_cache_only(i2s->regmap, true);
	if (!(tegra_platform_is_fpga())) {
		regcache_mark_dirty(i2s->regmap);
		clk_disable_unprepare(i2s->clk_i2s);
	}

	return 0;
}

static int tegra210_i2s_runtime_resume(struct device *dev)
{
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	int ret;

	if (!(tegra_platform_is_fpga())) {
		ret = clk_prepare_enable(i2s->clk_i2s);
		if (ret) {
			dev_err(dev, "clk_enable failed: %d\n", ret);
			return ret;
		}
	}

	regcache_cache_only(i2s->regmap, false);
	regcache_sync(i2s->regmap);

	return 0;
}

static void tegra210_i2s_set_data_offset(struct tegra210_i2s *i2s,
					 unsigned int data_offset)
{
	unsigned int reg, mask, shift;

	reg = TEGRA210_I2S_AXBAR_TX_CTRL;
	mask = TEGRA210_I2S_AXBAR_TX_CTRL_DATA_OFFSET_MASK;
	shift = TEGRA210_I2S_AXBAR_TX_CTRL_DATA_OFFSET_SHIFT;
	regmap_update_bits(i2s->regmap, reg, mask, data_offset << shift);

	reg = TEGRA210_I2S_AXBAR_RX_CTRL;
	mask = TEGRA210_I2S_AXBAR_RX_CTRL_DATA_OFFSET_MASK;
	shift = TEGRA210_I2S_AXBAR_RX_CTRL_DATA_OFFSET_SHIFT;
	regmap_update_bits(i2s->regmap, reg, mask, data_offset << shift);
}

static int tegra210_i2s_set_fmt(struct snd_soc_dai *dai,
				unsigned int fmt)
{
	struct tegra210_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int mask, val;

	mask = TEGRA210_I2S_CTRL_MASTER_EN_MASK;
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		val = 0;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		val = TEGRA210_I2S_CTRL_MASTER_EN;
		break;
	default:
		return -EINVAL;
	}

	mask |= TEGRA210_I2S_CTRL_FRAME_FORMAT_MASK |
		TEGRA210_I2S_CTRL_LRCK_POLARITY_MASK;
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
		val |= TEGRA210_I2S_CTRL_FRAME_FORMAT_FSYNC_MODE;
		val |= TEGRA210_I2S_CTRL_LRCK_POLARITY_HIGH;
		tegra210_i2s_set_data_offset(i2s, 1);
		break;
	case SND_SOC_DAIFMT_DSP_B:
		val |= TEGRA210_I2S_CTRL_FRAME_FORMAT_FSYNC_MODE;
		val |= TEGRA210_I2S_CTRL_LRCK_POLARITY_HIGH;
		tegra210_i2s_set_data_offset(i2s, 0);
		break;
	/* I2S mode has data offset of 1 */
	case SND_SOC_DAIFMT_I2S:
		val |= TEGRA210_I2S_CTRL_FRAME_FORMAT_LRCK_MODE;
		val |= TEGRA210_I2S_CTRL_LRCK_POLARITY_LOW;
		tegra210_i2s_set_data_offset(i2s, 1);
		break;
	/*
	 * For RJ mode data offset is dependent on the sample size
	 * and the bclk ratio, and so is set when hw_params is called.
	 */
	case SND_SOC_DAIFMT_RIGHT_J:
		val |= TEGRA210_I2S_CTRL_FRAME_FORMAT_LRCK_MODE;
		val |= TEGRA210_I2S_CTRL_LRCK_POLARITY_HIGH;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		val |= TEGRA210_I2S_CTRL_FRAME_FORMAT_LRCK_MODE;
		val |= TEGRA210_I2S_CTRL_LRCK_POLARITY_HIGH;
		tegra210_i2s_set_data_offset(i2s, 0);
		break;
	default:
		return -EINVAL;
	}

	mask |= TEGRA210_I2S_CTRL_EDGE_CTRL_MASK;
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		val |= TEGRA210_I2S_CTRL_EDGE_CTRL_POS_EDGE;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		val |= TEGRA210_I2S_CTRL_EDGE_CTRL_POS_EDGE;
		val ^= TEGRA210_I2S_CTRL_LRCK_POLARITY_MASK;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		val |= TEGRA210_I2S_CTRL_EDGE_CTRL_NEG_EDGE;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		val |= TEGRA210_I2S_CTRL_EDGE_CTRL_NEG_EDGE;
		val ^= TEGRA210_I2S_CTRL_LRCK_POLARITY_MASK;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, TEGRA210_I2S_CTRL, mask, val);

	i2s->format = fmt & SND_SOC_DAIFMT_FORMAT_MASK;

	return 0;
}

static int tegra210_i2s_set_tdm_slot(struct snd_soc_dai *dai,
		unsigned int tx_mask, unsigned int rx_mask,
		int slots, int slot_width)
{
	struct tegra210_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	/* copy the required tx and rx mask */
	i2s->tx_mask = (tx_mask > 0xFFFF) ? 0xFFFF : tx_mask;
	i2s->rx_mask = (rx_mask > 0xFFFF) ? 0xFFFF : rx_mask;

	return 0;
}

static int tegra210_i2s_set_dai_bclk_ratio(struct snd_soc_dai *dai,
					   unsigned int ratio)
{
	struct tegra210_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	i2s->bclk_ratio = ratio;

	return 0;
}

static int tegra210_i2s_get_format(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);
	long *uctl_val = &ucontrol->value.integer.value[0];

	/* get the format control flag */
	if (strstr(kcontrol->id.name, "Playback Audio Bit Format"))
		*uctl_val = i2s->audio_fmt_override[I2S_RX_PATH];
	else if (strstr(kcontrol->id.name, "Capture Audio Bit Format"))
		*uctl_val = i2s->audio_fmt_override[I2S_TX_PATH];
	else if (strstr(kcontrol->id.name, "codec"))
		*uctl_val = i2s->codec_bit_format;
	else if (strstr(kcontrol->id.name, "Sample Rate"))
		*uctl_val = i2s->sample_rate_via_control;
	else if (strstr(kcontrol->id.name, "Playback Audio Channels"))
		*uctl_val = i2s->audio_ch_override[I2S_RX_PATH];
	else if (strstr(kcontrol->id.name, "Capture Audio Channels"))
		*uctl_val = i2s->audio_ch_override[I2S_TX_PATH];
	else if (strstr(kcontrol->id.name, "Client Channels"))
		*uctl_val = i2s->client_ch_override;
	else if (strstr(kcontrol->id.name, "Capture stereo to mono"))
		*uctl_val = i2s->stereo_to_mono[I2S_TX_PATH];
	else if (strstr(kcontrol->id.name, "Capture mono to stereo"))
		*uctl_val = i2s->mono_to_stereo[I2S_TX_PATH];
	else if (strstr(kcontrol->id.name, "Playback stereo to mono"))
		*uctl_val = i2s->stereo_to_mono[I2S_RX_PATH];
	else if (strstr(kcontrol->id.name, "Playback mono to stereo"))
		*uctl_val = i2s->mono_to_stereo[I2S_RX_PATH];
	else if (strstr(kcontrol->id.name, "Playback FIFO threshold"))
		*uctl_val = i2s->rx_fifo_th;
	else if (strstr(kcontrol->id.name, "BCLK Ratio"))
		*uctl_val = i2s->bclk_ratio;

	return 0;
}

static int tegra210_i2s_put_format(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);
	int value = ucontrol->value.integer.value[0];

	/* set the format control flag */
	if (strstr(kcontrol->id.name, "Playback Audio Bit Format"))
		i2s->audio_fmt_override[I2S_RX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Capture Audio Bit Format"))
		i2s->audio_fmt_override[I2S_TX_PATH] = value;
	else if (strstr(kcontrol->id.name, "codec"))
		i2s->codec_bit_format = value;
	else if (strstr(kcontrol->id.name, "Sample Rate"))
		i2s->sample_rate_via_control = value;
	else if (strstr(kcontrol->id.name, "Playback Audio Channels"))
		i2s->audio_ch_override[I2S_RX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Capture Audio Channels"))
		i2s->audio_ch_override[I2S_TX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Client Channels"))
		i2s->client_ch_override = value;
	else if (strstr(kcontrol->id.name, "Capture stereo to mono"))
		i2s->stereo_to_mono[I2S_TX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Capture mono to stereo"))
		i2s->mono_to_stereo[I2S_TX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Playback stereo to mono"))
		i2s->stereo_to_mono[I2S_RX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Playback mono to stereo"))
		i2s->mono_to_stereo[I2S_RX_PATH] = value;
	else if (strstr(kcontrol->id.name, "Playback FIFO threshold")) {
		if (value >= 0 && value < TEGRA210_I2S_RX_FIFO_DEPTH)
			i2s->rx_fifo_th = value;
		else
			return -EINVAL;
	} else if (strstr(kcontrol->id.name, "BCLK Ratio"))
		i2s->bclk_ratio = value;

	return 0;
}

static const char * const tegra210_i2s_format_text[] = {
	"None",
	"16",
	"32",
};

static const int tegra210_cif_fmt[] = {
	0,
	TEGRA210_AUDIOCIF_BITS_16,
	TEGRA210_AUDIOCIF_BITS_32,
};

static const int tegra210_i2s_bit_fmt[] = {
	0,
	TEGRA210_I2S_CTRL_BIT_SIZE_16,
	TEGRA210_I2S_CTRL_BIT_SIZE_32,
};

static const int tegra210_i2s_sample_size[] = {
	0,
	16,
	32,
};

static const struct soc_enum tegra210_i2s_format_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0,
		ARRAY_SIZE(tegra210_i2s_format_text),
		tegra210_i2s_format_text);

static int tegra210_i2s_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	int ret;

	if (!(tegra_platform_is_fpga()) &&
							!i2s->loopback) {
		if (i2s->prod_name != NULL) {
			ret = tegra_pinctrl_config_prod(dev, i2s->prod_name);
			if (ret < 0) {
				dev_warn(dev, "Failed to set %s setting\n",
						i2s->prod_name);
			}
		}

		if (i2s->num_supplies > 0) {
			ret = regulator_bulk_enable(i2s->num_supplies,
								i2s->supplies);
			if (ret < 0)
				dev_err(dev, "failed to enable i2s io regulator\n");
		}
	}

	return 0;
}

static void tegra210_i2s_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	int ret;

	if (!(tegra_platform_is_fpga())) {
		if (i2s->num_supplies > 0) {
			ret = regulator_bulk_disable(i2s->num_supplies,
								i2s->supplies);
			if (ret < 0) {
				dev_err(dev,
				"failed to disable i2s io regulator\n");
			}
		}
	}
}

static int tegra210_i2s_set_timing_params(struct device *dev,
					  unsigned int sample_size,
					  unsigned int srate,
					  unsigned int channels)
{
	struct tegra210_i2s *i2s = dev_get_drvdata(dev);
	unsigned int val, bit_count, bclk_rate, num_bclk = sample_size;
	int ret;

	if (i2s->bclk_ratio)
		num_bclk *= i2s->bclk_ratio;

	if (i2s->format == SND_SOC_DAIFMT_RIGHT_J)
		tegra210_i2s_set_data_offset(i2s, num_bclk - sample_size);

	/* I2S bit clock rate */
	bclk_rate = srate * channels * num_bclk;

	ret = tegra210_i2s_set_clock_rate(dev, bclk_rate);
	if (ret) {
		dev_err(dev, "Can't set I2S bit clock rate for %u, err: %d\n",
			bclk_rate, ret);
		return ret;
	}

	regmap_read(i2s->regmap, TEGRA210_I2S_CTRL, &val);

	/*
	 * For LRCK mode, channel bit count depends on number of bit clocks
	 * on the left channel, where as for FSYNC mode bit count depends on
	 * the number of bit clocks in both left and right channels for DSP
	 * mode or the number of bit clocks in one TDM frame.
	 *
	 */
	switch (val & TEGRA210_I2S_CTRL_FRAME_FORMAT_MASK) {
	case TEGRA210_I2S_CTRL_FRAME_FORMAT_LRCK_MODE:
		bit_count = (bclk_rate / (srate * 2)) - 1;
		break;
	case TEGRA210_I2S_CTRL_FRAME_FORMAT_FSYNC_MODE:
		bit_count = (bclk_rate / srate) - 1;

		tegra210_i2s_set_slot_ctrl(i2s->regmap, channels,
					   i2s->tx_mask, i2s->rx_mask);
		break;
	default:
		dev_err(dev, "invalid I2S mode\n");
		return -EINVAL;
	}

	if (bit_count > TEGRA210_I2S_TIMING_CHANNEL_BIT_CNT_MASK) {
		dev_err(dev, "invalid channel bit count %u\n", bit_count);
		return -EINVAL;
	}

	regmap_write(i2s->regmap, TEGRA210_I2S_TIMING,
		     bit_count << TEGRA210_I2S_TIMING_CHANNEL_BIT_CNT_SHIFT);

	return 0;
}

static int tegra210_i2s_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct tegra210_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int sample_size, channels, srate, val, reg, path;
	struct tegra210_xbar_cif_conf cif_conf;
	int ret, max_th;

	memset(&cif_conf, 0, sizeof(struct tegra210_xbar_cif_conf));

	channels = params_channels(params);
	if (channels < 1) {
		dev_err(dev, "Doesn't support %d channels\n", channels);
		return -EINVAL;
	}

	cif_conf.audio_channels = channels;
	cif_conf.client_channels = channels;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S8:
		val = TEGRA210_I2S_CTRL_BIT_SIZE_8;
		sample_size = 8;
		cif_conf.audio_bits = TEGRA210_AUDIOCIF_BITS_8;
		cif_conf.client_bits = TEGRA210_AUDIOCIF_BITS_8;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		val = TEGRA210_I2S_CTRL_BIT_SIZE_16;
		sample_size = 16;
		cif_conf.audio_bits = TEGRA210_AUDIOCIF_BITS_16;
		cif_conf.client_bits = TEGRA210_AUDIOCIF_BITS_16;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		val = TEGRA210_I2S_CTRL_BIT_SIZE_32;
		sample_size = 32;
		cif_conf.audio_bits = TEGRA210_AUDIOCIF_BITS_32;
		cif_conf.client_bits = TEGRA210_AUDIOCIF_BITS_32;
		break;
	default:
		dev_err(dev, "Wrong format!\n");
		return -EINVAL;
	}

	if (i2s->codec_bit_format) {
		val = tegra210_i2s_bit_fmt[i2s->codec_bit_format];
		sample_size = tegra210_i2s_sample_size[i2s->codec_bit_format];
		cif_conf.client_bits =
			tegra210_cif_fmt[i2s->codec_bit_format];
	}

	regmap_update_bits(i2s->regmap, TEGRA210_I2S_CTRL,
			   TEGRA210_I2S_CTRL_BIT_SIZE_MASK, val);

	srate = params_rate(params);

	if (i2s->sample_rate_via_control)
		srate = i2s->sample_rate_via_control;

	/*
	 * For playback I2S RX-CIF and for capture TX-CIF is used.
	 * With reference to AHUB, for I2S, SNDRV_PCM_STREAM_CAPTURE stream is
	 * actually for playback.
	 */
	path = (substream->stream == SNDRV_PCM_STREAM_CAPTURE) ?
	       I2S_RX_PATH : I2S_TX_PATH;

	if (i2s->audio_ch_override[path])
		cif_conf.audio_channels = i2s->audio_ch_override[path];

	if (i2s->client_ch_override)
		cif_conf.client_channels = i2s->client_ch_override;

	if (i2s->audio_fmt_override[path])
		cif_conf.audio_bits =
			tegra210_cif_fmt[i2s->audio_fmt_override[path]];

	/* As a COCEC DAI, CAPTURE is transmit */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		unsigned int audio_ch = cif_conf.audio_channels;

		reg = TEGRA210_I2S_AXBAR_RX_CIF_CTRL;

		/* RX FIFO threshold interms of frames */
		max_th = (TEGRA210_I2S_RX_FIFO_DEPTH / audio_ch) - 1;
		if (max_th < 0)
			return -EINVAL;

		if (i2s->rx_fifo_th > max_th) /* error handling */
			i2s->rx_fifo_th = max_th;

		cif_conf.threshold = i2s->rx_fifo_th;
	} else
		reg = TEGRA210_I2S_AXBAR_TX_CIF_CTRL;

	cif_conf.stereo_conv = i2s->stereo_to_mono[path];
	cif_conf.mono_conv = i2s->mono_to_stereo[path];

	tegra210_xbar_set_cif(i2s->regmap, reg, &cif_conf);

	ret = tegra210_i2s_set_timing_params(dev, sample_size, srate,
					     cif_conf.client_channels);
	if (ret < 0)
		return ret;

	return 0;
}

static struct snd_soc_dai_ops tegra210_i2s_dai_ops = {
	.set_fmt	= tegra210_i2s_set_fmt,
	.hw_params	= tegra210_i2s_hw_params,
	.set_bclk_ratio	= tegra210_i2s_set_dai_bclk_ratio,
	.set_tdm_slot	= tegra210_i2s_set_tdm_slot,
	.startup	= tegra210_i2s_startup,
	.shutdown	= tegra210_i2s_shutdown,
};

static struct snd_soc_dai_driver tegra210_i2s_dais[] = {
	{
		.name = "CIF",
		.playback = {
			.stream_name = "CIF Receive",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.capture = {
			.stream_name = "CIF Transmit",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
	},
	{
		.name = "DAP",
		.playback = {
			.stream_name = "DAP Receive",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.capture = {
			.stream_name = "DAP Transmit",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.ops = &tegra210_i2s_dai_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "DUMMY",
		.playback = {
			.stream_name = "Dummy Playback",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.capture = {
			.stream_name = "Dummy Capture",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
	},
};

static int tegra210_i2s_loopback_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = i2s->loopback;

	return 0;
}

static int tegra210_i2s_loopback_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	i2s->loopback = ucontrol->value.integer.value[0];

	regmap_update_bits(i2s->regmap, TEGRA210_I2S_CTRL,
		TEGRA210_I2S_CTRL_LPBK_MASK,
		i2s->loopback << TEGRA210_I2S_CTRL_LPBK_SHIFT);

	return 0;
}

static int tegra210_i2s_get_bclk_ratio(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = i2s->bclk_ratio;

	return 0;
}

static int tegra210_i2s_put_bclk_ratio(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	i2s->bclk_ratio = ucontrol->value.integer.value[0];

	return 0;
}

static int tegra210_i2s_fsync_width_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = i2s->fsync_width;

	return 0;
}

static int tegra210_i2s_fsync_width_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tegra210_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	i2s->fsync_width = ucontrol->value.integer.value[0];

	/*
	 * frame sync width is used only for FSYNC modes and not applicable
	 * for LRCK modes. Reset value for this field is "0", which means
	 * the width is one bit clock wide. The width requirement may depend
	 * on the codec and in such cases mixer control is used to update
	 * custom values. A value of "N" here means, width is "N + 1" bit
	 * clock wide.
	 */
	regmap_update_bits(i2s->regmap, TEGRA210_I2S_CTRL,
			   TEGRA210_I2S_CTRL_FSYNC_WIDTH_MASK,
			   i2s->fsync_width <<
			   TEGRA210_I2S_CTRL_FSYNC_WIDTH_SHIFT);

	return 0;
}

static const char * const tegra210_i2s_stereo_conv_text[] = {
	"CH0", "CH1", "AVG",
};

static const char * const tegra210_i2s_mono_conv_text[] = {
	"Zero", "Copy",
};

static const struct soc_enum tegra210_i2s_mono_conv_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0,
		ARRAY_SIZE(tegra210_i2s_mono_conv_text),
		tegra210_i2s_mono_conv_text);

static const struct soc_enum tegra210_i2s_stereo_conv_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0,
		ARRAY_SIZE(tegra210_i2s_stereo_conv_text),
		tegra210_i2s_stereo_conv_text);

#define NV_SOC_SINGLE_RANGE_EXT(xname, xmin, xmax, xget, xput) \
{       .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
        .info = snd_soc_info_xr_sx, .get = xget, .put = xput, \
        .private_value = (unsigned long)&(struct soc_mixer_control) \
                {.invert = 0, .min = xmin, .max = xmax, \
                .platform_max = xmax}}

static const struct snd_kcontrol_new tegra210_i2s_controls[] = {
	SOC_SINGLE_EXT("Loopback", SND_SOC_NOPM, 0, 1, 0,
		tegra210_i2s_loopback_get, tegra210_i2s_loopback_put),
	SOC_ENUM_EXT("Playback Audio Bit Format", tegra210_i2s_format_enum,
		     tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_ENUM_EXT("Capture Audio Bit Format", tegra210_i2s_format_enum,
		     tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_ENUM_EXT("codec bit format", tegra210_i2s_format_enum,
		tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_SINGLE_EXT("FSYNC Width", SND_SOC_NOPM, 0, 255, 0,
		tegra210_i2s_fsync_width_get, tegra210_i2s_fsync_width_put),
	SOC_SINGLE_EXT("Sample Rate", 0, 0, 192000, 0,
		tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_SINGLE_EXT("Playback Audio Channels", 0, 0, 16, 0,
		tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_SINGLE_EXT("Capture Audio Channels", 0, 0, 16, 0,
		tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_SINGLE_EXT("Client Channels", 0, 0, 16, 0,
		tegra210_i2s_get_format, tegra210_i2s_put_format),
	SOC_SINGLE_EXT("BCLK Ratio", SND_SOC_NOPM, 0, INT_MAX, 0,
		       tegra210_i2s_get_bclk_ratio,
		       tegra210_i2s_put_bclk_ratio),
	SOC_ENUM_EXT("Capture stereo to mono conv",
		     tegra210_i2s_stereo_conv_enum, tegra210_i2s_get_format,
		     tegra210_i2s_put_format),
	SOC_ENUM_EXT("Capture mono to stereo conv",
		     tegra210_i2s_mono_conv_enum, tegra210_i2s_get_format,
		     tegra210_i2s_put_format),
	SOC_ENUM_EXT("Playback stereo to mono conv",
		     tegra210_i2s_stereo_conv_enum, tegra210_i2s_get_format,
		     tegra210_i2s_put_format),
	SOC_ENUM_EXT("Playback mono to stereo conv",
		     tegra210_i2s_mono_conv_enum, tegra210_i2s_get_format,
		     tegra210_i2s_put_format),
	NV_SOC_SINGLE_RANGE_EXT("Playback FIFO threshold", 0,
				TEGRA210_I2S_RX_FIFO_DEPTH - 1,
				tegra210_i2s_get_format,
				tegra210_i2s_put_format),
};

static const struct snd_soc_dapm_widget tegra210_i2s_widgets[] = {
	SND_SOC_DAPM_AIF_IN("CIF RX", NULL, 0, SND_SOC_NOPM,
				0, 0),
	SND_SOC_DAPM_AIF_OUT("CIF TX", NULL, 0, SND_SOC_NOPM,
				0, 0),
	SND_SOC_DAPM_AIF_IN_E("DAP RX", NULL, 0, TEGRA210_I2S_AXBAR_TX_ENABLE,
			      TEGRA210_I2S_AXBAR_TX_EN_SHIFT, 0,
			      tegra210_i2s_init, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_AIF_OUT_E("DAP TX", NULL, 0, TEGRA210_I2S_AXBAR_RX_ENABLE,
			       TEGRA210_I2S_AXBAR_RX_EN_SHIFT, 0,
			       tegra210_i2s_init, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIC("Dummy Input", NULL),
	SND_SOC_DAPM_SPK("Dummy Output", NULL),
};

static const struct snd_soc_dapm_route tegra210_i2s_routes[] = {
	{ "CIF RX",       NULL, "CIF Receive" },
	{ "DAP TX",       NULL, "CIF RX" },
	{ "DAP Transmit", NULL, "DAP TX" },

	{ "DAP RX",       NULL, "DAP Receive" },
	{ "CIF TX",       NULL, "DAP RX" },
	{ "CIF Transmit", NULL, "CIF TX" },

	{"Dummy Capture", NULL, "Dummy Input"},
	{"Dummy Output",  NULL, "Dummy Playback"},
};

static struct snd_soc_codec_driver tegra210_i2s_codec = {
	.idle_bias_off = 1,
	.component_driver = {
		.dapm_widgets = tegra210_i2s_widgets,
		.num_dapm_widgets = ARRAY_SIZE(tegra210_i2s_widgets),
		.dapm_routes = tegra210_i2s_routes,
		.num_dapm_routes = ARRAY_SIZE(tegra210_i2s_routes),
		.controls = tegra210_i2s_controls,
		.num_controls = ARRAY_SIZE(tegra210_i2s_controls),
	},
};

static bool tegra210_i2s_wr_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA210_I2S_AXBAR_RX_ENABLE:
	case TEGRA210_I2S_AXBAR_RX_SOFT_RESET:
	case TEGRA210_I2S_AXBAR_RX_INT_MASK:
	case TEGRA210_I2S_AXBAR_RX_INT_SET:
	case TEGRA210_I2S_AXBAR_RX_INT_CLEAR:
	case TEGRA210_I2S_AXBAR_RX_CIF_CTRL:
	case TEGRA210_I2S_AXBAR_RX_CTRL:
	case TEGRA210_I2S_AXBAR_RX_SLOT_CTRL:
	case TEGRA210_I2S_AXBAR_RX_CLK_TRIM:
	case TEGRA210_I2S_AXBAR_TX_ENABLE:
	case TEGRA210_I2S_AXBAR_TX_SOFT_RESET:
	case TEGRA210_I2S_AXBAR_TX_INT_MASK:
	case TEGRA210_I2S_AXBAR_TX_INT_SET:
	case TEGRA210_I2S_AXBAR_TX_INT_CLEAR:
	case TEGRA210_I2S_AXBAR_TX_CIF_CTRL:
	case TEGRA210_I2S_AXBAR_TX_CTRL:
	case TEGRA210_I2S_AXBAR_TX_SLOT_CTRL:
	case TEGRA210_I2S_AXBAR_TX_CLK_TRIM:
	case TEGRA210_I2S_ENABLE:
	case TEGRA210_I2S_SOFT_RESET:
	case TEGRA210_I2S_CG:
	case TEGRA210_I2S_CTRL:
	case TEGRA210_I2S_TIMING:
	case TEGRA210_I2S_SLOT_CTRL:
	case TEGRA210_I2S_CLK_TRIM:
	case TEGRA210_I2S_CYA:
		return true;
	default:
		return false;
	};
}

static bool tegra210_i2s_rd_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA210_I2S_AXBAR_RX_STATUS:
	case TEGRA210_I2S_AXBAR_RX_CIF_FIFO_STATUS:
	case TEGRA210_I2S_AXBAR_RX_ENABLE:
	case TEGRA210_I2S_AXBAR_RX_INT_MASK:
	case TEGRA210_I2S_AXBAR_RX_INT_SET:
	case TEGRA210_I2S_AXBAR_RX_INT_CLEAR:
	case TEGRA210_I2S_AXBAR_RX_CIF_CTRL:
	case TEGRA210_I2S_AXBAR_RX_CTRL:
	case TEGRA210_I2S_AXBAR_RX_SLOT_CTRL:
	case TEGRA210_I2S_AXBAR_RX_CLK_TRIM:
	case TEGRA210_I2S_AXBAR_RX_INT_STATUS:
	case TEGRA210_I2S_AXBAR_RX_SOFT_RESET:
	case TEGRA210_I2S_AXBAR_TX_STATUS:
	case TEGRA210_I2S_AXBAR_TX_CIF_FIFO_STATUS:
	case TEGRA210_I2S_AXBAR_TX_ENABLE:
	case TEGRA210_I2S_AXBAR_TX_INT_MASK:
	case TEGRA210_I2S_AXBAR_TX_INT_SET:
	case TEGRA210_I2S_AXBAR_TX_INT_CLEAR:
	case TEGRA210_I2S_AXBAR_TX_CIF_CTRL:
	case TEGRA210_I2S_AXBAR_TX_CTRL:
	case TEGRA210_I2S_AXBAR_TX_SLOT_CTRL:
	case TEGRA210_I2S_AXBAR_TX_CLK_TRIM:
	case TEGRA210_I2S_AXBAR_TX_INT_STATUS:
	case TEGRA210_I2S_AXBAR_TX_SOFT_RESET:
	case TEGRA210_I2S_ENABLE:
	case TEGRA210_I2S_STATUS:
	case TEGRA210_I2S_SOFT_RESET:
	case TEGRA210_I2S_CG:
	case TEGRA210_I2S_CTRL:
	case TEGRA210_I2S_TIMING:
	case TEGRA210_I2S_SLOT_CTRL:
	case TEGRA210_I2S_CLK_TRIM:
	case TEGRA210_I2S_INT_STATUS:
	case TEGRA210_I2S_CYA:
		return true;
	default:
		return false;
	};
}

static bool tegra210_i2s_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA210_I2S_AXBAR_RX_INT_STATUS:
	case TEGRA210_I2S_AXBAR_RX_STATUS:
	case TEGRA210_I2S_AXBAR_TX_STATUS:
	case TEGRA210_I2S_AXBAR_TX_INT_STATUS:
	case TEGRA210_I2S_INT_STATUS:
	case TEGRA210_I2S_AXBAR_RX_SOFT_RESET:
	case TEGRA210_I2S_AXBAR_TX_SOFT_RESET:
		return true;
	default:
		return false;
	};
}

static const struct regmap_config tegra210_i2s_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = TEGRA210_I2S_CYA,
	.writeable_reg = tegra210_i2s_wr_reg,
	.readable_reg = tegra210_i2s_rd_reg,
	.volatile_reg = tegra210_i2s_volatile_reg,
	.reg_defaults = tegra210_i2s_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tegra210_i2s_reg_defaults),
	.cache_type = REGCACHE_FLAT,
};

static const struct of_device_id tegra210_i2s_of_match[] = {
	{ .compatible = "nvidia,tegra210-i2s" },
	{},
};

static int tegra210_i2s_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct device_node *np = pdev->dev.of_node;
	struct tegra210_i2s *i2s;
	struct resource *mem;
	struct property *prop;
	void __iomem *regs;
	int ret = 0, count = 0, num_supplies;
	const char *supply;

	match = of_match_device(tegra210_i2s_of_match, &pdev->dev);
	if (!match) {
		dev_err(&pdev->dev, "Error: No device match found\n");
		return -ENODEV;
	}

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	i2s->tx_mask = i2s->rx_mask = 0xFFFF;
	i2s->loopback = 0;
	i2s->prod_name = NULL;
	/* default threshold settings */
	i2s->rx_fifo_th = 3;
	dev_set_drvdata(&pdev->dev, i2s);

	if (!(tegra_platform_is_fpga())) {
		i2s->clk_i2s = devm_clk_get(&pdev->dev, "i2s");
		if (IS_ERR(i2s->clk_i2s)) {
			dev_err(&pdev->dev, "Can't retrieve i2s clock\n");
			return PTR_ERR(i2s->clk_i2s);
		}
		i2s->clk_sync_input =
			devm_clk_get(&pdev->dev, "clk_sync_input");
		if (IS_ERR(i2s->clk_sync_input))
			dev_dbg(&pdev->dev, "Can't get i2s sync input clock\n");
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	i2s->base_addr = regs;

	i2s->regmap = devm_regmap_init_mmio(&pdev->dev, regs,
					    &tegra210_i2s_regmap_config);
	if (IS_ERR(i2s->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		return PTR_ERR(i2s->regmap);
	}
	regcache_cache_only(i2s->regmap, true);

	if (of_property_read_u32(pdev->dev.of_node, "bclk-ratio",
				 &i2s->bclk_ratio) < 0) {
		dev_dbg(&pdev->dev, "Missing prop bclk-ratio for I2S\n");
		i2s->bclk_ratio = 1;
	}

	if (!(tegra_platform_is_fpga())) {
		if (of_property_read_string(np, "prod-name",
					    &i2s->prod_name) == 0)
			tegra_pinctrl_config_prod(&pdev->dev, i2s->prod_name);

		num_supplies = of_property_count_strings(np,
							 "regulator-supplies");
		if (num_supplies > 0) {
			i2s->num_supplies = num_supplies;
			i2s->supplies = devm_kzalloc(&pdev->dev, num_supplies *
						     sizeof(*i2s->supplies),
						     GFP_KERNEL);
			if (!i2s->supplies)
				return -ENOMEM;

			of_property_for_each_string(np, "regulator-supplies",
						    prop, supply)
				i2s->supplies[count++].supply = supply;

			ret = devm_regulator_bulk_get(&pdev->dev,
						      i2s->num_supplies,
						      i2s->supplies);
			if (ret) {
				dev_err(&pdev->dev,
					"Failed to get supplies: %d\n", ret);
				return ret;
			}
		}
	}
	pm_runtime_enable(&pdev->dev);
	ret = snd_soc_register_codec(&pdev->dev, &tegra210_i2s_codec,
				     tegra210_i2s_dais,
				     ARRAY_SIZE(tegra210_i2s_dais));
	if (ret != 0) {
		dev_err(&pdev->dev, "Could not register CODEC: %d\n", ret);
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	return 0;
}

static int tegra210_i2s_platform_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);

	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		tegra210_i2s_runtime_suspend(&pdev->dev);

	return 0;
}

static const struct dev_pm_ops tegra210_i2s_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra210_i2s_runtime_suspend,
			   tegra210_i2s_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

static struct platform_driver tegra210_i2s_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = tegra210_i2s_of_match,
		.pm = &tegra210_i2s_pm_ops,
	},
	.probe = tegra210_i2s_platform_probe,
	.remove = tegra210_i2s_platform_remove,
};
module_platform_driver(tegra210_i2s_driver)

MODULE_AUTHOR("Songhee Baek <sbaek@nvidia.com>");
MODULE_DESCRIPTION("Tegra210 I2S ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, tegra210_i2s_of_match);
