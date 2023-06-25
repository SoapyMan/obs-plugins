/*
Copyright (C) 2019-present, Facebook, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <obs-module.h>
#include <obs-source.h>
#include <util/platform.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdint.h>

#include <filesystem>
#include <string>
#include <sstream>
#include <mutex>

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
#else
/* Assume that any non-Windows platform uses POSIX-style sockets instead. */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>  /* Needed for getaddrinfo() and freeaddrinfo() */
#include <unistd.h> /* Needed for close() */
typedef int SOCKET;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif // #ifdef _WIN32

#pragma warning(push)
#pragma warning(disable : 4244)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
}

#pragma warning(pop)

#include "oculus-mrc.h"
#include "frame.h"
#include "log.h"

#define TEXT_MODULE_NAME		obs_module_text("OculusMrcSource")
#define TEXT_CONNECT			obs_module_text("Connect to Quest")
#define TEXT_DISCONNECT			obs_module_text("Disconnect from Quest")
#define TEXT_IP_ADDRESS			obs_module_text("Quest IP Address")
#define TEXT_IP_PORT			obs_module_text("Port")
#define TEXT_STREAM_OVER_USB	obs_module_text("Stream over USB (ADB required)")
#define TEXT_HW_DECODE			obs_module_text("Hardware Decode")

#define OM_DEFAULT_IP_ADDRESS		"192.168.0.1"
#define OM_DEFAULT_PORT				28734

#define OM_DEFAULT_WIDTH			(1920 * 2)
#define OM_DEFAULT_HEIGHT			1080
#define OM_DEFAULT_USBMODE			false
#define OM_DEFAULT_HWDECODE			true

static int closeSocket(SOCKET sock)
{
	int status = 0;
#ifdef _WIN32
	status = closesocket(sock);
	if (status != 0) {
		status = WSAGetLastError();
	}
#else
	status = shutdown(sock, SHUT_RDWR);
	if (status == 0) {
		status = close(sock);
	}
#endif
	return status;
}

static bool isSocketInvalid(SOCKET socket)
{
#if _WIN32
	return socket == INVALID_SOCKET;
#else
	return socket < 0;
#endif
}

static bool isHwDevice(const AVCodec* c, enum AVHWDeviceType type, enum AVPixelFormat* hw_format)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig* config = avcodec_get_hw_config(c, i);
		if (!config) {
			break;
		}

		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
			config->device_type == type) {
			*hw_format = config->pix_fmt;
			return true;
		}
	}

	return false;
}

static std::string GetAvErrorString(int errNum)
{
	char buf[1024];
	return av_make_error_string(buf, 1024, errNum);
}

class OculusMrcSource {
public:
	// OBS source interfaces

	static const char *GetName(void *)
	{
		return TEXT_MODULE_NAME;
	}

	static void Show(void* data)
	{
		OculusMrcSource* context = (OculusMrcSource*)data;
		context->Connect();
	}

	static void Hide(void* data)
	{
		OculusMrcSource* context = (OculusMrcSource*)data;
		context->Disconnect();
	}

	static void Update(void *data, obs_data_t *settings)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		context->Update(settings);
	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		OculusMrcSource *context = new OculusMrcSource(source);
		Update(context, settings);
		return context;
	}

	static void Destroy(void *data) 
	{
		delete (OculusMrcSource *)data;
	}

	static obs_properties_t *GetProperties(void *source)
	{
		OculusMrcSource* context = (OculusMrcSource *)source;

		obs_properties_t* props = obs_properties_create();

		const char* activateText = TEXT_CONNECT;
		if (context->IsConnected()) {
			activateText = TEXT_DISCONNECT;
		}

		obs_properties_add_button(props, "activate", activateText, [](obs_properties_t* props, obs_property_t* p, void* data) {
				OculusMrcSource* context = (OculusMrcSource*)data;

				if (context->IsConnected()) {
					obs_property_set_description(p, TEXT_CONNECT);
					context->DisconnectClicked(props);
				} else {
					obs_property_set_description(p, TEXT_DISCONNECT);
					context->ConnectClicked(props);
				}
				return true;
			});

		obs_properties_add_text(props, "ipaddr", TEXT_IP_ADDRESS, OBS_TEXT_DEFAULT);
		obs_properties_add_int(props, "port", TEXT_IP_PORT, 1025, 65535, 1);
		obs_properties_add_bool(props, "usb_mode", TEXT_STREAM_OVER_USB);
		obs_properties_add_bool(props, "hw_decode", TEXT_HW_DECODE);

		context->RefreshButtons(props);

		return props;
	}

	static void VideoTick(void *data, float seconds)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		context->VideoTick(seconds);
	}

	static void VideoRender(void *data, gs_effect_t *effect)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		context->VideoRender(effect);
	}

	static uint32_t GetWidth(void *data)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		return context->GetWidth();
	}

	static uint32_t GetHeight(void *data)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		return context->GetHeight();
	}

	static void GetDefaults(obs_data_t *settings)
	{
		obs_data_set_default_int(settings, "width", OM_DEFAULT_WIDTH);
		obs_data_set_default_int(settings, "height", OM_DEFAULT_HEIGHT);
		obs_data_set_default_string(settings, "ipaddr", OM_DEFAULT_IP_ADDRESS);
		obs_data_set_default_int(settings, "port", OM_DEFAULT_PORT);
		obs_data_set_default_bool(settings, "usb_mode", OM_DEFAULT_USBMODE);
		obs_data_set_default_bool(settings, "hw_decode", OM_DEFAULT_HWDECODE);
	}

	void VideoTick(float /*seconds*/)
	{
		VideoTickImpl();
	}

	void VideoRender(gs_effect_t * /*effect*/)
	{
		VideoRenderImpl();
	}

	void RefreshButtons(obs_properties_t *props)
	{
		bool usbModeAllowed = true;
		std::string adb_cmd = "adb version";
#ifdef _WIN32
		if (system(adb_cmd.c_str()) == 1) {
			usbModeAllowed = false;
			OM_LOG(LOG_ERROR, "ADB is missing from your system's PATH.");
		}
#endif

		obs_property_set_enabled(obs_properties_get(props, "ipaddr"), isSocketInvalid(m_connectSocket));
		obs_property_set_enabled(obs_properties_get(props, "port"), isSocketInvalid(m_connectSocket));
		obs_property_set_enabled(obs_properties_get(props, "usb_mode"), usbModeAllowed && isSocketInvalid(m_connectSocket));
		obs_property_set_enabled(obs_properties_get(props, "hw_decode"), isSocketInvalid(m_connectSocket));
	}

	bool IsConnected() const
	{
		return !isSocketInvalid(m_connectSocket);
	}

	bool ConnectClicked(obs_properties_t *props)
	{
		OM_BLOG(LOG_INFO, "ConnectClicked");

		Connect();
		RefreshButtons(props);

		return true;
	}

	bool DisconnectClicked(obs_properties_t *props)
	{
		OM_BLOG(LOG_INFO, "DisconnectClicked");

		Disconnect();
		RefreshButtons(props);

		return true;
	}

private:
	OculusMrcSource(obs_source_t *source)
		: m_src(source)
	{
		m_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!m_codec) {
			OM_BLOG(LOG_ERROR, "Unable to find decoder");
		} else {
			OM_BLOG(LOG_INFO, "Codec found. Capabilities 0x%x", m_codec->capabilities);
		}

		obs_enter_graphics();
		char *filename = obs_module_file("oculusmrc.effect");
		m_mrc_effect = gs_effect_create_from_file(filename, NULL);
		bfree(filename);
		assert(m_mrc_effect);
		obs_leave_graphics();
	}

	~OculusMrcSource()
	{
		StopDecoder();
		obs_enter_graphics();
		if (m_mrc_effect) {
			gs_effect_destroy(m_mrc_effect);
			m_mrc_effect = nullptr;
		}
		if (m_temp_texture) {
			gs_texture_destroy(m_temp_texture);
			m_temp_texture = nullptr;
		}
		obs_leave_graphics();
	}

	void StartDecoder()
	{
		if (m_codecContext != nullptr) {
			OM_BLOG(LOG_ERROR, "Decoder already started");
			return;
		}

		if (!m_codec) {
			OM_BLOG(LOG_ERROR, "m_codec not initalized");
			return;
		}

		m_codecContext = avcodec_alloc_context3(m_codec);
		if (!m_codecContext) {
			OM_BLOG(LOG_ERROR, "Unable to create codec context");
			return;
		}

		// Hardware acceleration for decoding
		if(m_hw_decode)
		{
			enum AVHWDeviceType hw_priority[] = {
				AV_HWDEVICE_TYPE_D3D11VA,      AV_HWDEVICE_TYPE_DXVA2,
				AV_HWDEVICE_TYPE_CUDA,         AV_HWDEVICE_TYPE_VAAPI,
				AV_HWDEVICE_TYPE_VDPAU,        AV_HWDEVICE_TYPE_QSV,
				AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_NONE,
			};

			enum AVHWDeviceType* priority = hw_priority;

			while (*priority != AV_HWDEVICE_TYPE_NONE) {
				if (isHwDevice(m_codec, *priority, &m_codecHwPixFmt)) {
					const int ret = av_hwdevice_ctx_create(&m_codecHwCtx, *priority,	nullptr, nullptr, 0);
					if (ret == 0)
						break;
				}
				priority++;
			}

			if (m_codecHwCtx) {
				OM_BLOG(LOG_INFO, "Using HW device context");
				m_codecContext->hw_device_ctx = av_buffer_ref(m_codecHwCtx);
			} else {
				OM_BLOG(LOG_ERROR, "No HW device support, using software decoder");
			}
		}

		AVDictionary *dict = nullptr;
		int ret = avcodec_open2(m_codecContext, m_codec, &dict);
		av_dict_free(&dict);
		if (ret < 0) {
			OM_BLOG(LOG_ERROR, "Unable to open codec context");
			avcodec_free_context(&m_codecContext);
			return;
		}

		m_avPacket = av_packet_alloc();
		m_avPicture = av_frame_alloc();
		m_avPictureHw = av_frame_alloc();

		OM_BLOG(LOG_INFO, "m_codecContext constructed and opened");
	}

	void StopDecoder()
	{
		if (m_swsContext) {
			sws_freeContext(m_swsContext);
			m_swsContext = nullptr;
		}

		av_frame_free(&m_avPicture);
		av_frame_free(&m_avPictureHw);
		av_packet_free(&m_avPacket);

		m_codecHwPixFmt = AV_PIX_FMT_NONE;

		if (m_codecContext) {
			avcodec_close(m_codecContext);
			avcodec_free_context(&m_codecContext);
			OM_BLOG(LOG_INFO, "m_codecContext freed");
		}

		if (m_codecHwCtx) {
			av_buffer_unref(&m_codecHwCtx);
		}

		if (m_temp_texture) {
			obs_enter_graphics();
			gs_texture_destroy(m_temp_texture);
			obs_leave_graphics();
			m_temp_texture = nullptr;
		}
	}

	// settings
	uint32_t m_width = OM_DEFAULT_WIDTH;
	uint32_t m_height = OM_DEFAULT_HEIGHT;
	uint32_t m_audioSampleRate = 0;
	std::string m_ipaddr = OM_DEFAULT_IP_ADDRESS;
	uint32_t m_port = OM_DEFAULT_PORT;
	bool m_usbMode = OM_DEFAULT_USBMODE;
	bool m_hw_decode = OM_DEFAULT_HWDECODE;

	std::mutex m_updateMutex;

	obs_source_t* m_src = nullptr;
	gs_texture_t* m_temp_texture = nullptr;
	gs_effect_t* m_mrc_effect = nullptr;

	AVPacket* m_avPacket = nullptr;
	AVFrame* m_avPicture = nullptr;
	AVFrame* m_avPictureHw = nullptr;
	
	const AVCodec* m_codec = nullptr;
	AVCodecContext* m_codecContext = nullptr;
	AVBufferRef* m_codecHwCtx = nullptr;
	AVPixelFormat m_codecHwPixFmt = AV_PIX_FMT_NONE;

	SOCKET m_connectSocket = INVALID_SOCKET;
	FrameCollection m_frameCollection;

	SwsContext *m_swsContext = nullptr;
	int m_swsContext_SrcWidth = 0;
	int m_swsContext_SrcHeight = 0;
	AVPixelFormat m_swsContext_SrcPixelFormat = AV_PIX_FMT_NONE;
	int m_swsContext_DestWidth = 0;
	int m_swsContext_DestHeight = 0;

	uint8_t* m_codecScaleData = nullptr;

	int m_audioFrameIndex = 0;
	int m_videoFrameIndex = 0;
	int m_processedAudioFrameIndex = 0;
	int m_processedVideoFrameIndex = 0;

	void Update(obs_data_t *settings)
	{
		m_usbMode = obs_data_get_bool(settings, "usb_mode");
		m_width = (uint32_t)obs_data_get_int(settings, "width");
		m_height = (uint32_t)obs_data_get_int(settings, "height");
		m_ipaddr = m_usbMode ? "127.0.0.1" : obs_data_get_string(settings, "ipaddr");
		m_port = (uint32_t)obs_data_get_int(settings, "port");
		m_hw_decode = obs_data_get_bool(settings, "hw_decode");
	}

	uint32_t GetWidth() { return m_width; }
	uint32_t GetHeight() { return m_height; }

	void ReceiveData()
	{
		for (;;) {
			fd_set socketSet = {0};
			FD_ZERO(&socketSet);
			FD_SET(m_connectSocket, &socketSet);

			timeval t = {0, 0};
			int num = select(0, &socketSet, nullptr, nullptr, &t);
			if (num >= 1) {

				static const int bufferSize = 65536;
				uint8_t buf[bufferSize];

				const int iResult = recv(m_connectSocket, (char *)buf, bufferSize, 0);
				if (iResult < 0) {
					OM_BLOG(LOG_ERROR, "recv error %d, closing socket", iResult);
					Disconnect();
				} else if (iResult == 0) {
					OM_BLOG(LOG_INFO, "recv 0 bytes, closing socket");
					Disconnect();
				} else {
					//OM_BLOG(LOG_INFO, "recv: %d bytes received", iResult);
					m_frameCollection.AddData(buf, iResult);
				}
			} else {
				break;
			}
		}
	}

	void VideoTickImpl()
	{
		if (!IsConnected()){
			return;
		}

		ReceiveData();

		std::lock_guard<std::mutex> lock(m_updateMutex);

		while (m_frameCollection.HasCompletedFrame()) {
			auto frame = m_frameCollection.PopFrame();

			if (frame->m_type == PayloadType::VIDEO_DIMENSION) {
				const FrameDimension *dim = (const FrameDimension *)frame->m_payload.data();
				m_width = dim->w;
				m_height = dim->h;

				OM_BLOG(LOG_INFO, "[VIDEO_DIMENSION] width %d height %d", m_width, m_height);
			} else if (frame->m_type == PayloadType::VIDEO_DATA) {
				++m_videoFrameIndex;

				av_new_packet(m_avPacket, (int)frame->m_payload.size());
				assert(m_avPacket->data);
				memcpy(m_avPacket->data, frame->m_payload.data(), frame->m_payload.size());

				int ret = avcodec_send_packet(m_codecContext, m_avPacket);
				if (ret < 0) {
					OM_BLOG(LOG_ERROR, "avcodec_send_packet error %s", GetAvErrorString(ret).c_str());
					continue;
				} 

				AVFrame* receivePicture = (m_codecHwPixFmt != AV_PIX_FMT_NONE) ? m_avPictureHw : m_avPicture;
				ret = avcodec_receive_frame(m_codecContext, receivePicture);
				if (ret < 0) {
					OM_BLOG(LOG_ERROR, "avcodec_receive_frame error %s", GetAvErrorString(ret).c_str());
					continue;
				}
#if _DEBUG
				std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
				OM_BLOG(LOG_DEBUG, "[%d][VIDEO_DATA] size %d width %d height %d format %d",
					timePassed, m_avPacket->size, receivePicture->width, receivePicture->height, receivePicture->format);
#endif
				AVFrame* usePicture = m_avPicture;
				if (m_hw_decode && receivePicture->format == m_codecHwPixFmt) {
					// retrieve data from GPU to CPU
					ret = av_hwframe_transfer_data(m_avPicture, m_avPictureHw, 0);

					if (ret != 0) {
						OM_BLOG(LOG_ERROR, "av_hwframe_transfer_data error %s", GetAvErrorString(ret).c_str());
						continue;
					}
					m_avPicture->color_range = m_avPictureHw->color_range;
					m_avPicture->color_primaries = m_avPictureHw->color_primaries;
					m_avPicture->color_trc = m_avPictureHw->color_trc;
					m_avPicture->colorspace = m_avPictureHw->colorspace;
				} else {
					usePicture = receivePicture;
				}

				if (m_swsContext != nullptr) {
					if (m_swsContext_SrcWidth != m_codecContext->width ||  m_swsContext_SrcHeight != m_codecContext->height ||
						m_swsContext_SrcPixelFormat != m_codecContext->pix_fmt ||
						m_swsContext_DestWidth != m_codecContext->width || m_swsContext_DestHeight !=  m_codecContext->height) {

						OM_BLOG(LOG_DEBUG, "Need recreate m_swsContext");
						sws_freeContext(m_swsContext);
						m_swsContext = nullptr;
						delete[] m_codecScaleData;
						m_codecScaleData = nullptr;

						obs_enter_graphics();
						if (m_temp_texture) {
							gs_texture_destroy(m_temp_texture);
							m_temp_texture = nullptr;
						}
						obs_leave_graphics();
					}
				}

				if (m_swsContext == nullptr) {
					m_swsContext = sws_getContext(
						m_codecContext->width, m_codecContext->height,
						m_codecContext->pix_fmt,
						m_codecContext->width, m_codecContext->height,
						AV_PIX_FMT_RGBA,
						SWS_POINT,
						nullptr,
						nullptr,
						nullptr);

					m_swsContext_SrcWidth = m_codecContext->width;
					m_swsContext_SrcHeight = m_codecContext->height;
					m_swsContext_SrcPixelFormat = m_codecContext->pix_fmt;
					m_swsContext_DestWidth = m_codecContext->width;
					m_swsContext_DestHeight = m_codecContext->height;
					OM_BLOG(LOG_DEBUG, "sws_getContext(%d, %d, %d)", m_codecContext->width, m_codecContext->height, m_codecContext->pix_fmt);

					m_codecScaleData = new uint8_t[m_codecContext->width * m_codecContext->height * 4];
				}

				assert(m_swsContext);
				uint8_t* data[1] = {
					m_codecScaleData
				};
				int stride[1] = { 
					(int)m_codecContext->width * 4
				};
				sws_scale(m_swsContext, usePicture->data, usePicture->linesize, 0, usePicture->height, data, stride);

				obs_enter_graphics();
				if (m_temp_texture) {
					gs_texture_destroy(m_temp_texture);
					m_temp_texture = nullptr;
				}
				m_temp_texture = gs_texture_create(m_codecContext->width, m_codecContext->height, GS_RGBA, 1, const_cast<const uint8_t**>(&data[0]), 0);
				obs_leave_graphics();

				m_processedVideoFrameIndex = m_videoFrameIndex;
#if _DEBUG
				OM_BLOG(LOG_DEBUG, "[VIDEO_DATA] frameTS %lld VFI=%d AFI=%d", frame->m_timeStamp, m_audioFrameIndex, m_videoFrameIndex);
#endif
			} else if (frame->m_type == PayloadType::AUDIO_SAMPLERATE) {
				m_audioSampleRate = *(uint32_t *)(frame->m_payload.data());
				OM_BLOG(LOG_DEBUG, "[AUDIO_SAMPLERATE] %d", m_audioSampleRate);
			} else if (frame->m_type == PayloadType::AUDIO_DATA) {
				++m_audioFrameIndex;

				// NOTE: audioDataHeader timestamp is ZERO, as well as AVPacket timestamp...
				//		 looks like it is broken and there's no way to properly sync
				//		 audio and video at this point
				
				const AudioDataHeader* audioDataHeader = (AudioDataHeader*)(frame->m_payload.data());
				if (m_videoFrameIndex > 0 && m_audioSampleRate != 0) {
						if(audioDataHeader->channels == 1 || audioDataHeader->channels == 2) {
						const uint8_t* audioData = (uint8_t*)frame->m_payload.data() + sizeof(AudioDataHeader);
						const uint32_t numSamples = audioDataHeader->dataLength / sizeof(float) / audioDataHeader->channels;

						obs_source_audio audio = { 0 };
						audio.data[0] = (uint8_t*)audioData;
						audio.frames = numSamples;
						audio.speakers = audioDataHeader->channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
						audio.format = AUDIO_FORMAT_FLOAT;
						audio.samples_per_sec = m_audioSampleRate;
						audio.timestamp = frame->m_timeStamp - (300 * 1000000); // os_gettime_ns();
						obs_source_output_audio(m_src, &audio);

						m_processedAudioFrameIndex = m_audioFrameIndex;
#if _DEBUG
						OM_BLOG(LOG_DEBUG, "[AUDIO_DATA] frameTS %lld AFI=%d VFI=%d", frame->m_timeStamp, m_audioFrameIndex, m_videoFrameIndex);
#endif
					} else {
						OM_BLOG(LOG_ERROR, "[AUDIO_DATA] unimplemented audio channels %d", audioDataHeader->channels);
					}

				}
#if _DEBUG
				std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
				OM_BLOG(LOG_DEBUG, "[%f][AUDIO_DATA] timestamp %llu", timePassed.count());
#endif
			} 
			else if (frame->m_type == PayloadType::CAPTURE_CONTROL_DATA) {
				OM_BLOG(LOG_DEBUG, "[CAPTURE_CONTROL_DATA] Got CAPTURE_CONTROL_DATA");
			} else if (frame->m_type == PayloadType::DATA_VERSION) {
				OM_BLOG(LOG_DEBUG, "[DATA_VERSION] Got DATA_VERSION");
			} else {
#if _DEBUG
				std::stringstream tempStr;
				unsigned char* data = (unsigned char*)frame->m_payload.data();
				for (unsigned long i = 0; i < frame->m_payload.size(); i++) {
					tempStr << std::hex << (int)data[i];
				}
				OM_BLOG(LOG_DEBUG, "Unknown payload type: %u, size %lld, bytes %s", frame->m_type, frame->m_payload.size(), tempStr.str().c_str());
#endif
			}
		}
	}

	void VideoRenderImpl()
	{
#if _DEBUG
		if (!isSocketInvalid(m_connectSocket) && m_frameCollection.HasFirstFrame()) {
			std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
			OM_BLOG(LOG_DEBUG, "[%f] VideoRenderImpl", timePassed.count());
		}
#endif

		if (IsConnected() && m_temp_texture) {
			gs_technique_t *tech = gs_effect_get_technique(m_mrc_effect, "Frame");

			gs_technique_begin(tech);
			gs_technique_begin_pass(tech, 0);

			obs_source_draw(m_temp_texture, 0, 0, m_width, m_height, true);

			gs_technique_end_pass(tech);
			gs_technique_end(tech);
		} else {
			gs_technique_t *tech = gs_effect_get_technique(m_mrc_effect, "Empty");

			gs_technique_begin(tech);
			gs_technique_begin_pass(tech, 0);

			gs_draw_sprite(0, 0, m_width, m_height);

			gs_technique_end_pass(tech);
			gs_technique_end(tech);
		}
	}

	void Connect()
	{
		std::lock_guard<std::mutex> lock(m_updateMutex);

		if (m_usbMode) {
			m_ipaddr = "127.0.0.1";
			std::string adb_forwardcmd = string_format("adb forward tcp:%d tcp:%d", m_port, m_port);
#ifdef _WIN32
			system(adb_forwardcmd.c_str());
#endif
		}

		if (!isSocketInvalid(m_connectSocket)) {
			OM_BLOG(LOG_ERROR, "Already connected");
			return;
		}

		struct addrinfo *result = NULL;
		struct addrinfo *ptr = NULL;
		struct addrinfo hints = {0};

		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		int iResult;
		iResult = getaddrinfo(m_ipaddr.c_str(), std::to_string(m_port).c_str(), &hints,  &result);
		if (iResult != 0) {
			OM_BLOG(LOG_ERROR, "getaddrinfo failed: %d", iResult);
			return;
		}

		ptr = result;
		m_connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (isSocketInvalid(m_connectSocket)) {
#if _WIN32
			OM_BLOG(LOG_ERROR, "Error at socket(): %d", WSAGetLastError());
#else
			OM_BLOG(LOG_ERROR, "Error at socket(): %d", m_connectSocket);
#endif
			freeaddrinfo(result);
		}

		if (!isSocketInvalid(m_connectSocket)) {
			// put socked in non-blocking mode...
#ifdef _WIN32
			u_long block = 1;
			if (ioctlsocket(m_connectSocket, FIONBIO, &block) == SOCKET_ERROR) {
#else
			int socketFlags = fcntl(m_connectSocket, F_GETFL);
			if (fcntl(m_connectSocket, F_SETFL, socketFlags | O_NONBLOCK) == -1) {
#endif
				OM_BLOG(LOG_ERROR, "Unable to put socket to unblocked mode");
			}

			iResult = connect(m_connectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
			bool hasError = true;
			if (iResult == SOCKET_ERROR) {
#ifdef _WIN32
				if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
				if (errno == EWOULDBLOCK) {
#endif
					fd_set setW, setE;

					FD_ZERO(&setW);
					FD_SET(m_connectSocket, &setW);
					FD_ZERO(&setE);
					FD_SET(m_connectSocket, &setE);

					timeval time_out = {0};
					time_out.tv_sec = 2;
					time_out.tv_usec = 0;

					int ret = select(0, NULL, &setW, &setE, &time_out);
					if (ret > 0 && !FD_ISSET(m_connectSocket, &setE)) {
						hasError = false;
					}
				}
			}
			if (hasError) {
				OM_BLOG(LOG_ERROR, "Unable to connect (usb mode %d)", m_usbMode);

				closeSocket(m_connectSocket);
				m_connectSocket = INVALID_SOCKET;
			} else {
				OM_BLOG(LOG_INFO, "Socket connected to %s:%d", m_ipaddr.c_str(), m_port);
				block = 0;
#ifdef _WIN32
				if (ioctlsocket(m_connectSocket, FIONBIO, &block) == SOCKET_ERROR) {
#else
				if (fcntl(m_connectSocket, F_SETFL, socketFlags) == -1) {
#endif
					OM_BLOG(LOG_ERROR, "Unable to put socket to blocked mode");
				}
			}
		}

		freeaddrinfo(result);

		delete[] m_codecScaleData;
		m_codecScaleData = nullptr;

		m_frameCollection.Reset();

		m_audioFrameIndex = 0;
		m_videoFrameIndex = 0;
		m_processedAudioFrameIndex = 0;
		m_processedVideoFrameIndex = 0;
		m_audioSampleRate = 0;

		if (!isSocketInvalid(m_connectSocket)) {
			StartDecoder();
		}
	}

	void Disconnect()
	{
		std::lock_guard<std::mutex> lock(m_updateMutex);

		if (isSocketInvalid(m_connectSocket)) {
			OM_BLOG(LOG_ERROR, "Not connected");
			return;
		}

		if (m_usbMode) {
			std::string adb_forwardcmd = string_format("adb forward --remove tcp:%d", m_port);
#ifdef _WIN32
			system(adb_forwardcmd.c_str());
#endif
		}

		OM_BLOG(LOG_INFO, "Closing socket...");
		int ret = closeSocket(m_connectSocket);
		if (ret != 0) {
			OM_BLOG(LOG_ERROR, "closeSocket error %d", ret);
		}
		m_connectSocket = INVALID_SOCKET;
		OM_BLOG(LOG_INFO, "Socket disconnected");

		StopDecoder();
	}
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("oculus-mrc", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Oculus MRC source";
}

bool obs_module_load(void)
{
	// avcodec_register_all();

#ifdef _WIN32
	// Initialize Winsock
	WSADATA wsaData = {0};
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		OM_LOG(LOG_ERROR, "WSAStartup failed: %d\n", iResult);
		return false;
	}
#endif

	struct obs_source_info oculus_mrc_source_info = {0};
	oculus_mrc_source_info.id = "oculus_mrc_source";
	oculus_mrc_source_info.type = OBS_SOURCE_TYPE_INPUT;
	oculus_mrc_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	oculus_mrc_source_info.create = &OculusMrcSource::Create;
	oculus_mrc_source_info.destroy = &OculusMrcSource::Destroy;
	oculus_mrc_source_info.show = &OculusMrcSource::Show;
	oculus_mrc_source_info.hide = &OculusMrcSource::Hide;
	oculus_mrc_source_info.update = &OculusMrcSource::Update;
	oculus_mrc_source_info.get_name = &OculusMrcSource::GetName;
	oculus_mrc_source_info.get_defaults = &OculusMrcSource::GetDefaults;
	oculus_mrc_source_info.get_width = &OculusMrcSource::GetWidth;
	oculus_mrc_source_info.get_height = &OculusMrcSource::GetHeight;
	oculus_mrc_source_info.video_tick = &OculusMrcSource::VideoTick;
	oculus_mrc_source_info.video_render = &OculusMrcSource::VideoRender;
	oculus_mrc_source_info.get_properties = &OculusMrcSource::GetProperties;
	oculus_mrc_source_info.icon_type = OBS_ICON_TYPE_CAMERA;

	obs_register_source(&oculus_mrc_source_info);
	return true;
}

void obs_module_unload(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}
