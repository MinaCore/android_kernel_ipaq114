/*
 * Handles the Samsung I780-I900 SoC system
 *
 * Copyright (C) 2009 Mustafa Ozsakalli
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation in version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/irq.h>

#include <asm/mach-types.h>
#include <mach/audio.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/ac97_codec.h>

#include "pxa2xx-pcm.h"
#include "pxa2xx-ac97.h"
#include "../codecs/wm9713.h"
#include "pxa-ssp.h"

#define ARRAY_AND_SIZE(x)	(x), ARRAY_SIZE(x)

#define SGH_I780_AUDIO_GPIO	0x13
#define SGH_I900_AUDIO_GPIO	0x11

static const struct snd_soc_dapm_widget sgh_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Front Speaker", NULL),
	SND_SOC_DAPM_HP("Headset", NULL),
	SND_SOC_DAPM_LINE("GSM Line Out", NULL),
	SND_SOC_DAPM_LINE("GSM Line In", NULL),
	SND_SOC_DAPM_LINE("Radio Line Out", NULL),
	SND_SOC_DAPM_MIC("Front Mic", NULL),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* Microphone */
	{"MIC1", NULL, "Front Mic"},

	/* Speaker */
	{"Front Speaker", NULL, "SPKL"},
	{"Front Speaker", NULL, "SPKR"},

	/* Earpiece */
	{"Headset", NULL, "HPL"},
	{"Headset", NULL, "HPR"},

	/* GSM Module */
	{"MONOIN", NULL, "GSM Line Out"},
	{"PCBEEP", NULL, "GSM Line Out"},
	{"GSM Line In", NULL, "MONO"},

	/* FM Radio Module */
	{"LINEL", NULL, "Radio Line Out"},
	{"LINER", NULL, "Radio Line Out"},
};

static int sgh_wm9713_init(struct snd_soc_codec *codec)
{
	unsigned short reg;

	snd_soc_dapm_new_controls(codec, ARRAY_AND_SIZE(sgh_dapm_widgets));
	snd_soc_dapm_add_routes(codec, ARRAY_AND_SIZE(audio_map));

	snd_soc_dapm_enable_pin(codec, "Front Speaker");

	snd_soc_dapm_sync(codec);

	
	return 0;
}

static int sgh_hifi_startup(struct snd_pcm_substream *substream){
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	cpu_dai->playback.channels_min = 2;
    cpu_dai->playback.channels_max = 2;
	
    return 0;
}

static int sgh_hifi_prepare(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->socdev->card->codec;
	struct snd_soc_dai *codec_dai = rtd->dai->codec_dai;
	u16 reg;
	int gpio = machine_is_sgh_i780() ? SGH_I780_AUDIO_GPIO : SGH_I900_AUDIO_GPIO;

	codec->write(codec, AC97_POWERDOWN, 0);
	mdelay(1);
	codec_dai->ops->set_pll(codec_dai, 0, 4096000, 0);
	schedule_timeout_interruptible(msecs_to_jiffies(10));
	codec->write(codec, AC97_HANDSET_RATE, 0x0000);
	schedule_timeout_interruptible(msecs_to_jiffies(10));

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		reg = AC97_PCM_FRONT_DAC_RATE;
	else
		reg = AC97_PCM_LR_ADC_RATE;
	codec->write(codec, AC97_EXTENDED_STATUS, 0x1);
	codec->write(codec, reg, substream->runtime->rate);

	//Turn on external speaker
	//TODO: Headset detection
	gpio_set_value(gpio, 1);

	return 0;
}

static void sgh_hifi_shutdown(struct snd_pcm_substream *substream) {
	int gpio = machine_is_sgh_i780() ? SGH_I780_AUDIO_GPIO : SGH_I900_AUDIO_GPIO;
	gpio_direction_output(gpio, 1);
	gpio_set_value(gpio, 0);
}

static int sgh_voice_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	cpu_dai->playback.channels_min = 1;
	cpu_dai->playback.channels_max = 1;

	return 0;
};

static int sgh_voice_prepare(struct snd_pcm_substream *substream)
{
#define WM9713_DR_8000     0x1F40  /*  8000 samples/sec */
#define WM9713_DR_11025    0x2B11  /* 11025 samples/sec */
#define WM9713_DR_12000    0x2EE0  /* 12000 samples/sec */
#define WM9713_DR_16000    0x3E80  /* 16000 samples/sec */
#define WM9713_DR_22050    0x5622  /* 22050 samples/sec */
#define WM9713_DR_24000    0x5DC0  /* 24000 samples/sec */
#define WM9713_DR_32000    0x7D00  /* 32000 samples/sec */
#define WM9713_DR_44100    0xAC44  /* 44100 samples/sec */
#define WM9713_DR_48000    0xBB80  /* 48000 samples/sec */

	return 0;
};

static void sgh_voice_shutdown(struct snd_pcm_substream *substream)
{

};

static struct snd_soc_ops sgh_ops[] = {
{
		.startup	= sgh_hifi_startup,
		.prepare	= sgh_hifi_prepare,
		.shutdown 	= sgh_hifi_shutdown,
},
{
		.startup	= sgh_voice_startup,
		.prepare	= sgh_voice_prepare,
		.shutdown	= sgh_voice_shutdown,
},
};

static struct snd_soc_dai_link sgh_dai[] = {
	{
		.name = "AC97",
		.stream_name = "AC97 HiFi",
		.cpu_dai = &pxa_ac97_dai[PXA2XX_DAI_AC97_HIFI],
		.codec_dai = &wm9713_dai[WM9713_DAI_AC97_HIFI],
		.init = sgh_wm9713_init,
		.ops = &sgh_ops[0],
	},
	{
		.name = "AC97 Aux",
		.stream_name = "AC97 Aux",
		.cpu_dai = &pxa_ac97_dai[PXA2XX_DAI_AC97_AUX],
		.codec_dai = &wm9713_dai[WM9713_DAI_AC97_AUX],
	},
	{
		.name = "WM9713 Voice",
		.stream_name = "WM9713 Voice",
		.cpu_dai = &pxa_ssp_dai[PXA_DAI_SSP3],
		.codec_dai = &wm9713_dai[WM9713_DAI_PCM_VOICE],
		.ops = &sgh_ops[1],
	},
};

static struct snd_soc_card sgh = {
	.name = "SGHAudio",
	.platform = &pxa2xx_soc_platform,
	.dai_link = sgh_dai,
	.num_links = ARRAY_SIZE(sgh_dai),
};

static struct snd_soc_device sgh_snd_devdata = {
	.card = &sgh,
	.codec_dev = &soc_codec_dev_wm9713,
};

static struct platform_device *sgh_snd_device;

static int sgh_wm9713_probe(struct platform_device *pdev)
{
	int ret;
	int gpio = machine_is_sgh_i780() ? SGH_I780_AUDIO_GPIO : SGH_I900_AUDIO_GPIO;

	gpio_request(0x64, "WM9713 Power");
	gpio_direction_output(0x64, 1);
	gpio_set_value(0x64, 0);
	mdelay(10);
	gpio_set_value(0x64, 1);

	gpio_request(gpio, "Speaker");
	gpio_direction_output(gpio, 1);
	gpio_set_value(gpio, 0);

	sgh_snd_device = platform_device_alloc("soc-audio", -1);
	if (!sgh_snd_device)
		return -ENOMEM;

	platform_set_drvdata(sgh_snd_device, &sgh_snd_devdata);
	sgh_snd_devdata.dev = &sgh_snd_device->dev;

	ret = platform_device_add(sgh_snd_device);
	if (ret != 0)
		platform_device_put(sgh_snd_device);

	return ret;
}

static int __devexit sgh_wm9713_remove(struct platform_device *pdev)
{
	platform_device_unregister(sgh_snd_device);
	return 0;
}

#ifdef CONFIG_PM

static int sgh_wm9713_suspend(struct platform_device *pdev,
	pm_message_t state)
{
	//struct snd_soc_card *card = platform_get_drvdata(pdev);
	return 0;
	//return snd_soc_card_suspend_pcms(card, state);
}

static int sgh_wm9713_resume(struct platform_device *pdev)
{
	//struct snd_soc_card *card = platform_get_drvdata(pdev);
	return 0;
	//return snd_soc_card_resume_pcms(card);
}

#else
#define sgh_wm9713_suspend NULL
#define sgh_wm9713_resume  NULL
#define sgh_wm9713_suspend_late NULL
#define sgh_wm9713_resume_early  NULL
#endif

static struct platform_driver sgh_wm9713_driver = {
	.probe			= sgh_wm9713_probe,
	.remove			= __devexit_p(sgh_wm9713_remove),
	.suspend		= sgh_wm9713_suspend,
	.resume			= sgh_wm9713_resume,
	.driver			= {
		.name		= "sgh-asoc",
		.owner		= THIS_MODULE,
	},
};

static int __init sgh_asoc_init(void)
{
	int ret;

	ret = platform_driver_register(&sgh_wm9713_driver);

	return ret;
}

static void __exit sgh_asoc_exit(void)
{
	platform_driver_unregister(&sgh_wm9713_driver);
}

module_init(sgh_asoc_init);
module_exit(sgh_asoc_exit);

/* Module information */
MODULE_AUTHOR("Mustafa Ozsakalli (ozsakalli@hotmail.com)");
MODULE_DESCRIPTION("ALSA SoC WM9713 Samsung SGH I780/I900");
MODULE_LICENSE("GPL");
