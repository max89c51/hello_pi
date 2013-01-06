/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Audio output demo using OpenMAX IL though the ilcient helper library

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <semaphore.h>

#include "bcm_host.h"
#include "ilclient.h"

#define N_WAVE          1024    /* dimension of Sinewave[] */
#define PI (1<<16>>1)
#define SIN(x) Sinewave[((x)>>6) & (N_WAVE-1)]
#define COS(x) SIN((x)+(PI>>1))
extern short Sinewave[];

#ifndef countof
   #define countof(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

#define BUFFER_SIZE_SAMPLES 1024

typedef int int32_t;

typedef struct {
   sem_t sema;
   ILCLIENT_T *client;
   COMPONENT_T *audio_render;
   COMPONENT_T *mp3_decoder;
   COMPONENT_T *list[2];
   OMX_BUFFERHEADERTYPE *user_buffer_list; // buffers owned by the client
   uint32_t pcm_num_buffers;
   uint32_t mp3_in_num_buffers;
   uint32_t mp3_out_num_buffers;
   uint32_t pcm_bytes_per_sample;
} AUDIOPLAY_STATE_T;

static void input_buffer_callback(void *data, COMPONENT_T *comp)
{
   // do nothing - could add a callback to the user
   // to indicate more buffers may be available.
   printf("input_buffer_callback\n");
}

static void error_callback(void *data, COMPONENT_T *comp, OMX_U32 d)
{
   printf("error callback, code=%d (0x%x)\n", d, d);
}

static void port_settings_callback(void *data, COMPONENT_T *comp, OMX_U32 d)
{
   printf("port settings callback\n");
}

static void config_changed_callback(void *data, COMPONENT_T *comp, OMX_U32 d)
{
   printf("config changed callback\n");
}






int32_t pcm_audioplay_create(AUDIOPLAY_STATE_T **handle,
                         uint32_t sample_rate,
                         uint32_t num_channels,
                         uint32_t bit_depth,
                         uint32_t num_buffers,
                         uint32_t buffer_size)
{
   uint32_t bytes_per_sample = (bit_depth * num_channels) >> 3;
   int32_t ret = -1;

   *handle = NULL;

   // basic sanity check on arguments
   if(sample_rate >= 8000 && sample_rate <= 96000 &&
      (num_channels == 1 || num_channels == 2 || num_channels == 4 || num_channels == 8) &&
      (bit_depth == 16 || bit_depth == 32) &&
      num_buffers > 0 &&
      buffer_size >= bytes_per_sample)
   {
      // buffer lengths must be 16 byte aligned for VCHI
      int size = (buffer_size + 15) & ~15;
      AUDIOPLAY_STATE_T *st;

      // buffer offsets must also be 16 byte aligned for VCHI
      st = calloc(1, sizeof(AUDIOPLAY_STATE_T));

      if(st)
      {
         OMX_ERRORTYPE error;
         OMX_PARAM_PORTDEFINITIONTYPE param;
         OMX_AUDIO_PARAM_PCMMODETYPE pcm;
         int32_t s;

         ret = 0;
         *handle = st;

         // create and start up everything
         s = sem_init(&st->sema, 0, 1);
         assert(s == 0);

         st->pcm_bytes_per_sample = bytes_per_sample;
         st->pcm_num_buffers = num_buffers;

         st->client = ilclient_init();
         assert(st->client != NULL);

         ilclient_set_empty_buffer_done_callback(st->client, input_buffer_callback, st);

         error = OMX_Init();
         assert(error == OMX_ErrorNone);

         ilclient_create_component(st->client, &st->audio_render, "audio_render", ILCLIENT_ENABLE_INPUT_BUFFERS | ILCLIENT_DISABLE_ALL_PORTS);
         assert(st->audio_render != NULL);

         st->list[0] = st->audio_render;

         // set up the number/size of buffers
         memset(&param, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
         param.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
         param.nVersion.nVersion = OMX_VERSION;
         param.nPortIndex = 100;

         error = OMX_GetParameter(ILC_GET_HANDLE(st->audio_render), OMX_IndexParamPortDefinition, &param);
         assert(error == OMX_ErrorNone);

         param.nBufferSize = size;
         param.nBufferCountActual = num_buffers;

         error = OMX_SetParameter(ILC_GET_HANDLE(st->audio_render), OMX_IndexParamPortDefinition, &param);
         assert(error == OMX_ErrorNone);

         // set the pcm parameters
         memset(&pcm, 0, sizeof(OMX_AUDIO_PARAM_PCMMODETYPE));
         pcm.nSize = sizeof(OMX_AUDIO_PARAM_PCMMODETYPE);
         pcm.nVersion.nVersion = OMX_VERSION;
         pcm.nPortIndex = 100;
         pcm.nChannels = num_channels;
         pcm.eNumData = OMX_NumericalDataSigned;
         pcm.eEndian = OMX_EndianLittle;
         pcm.nSamplingRate = sample_rate;
         pcm.bInterleaved = OMX_TRUE;
         pcm.nBitPerSample = bit_depth;
         pcm.ePCMMode = OMX_AUDIO_PCMModeLinear;

         switch(num_channels) {
         case 1:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelCF;
            break;
         case 8:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcm.eChannelMapping[2] = OMX_AUDIO_ChannelCF;
            pcm.eChannelMapping[3] = OMX_AUDIO_ChannelLFE;
            pcm.eChannelMapping[4] = OMX_AUDIO_ChannelLR;
            pcm.eChannelMapping[5] = OMX_AUDIO_ChannelRR;
            pcm.eChannelMapping[6] = OMX_AUDIO_ChannelLS;
            pcm.eChannelMapping[7] = OMX_AUDIO_ChannelRS;
            break;
         case 4:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcm.eChannelMapping[2] = OMX_AUDIO_ChannelLR;
            pcm.eChannelMapping[3] = OMX_AUDIO_ChannelRR;
            break;
         case 2:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            break;
         }

         error = OMX_SetParameter(ILC_GET_HANDLE(st->audio_render), OMX_IndexParamAudioPcm, &pcm);
         assert(error == OMX_ErrorNone);

         ilclient_change_component_state(st->audio_render, OMX_StateIdle);
         if(ilclient_enable_port_buffers(st->audio_render, 100, NULL, NULL, NULL) < 0)
         {
            // error
            ilclient_change_component_state(st->audio_render, OMX_StateLoaded);
            ilclient_cleanup_components(st->list);

            error = OMX_Deinit();
            assert(error == OMX_ErrorNone);

            ilclient_destroy(st->client);

            sem_destroy(&st->sema);
            free(st);
            *handle = NULL;
            return -1;
         }

         ilclient_change_component_state(st->audio_render, OMX_StateExecuting);
      }
   }

   return ret;
}

int32_t mp3_audioplay_create(AUDIOPLAY_STATE_T *st,
                         uint32_t in_num_buffers,
                         uint32_t in_buffer_size,
                         uint32_t out_num_buffers,
                         uint32_t out_buffer_size
                         )
{
   int32_t ret = -1;

   // basic sanity check on arguments
   if(in_num_buffers==0 || in_buffer_size==0 || out_num_buffers==0 || out_buffer_size==0 || st==NULL)
      return -1;
      
   // buffer lengths must be 16 byte aligned for VCHI
   int in_size = (in_buffer_size + 15) & ~15;
   int out_size = (out_buffer_size + 15) & ~15;
   

   OMX_ERRORTYPE error;
   OMX_PARAM_PORTDEFINITIONTYPE param;
   OMX_AUDIO_PARAM_MP3TYPE mp3;

   ret = 0;

// INPUT PORT
         // create and start up everything
///         s = sem_init(&st->sema, 0, 1);
///         assert(s == 0);

         st->mp3_in_num_buffers = in_num_buffers;

///         st->client = ilclient_init();
///         assert(st->client != NULL);

///         ilclient_set_empty_buffer_done_callback(st->client, input_buffer_callback, st);

///         error = OMX_Init();
///         assert(error == OMX_ErrorNone);

ilclient_set_port_settings_callback(st->client, port_settings_callback, st);
ilclient_set_error_callback(st->client, error_callback, st);
ilclient_set_configchanged_callback(st->client, config_changed_callback, st);

         ilclient_create_component(st->client, &st->mp3_decoder, "audio_decode", 
            ILCLIENT_ENABLE_INPUT_BUFFERS | ILCLIENT_ENABLE_OUTPUT_BUFFERS | ILCLIENT_DISABLE_ALL_PORTS);
         assert(st->mp3_decoder != NULL);

         st->list[1] = st->mp3_decoder;

// INPUT PORT PARAMS

         // set up the number/size of buffers
         memset(&param, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
         param.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
         param.nVersion.nVersion = OMX_VERSION;
         param.nPortIndex = 120;

         error = OMX_GetParameter(ILC_GET_HANDLE(st->mp3_decoder), OMX_IndexParamPortDefinition, &param);
         assert(error == OMX_ErrorNone);

         param.nBufferSize = in_size;
         param.nBufferCountActual = in_num_buffers;

         error = OMX_SetParameter(ILC_GET_HANDLE(st->mp3_decoder), OMX_IndexParamPortDefinition, &param);
         assert(error == OMX_ErrorNone);

         // set the mp3 parameters
         
         memset(&mp3, 0, sizeof(OMX_AUDIO_PARAM_PCMMODETYPE));
         mp3.nSize = sizeof(OMX_AUDIO_PARAM_PCMMODETYPE);
         mp3.nVersion.nVersion = OMX_VERSION;
         mp3.nPortIndex = 120;
         mp3.nChannels = 2;
         mp3.nBitRate=0;              // Bit rate of the input data.  Use 0 for variable rate or unknown bit rates
         mp3.nSampleRate=0;           // Sampling rate of the source data.  Use 0 for variable or unknown sampling rate.
         mp3.nAudioBandWidth=0;       // Audio band width (in Hz) to which an encoder should limit the audio signal. Use 0 to let encoder decide
         mp3.eChannelMode=OMX_AUDIO_ChannelModeStereo;   // Channel mode enumeration 
         mp3.eFormat=OMX_AUDIO_MP3StreamFormatMP1Layer3;  // MP3 stream format 

         
         error = OMX_SetParameter(ILC_GET_HANDLE(st->mp3_decoder), OMX_IndexParamAudioMp3, &mp3);
         assert(error == OMX_ErrorNone);
         
// OUTPUT PORT PARAMS

         // set up the number/size of buffers
         memset(&param, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
         param.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
         param.nVersion.nVersion = OMX_VERSION;
         param.nPortIndex = 121;

         error = OMX_GetParameter(ILC_GET_HANDLE(st->mp3_decoder), OMX_IndexParamPortDefinition, &param);
         assert(error == OMX_ErrorNone);

         param.nBufferSize = out_size;
         param.nBufferCountActual = out_num_buffers;

         error = OMX_SetParameter(ILC_GET_HANDLE(st->mp3_decoder), OMX_IndexParamPortDefinition, &param);
         assert(error == OMX_ErrorNone);

// CHANGE STATE


         error = ilclient_change_component_state(st->mp3_decoder, OMX_StateIdle);
	 assert(error == 0);

	 error = ilclient_enable_port_buffers(st->mp3_decoder, 121, NULL, NULL, NULL);
	 assert(error == 0);


	 error = ilclient_enable_port_buffers(st->mp3_decoder, 120, NULL, NULL, NULL);
	 assert(error == 0);


         
         ilclient_change_component_state(st->mp3_decoder, OMX_StateExecuting);
      
   

   return ret;
}

int32_t audioplay_delete(AUDIOPLAY_STATE_T *st)
{
   OMX_ERRORTYPE error;

   ilclient_change_component_state(st->audio_render, OMX_StateIdle);

   error = OMX_SendCommand(ILC_GET_HANDLE(st->audio_render), OMX_CommandStateSet, OMX_StateLoaded, NULL);
   assert(error == OMX_ErrorNone);

   ilclient_disable_port_buffers(st->audio_render, 100, st->user_buffer_list, NULL, NULL);
   ilclient_change_component_state(st->audio_render, OMX_StateLoaded);
   ilclient_cleanup_components(st->list);

   error = OMX_Deinit();
   assert(error == OMX_ErrorNone);

   ilclient_destroy(st->client);

   sem_destroy(&st->sema);
   free(st);

   return 0;
}

uint8_t *audioplay_get_buffer(AUDIOPLAY_STATE_T *st)
{
   OMX_BUFFERHEADERTYPE *hdr = NULL;

   hdr = ilclient_get_input_buffer(st->audio_render, 100, 0);

   if(hdr)
   {
      // put on the user list
      sem_wait(&st->sema);

      hdr->pAppPrivate = st->user_buffer_list;
      st->user_buffer_list = hdr;

      sem_post(&st->sema);
   }

   return hdr ? hdr->pBuffer : NULL;
}

int32_t audioplay_play_buffer(AUDIOPLAY_STATE_T *st,
                              uint8_t *buffer,
                              uint32_t length)
{
   OMX_BUFFERHEADERTYPE *hdr = NULL, *prev = NULL;
   int32_t ret = -1;

   if(length % st->pcm_bytes_per_sample)
      return ret;

   sem_wait(&st->sema);

   // search through user list for the right buffer header
   hdr = st->user_buffer_list;
   while(hdr != NULL && hdr->pBuffer != buffer && hdr->nAllocLen < length)
   {
      prev = hdr;
      hdr = hdr->pAppPrivate;
   }

   if(hdr) // we found it, remove from list
   {
      ret = 0;
      if(prev)
         prev->pAppPrivate = hdr->pAppPrivate;
      else
         st->user_buffer_list = hdr->pAppPrivate;
   }

   sem_post(&st->sema);

   if(hdr)
   {
      OMX_ERRORTYPE error;

      hdr->pAppPrivate = NULL;
      hdr->nOffset = 0;
      hdr->nFilledLen = length;

      error = OMX_EmptyThisBuffer(ILC_GET_HANDLE(st->audio_render), hdr);
      assert(error == OMX_ErrorNone);
   }

   return ret;
}

int32_t audioplay_set_dest(AUDIOPLAY_STATE_T *st, const char *name)
{
   int32_t success = -1;
   OMX_CONFIG_BRCMAUDIODESTINATIONTYPE ar_dest;

   if (name && strlen(name) < sizeof(ar_dest.sName))
   {
      OMX_ERRORTYPE error;
      memset(&ar_dest, 0, sizeof(ar_dest));
      ar_dest.nSize = sizeof(OMX_CONFIG_BRCMAUDIODESTINATIONTYPE);
      ar_dest.nVersion.nVersion = OMX_VERSION;
      strcpy((char *)ar_dest.sName, name);

      error = OMX_SetConfig(ILC_GET_HANDLE(st->audio_render), OMX_IndexConfigBrcmAudioDestination, &ar_dest);
      assert(error == OMX_ErrorNone);
      success = 0;
   }

   return success;
}


uint32_t audioplay_get_latency(AUDIOPLAY_STATE_T *st)
{
   OMX_PARAM_U32TYPE param;
   OMX_ERRORTYPE error;

   memset(&param, 0, sizeof(OMX_PARAM_U32TYPE));
   param.nSize = sizeof(OMX_PARAM_U32TYPE);
   param.nVersion.nVersion = OMX_VERSION;
   param.nPortIndex = 100;

   error = OMX_GetConfig(ILC_GET_HANDLE(st->audio_render), OMX_IndexConfigAudioRenderingLatency, &param);
   assert(error == OMX_ErrorNone);

   return param.nU32;
}

#define CTTW_SLEEP_TIME 10
#define MIN_LATENCY_TIME 20

const int SAMPLERATE[] = {8000, 11025, 44100, 96000};
const int BITDEPTH[] = {16, 32};
const int CHANNELS[] = {1, 2, 4, 8};

static const char *audio_dest[] = {"local", "hdmi"};
void play_api_test(int samplerate, int bitdepth, int nchannels, int dest)
{
   AUDIOPLAY_STATE_T *st;
   int32_t ret;
   unsigned int i, j, n;
   int phase = 0;
   int inc = 256<<16;
   int dinc = 0;
   int buffer_size = (BUFFER_SIZE_SAMPLES * bitdepth * nchannels)>>3;

   assert(dest == 0 || dest == 1);

   ret = pcm_audioplay_create(&st, samplerate, nchannels, bitdepth, 10, buffer_size);
   assert(ret == 0);

   ret = mp3_audioplay_create(st, 10, buffer_size, 10, buffer_size);
   assert(ret == 0);


   ret = audioplay_set_dest(st, audio_dest[dest]);
   assert(ret == 0);

   // iterate for 5 seconds worth of packets
   for (n=0; n<((samplerate * 5)/ BUFFER_SIZE_SAMPLES); n++)
   {
      uint8_t *buf;
      int16_t *p;
      uint32_t latency;

      while((buf = audioplay_get_buffer(st)) == NULL)
         usleep(10*1000);

      p = (int16_t *) buf;

      // fill the buffer
      for (i=0; i<BUFFER_SIZE_SAMPLES; i++)
      {
         int16_t val = SIN(phase);
         phase += inc>>16;
         inc += dinc;
         if (inc>>16 < 512)
            dinc++;
         else
            dinc--;

         for(j=0; j<nchannels; j++)
         {
            if (bitdepth == 32)
               *p++ = 0;
            *p++ = val;
         }
      }

      // try and wait for a minimum latency time (in ms) before
      // sending the next packet
      while((latency = audioplay_get_latency(st)) > (samplerate * (MIN_LATENCY_TIME + CTTW_SLEEP_TIME) / 1000))
         usleep(CTTW_SLEEP_TIME*1000);

      ret = audioplay_play_buffer(st, buf, buffer_size);
      assert(ret == 0);
   }

   audioplay_delete(st);
}

int main (int argc, char **argv)
{
   // 0=headphones, 1=hdmi
   int audio_dest = 0;
   // audio sample rate in Hz
   int samplerate = 48000;
   // numnber of audio channels
   int channels = 2;
   // number of bits per sample
   int bitdepth = 16;
   bcm_host_init();

   if (argc > 1)
      audio_dest = atoi(argv[1]);

   printf("Outputting audio to %s\n", audio_dest==0 ? "analogue":"hdmi");

   play_api_test(samplerate, bitdepth, channels, audio_dest);
   return 0;
}

