/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
#include "system.h"
#ifdef HAS_PULSEAUDIO
#include "AESinkPULSE.h"
#include "utils/log.h"
#include "Util.h"
#include "guilib/LocalizeStrings.h"

using namespace std;

static const char *ContextStateToString(pa_context_state s)
{
  switch (s)
  {
    case PA_CONTEXT_UNCONNECTED:
      return "unconnected";
    case PA_CONTEXT_CONNECTING:
      return "connecting";
    case PA_CONTEXT_AUTHORIZING:
      return "authorizing";
    case PA_CONTEXT_SETTING_NAME:
      return "setting name";
    case PA_CONTEXT_READY:
      return "ready";
    case PA_CONTEXT_FAILED:
      return "failed";
    case PA_CONTEXT_TERMINATED:
      return "terminated";
    default:
      return "none";
  }
}

static const char *StreamStateToString(pa_stream_state s)
{
  switch(s)
  {
    case PA_STREAM_UNCONNECTED:
      return "unconnected";
    case PA_STREAM_CREATING:
      return "creating";
    case PA_STREAM_READY:
      return "ready";
    case PA_STREAM_FAILED:
      return "failed";
    case PA_STREAM_TERMINATED:
      return "terminated";
    default:
      return "none";
  }
}

static pa_sample_format AEFormatToPulseFormat(AEDataFormat format)
{
  switch (format)
  {
    case AE_FMT_U8    : return PA_SAMPLE_U8;
    case AE_FMT_S16LE : return PA_SAMPLE_S16LE;
    case AE_FMT_S16BE : return PA_SAMPLE_S16BE;
    case AE_FMT_S16NE : return PA_SAMPLE_S16NE;
    case AE_FMT_S24LE3: return PA_SAMPLE_S24LE;
    case AE_FMT_S24BE3: return PA_SAMPLE_S24BE;
    case AE_FMT_S24NE3: return PA_SAMPLE_S24NE;
    case AE_FMT_S24LE4: return PA_SAMPLE_S24_32LE;
    case AE_FMT_S24BE4: return PA_SAMPLE_S24_32BE;
    case AE_FMT_S24NE4: return PA_SAMPLE_S24_32NE;
    case AE_FMT_S32BE : return PA_SAMPLE_S32LE;
    case AE_FMT_S32LE : return PA_SAMPLE_S32LE;
    case AE_FMT_S32NE : return PA_SAMPLE_S32NE;
    case AE_FMT_FLOAT : return PA_SAMPLE_FLOAT32;

    case AE_FMT_AC3:
    case AE_FMT_DTS:
    case AE_FMT_EAC3:
      return PA_SAMPLE_S16NE;

    default:
      return PA_SAMPLE_INVALID;
  }
}

static pa_encoding AEFormatToPulseEncoding(AEDataFormat format)
{
  switch (format)
  {
    case AE_FMT_AC3   : return PA_ENCODING_AC3_IEC61937;
    case AE_FMT_DTS   : return PA_ENCODING_DTS_IEC61937;
    case AE_FMT_EAC3  : return PA_ENCODING_EAC3_IEC61937;

    default:
      return PA_ENCODING_PCM;
  }
}

static AEDataFormat defaultDataFormats[] = {
  AE_FMT_U8,
  AE_FMT_S16LE,
  AE_FMT_S16BE,
  AE_FMT_S16NE,
  AE_FMT_S24LE3,
  AE_FMT_S24BE3,
  AE_FMT_S24NE3,
  AE_FMT_S24LE4,
  AE_FMT_S24BE4,
  AE_FMT_S24NE4,
  AE_FMT_S32BE,
  AE_FMT_S32LE,
  AE_FMT_S32NE,
  AE_FMT_FLOAT
};

static unsigned int defaultSampleRates[] = {
  5512,
  8000,
  11025,
  16000,
  22050,
  32000,
  44100,
  48000,
  64000,
  88200,
  96000,
  176400,
  192000,
  384000
};

/* Static callback functions */

static void ContextStateCallback(pa_context *c, void *userdata)
{
  pa_threaded_mainloop *m = (pa_threaded_mainloop *)userdata;
  switch (pa_context_get_state(c))
  {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_TERMINATED:
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
    case PA_CONTEXT_FAILED:
      pa_threaded_mainloop_signal(m, 0);
      break;
  }
}

static void StreamStateCallback(pa_stream *s, void *userdata)
{
  pa_threaded_mainloop *m = (pa_threaded_mainloop *)userdata;
  switch (pa_stream_get_state(s))
  {
    case PA_STREAM_UNCONNECTED:
    case PA_STREAM_CREATING:
    case PA_STREAM_READY:
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
      pa_threaded_mainloop_signal(m, 0);
      break;
  }
}

static void StreamRequestCallback(pa_stream *s, size_t length, void *userdata)
{
  pa_threaded_mainloop *m = (pa_threaded_mainloop *)userdata;
  pa_threaded_mainloop_signal(m, 0);
}

static void StreamLatencyUpdateCallback(pa_stream *s, void *userdata)
{
  pa_threaded_mainloop *m = (pa_threaded_mainloop *)userdata;
  pa_threaded_mainloop_signal(m, 0);
}

static AEChannel PAChannelToAEChannel(pa_channel_position_t channel)
{
  AEChannel ae_channel;
  switch (channel)
  {
  case PA_CHANNEL_POSITION_FRONT_LEFT:
    ae_channel = AE_CH_FL;
    break;
  case PA_CHANNEL_POSITION_FRONT_RIGHT:
    ae_channel = AE_CH_FR;
    break;
  case PA_CHANNEL_POSITION_FRONT_CENTER:
    ae_channel = AE_CH_FC;
    break;
  case PA_CHANNEL_POSITION_SIDE_LEFT:
    ae_channel = AE_CH_SL;
    break;
  case PA_CHANNEL_POSITION_SIDE_RIGHT:
    ae_channel = AE_CH_FL;
    break;
  case PA_CHANNEL_POSITION_REAR_LEFT:
    ae_channel = AE_CH_BL;
    break;
  case PA_CHANNEL_POSITION_REAR_RIGHT:
    ae_channel = AE_CH_BR;
    break;
  case PA_CHANNEL_POSITION_LFE:
    ae_channel = AE_CH_LFE;
    break;
  default:
    ae_channel = AE_CH_NULL;
    break;
  }
  return ae_channel;
}

static CAEChannelInfo PAChannelToAEChannelMap(pa_channel_map channels)
{
  CAEChannelInfo info;
  info.Reset();
  for (unsigned int i=0; i<channels.channels; i++)
  {
    info += PAChannelToAEChannel(channels.map[i]);
  }
  return info;
}

struct SinkInfoStruct
{
  AEDeviceInfoList *list;
  pa_threaded_mainloop *mainloop;
};

static void SinkInfoRequestCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
  SinkInfoStruct *sinkStruct = (SinkInfoStruct *)userdata;

  if (i && i->name)
  {
    CAEDeviceInfo device;

    device.m_deviceName = string(i->name);
    device.m_displayName = string(i->description);
    device.m_displayNameExtra = std::string("PULSE: ").append(i->description);
    unsigned int device_type = AE_DEVTYPE_PCM; //0

    device.m_channels = PAChannelToAEChannelMap(i->channel_map);
    device.m_sampleRates.assign(defaultSampleRates, defaultSampleRates + sizeof(defaultSampleRates) / sizeof(defaultSampleRates[0]));

    for (unsigned int j = 0; j < i->n_formats; j++)
    {
      switch(i->formats[j]->encoding)
      {
        case PA_ENCODING_AC3_IEC61937:
          device.m_dataFormats.push_back(AE_FMT_AC3);
          device_type |= AE_DEVTYPE_IEC958;
          break;
        case PA_ENCODING_DTS_IEC61937:
          device.m_dataFormats.push_back(AE_FMT_DTS);
          device_type |= AE_DEVTYPE_IEC958;
          break;
        case PA_ENCODING_EAC3_IEC61937:
          device.m_dataFormats.push_back(AE_FMT_EAC3);
          device_type |= AE_DEVTYPE_IEC958;
          break;
        case PA_ENCODING_PCM:
          device.m_dataFormats.insert(device.m_dataFormats.end(), defaultDataFormats, defaultDataFormats + sizeof(defaultDataFormats) / sizeof(defaultDataFormats[0]));
          break;
      }
    }
    // passthrough is only working when device has Stereo channel config
    if (device_type > AE_DEVTYPE_PCM && device.m_channels.Count() == 2)
      device.m_deviceType = AE_DEVTYPE_IEC958;
    else
      device.m_deviceType = AE_DEVTYPE_PCM;

    CLog::Log(LOGDEBUG, "PulseAudio: Found %s with devicestring %s", device.m_displayName.c_str(), device.m_deviceName.c_str());
    sinkStruct->list->push_back(device);
 }

  pa_threaded_mainloop_signal(sinkStruct->mainloop, 0);
}

/* PulseAudio class memberfunctions*/


CAESinkPULSE::CAESinkPULSE()
{
  m_IsAllocated = false;
  m_BytesPerSecond = 0;
  m_BufferSize = 0;
  m_Channels = 0;
  m_Stream = NULL;
  m_Context = NULL;
}

CAESinkPULSE::~CAESinkPULSE()
{
  Deinitialize();
}

bool CAESinkPULSE::Initialize(AEAudioFormat &format, std::string &device)
{
  m_IsAllocated = false;
  m_BytesPerSecond = 0;
  m_FrameSize = 0;
  m_BufferSize = 0;
  m_Channels = 0;
  m_Stream = NULL;
  m_Context = NULL;

  if (!SetupContext(NULL, &m_Context, &m_MainLoop))
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to create context");
    Deinitialize();
    return false;
  }

  pa_threaded_mainloop_lock(m_MainLoop);

  m_Channels = format.m_channelLayout.Count();

  struct pa_channel_map map;
  /* TODO Add proper mapping */
  pa_channel_map_init_auto(&map, m_Channels, PA_CHANNEL_MAP_ALSA);

  pa_cvolume_reset(&m_Volume, m_Channels);

  pa_format_info *info[1];
  info[0] = pa_format_info_new();
  info[0]->encoding = AEFormatToPulseEncoding(format.m_dataFormat);
  pa_format_info_set_sample_format(info[0], AEFormatToPulseFormat(format.m_dataFormat));
  pa_format_info_set_channels(info[0], m_Channels);
  unsigned int samplerate = AE_IS_RAW(format.m_dataFormat) ? format.m_encodedRate : format.m_sampleRate;
  pa_format_info_set_rate(info[0], samplerate);

  if (!pa_format_info_valid(info[0]))
  {
    CLog::Log(LOGERROR, "PulseAudio: Invalid format info");
    pa_threaded_mainloop_unlock(m_MainLoop);
    Deinitialize();
    return false;
  }

  pa_sample_spec spec;
  pa_format_info_to_sample_spec(info[0], &spec, &map);
  if (!pa_sample_spec_valid(&spec))
  {
    CLog::Log(LOGERROR, "PulseAudio: Invalid sample spec");
    pa_threaded_mainloop_unlock(m_MainLoop);
    Deinitialize();
    return false;
  }

  m_BytesPerSecond = pa_bytes_per_second(&spec);
  m_FrameSize = pa_frame_size(&spec);

  m_Stream = pa_stream_new_extended(m_Context, "audio stream", info, 1, NULL);
  pa_format_info_free(info[0]);

  if (m_Stream == NULL)
  {
    CLog::Log(LOGERROR, "PulseAudio: Could not create a stream");
    pa_threaded_mainloop_unlock(m_MainLoop);
    Deinitialize();
    return false;
  }

  pa_stream_set_state_callback(m_Stream, StreamStateCallback, m_MainLoop);
  pa_stream_set_write_callback(m_Stream, StreamRequestCallback, m_MainLoop);
  pa_stream_set_latency_update_callback(m_Stream, StreamLatencyUpdateCallback, m_MainLoop);

  pa_buffer_attr buffer_attr;
  // 200ms max latency
  // 50ms min packet size
  unsigned int latency = m_BytesPerSecond / 5;
  unsigned int process_time = latency / 4;
  memset(&buffer_attr, 0, sizeof(buffer_attr));
  buffer_attr.tlength = (uint32_t) latency;
  buffer_attr.minreq = (uint32_t) process_time;
  buffer_attr.maxlength = (uint32_t) -1;
  buffer_attr.prebuf = (uint32_t) -1;
  buffer_attr.fragsize = (uint32_t) latency;

  if (pa_stream_connect_playback(m_Stream, device.c_str(), &buffer_attr, ((pa_stream_flags)(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_ADJUST_LATENCY)), &m_Volume, NULL) < 0)
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to connect stream to output");
    pa_threaded_mainloop_unlock(m_MainLoop);
    Deinitialize();
    return false;
  }

  /* Wait until the stream is ready */
  do
  {
    pa_threaded_mainloop_wait(m_MainLoop);
    CLog::Log(LOGDEBUG, "PulseAudio: Stream %s", StreamStateToString(pa_stream_get_state(m_Stream)));
  }
  while (pa_stream_get_state(m_Stream) != PA_STREAM_READY && pa_stream_get_state(m_Stream) != PA_STREAM_FAILED);

  if (pa_stream_get_state(m_Stream) == PA_STREAM_FAILED)
  {
    CLog::Log(LOGERROR, "PulseAudio: Waited for the stream but it failed");
    pa_threaded_mainloop_unlock(m_MainLoop);
    Deinitialize();
    return false;
  }

  const pa_buffer_attr *a;

  if (!(a = pa_stream_get_buffer_attr(m_Stream)))
      CLog::Log(LOGERROR, "PulseAudio: %s", pa_strerror(pa_context_errno(m_Context)));
  else
  {
    unsigned int packetSize = a->minreq;
    m_BufferSize = a->tlength;

    format.m_frames = packetSize / m_FrameSize;
  }

  pa_threaded_mainloop_unlock(m_MainLoop);

  m_IsAllocated = true;
  format.m_frameSize = m_FrameSize;
  format.m_frameSamples = format.m_frames * format.m_channelLayout.Count();
  m_format = format;

  SetVolume(1.0);
  Cork(false);

  return true;
}

void CAESinkPULSE::Deinitialize()
{
  m_IsAllocated = false;

  if (m_Stream)
    Drain();

  if (m_MainLoop)
    pa_threaded_mainloop_stop(m_MainLoop);

  if (m_Stream)
  {
    pa_stream_disconnect(m_Stream);
    pa_stream_unref(m_Stream);
    m_Stream = NULL;
  }

  if (m_Context)
  {
    pa_context_disconnect(m_Context);
    pa_context_unref(m_Context);
    m_Context = NULL;
  }

  if (m_MainLoop)
  {
    pa_threaded_mainloop_free(m_MainLoop);
    m_MainLoop = NULL;
  }
}

double CAESinkPULSE::GetDelay()
{
  if (!m_IsAllocated)
    return 0;

  pa_usec_t latency = (pa_usec_t) -1;
  pa_threaded_mainloop_lock(m_MainLoop);
  while (pa_stream_get_latency(m_Stream, &latency, NULL) < 0)
  {
    if (pa_context_errno(m_Context) != PA_ERR_NODATA)
    {
      CLog::Log(LOGERROR, "PulseAudio: pa_stream_get_latency() failed");
      break;
    }
    /* Wait until latency data is available again */
    pa_threaded_mainloop_wait(m_MainLoop);
  }
  pa_threaded_mainloop_unlock(m_MainLoop);
  return latency / 1000000.0;
}

double CAESinkPULSE::GetCacheTotal()
{
  return (float)m_BufferSize / (float)m_BytesPerSecond;
}

unsigned int CAESinkPULSE::AddPackets(uint8_t *data, unsigned int frames, bool hasAudio, bool blocking)
{
  if (!m_IsAllocated)
    return frames;

  pa_threaded_mainloop_lock(m_MainLoop);

  unsigned int available = frames * m_format.m_frameSize;
  unsigned int length = std::min((unsigned int)pa_stream_writable_size(m_Stream), available);
  int error = pa_stream_write(m_Stream, data, length, NULL, 0, PA_SEEK_RELATIVE);
  pa_threaded_mainloop_unlock(m_MainLoop);

  if (error)
  {
    CLog::Log(LOGERROR, "CPulseAudioDirectSound::AddPackets - pa_stream_write failed\n");
    return 0;
  }

  return (unsigned int)(length / m_format.m_frameSize);
}

void CAESinkPULSE::Drain()
{
  if (!m_IsAllocated)
    return;

  pa_threaded_mainloop_lock(m_MainLoop);
  WaitForOperation(pa_stream_drain(m_Stream, NULL, NULL), m_MainLoop, "Drain");
  pa_threaded_mainloop_unlock(m_MainLoop);
}

void CAESinkPULSE::SetVolume(float volume)
{
  if (m_IsAllocated)
  {
    pa_threaded_mainloop_lock(m_MainLoop);
    pa_volume_t pavolume = pa_sw_volume_from_linear(volume);
    if ( pavolume <= 0 )
      pa_cvolume_mute(&m_Volume, m_Channels);
    else
      pa_cvolume_set(&m_Volume, m_Channels, pavolume);
    pa_operation *op = pa_context_set_sink_input_volume(m_Context, pa_stream_get_index(m_Stream), &m_Volume, NULL, NULL);
    if (op == NULL)
      CLog::Log(LOGERROR, "PulseAudio: Failed to set volume");
    else
      pa_operation_unref(op);

    pa_threaded_mainloop_unlock(m_MainLoop);
  }
}

void CAESinkPULSE::EnumerateDevicesEx(AEDeviceInfoList &list, bool force)
{
  pa_context *context;
  pa_threaded_mainloop *mainloop;

  if (!SetupContext(NULL, &context, &mainloop))
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to create context");
    return;
  }

  pa_threaded_mainloop_lock(mainloop);

  SinkInfoStruct sinkStruct;
  sinkStruct.mainloop = mainloop;
  sinkStruct.list = &list;
  WaitForOperation(pa_context_get_sink_info_list(context, SinkInfoRequestCallback, &sinkStruct), mainloop, "EnumerateAudioSinks");

  pa_threaded_mainloop_unlock(mainloop);

  if (mainloop)
    pa_threaded_mainloop_stop(mainloop);

  if (context)
  {
    pa_context_disconnect(context);
    pa_context_unref(context);
    context = NULL;
  }

  if (mainloop)
  {
    pa_threaded_mainloop_free(mainloop);
    mainloop = NULL;
  }
}

bool CAESinkPULSE::Cork(bool cork)
{
  pa_threaded_mainloop_lock(m_MainLoop);

  if (!WaitForOperation(pa_stream_cork(m_Stream, cork ? 1 : 0, NULL, NULL), m_MainLoop, cork ? "Pause" : "Resume"))
    cork = !cork;

  pa_threaded_mainloop_unlock(m_MainLoop);

  return cork;
}

inline bool CAESinkPULSE::WaitForOperation(pa_operation *op, pa_threaded_mainloop *mainloop, const char *LogEntry = "")
{
  if (op == NULL)
    return false;

  bool sucess = true;

  while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    pa_threaded_mainloop_wait(mainloop);

  if (pa_operation_get_state(op) != PA_OPERATION_DONE)
  {
    CLog::Log(LOGERROR, "PulseAudio: %s Operation failed", LogEntry);
    sucess = false;
  }

  pa_operation_unref(op);
  return sucess;
}

bool CAESinkPULSE::SetupContext(const char *host, pa_context **context, pa_threaded_mainloop **mainloop)
{
  if ((*mainloop = pa_threaded_mainloop_new()) == NULL)
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to allocate main loop");
    return false;
  }

  if (((*context) = pa_context_new(pa_threaded_mainloop_get_api(*mainloop), "XBMC")) == NULL)
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to allocate context");
    return false;
  }

  pa_context_set_state_callback(*context, ContextStateCallback, *mainloop);

  if (pa_context_connect(*context, host, (pa_context_flags_t)0, NULL) < 0)
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to connect context");
    return false;
  }
  pa_threaded_mainloop_lock(*mainloop);

  if (pa_threaded_mainloop_start(*mainloop) < 0)
  {
    CLog::Log(LOGERROR, "PulseAudio: Failed to start MainLoop");
    pa_threaded_mainloop_unlock(*mainloop);
    return false;
  }

  /* Wait until the context is ready */
  do
  {
    pa_threaded_mainloop_wait(*mainloop);
    CLog::Log(LOGDEBUG, "PulseAudio: Context %s", ContextStateToString(pa_context_get_state(*context)));
  }
  while (pa_context_get_state(*context) != PA_CONTEXT_READY && pa_context_get_state(*context) != PA_CONTEXT_FAILED);

  if (pa_context_get_state(*context) == PA_CONTEXT_FAILED)
  {
    CLog::Log(LOGERROR, "PulseAudio: Waited for the Context but it failed");
    pa_threaded_mainloop_unlock(*mainloop);
    return false;
  }

  pa_threaded_mainloop_unlock(*mainloop);
  return true;
}
#endif
