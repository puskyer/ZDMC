/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "ActiveAEBuffer.h"
#include "ActiveAEFilter.h"
#include "cores/AudioEngine/AEFactory.h"
#include "cores/AudioEngine/Engines/ActiveAE/AudioDSPAddons/ActiveAEDSPProcess.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAE.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/AEResampleFactory.h"


using namespace ActiveAE;

#if defined(ADSP_COUT_DEBUG_OUTPUT)
#include <iostream>
using namespace std;
#endif

/* typecast AE to CActiveAE */
#define AE (*((CActiveAE*)CAEFactory::GetEngine()))

CSoundPacket::CSoundPacket(SampleConfig conf, int samples) : config(conf)
{
  data = AE.AllocSoundSample(config, samples, bytes_per_sample, planes, linesize);
  max_nb_samples = samples;
  nb_samples = 0;
  pause_burst_ms = 0;
}

CSoundPacket::~CSoundPacket()
{
  if (data)
    AE.FreeSoundSample(data);
}

CSampleBuffer::CSampleBuffer() : pkt(NULL), pool(NULL)
{
  refCount = 0;
  timestamp = 0;
  pkt_start_offset = 0;
}

CSampleBuffer::~CSampleBuffer()
{
  delete pkt;
}

CSampleBuffer* CSampleBuffer::Acquire()
{
  refCount++;
  return this;
}

void CSampleBuffer::Return()
{
  refCount--;
  if (pool && refCount <= 0)
    pool->ReturnBuffer(this);
}

CActiveAEBufferPool::CActiveAEBufferPool(AEAudioFormat format)
{
  m_format = format;
  if (m_format.m_dataFormat == AE_FMT_RAW)
  {
    m_format.m_frameSize = 1;
    m_format.m_frames = 61440;
    m_format.m_channelLayout.Reset();
    m_format.m_channelLayout += AE_CH_FC;
  }
}

CActiveAEBufferPool::~CActiveAEBufferPool()
{
  CSampleBuffer *buffer;
  while(!m_allSamples.empty())
  {
    buffer = m_allSamples.front();
    m_allSamples.pop_front();
    delete buffer;
  }
}

CSampleBuffer* CActiveAEBufferPool::GetFreeBuffer()
{
  CSampleBuffer* buf = NULL;

  if (!m_freeSamples.empty())
  {
    buf = m_freeSamples.front();
    m_freeSamples.pop_front();
    buf->refCount = 1;
  }
  return buf;
}

void CActiveAEBufferPool::ReturnBuffer(CSampleBuffer *buffer)
{
  buffer->pkt->nb_samples = 0;
  buffer->pkt->pause_burst_ms = 0;
  m_freeSamples.push_back(buffer);
}

bool CActiveAEBufferPool::Create(unsigned int totaltime)
{
  CSampleBuffer *buffer;
  SampleConfig config;
  config.fmt = CAEUtil::GetAVSampleFormat(m_format.m_dataFormat);
  config.bits_per_sample = CAEUtil::DataFormatToUsedBits(m_format.m_dataFormat);
  config.dither_bits = CAEUtil::DataFormatToDitherBits(m_format.m_dataFormat);
  config.channels = m_format.m_channelLayout.Count();
  config.sample_rate = m_format.m_sampleRate;
  config.channel_layout = CAEUtil::GetAVChannelLayout(m_format.m_channelLayout);

  // discard already created buffers
  if (m_allSamples.size() > 0 || m_freeSamples.size() > 0)
  {
    m_freeSamples.clear();
    while (m_allSamples.size() > 0)
    {
      buffer = m_allSamples.front();
      m_allSamples.pop_front();
      delete buffer;
    }
    buffer = nullptr;
  }

  unsigned int time = 0;
  unsigned int buffertime = (m_format.m_frames*1000) / m_format.m_sampleRate;
  if (m_format.m_dataFormat == AE_FMT_RAW)
  {
    buffertime = m_format.m_streamInfo.GetDuration();
  }
  unsigned int n = 0;
  while (time < totaltime || n < 5)
  {
    buffer = new CSampleBuffer();
    buffer->pool = this;
    buffer->pkt = new CSoundPacket(config, m_format.m_frames);

    m_allSamples.push_back(buffer);
    m_freeSamples.push_back(buffer);
    time += buffertime;
    n++;
  }

  return true;
}

// ----------------------------------------------------------------------------------
// Resample
// ----------------------------------------------------------------------------------

CActiveAEBufferPoolResample::CActiveAEBufferPoolResample(AEAudioFormat inputFormat, AEAudioFormat outputFormat, AEQuality quality)
  : CActiveAEBufferPool(outputFormat)
{
#if defined(ADSP_COUT_DEBUG_OUTPUT)
  CLog::Log(LOGDEBUG, "------CActiveAEBufferPoolResample::CActiveAEBufferPoolResample(...): inputFormat.m_dataFormat=%f, inputFormat.m_frames=%f, inputFormat.m_frameSize=%f, inputFormat.m_sampleRate=%f", (double)inputFormat.m_dataFormat, (double)inputFormat.m_frames, (double)inputFormat.m_frameSize, (double)inputFormat.m_sampleRate);
  CLog::Log(LOGDEBUG, "------CActiveAEBufferPoolResample::CActiveAEBufferPoolResample(...): outputFormat.m_dataFormat=%f, outputFormat.m_frames=%f, outputFormat.m_frameSize=%f, outputFormat.m_sampleRate=%f", (double)outputFormat.m_dataFormat, (double)outputFormat.m_frames, (double)outputFormat.m_frameSize, (double)outputFormat.m_sampleRate);
#endif
  m_inputFormat = inputFormat;
  if (m_inputFormat.m_dataFormat == AE_FMT_RAW)
  {
    m_format.m_frameSize = 1;
    m_format.m_frames = 61440;
    m_inputFormat.m_channelLayout.Reset();
    m_inputFormat.m_channelLayout += AE_CH_FC;
  }
  m_resampler = NULL;
  m_fillPackets = false;
  m_drain = false;
  m_empty = true;
  m_procSample = NULL;
  m_dspSample = NULL;
  m_dspBuffer = NULL;
  m_resampleRatio = 1.0;
  m_resampleQuality = quality;
  m_forceResampler = false;
  m_stereoUpmix = false;
  m_normalize = true;
  m_useResampler = false;
  m_useDSP = false;
  m_bypassDSP = false;
  m_changeResampler = false;
  m_changeDSP = false;
  m_lastSamplePts = 0;
}

CActiveAEBufferPoolResample::~CActiveAEBufferPoolResample()
{
  Flush();

  delete m_resampler;
  
  if (m_useDSP)
    CServiceBroker::GetADSP().DestroyDSPs(m_streamId);
  if (m_dspBuffer)
    delete m_dspBuffer;
}

void CActiveAEBufferPoolResample::SetExtraData(int profile, enum AVMatrixEncoding matrix_encoding, enum AVAudioServiceType audio_service_type)
{
  m_Profile = profile;
  m_MatrixEncoding = matrix_encoding;
  m_AudioServiceType = audio_service_type;

  if (m_useDSP)
    ChangeAudioDSP();
}

bool CActiveAEBufferPoolResample::Create(unsigned int totaltime, bool remap, bool upmix, bool normalize, bool useDSP)
{
  CActiveAEBufferPool::Create(totaltime);

  m_remap = remap;
  m_stereoUpmix = upmix;
  m_useResampler = m_changeResampler;                                       /* if m_changeResampler is true on Create, system require the usage of resampler */

  /*
   * On first used resample class, DSP signal processing becomes performed.
   * For the signal processing the input data for CActiveAEResample must be
   * modified to have a full supported audio stream with all available
   * channels, for this reason also some in resample used functions must be
   * disabled.
   *
   * The reason to perform this before CActiveAEResample is to have a unmodified
   * stream with the basic data to have best quality like for surround upmix.
   *
   * The value m_streamId and address pointer m_processor are passed a pointers
   * to CServiceBroker::GetADSP().CreateDSPs and set from it.
   */
  if ((useDSP || m_changeDSP) && !m_bypassDSP)
  {
    // use input format from this CActiveAEBufferPoolResample as input for AudioDSP
    // and use CActiveAEBufferPool::m_format as output format of AudioDSP
    m_useDSP = CServiceBroker::GetADSP().CreateDSPs(m_streamId, m_processor, m_inputFormat, CActiveAEBufferPool::m_format, 
                                                    upmix, m_resampleQuality, m_MatrixEncoding, m_AudioServiceType, m_Profile);
    if (m_useDSP)
    {
      m_adspOutFormat = CActiveAEBufferPool::m_format;
      m_adspOutFormat.m_channelLayout = m_processor->GetChannelLayout();    /* Overide input format with DSP's supported format */
      m_adspOutFormat.m_sampleRate    = m_processor->GetOutputSamplerate(); /* Overide input format with DSP's generated samplerate */
      m_adspOutFormat.m_dataFormat    = m_processor->GetDataFormat();       /* Overide input format with DSP's processed data format, normally it is float */
      m_adspOutFormat.m_frames        = m_processor->GetOutputFrames();
      m_forceResampler              = true;                               /* Force load of ffmpeg resampler, required to detect exact input and output channel alignment pointers */
      if (m_processor->GetChannelLayout().Count() > 2)                    /* Disable upmix for CActiveAEResample if DSP layout > 2.0, becomes perfomed by DSP */
        upmix = false;

      if (!m_dspBuffer)
      {
        m_dspBuffer = new CActiveAEBufferPool(m_adspOutFormat); /* Get dsp processing buffer class, based on dsp output format */
      }
      else
      {
        m_dspBuffer->m_format = m_adspOutFormat;
      }
      m_dspBuffer->Create(totaltime);
      CActiveAEBufferPool::Create(totaltime); // recreate output buffer cause format has changed
    }
  }
  m_changeDSP  = false;

  m_normalize = true;
  if ((m_format.m_channelLayout.Count() < m_inputFormat.m_channelLayout.Count() && !normalize))
    m_normalize = false;

  /*
   * Compare the input format with output, if there is one different format, perform the
   * kodi based resample processing.
   *
   * The input format can be become modified if addon dsp processing is enabled. For this
   * reason some things are no more required for it. As example, if resampling is performed
   * by the addons it is no more required here, also the channel layout can be modified
   * from addons. The output data format from dsp is always float and if something other
   * is required here, the CActiveAEResample must change it.
   */
  if (m_inputFormat.m_channelLayout != m_format.m_channelLayout ||
      m_inputFormat.m_sampleRate != m_format.m_sampleRate ||
      m_inputFormat.m_dataFormat != m_format.m_dataFormat ||
      m_forceResampler)
    m_useResampler = true;

  if (m_useResampler || m_changeResampler)
  {
    if(!m_resampler)
      m_resampler = CAEResampleFactory::Create();

    AEAudioFormat m_resamplerInFormat;
    if (m_useDSP)
    {
      m_resamplerInFormat = m_adspOutFormat;
    }
    else
    {
      m_resamplerInFormat = m_inputFormat;
    }

    m_resampler->Init(CAEUtil::GetAVChannelLayout(m_format.m_channelLayout),
                                m_format.m_channelLayout.Count(),
                                m_format.m_sampleRate,
                                CAEUtil::GetAVSampleFormat(m_format.m_dataFormat),
                                CAEUtil::DataFormatToUsedBits(m_format.m_dataFormat),
                                CAEUtil::DataFormatToDitherBits(m_format.m_dataFormat),
                                CAEUtil::GetAVChannelLayout(m_resamplerInFormat.m_channelLayout),
                                m_resamplerInFormat.m_channelLayout.Count(),
                                m_resamplerInFormat.m_sampleRate,
                                CAEUtil::GetAVSampleFormat(m_resamplerInFormat.m_dataFormat),
                                CAEUtil::DataFormatToUsedBits(m_resamplerInFormat.m_dataFormat),
                                CAEUtil::DataFormatToDitherBits(m_resamplerInFormat.m_dataFormat),
                                upmix,
                                m_normalize,
                                remap ? &m_format.m_channelLayout : NULL,
                                m_resampleQuality,
                                m_forceResampler);
  }

  m_changeResampler = false;

  return true;
}

void CActiveAEBufferPoolResample::ChangeResampler()
{
  if (m_resampler)
  {
    delete m_resampler;
    m_resampler = NULL;
  }

  bool upmix = m_stereoUpmix;
  if (m_useDSP && m_processor && m_processor->GetChannelLayout().Count() > 2)
    upmix = false;

  m_resampler = CAEResampleFactory::Create();

  AEAudioFormat m_resamplerInFormat;
  if (m_useDSP)
  {
    m_resamplerInFormat = m_adspOutFormat;
  }
  else
  {
    m_resamplerInFormat = m_inputFormat;
  }

  m_resampler->Init(CAEUtil::GetAVChannelLayout(m_format.m_channelLayout),
                                m_format.m_channelLayout.Count(),
                                m_format.m_sampleRate,
                                CAEUtil::GetAVSampleFormat(m_format.m_dataFormat),
                                CAEUtil::DataFormatToUsedBits(m_format.m_dataFormat),
                                CAEUtil::DataFormatToDitherBits(m_format.m_dataFormat),
                                CAEUtil::GetAVChannelLayout(m_resamplerInFormat.m_channelLayout),
                                m_resamplerInFormat.m_channelLayout.Count(),
                                m_resamplerInFormat.m_sampleRate,
                                CAEUtil::GetAVSampleFormat(m_resamplerInFormat.m_dataFormat),
                                CAEUtil::DataFormatToUsedBits(m_resamplerInFormat.m_dataFormat),
                                CAEUtil::DataFormatToDitherBits(m_resamplerInFormat.m_dataFormat),
                                upmix,
                                m_normalize,
                                m_remap ? &m_format.m_channelLayout : NULL,
                                m_resampleQuality,
                                m_forceResampler);

  m_changeResampler = false;
}

void CActiveAEBufferPoolResample::ChangeAudioDSP()
{
  /* if dsp was enabled before reset input format, also used for failed dsp creation */
  bool wasActive = false;
  if (m_useDSP && m_processor != NULL)
  {
    wasActive = true;
  }

  m_useDSP = CServiceBroker::GetADSP().CreateDSPs(m_streamId, m_processor, m_inputFormat,
                                                  CActiveAEBufferPool::m_format, m_stereoUpmix, 
                                                  m_resampleQuality, m_MatrixEncoding, 
                                                  m_AudioServiceType, m_Profile, wasActive);
  if (m_useDSP)
  {
    m_adspOutFormat = CActiveAEBufferPool::m_format;
    m_adspOutFormat.m_channelLayout = m_processor->GetChannelLayout();    /* Overide output format with DSP's supported format */
    m_adspOutFormat.m_sampleRate    = m_processor->GetOutputSamplerate(); /* Overide output format with DSP's generated samplerate */
    m_adspOutFormat.m_dataFormat    = m_processor->GetDataFormat();       /* Overide output format with DSP's processed data format, normally it is float */
    m_adspOutFormat.m_frames        = m_processor->GetOutputFrames();
    m_changeResampler             = true;                               /* Force load of ffmpeg resampler, required to detect exact input and output channel alignment pointers */
  }
  else if (wasActive)
  {
    /*
     * Check now after the dsp processing becomes disabled, that the resampler is still
     * required, if not unload it also.
     */
    if (m_inputFormat.m_channelLayout == m_format.m_channelLayout &&
        m_inputFormat.m_sampleRate == m_format.m_sampleRate &&
        m_inputFormat.m_dataFormat == m_format.m_dataFormat &&
        !m_useResampler)
    {
      delete m_resampler;
      m_resampler = NULL;
      delete m_dspBuffer;
      m_dspBuffer = NULL;
      m_changeResampler = false;
    }
    else
      m_changeResampler = true;

    m_useDSP = false;
    CServiceBroker::GetADSP().DestroyDSPs(m_streamId);
  }
  else
  {
    m_useDSP = false;
  }

  m_changeDSP = false;
}

bool CActiveAEBufferPoolResample::ResampleBuffers(int64_t timestamp)
{
  bool busy = false;
  CSampleBuffer *in;

  if (!m_resampler)
  {
    if (m_changeDSP || m_changeResampler)
    {
      if (m_changeDSP)
        ChangeAudioDSP();
      if (m_changeResampler)
        ChangeResampler();
      return true;
    }
    while(!m_inputSamples.empty())
    {
      in = m_inputSamples.front();
      m_inputSamples.pop_front();
      if (timestamp)
      {
        in->timestamp = timestamp;
      }
      m_outputSamples.push_back(in);
      busy = true;
    }
  }
  else if (m_procSample || !m_freeSamples.empty())
  {
    int free_samples;
    if (m_procSample)
      free_samples = m_procSample->pkt->max_nb_samples - m_procSample->pkt->nb_samples;
    else
      free_samples = m_format.m_frames;

    bool skipInput = false;
    // avoid that ffmpeg resample buffer grows too large
    if (!m_resampler->WantsNewSamples(free_samples) && !m_empty)
      skipInput = true;

    bool hasInput = !m_inputSamples.empty();

    if (hasInput || skipInput || m_drain || m_changeResampler || m_changeDSP)
    {
      if (!m_procSample)
      {
        m_procSample = GetFreeBuffer();
      }

      if (hasInput && !skipInput && !m_changeResampler && !m_changeDSP)
      {
        in = m_inputSamples.front();
        m_inputSamples.pop_front();
#if defined(ADSP_COUT_DEBUG_OUTPUT)
        CLog::Log(LOGDEBUG, "------in->timestamp: %i, in->pkt_start_offset:%i, in->pkt->bytes_per_sample:%i, in->pkt->nb_samples:%i, in->pkt->pause_burst_ms:%i, in->pkt->planes:%i, in->pkt->linesize:%i, in->pkt->max_nb_samples:%i", in->timestamp, in->pkt_start_offset, in->pkt->bytes_per_sample, in->pkt->nb_samples, in->pkt->pause_burst_ms, in->pkt->planes, in->pkt->linesize, in->pkt->max_nb_samples);
        CLog::Log(LOGDEBUG, "------in->pkt->config.bits_per_sample:%i, in->pkt->config.channels:%i, in->pkt->config.channel_layout:%lld, in->pkt->config.dither_bits:%i, in->pkt->config.sample_rate:%i", in->pkt->config.bits_per_sample, in->pkt->config.channels, (unsigned long long)in->pkt->config.channel_layout, in->pkt->config.dither_bits, in->pkt->config.sample_rate);
#endif
      }
      else
        in = NULL;

      /*
       * DSP need always a available input packet! To pass it step by step
       * over all enabled addons and processing segments.
       */
      if (m_useDSP && in)
      {
        if (!m_dspSample)
          m_dspSample = m_dspBuffer->GetFreeBuffer();

#if defined(ADSP_COUT_DEBUG_OUTPUT)
        CLog::Log(LOGDEBUG, "------m_dspSample->timestamp: %i, m_dspSample->pkt_start_offset:%i, m_dspSample->pkt->bytes_per_sample:%i, m_dspSample->pkt->nb_samples:%i, m_dspSample->pkt->pause_burst_ms:%i, m_dspSample->pkt->planes:%i, m_dspSample->pkt->linesize:%i, m_dspSample->pkt->max_nb_samples:%i", m_dspSample->timestamp, m_dspSample->pkt_start_offset, m_dspSample->pkt->bytes_per_sample, m_dspSample->pkt->nb_samples, m_dspSample->pkt->pause_burst_ms, m_dspSample->pkt->planes, m_dspSample->pkt->linesize, m_dspSample->pkt->max_nb_samples);
        CLog::Log(LOGDEBUG, "------m_dspSample->pkt->config.bits_per_sample:%i, m_dspSample->pkt->config.channels:%i, m_dspSample->pkt->config.channel_layout:%lld, m_dspSample->pkt->config.dither_bits:%i, m_dspSample->pkt->config.sample_rate:%i", m_dspSample->pkt->config.bits_per_sample, m_dspSample->pkt->config.channels, (unsigned long long)m_dspSample->pkt->config.channel_layout, m_dspSample->pkt->config.dither_bits, m_dspSample->pkt->config.sample_rate);
#endif
        // currently AudioDSP doesn't do any internal buffering that's why timestamp and pkt_start_offset
        // is the same as the input
        m_dspSample->timestamp = in->timestamp;
        m_dspSample->pkt_start_offset = in->pkt_start_offset;
        if (m_dspSample && m_processor->Process(in, m_dspSample))
        {
          in->Return();
          in = m_dspSample;
          m_dspSample = NULL;
        }
        else
        {
          in->Return();
          in = NULL;
        }
      }

      int start = m_procSample->pkt->nb_samples *
                  m_procSample->pkt->bytes_per_sample *
                  m_procSample->pkt->config.channels /
                  m_procSample->pkt->planes;

      for(int i=0; i<m_procSample->pkt->planes; i++)
      {
        m_planes[i] = m_procSample->pkt->data[i] + start;
      }

      int out_samples = m_resampler->Resample(m_planes,
                                              m_procSample->pkt->max_nb_samples - m_procSample->pkt->nb_samples,
                                              in ? in->pkt->data : NULL,
                                              in ? in->pkt->nb_samples : 0,
                                              m_resampleRatio);
      // in case of error, trigger re-create of resampler
      if (out_samples < 0)
      {
        out_samples = 0;
        m_changeResampler = true;
      }

      m_procSample->pkt->nb_samples += out_samples;
      busy = true;
      m_empty = (out_samples == 0);

      if (in)
      {
#if defined(ADSP_COUT_DEBUG_OUTPUT)
        CLog::Log(LOGDEBUG, "------in->timestamp: %i, in->pkt_start_offset:%i, in->pkt->bytes_per_sample:%i, in->pkt->nb_samples:%i, in->pkt->pause_burst_ms:%i, in->pkt->planes:%i, in->pkt->linesize:%i, in->pkt->max_nb_samples:%i", in->timestamp, in->pkt_start_offset, in->pkt->bytes_per_sample, in->pkt->nb_samples, in->pkt->pause_burst_ms, in->pkt->planes, in->pkt->linesize, in->pkt->max_nb_samples);
        CLog::Log(LOGDEBUG, "------in->pkt->config.bits_per_sample:%i, in->pkt->config.channels:%i, in->pkt->config.channel_layout:%lld, in->pkt->config.dither_bits:%i, in->pkt->config.sample_rate:%i", in->pkt->config.bits_per_sample, in->pkt->config.channels, (unsigned long long)in->pkt->config.channel_layout, in->pkt->config.dither_bits, in->pkt->config.sample_rate);
#endif
        if (!timestamp)
        {
          if (in->timestamp)
            m_lastSamplePts = in->timestamp;
          else
            in->pkt_start_offset = 0;
        }
        else
        {
          m_lastSamplePts = timestamp;
          in->pkt_start_offset = 0;
        }

        // pts of last sample we added to the buffer
        m_lastSamplePts += (in->pkt->nb_samples-in->pkt_start_offset) * 1000 / m_format.m_sampleRate;
      }

      // calculate pts for last sample in m_procSample
      int bufferedSamples = m_resampler->GetBufferedSamples();
      m_procSample->pkt_start_offset = m_procSample->pkt->nb_samples;
      m_procSample->timestamp = m_lastSamplePts - bufferedSamples * 1000 / m_format.m_sampleRate;

      if ((m_drain || m_changeResampler || m_changeDSP) && m_empty)
      {
        if (m_fillPackets && m_procSample->pkt->nb_samples != 0)
        {
          // pad with zero
          start = m_procSample->pkt->nb_samples *
                  m_procSample->pkt->bytes_per_sample *
                  m_procSample->pkt->config.channels /
                  m_procSample->pkt->planes;
          for(int i=0; i<m_procSample->pkt->planes; i++)
          {
            memset(m_procSample->pkt->data[i]+start, 0, m_procSample->pkt->linesize-start);
          }
        }

        // check if draining is finished
        if (m_drain && m_procSample->pkt->nb_samples == 0)
        {
          m_procSample->Return();
          busy = false;
        }
        else
          m_outputSamples.push_back(m_procSample);

        m_procSample = NULL;
        if (m_changeDSP)
          ChangeAudioDSP();
        if (m_changeResampler)
          ChangeResampler();
      }
      // some methods like encode require completely filled packets
      else if (!m_fillPackets || (m_procSample->pkt->nb_samples == m_procSample->pkt->max_nb_samples))
      {
        m_outputSamples.push_back(m_procSample);
        m_procSample = NULL;
      }

      if (in)
        in->Return();
    }
  }
  return busy;
}

void CActiveAEBufferPoolResample::ConfigureResampler(bool normalizelevels, bool dspenabled, bool stereoupmix, AEQuality quality)
{
  bool normalize = true;
  if ((m_format.m_channelLayout.Count() < m_inputFormat.m_channelLayout.Count()) &&
      normalizelevels)
    normalize = false;

  /* Disable upmix if DSP layout > 2.0, becomes perfomed by DSP */
  bool ignoreUpmix = false;
  if (dspenabled && m_useDSP && m_processor->GetChannelLayout().Count() > 2)
    ignoreUpmix = true;

  if (m_useResampler &&
      ((m_resampleQuality != quality) ||
       ((m_stereoUpmix != stereoupmix) && !ignoreUpmix) ||
       (m_normalize != normalize)))
  {
    m_changeResampler = true;
  }

  if (m_useDSP != dspenabled ||
      (m_useDSP &&
      ((m_resampleQuality != quality) ||
      (m_stereoUpmix != stereoupmix))))
  {
    m_changeDSP = true;
  }

  m_useDSP = dspenabled;
  m_resampleQuality = quality;
  m_stereoUpmix = stereoupmix;
  m_normalize = normalize;
}

float CActiveAEBufferPoolResample::GetDelay()
{
  float delay = 0;
  std::deque<CSampleBuffer*>::iterator itBuf;

  if (m_procSample)
    delay += (float)m_procSample->pkt->nb_samples / m_procSample->pkt->config.sample_rate;
  //if (m_dspSample) // TODO this needs to be implemented when adsp supports asynchronous processing
  //  delay += (float)m_dspSample->pkt->nb_samples / m_dspSample->pkt->config.sample_rate;

  for(itBuf=m_inputSamples.begin(); itBuf!=m_inputSamples.end(); ++itBuf)
  {
    delay += (float)(*itBuf)->pkt->nb_samples / (*itBuf)->pkt->config.sample_rate;
  }

  for(itBuf=m_outputSamples.begin(); itBuf!=m_outputSamples.end(); ++itBuf)
  {
    delay += (float)(*itBuf)->pkt->nb_samples / (*itBuf)->pkt->config.sample_rate;
  }

  if (m_resampler)
  {
    int samples = m_resampler->GetBufferedSamples();
    delay += (float)samples / m_format.m_sampleRate;
  }

#if defined(ADSP_COUT_DEBUG_OUTPUT)
  CLog::Log(LOGDEBUG, "------delay: %f", delay);
  if (m_useDSP)
  {
    CLog::Log(LOGDEBUG, "------m_processor->GetDelay(): %f", m_processor->GetDelay());
    //delay += m_processor->GetDelay();  // TODO this needs to be implemented when adsp supports asynchronous processing
  }
#endif

  return delay;
}

void CActiveAEBufferPoolResample::Flush()
{
  if (m_procSample)
  {
    m_procSample->Return();
    m_procSample = NULL;
  }
  if (m_dspSample)
  {
    m_dspSample->Return();
    m_dspSample = NULL;
  }
  while (!m_inputSamples.empty())
  {
    m_inputSamples.front()->Return();
    m_inputSamples.pop_front();
  }
  while (!m_outputSamples.empty())
  {
    m_outputSamples.front()->Return();
    m_outputSamples.pop_front();
  }
  if (m_resampler)
    ChangeResampler();
}

void CActiveAEBufferPoolResample::SetDrain(bool drain)
{
  m_drain = drain;
}

void CActiveAEBufferPoolResample::SetRR(double rr)
{
  m_resampleRatio = rr;
}

double CActiveAEBufferPoolResample::GetRR()
{
  return m_resampleRatio;
}

void CActiveAEBufferPoolResample::FillBuffer()
{
  m_fillPackets = true;
}

bool CActiveAEBufferPoolResample::DoesNormalize()
{
  return m_normalize;
}

void CActiveAEBufferPoolResample::ForceResampler(bool force)
{
  m_forceResampler = force;
}

void CActiveAEBufferPoolResample::SetDSPConfig(bool usedsp, bool bypassdsp)
{
  m_useDSP = usedsp;
  m_bypassDSP = bypassdsp;
}

// ----------------------------------------------------------------------------------
// Atempo
// ----------------------------------------------------------------------------------

CActiveAEBufferPoolAtempo::CActiveAEBufferPoolAtempo(AEAudioFormat format) : CActiveAEBufferPool(format)
{
  m_drain = false;
  m_empty = true;
  m_tempo = 1.0;
  m_changeFilter = false;
  m_procSample = nullptr;
}

CActiveAEBufferPoolAtempo::~CActiveAEBufferPoolAtempo()
{
  Flush();
}

bool CActiveAEBufferPoolAtempo::Create(unsigned int totaltime)
{
  CActiveAEBufferPool::Create(totaltime);

  m_pTempoFilter.reset(new CActiveAEFilter());
  m_pTempoFilter->Init(CAEUtil::GetAVSampleFormat(m_format.m_dataFormat), m_format.m_sampleRate, CAEUtil::GetAVChannelLayout(m_format.m_channelLayout));

  return true;
}

void CActiveAEBufferPoolAtempo::ChangeFilter()
{
  m_pTempoFilter->SetTempo(m_tempo);
  m_changeFilter = false;
}

bool CActiveAEBufferPoolAtempo::ProcessBuffers()
{
  bool busy = false;
  CSampleBuffer *in;

  if (!m_pTempoFilter->IsActive())
  {
    if (m_changeFilter)
    {
      if (m_changeFilter)
        ChangeFilter();
      return true;
    }
    while(!m_inputSamples.empty())
    {
      in = m_inputSamples.front();
      m_inputSamples.pop_front();
      m_outputSamples.push_back(in);
      busy = true;
    }
  }
  else if (m_procSample || !m_freeSamples.empty())
  {
    bool skipInput = false;

    // avoid that bufferscr grows too large
    if (!m_pTempoFilter->NeedData())
      skipInput = true;

    bool hasInput = !m_inputSamples.empty();

    if (hasInput || skipInput || m_drain || m_changeFilter)
    {
      if (!m_procSample)
      {
        m_procSample = GetFreeBuffer();
      }

      if (hasInput && !skipInput && !m_changeFilter)
      {
        in = m_inputSamples.front();
        m_inputSamples.pop_front();
      }
      else
        in = nullptr;

      int start = m_procSample->pkt->nb_samples *
                  m_procSample->pkt->bytes_per_sample *
                  m_procSample->pkt->config.channels /
                  m_procSample->pkt->planes;

      for (int i=0; i<m_procSample->pkt->planes; i++)
      {
        m_planes[i] = m_procSample->pkt->data[i] + start;
      }

      int out_samples = m_pTempoFilter->ProcessFilter(m_planes,
                                                      m_procSample->pkt->max_nb_samples - m_procSample->pkt->nb_samples,
                                                      in ? in->pkt->data : nullptr,
                                                      in ? in->pkt->nb_samples : 0,
                                                      in ? in->pkt->linesize * in->pkt->planes : 0);

      // in case of error, trigger re-create of filter
      if (out_samples < 0)
      {
        out_samples = 0;
        m_changeFilter = true;
      }

      m_procSample->pkt->nb_samples += out_samples;
      busy = true;
      m_empty = m_pTempoFilter->IsEof();

      if (in)
      {
        if (in->timestamp)
          m_lastSamplePts = in->timestamp;
        else
          in->pkt_start_offset = 0;

        // pts of last sample we added to the buffer
        m_lastSamplePts += (in->pkt->nb_samples-in->pkt_start_offset) * 1000 / m_format.m_sampleRate;
      }

      // calculate pts for last sample in m_procSample
      int bufferedSamples = m_pTempoFilter->GetBufferedSamples();
      m_procSample->pkt_start_offset = m_procSample->pkt->nb_samples;
      m_procSample->timestamp = m_lastSamplePts - bufferedSamples * 1000 / m_format.m_sampleRate;

      if ((m_drain || m_changeFilter) && m_empty)
      {
        if (m_fillPackets && m_procSample->pkt->nb_samples != 0)
        {
          // pad with zero
          start = m_procSample->pkt->nb_samples *
          m_procSample->pkt->bytes_per_sample *
          m_procSample->pkt->config.channels /
          m_procSample->pkt->planes;
          for (int i=0; i<m_procSample->pkt->planes; i++)
          {
            memset(m_procSample->pkt->data[i]+start, 0, m_procSample->pkt->linesize-start);
          }
        }

        // check if draining is finished
        if (m_drain && m_procSample->pkt->nb_samples == 0)
        {
          m_procSample->Return();
          busy = false;
        }
        else
          m_outputSamples.push_back(m_procSample);

        m_procSample = nullptr;

        if (m_changeFilter)
        {
          ChangeFilter();
        }
      }
      // some methods like encode require completely filled packets
      else if (!m_fillPackets || (m_procSample->pkt->nb_samples == m_procSample->pkt->max_nb_samples))
      {
        m_outputSamples.push_back(m_procSample);
        m_procSample = nullptr;
      }

      if (in)
        in->Return();
    }
  }
  return busy;
}

void CActiveAEBufferPoolAtempo::Flush()
{
  if (m_procSample)
  {
    m_procSample->Return();
    m_procSample = nullptr;
  }
  while (!m_inputSamples.empty())
  {
    m_inputSamples.front()->Return();
    m_inputSamples.pop_front();
  }
  while (!m_outputSamples.empty())
  {
    m_outputSamples.front()->Return();
    m_outputSamples.pop_front();
  }
  if (m_pTempoFilter)
    ChangeFilter();
}

float CActiveAEBufferPoolAtempo::GetDelay()
{
  float delay = 0;

  if (m_procSample)
    delay += (float)m_procSample->pkt->nb_samples / m_procSample->pkt->config.sample_rate;

  for (auto &buf : m_inputSamples)
  {
    delay += (float)buf->pkt->nb_samples / buf->pkt->config.sample_rate;
  }

  for (auto &buf : m_outputSamples)
  {
    delay += (float)buf->pkt->nb_samples / buf->pkt->config.sample_rate;
  }

  if (m_pTempoFilter->IsActive())
  {
    int samples = m_pTempoFilter->GetBufferedSamples();
    delay += (float)samples / m_format.m_sampleRate;
  }

  return delay;
}

void CActiveAEBufferPoolAtempo::SetTempo(float tempo)
{
  if (tempo > 2.0)
    tempo = 2.0;
  else if (tempo < 0.5)
    tempo = 0.5;

  if (tempo != m_tempo)
    m_changeFilter = true;

  m_tempo = tempo;
}

float CActiveAEBufferPoolAtempo::GetTempo()
{
  return m_tempo;
}

void CActiveAEBufferPoolAtempo::FillBuffer()
{
  m_fillPackets = true;
}

void CActiveAEBufferPoolAtempo::SetDrain(bool drain)
{
  m_drain = drain;
  if (!m_drain)
    m_changeFilter = true;
}
