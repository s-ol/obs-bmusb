#include <stdlib.h>
#include <obs.h>
#include <obs-module.h>
#include <bmusb/bmusb.h>

#include <stdio.h>

using bmusb::BMUSBCapture;
using bmusb::FrameAllocator;
using bmusb::VideoFormat;
using bmusb::AudioFormat;

struct bmusb_inst {
	obs_source_t *source;
	BMUSBCapture *capture;
	bool         initialized;
};

static const char *bmusb_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Blackmagic USB3 source (bmusb)";
}

static void bmusb_destroy(void *data)
{
	struct bmusb_inst *rt = (bmusb_inst *) data;

	if (rt) {
		if (rt->initialized) {
			BMUSBCapture::stop_bm_thread();
			delete rt->capture;
		}

		bfree(rt);
	}
}

static void *bmusb_create(obs_data_t *settings, obs_source_t *source)
{
	struct bmusb_inst *rt = (bmusb_inst *) bzalloc(sizeof(struct bmusb_inst));
	rt->source = source;

	rt->capture = new BMUSBCapture(0); // @TODO select card
	rt->capture->set_pixel_format(bmusb::PixelFormat_8BitYCbCr);
	rt->capture->set_frame_callback(
		[rt](uint16_t timecode,
			FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
			FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format
		) {
			printf("got fraem %d\n", timecode);
			UNUSED_PARAMETER(video_format);
			UNUSED_PARAMETER(audio_format);
			rt->capture->get_video_frame_allocator()->release_frame(video_frame);
			rt->capture->get_audio_frame_allocator()->release_frame(audio_frame);
		}
	);
	rt->capture->configure_card();
	BMUSBCapture::start_bm_thread();
	rt->capture->start_bm_capture();

	rt->initialized = true;

	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(source);
	return rt;
}

struct obs_source_info bmusb_source_info = {
	.id           = "bmusb",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name     = bmusb_getname,
	.create       = bmusb_create,
	.destroy      = bmusb_destroy,
};

OBS_DECLARE_MODULE()

extern "C" bool obs_module_load(void)
{
	obs_register_source(&bmusb_source_info);
	return true;
}
