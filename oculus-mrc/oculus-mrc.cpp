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

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdint.h>

#include <filesystem>
#include <string>
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

#define OM_DEFAULT_WIDTH (1920 * 2)
#define OM_DEFAULT_HEIGHT 1080
#define OM_DEFAULT_AUDIO_SAMPLERATE 48000
#define OM_DEFAULT_IP_ADDRESS "192.168.0.1"
#define OM_DEFAULT_PORT 28734
#define OM_DEFAULT_USEADB false

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
		return obs_module_text("OculusMrcSource");
	}

	std::string GetAdbPath()
	{
		char *appdatap = std::getenv("APPDATA");
		std::filesystem::path sqpath(appdatap);
		sqpath /= "SideQuest/platform-tools/";
		if (std::filesystem::is_directory(sqpath)) {
			return sqpath.string().c_str();
		} else {
			return "";
		}
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

	static void Destroy(void *data) { delete (OculusMrcSource *)data; }

	static obs_properties_t *GetProperties(void *source)
	{
		OculusMrcSource *context = (OculusMrcSource *)source;

		obs_properties_t *props = obs_properties_create();

		obs_properties_add_text(props, "ipaddr", obs_module_text("Quest IP Address"), OBS_TEXT_DEFAULT);
		obs_properties_add_int(props, "port", obs_module_text("Port"), 1025, 65535, 1);
		obs_properties_add_bool(props, "use_adb", obs_module_text("Stream over USB (ADB required)"));

		obs_property_t *connectButton = obs_properties_add_button(props, "connect",
			obs_module_text("Connect to MRC-enabled game running on Quest"),
			[](obs_properties_t *props, obs_property_t *property, void *data) {
				return ((OculusMrcSource *)data)->ConnectClicked(props, property);
			});
		obs_property_set_enabled( connectButton, isSocketInvalid(context->m_connectSocket));

		obs_property_t *disconnectButton = obs_properties_add_button(props, "disconnect",
			obs_module_text("Disconnect from Quest game"),
			[](obs_properties_t *props, obs_property_t *property, void *data) {
				return ((OculusMrcSource *)data)->DisconnectClicked(props, property);
			});
		obs_property_set_enabled( disconnectButton, !isSocketInvalid(context->m_connectSocket));

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
		obs_data_set_default_bool(settings, "use_adb", OM_DEFAULT_USEADB);
	}

	void VideoTick(float /*seconds*/)
	{
		std::lock_guard<std::mutex> lock(m_updateMutex);
		VideoTickImpl();
	}

	void VideoRender(gs_effect_t * /*effect*/)
	{
		//std::lock_guard<std::mutex> lock(m_updateMutex);
		VideoRenderImpl();
	}

	void RefreshButtons(obs_properties_t *props)
	{
		obs_property_set_enabled(obs_properties_get(props, "connect"), isSocketInvalid(m_connectSocket));
		obs_property_set_enabled(obs_properties_get(props, "use_adb"), isSocketInvalid(m_connectSocket));
		obs_property_set_enabled(obs_properties_get(props, "disconnect"), !isSocketInvalid(m_connectSocket));
	}

	bool ConnectClicked(obs_properties_t *props,  obs_property_t * /*property*/)
	{
		OM_BLOG(LOG_INFO, "ConnectClicked");

		std::lock_guard<std::mutex> lock(m_updateMutex);
		Connect();
		RefreshButtons(props);

		return true;
	}

	bool DisconnectClicked(obs_properties_t *props, obs_property_t * /*property*/)
	{
		OM_BLOG(LOG_INFO, "DisconnectClicked");

		std::lock_guard<std::mutex> lock(m_updateMutex);
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

		std::string adb_cmd = GetAdbPath() + "adb version";
		if (system(adb_cmd.c_str()) == 1) {
			OM_BLOG(LOG_ERROR, "ADB is missing from your system's PATH.");
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
	uint32_t m_audioSampleRate = OM_DEFAULT_AUDIO_SAMPLERATE;
	std::string m_ipaddr = OM_DEFAULT_IP_ADDRESS;
	uint32_t m_port = OM_DEFAULT_PORT;
	bool m_usbMode = OM_DEFAULT_USEADB;

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

	std::list<std::pair<int, std::shared_ptr<Frame>>> m_cachedAudioFrames;
	int m_audioFrameIndex = 0;
	int m_videoFrameIndex = 0;

	void Update(obs_data_t *settings)
	{
		m_usbMode = obs_data_get_bool(settings, "use_adb");
		m_width = (uint32_t)obs_data_get_int(settings, "width");
		m_height = (uint32_t)obs_data_get_int(settings, "height");
		m_ipaddr = m_usbMode ? "127.0.0.1" : obs_data_get_string(settings, "ipaddr");
		m_port = (uint32_t)obs_data_get_int(settings, "port");
	}

	uint32_t GetWidth() { return m_width; }

	uint32_t GetHeight() { return m_height; }

	void ReceiveData()
	{
		if (isSocketInvalid(m_connectSocket)) {
			return;
		}

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

	void ProcessAudioFrames()
	{
		while (m_cachedAudioFrames.size() > 0 && m_cachedAudioFrames.front().first <= m_videoFrameIndex) {
			std::shared_ptr<Frame> audioFrame = m_cachedAudioFrames.front().second;
			m_cachedAudioFrames.pop_front();

			const AudioDataHeader* audioDataHeader = (AudioDataHeader*)(audioFrame->m_payload.data());
			
			if (audioDataHeader->channels == 1 || audioDataHeader->channels == 2) {
				obs_source_audio audio = { 0 };
				audio.data[0] = (uint8_t*)audioFrame->m_payload.data() + sizeof(AudioDataHeader);
				audio.frames = audioDataHeader->dataLength / sizeof(float) / audioDataHeader->channels;
				audio.speakers = audioDataHeader->channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
				audio.format = AUDIO_FORMAT_FLOAT;
				audio.samples_per_sec = m_audioSampleRate;
				audio.timestamp = audioDataHeader->timestamp;
				obs_source_output_audio(m_src, &audio);
			} else {
				OM_BLOG(LOG_ERROR, "[AUDIO_DATA] unimplemented audio channels %d", audioDataHeader->channels);
			}
		}
	}

	void VideoTickImpl()
	{
		if (isSocketInvalid(m_connectSocket))
			return;

		ReceiveData();

		if (isSocketInvalid(m_connectSocket)) // socket disconnected
			return;

		//std::chrono::time_point<std::chrono::system_clock> startTime = std::chrono::system_clock::now();
		while (m_frameCollection.HasCompletedFrame()) {
			//std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - startTime;
			//if (timePassed.count() > 0.05)
			//	break;

			auto frame = m_frameCollection.PopFrame();

			//auto current_time = std::chrono::system_clock::now();
			//auto seconds_since_epoch = std::chrono::duration<double>(current_time.time_since_epoch()).count();
			//double latency = seconds_since_epoch - frame->m_secondsSinceEpoch;

			if (frame->m_type == PayloadType::VIDEO_DIMENSION) {
				const FrameDimension *dim = (const FrameDimension *)frame->m_payload.data();
				m_width = dim->w;
				m_height = dim->h;

				OM_BLOG(LOG_INFO, "[VIDEO_DIMENSION] width %d height %d", m_width, m_height);
			} else if (frame->m_type == PayloadType::VIDEO_DATA) {
				av_new_packet(m_avPacket, (int)frame->m_payload.size());
				assert(m_avPacket->data);
				memcpy(m_avPacket->data, frame->m_payload.data(), frame->m_payload.size());

				int ret = avcodec_send_packet(m_codecContext, m_avPacket);
				if (ret < 0) {
					OM_BLOG(LOG_ERROR, "avcodec_send_packet error %s", GetAvErrorString(ret).c_str());
				} else {
					AVFrame* recievePicture = (m_codecHwPixFmt != AV_PIX_FMT_NONE) ? m_avPictureHw : m_avPicture;
					ret = avcodec_receive_frame(m_codecContext, recievePicture);
					if (ret < 0) {
						OM_BLOG(LOG_ERROR, "avcodec_receive_frame error %s", GetAvErrorString(ret).c_str());
					} else {
#if _DEBUG
						std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
						OM_BLOG(LOG_DEBUG, "[%f][VIDEO_DATA] size %d width %d height %d format %d",
							timePassed.count(), packet->size, picture->width, picture->height, picture->format);
#endif
						++m_videoFrameIndex;

						AVFrame* usePicture = m_avPicture;
						if (recievePicture->format == m_codecHwPixFmt) {
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
							usePicture = recievePicture;
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
					}
				}
			} else if (frame->m_type == PayloadType::AUDIO_SAMPLERATE) {
				m_audioSampleRate = *(uint32_t *)(frame->m_payload.data());
				OM_BLOG(LOG_DEBUG, "[AUDIO_SAMPLERATE] %d", m_audioSampleRate);
			} else if (frame->m_type == PayloadType::AUDIO_DATA) {
				m_cachedAudioFrames.push_back(std::make_pair(m_audioFrameIndex, frame));
				++m_audioFrameIndex;
#if _DEBUG
				std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
				OM_BLOG(LOG_DEBUG, "[%f][AUDIO_DATA] timestamp %llu", timePassed.count());
#endif
			} 
#if _DEBUG
			else if (frame->m_type == PayloadType::CAPTURE_CONTROL_DATA) {
				OM_BLOG(LOG_DEBUG, "[CAPTURE_CONTROL_DATA] Got CAPTURE_CONTROL_DATA");
			} else if (frame->m_type == PayloadType::DATA_VERSION) {
				OM_BLOG(LOG_DEBUG, "[DATA_VERSION] Got DATA_VERSION");
			} else {
				OM_BLOG(LOG_ERROR, "Unknown payload type: %u", frame->m_type);
			}
#endif
		}

		// extra processing of audio frames
		ProcessAudioFrames();
	}

	void VideoRenderImpl()
	{
#if _DEBUG
		if (!isSocketInvalid(m_connectSocket) && m_frameCollection.HasFirstFrame()) {
			std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
			OM_BLOG(LOG_DEBUG, "[%f] VideoRenderImpl", timePassed.count());
		}
#endif

		if (m_temp_texture) {
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
		if (m_usbMode) {
			m_ipaddr = "127.0.0.1";
			std::string adb_forwardcmd = string_format("adb forward tcp:%d tcp:%d", m_port, m_port);
			std::string adb_cmd = GetAdbPath() + adb_forwardcmd;
			system(adb_cmd.c_str());
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

					int ret = select(0, NULL, &setW, &setE,
							 &time_out);
					if (ret > 0 &&
					    !FD_ISSET(m_connectSocket, &setE)) {
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
		m_cachedAudioFrames.clear();

		m_audioFrameIndex = 0;
		m_videoFrameIndex = 0;

		if (!isSocketInvalid(m_connectSocket)) {
			StartDecoder();
		}
	}

	void Disconnect()
	{
		if (isSocketInvalid(m_connectSocket)) {
			OM_BLOG(LOG_ERROR, "Not connected");
			return;
		}

		if (m_usbMode) {
			std::string adb_forwardcmd = string_format("adb forward --remove tcp:%d", m_port);
			std::string adb_cmd = GetAdbPath() + adb_forwardcmd;
			system(adb_cmd.c_str());
		}

		StopDecoder();

		int ret = closeSocket(m_connectSocket);
		if (ret != 0) {
			OM_BLOG(LOG_ERROR, "closeSocket error %d", ret);
		}
		m_connectSocket = INVALID_SOCKET;
		OM_BLOG(LOG_INFO, "Socket disconnected");
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
	oculus_mrc_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO /* | OBS_SOURCE_CUSTOM_DRAW*/;
	oculus_mrc_source_info.create = &OculusMrcSource::Create;
	oculus_mrc_source_info.destroy = &OculusMrcSource::Destroy;
	oculus_mrc_source_info.update = &OculusMrcSource::Update;
	oculus_mrc_source_info.get_name = &OculusMrcSource::GetName;
	oculus_mrc_source_info.get_defaults = &OculusMrcSource::GetDefaults;
	oculus_mrc_source_info.get_width = &OculusMrcSource::GetWidth;
	oculus_mrc_source_info.get_height = &OculusMrcSource::GetHeight;
	oculus_mrc_source_info.video_tick = &OculusMrcSource::VideoTick;
	oculus_mrc_source_info.video_render = &OculusMrcSource::VideoRender;
	oculus_mrc_source_info.get_properties = &OculusMrcSource::GetProperties;

	obs_register_source(&oculus_mrc_source_info);
	return true;
}

void obs_module_unload(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}
