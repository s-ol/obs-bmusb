#include <stdlib.h>
#include <obs.h>
#include <util/platform.h>
#include <obs-module.h>
#include <bmusb/bmusb.h>
#include <iostream>
#include <map>
#include <mutex>

using bmusb::BMUSBCapture;
using bmusb::FrameAllocator;
using bmusb::VideoFormat;
using bmusb::AudioFormat;

#define S_CARD_INDEX "card_index"
#define S_PIXEL_FORMAT "pixel_format"
#define S_VIDEO_INPUT "video_input"
#define S_AUDIO_INPUT "audio_input"

/* 
 * Workaround for bmusb library limitation:
 * The current bmusb library does not release libusb handles or interfaces in its destructor.
 * Re-opening a card after deleting its instance results in LIBUSB_ERROR_BUSY because the 
 * interface remains claimed by a leaked handle. To fix this without modifying bmusb, 
 * we manage capture instances as singletons per card_index and keep them alive globally.
 */
static std::map<int, BMUSBCapture *> global_instances;
static std::mutex instance_mutex;
static bool usb_thread_started = false;

static BMUSBCapture *get_capture_instance(int card_index)
{
	std::lock_guard<std::mutex> lock(instance_mutex);

	if (global_instances.count(card_index)) {
		return global_instances[card_index];
	}

	if (card_index < 0 || (unsigned int)card_index >= BMUSBCapture::num_cards()) {
		return nullptr;
	}

	BMUSBCapture *cap = new BMUSBCapture(card_index);
	cap->configure_card();
	if (!usb_thread_started) {
		BMUSBCapture::start_bm_thread();
		usb_thread_started = true;
	}
	cap->start_bm_capture();
	global_instances[card_index] = cap;
	return cap;
}

struct bmusb_inst {
	obs_source_t            *source;
	BMUSBCapture            *capture;
	int                     card_index;
};

static const char *bmusb_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Blackmagic USB3 source (bmusb)";
}

static void bmusb_cleanup(struct bmusb_inst *rt)
{
	if (rt->capture) {
		// Library leaks handles on delete, so we keep instances alive.
		// We set a "null" callback that just releases frames back to the allocator.
		auto cap = rt->capture;
		cap->set_frame_callback([cap](uint16_t, FrameAllocator::Frame v, size_t, VideoFormat,
					      FrameAllocator::Frame a, size_t, AudioFormat) {
			if (v.data) cap->get_video_frame_allocator()->release_frame(v);
			if (a.data) cap->get_audio_frame_allocator()->release_frame(a);
		});
		rt->capture = nullptr;
	}
}

static void bmusb_destroy(void *data)
{
	struct bmusb_inst *rt = (bmusb_inst *)data;

	if (rt) {
		bmusb_cleanup(rt);

		bfree(rt);
	}
}

static void bmusb_update(void *data, obs_data_t *settings)
{
	struct bmusb_inst *rt = (struct bmusb_inst *)data;
	int card_index = (int)obs_data_get_int(settings, S_CARD_INDEX);
	bmusb::PixelFormat pixel_format = (bmusb::PixelFormat)obs_data_get_int(settings, S_PIXEL_FORMAT);
	uint32_t video_input = obs_data_get_int(settings, S_VIDEO_INPUT);
	uint32_t audio_input = obs_data_get_int(settings, S_AUDIO_INPUT);

	if (rt->capture && rt->card_index == card_index) {
		if (pixel_format != rt->capture->get_current_pixel_format()) {
			rt->capture->set_pixel_format(pixel_format);
		}
		if (video_input != rt->capture->get_current_video_input()) {
			rt->capture->set_video_input(video_input);
		}
		if (audio_input != rt->capture->get_current_audio_input()) {
			rt->capture->set_audio_input(audio_input);
		}
		return;
	}

	bmusb_cleanup(rt);

	rt->card_index = card_index;
	rt->capture = get_capture_instance(card_index);
	if (!rt->capture) {
		obs_source_output_video(rt->source, nullptr);  // Signal no video
		return;
	}

	rt->capture->set_pixel_format(pixel_format);
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
				if (rt->capture->get_current_pixel_format() == bmusb::PixelFormat_10BitYCbCr) {
					frame.format = VIDEO_FORMAT_V210;
				} else {
					frame.format = VIDEO_FORMAT_UYVY;
				}
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
}

static void *bmusb_create(obs_data_t *settings, obs_source_t *source)
{
	struct bmusb_inst *rt = (bmusb_inst *)bzalloc(sizeof(struct bmusb_inst));
	rt->source = source;

	bmusb_update(rt, settings);

	return rt;
}

static void bmusb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_CARD_INDEX, 0);
	obs_data_set_default_int(settings, S_PIXEL_FORMAT, bmusb::PixelFormat_8BitYCbCr);
	obs_data_set_default_int(settings, S_VIDEO_INPUT, 0);
	obs_data_set_default_int(settings, S_AUDIO_INPUT, 0);
}

static obs_properties_t *bmusb_get_properties(void* data)
{
	struct bmusb_inst *rt = (struct bmusb_inst *)data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, S_CARD_INDEX, "Card Index", 0, 8, 1);

	obs_property_t *pix_fmt_list = obs_properties_add_list(props, S_PIXEL_FORMAT, "Pixel Format",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pix_fmt_list, "8-bit YCbCr (UYVY)", bmusb::PixelFormat_8BitYCbCr);
	obs_property_list_add_int(pix_fmt_list, "10-bit YCbCr (v210)", bmusb::PixelFormat_10BitYCbCr);

	obs_property_t *vid_inp_list = obs_properties_add_list(props, S_VIDEO_INPUT, "Video Input",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	if (rt && rt->capture) {
		for (const auto& inp : rt->capture->get_available_video_inputs()) {
			obs_property_list_add_int(vid_inp_list, inp.second.c_str(), inp.first);
		}
	}
	
	obs_property_t *aud_inp_list = obs_properties_add_list(props, S_AUDIO_INPUT, "Audio Input",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	if (rt && rt->capture) {
		for (const auto& inp : rt->capture->get_available_audio_inputs()) {
			obs_property_list_add_int(aud_inp_list, inp.second.c_str(), inp.first);
		}
	}

	return props;
}

struct obs_source_info bmusb_source_info = {
	.id           = "bmusb",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name     = bmusb_getname,
	.create       = bmusb_create,
	.destroy      = bmusb_destroy,
	.get_defaults = bmusb_get_defaults,
	.get_properties = bmusb_get_properties,
	.update       = bmusb_update,
	.icon_type    = OBS_ICON_TYPE_CAMERA,
};

OBS_DECLARE_MODULE()

extern "C" bool obs_module_load(void)
{
	obs_register_source(&bmusb_source_info);
	return true;
}

extern "C" void obs_module_unload(void)
{
	std::lock_guard<std::mutex> lock(instance_mutex);

	// Stop dequeue threads and delete all cached BMUSBCapture instances.
	for (auto const& [card_idx, cap] : global_instances) {
		if (cap) {
			cap->stop_dequeue_thread(); // Stop the individual dequeue thread
			delete cap; // Delete the BMUSBCapture object
		}
	}
	global_instances.clear();
	if (usb_thread_started) {
		BMUSBCapture::stop_bm_thread();
		usb_thread_started = false;
	}
}
