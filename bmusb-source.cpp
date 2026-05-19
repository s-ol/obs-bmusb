#include <stdlib.h>
#include <obs.h>
#include <util/platform.h>
#include <obs-module.h>
#include <bmusb/bmusb.h>
#include <iostream>

using bmusb::BMUSBCapture;
using bmusb::FrameAllocator;
using bmusb::VideoFormat;
using bmusb::AudioFormat;

struct bmusb_inst {
	obs_source_t            *source;
	BMUSBCapture            *capture;
	bool                    initialized;
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
			rt->capture->stop_dequeue_thread();
			delete rt->capture;
			BMUSBCapture::stop_bm_thread();
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
			uint64_t cur_time = os_gettime_ns();

			if (!video_format.has_signal) {
				std::cerr << "Video has no signal, dropping frame" << std::endl;
				rt->capture->get_video_frame_allocator()->release_frame(video_frame);
				rt->capture->get_audio_frame_allocator()->release_frame(audio_frame);
				return;
			}

			if (video_format.interlaced) {
				std::cerr << "Video is interlaced, dropping frame" << std::endl;
				rt->capture->get_video_frame_allocator()->release_frame(video_frame);
				rt->capture->get_audio_frame_allocator()->release_frame(audio_frame);
				return;
			}

			if (video_frame.interleaved) {
				std::cerr << "Video is interleaved, dropping frame" << std::endl;
				rt->capture->get_video_frame_allocator()->release_frame(video_frame);
				rt->capture->get_audio_frame_allocator()->release_frame(audio_frame);
				return;
			}

			// Output video frame
			if (video_frame.data != nullptr) {
				struct obs_source_frame frame{};
				frame.width = video_format.width;
				frame.height = video_format.height;
				frame.format = VIDEO_FORMAT_UYVY;
				frame.linesize[0] = video_format.stride;
				frame.data[0] = video_frame.data + video_offset + video_format.extra_lines_top * video_format.stride;
				frame.timestamp = cur_time;
				video_format_get_parameters_for_format(VIDEO_CS_SRGB, VIDEO_RANGE_FULL, frame.format, frame.color_matrix, frame.color_range_min, frame.color_range_max);
				obs_source_output_video(rt->source, &frame);
			}
			rt->capture->get_video_frame_allocator()->release_frame(video_frame);

			// Output audio frame if enabled
			if (audio_frame.data != nullptr) {
				struct obs_source_audio audio;
				audio.samples_per_sec = audio_format.sample_rate;
				audio.frames = (audio_frame.len > audio_offset) ?
					(audio_frame.len - audio_offset) / audio_format.num_channels /
					(audio_format.bits_per_sample / 8) : 0;
				audio.format = AUDIO_FORMAT_32BIT;
				audio.data[0] = audio_frame.data + audio_offset;
				audio.timestamp = cur_time;
				switch (audio_format.num_channels) {
					case 1: audio.speakers = SPEAKERS_MONO; break;
					case 2: audio.speakers = SPEAKERS_STEREO; break;
					case 3: audio.speakers = SPEAKERS_2POINT1; break;
					case 4: audio.speakers = SPEAKERS_4POINT0; break;
					case 5: audio.speakers = SPEAKERS_4POINT1; break;
					case 6: audio.speakers = SPEAKERS_5POINT1; break;
					case 8: audio.speakers = SPEAKERS_7POINT1; break;
					default: audio.speakers = SPEAKERS_UNKNOWN; break;
				}
				obs_source_output_audio(rt->source, &audio);
			}
			rt->capture->get_audio_frame_allocator()->release_frame(audio_frame);
		}
	);

	// Configure and start capture
	rt->capture->configure_card();
	BMUSBCapture::start_bm_thread();
	rt->capture->start_bm_capture();

	rt->initialized = true;

	return rt;
}

struct obs_source_info bmusb_source_info = {
	.id           = "bmusb",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
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
