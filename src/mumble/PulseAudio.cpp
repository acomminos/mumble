/* Copyright (C) 2005-2011, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "mumble_pch.hpp"

#include "PulseAudio.h"

#include <sys/soundcard.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "Global.h"
#include "MainWindow.h"
#include "Timer.h"
#include "User.h"


static const char *mumble_sink_input = "Mumble Speakers";
static const char *mumble_echo = "Mumble Speakers (Echo)";

static PulseAudioSystem *pasys = NULL;

#define NBLOCKS 8

class PulseAudioInputRegistrar : public AudioInputRegistrar {
	public:
		PulseAudioInputRegistrar();
		virtual AudioInput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &, Settings &);
		virtual bool canEcho(const QString &) const;
};


class PulseAudioOutputRegistrar : public AudioOutputRegistrar {
	public:
		PulseAudioOutputRegistrar();
		virtual AudioOutput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &, Settings &);
		bool canMuteOthers() const;
};

class PulseAudioInit : public DeferInit {
	public:
		PulseAudioInputRegistrar *airPulseAudio;
		PulseAudioOutputRegistrar *aorPulseAudio;
		void initialize() {
			pasys = new PulseAudioSystem();
			pasys->qmWait.lock();
			pasys->qwcWait.wait(&pasys->qmWait, 1000);
			pasys->qmWait.unlock();
			if (pasys->bPulseIsGood) {
				airPulseAudio = new PulseAudioInputRegistrar();
				aorPulseAudio = new PulseAudioOutputRegistrar();
			} else {
				airPulseAudio = NULL;
				aorPulseAudio = NULL;
				delete pasys;
				pasys = NULL;
			}
		};
		void destroy() {
			delete airPulseAudio;
			delete aorPulseAudio;
			delete pasys;
			pasys = NULL;
		};
};

static PulseAudioInit pulseinit;

PulseAudioSystem::PulseAudioSystem() {
	pasInput = pasOutput = pasSpeaker = NULL;
	bSourceDone=bSinkDone=bServerDone = false;
	iDelayCache = 0;
	bPositionalCache = false;
	bAttenuating = false;
	iRemainingOperations = 0;
	bPulseIsGood = false;

	pam = pa_threaded_mainloop_new();
	pa_mainloop_api *api = pa_threaded_mainloop_get_api(pam);

	pa_proplist *proplist;

	proplist = pa_proplist_new();
	pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "Mumble");
	pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "net.sourceforge.mumble.mumble");
	pa_proplist_sets(proplist, PA_PROP_APPLICATION_ICON_NAME, "mumble");
	pa_proplist_sets(proplist, PA_PROP_MEDIA_ROLE, "phone");

	pacContext = pa_context_new_with_proplist(api, NULL, proplist);
	pa_proplist_free(proplist);

	pa_context_set_subscribe_callback(pacContext, subscribe_callback, this);

	pa_context_set_state_callback(pacContext, context_state_callback, this);
	pa_context_connect(pacContext, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);

	pade = api->defer_new(api, defer_event_callback, this);
	api->defer_enable(pade, false);

	pa_threaded_mainloop_start(pam);

	bRunning = true;
}

PulseAudioSystem::~PulseAudioSystem() {
	bRunning = false;
	if (bAttenuating) {
		qmWait.lock();
		bAttenuating = false;
		setVolumes();
		bool success = qwcWait.wait(&qmWait, 1000);
		if (! success) {
			qWarning("PulseAudio: Shutdown timeout when attempting to restore volumes.");
		}
		qmWait.unlock();
	}
	pa_threaded_mainloop_stop(pam);
	pa_context_disconnect(pacContext);
	pa_context_unref(pacContext);
	pa_threaded_mainloop_free(pam);
}

void PulseAudioSystem::wakeup() {
	pa_mainloop_api *api = pa_threaded_mainloop_get_api(pam);
	api->defer_enable(pade, true);
}

void PulseAudioSystem::wakeup_lock() {
	pa_threaded_mainloop_lock(pam);
	pa_mainloop_api *api = pa_threaded_mainloop_get_api(pam);
	api->defer_enable(pade, true);
	pa_threaded_mainloop_unlock(pam);
}

void PulseAudioSystem::defer_event_callback(pa_mainloop_api *a, pa_defer_event *e, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	pas->eventCallback(a, e);
}

void PulseAudioSystem::eventCallback(pa_mainloop_api *api, pa_defer_event *) {
	api->defer_enable(pade, false);

	if (! bSourceDone || ! bSinkDone || ! bServerDone)
		return;

	AudioInputPtr ai = g.ai;
	AudioOutputPtr ao = g.ao;
	AudioInput *raw_ai = ai.get();
	AudioOutput *raw_ao = ao.get();
	PulseAudioInput *pai = dynamic_cast<PulseAudioInput *>(raw_ai);
	PulseAudioOutput *pao = dynamic_cast<PulseAudioOutput *>(raw_ao);

	if (raw_ao) {
		QString odev = g.s.qsPulseAudioOutput.isEmpty() ? qsDefaultOutput : g.s.qsPulseAudioOutput;
		pa_stream_state ost = pasOutput ? pa_stream_get_state(pasOutput) : PA_STREAM_TERMINATED;
		bool do_stop = false;
		bool do_start = false;

		if (! pao && (ost == PA_STREAM_READY)) {
			do_stop = true;
		} else if (pao) {
			switch (ost) {
				case PA_STREAM_TERMINATED: {
						if (pasOutput)
							pa_stream_unref(pasOutput);

						pa_sample_spec pss = qhSpecMap.value(odev);
						pa_channel_map pcm = qhChanMap.value(odev);
						if ((pss.format != PA_SAMPLE_FLOAT32NE) && (pss.format != PA_SAMPLE_S16NE))
							pss.format = PA_SAMPLE_FLOAT32NE;
						if (pss.rate == 0)
							pss.rate = SAMPLE_RATE;
						if ((pss.channels == 0) || (! g.s.doPositionalAudio()))
							pss.channels = 1;

						pasOutput = pa_stream_new(pacContext, mumble_sink_input, &pss, (pss.channels == 1) ? NULL : &pcm);
						pa_stream_set_state_callback(pasOutput, stream_callback, this);
						pa_stream_set_write_callback(pasOutput, write_callback, this);
					}
				case PA_STREAM_UNCONNECTED:
					do_start = true;
					break;
				case PA_STREAM_READY: {
						if (g.s.iOutputDelay != iDelayCache) {
							do_stop = true;
						} else if (g.s.doPositionalAudio() != bPositionalCache) {
							do_stop = true;
						} else if (odev != qsOutputCache) {
							do_stop = true;
						}
						break;
					}
				default:
					break;
			}
		}
		if (do_stop) {
			qWarning("PulseAudio: Stopping output");
			pa_stream_disconnect(pasOutput);
		} else if (do_start) {
			qWarning("PulseAudio: Starting output: %s", qPrintable(odev));
			pa_buffer_attr buff;
			const pa_sample_spec *pss = pa_stream_get_sample_spec(pasOutput);
			const unsigned int iBlockLen = ((pao->iFrameSize * pss->rate) / SAMPLE_RATE) * pss->channels * ((pss->format == PA_SAMPLE_FLOAT32NE) ? sizeof(float) : sizeof(short));
			buff.tlength = iBlockLen * (g.s.iOutputDelay+1);
			buff.minreq = iBlockLen;
			buff.maxlength = -1;
			buff.prebuf = -1;
			buff.fragsize = iBlockLen;

			iDelayCache = g.s.iOutputDelay;
			bPositionalCache = g.s.doPositionalAudio();
			qsOutputCache = odev;

			pa_stream_connect_playback(pasOutput, qPrintable(odev), &buff, PA_STREAM_ADJUST_LATENCY, NULL, NULL);
		}
	}

	if (raw_ai) {
		QString idev = g.s.qsPulseAudioInput.isEmpty() ? qsDefaultInput : g.s.qsPulseAudioInput;
		pa_stream_state ist = pasInput ? pa_stream_get_state(pasInput) : PA_STREAM_TERMINATED;
		bool do_stop = false;
		bool do_start = false;

		if (! pai && (ist == PA_STREAM_READY)) {
			do_stop = true;
		} else if (pai) {
			switch (ist) {
				case PA_STREAM_TERMINATED: {
						if (pasInput)
							pa_stream_unref(pasInput);

						pa_sample_spec pss = qhSpecMap.value(idev);
						if ((pss.format != PA_SAMPLE_FLOAT32NE) && (pss.format != PA_SAMPLE_S16NE))
							pss.format = PA_SAMPLE_FLOAT32NE;
						if (pss.rate == 0)
							pss.rate = SAMPLE_RATE;
						pss.channels = 1;

						pasInput = pa_stream_new(pacContext, "Microphone", &pss, NULL);
						pa_stream_set_state_callback(pasInput, stream_callback, this);
						pa_stream_set_read_callback(pasInput, read_callback, this);
					}
				case PA_STREAM_UNCONNECTED:
					do_start = true;
					break;
				case PA_STREAM_READY: {
						if (idev != qsInputCache) {
							do_stop = true;
						}
						break;
					}
				default:
					break;
			}
		}
		if (do_stop) {
			qWarning("PulseAudio: Stopping input");
			pa_stream_disconnect(pasInput);
		} else if (do_start) {
			qWarning("PulseAudio: Starting input %s",qPrintable(idev));
			pa_buffer_attr buff;
			const pa_sample_spec *pss = pa_stream_get_sample_spec(pasInput);
			const unsigned int iBlockLen = ((pai->iFrameSize * pss->rate) / SAMPLE_RATE) * pss->channels * ((pss->format == PA_SAMPLE_FLOAT32NE) ? sizeof(float) : sizeof(short));
			buff.tlength = iBlockLen;
			buff.minreq = iBlockLen;
			buff.maxlength = -1;
			buff.prebuf = -1;
			buff.fragsize = iBlockLen;

			qsInputCache = idev;

			pa_stream_connect_record(pasInput, qPrintable(idev), &buff, PA_STREAM_ADJUST_LATENCY);
		}
	}

	if (raw_ai) {
		QString odev = g.s.qsPulseAudioOutput.isEmpty() ? qsDefaultOutput : g.s.qsPulseAudioOutput;
		QString edev = qhEchoMap.value(odev);
		pa_stream_state est = pasSpeaker ? pa_stream_get_state(pasSpeaker) : PA_STREAM_TERMINATED;
		bool do_stop = false;
		bool do_start = false;

		if ((! pai || ! g.s.doEcho()) && (est == PA_STREAM_READY)) {
			do_stop = true;
		} else if (pai && g.s.doEcho()) {
			switch (est) {
				case PA_STREAM_TERMINATED: {
						if (pasSpeaker)
							pa_stream_unref(pasSpeaker);

						pa_sample_spec pss = qhSpecMap.value(edev);
						pa_channel_map pcm = qhChanMap.value(edev);
						if ((pss.format != PA_SAMPLE_FLOAT32NE) && (pss.format != PA_SAMPLE_S16NE))
							pss.format = PA_SAMPLE_FLOAT32NE;
						if (pss.rate == 0)
							pss.rate = SAMPLE_RATE;
						if ((pss.channels == 0) || (! g.s.bEchoMulti))
							pss.channels = 1;

						pasSpeaker = pa_stream_new(pacContext, mumble_echo, &pss, (pss.channels == 1) ? NULL : &pcm);
						pa_stream_set_state_callback(pasSpeaker, stream_callback, this);
						pa_stream_set_read_callback(pasSpeaker, read_callback, this);
					}
				case PA_STREAM_UNCONNECTED:
					do_start = true;
					break;
				case PA_STREAM_READY: {
						if (g.s.bEchoMulti != bEchoMultiCache) {
							do_stop = true;
						} else if (edev != qsEchoCache) {
							do_stop = true;
						}
						break;
					}
				default:
					break;
			}
		}
		if (do_stop) {
			qWarning("PulseAudio: Stopping echo");
			pa_stream_disconnect(pasSpeaker);
		} else if (do_start) {
			qWarning("PulseAudio: Starting echo: %s", qPrintable(edev));
			pa_buffer_attr buff;
			const pa_sample_spec *pss = pa_stream_get_sample_spec(pasSpeaker);
			const unsigned int iBlockLen = ((pai->iFrameSize * pss->rate) / SAMPLE_RATE) * pss->channels * ((pss->format == PA_SAMPLE_FLOAT32NE) ? sizeof(float) : sizeof(short));
			buff.tlength = iBlockLen;
			buff.minreq = iBlockLen;
			buff.maxlength = -1;
			buff.prebuf = -1;
			buff.fragsize = iBlockLen;

			bEchoMultiCache = g.s.bEchoMulti;
			qsEchoCache = edev;

			pa_stream_connect_record(pasSpeaker, qPrintable(edev), &buff, PA_STREAM_ADJUST_LATENCY);
		}
	}
}

void PulseAudioSystem::context_state_callback(pa_context *c, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	pas->contextCallback(c);
}

void PulseAudioSystem::subscribe_callback(pa_context *, pa_subscription_event_type evt, unsigned int, void *userdata) {
	switch (evt & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {
		case PA_SUBSCRIPTION_EVENT_NEW:
		case PA_SUBSCRIPTION_EVENT_REMOVE:
			break;
		default:
			return;
	}
	switch (evt & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
		case PA_SUBSCRIPTION_EVENT_SINK:
		case PA_SUBSCRIPTION_EVENT_SOURCE:
			break;
		default:
			return;
	}
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	qWarning("PulseAudio: Sinks or inputs changed (inserted or removed sound card)");
	pas->query();
}

void PulseAudioSystem::sink_callback(pa_context *, const pa_sink_info *i, int eol, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	if (!i || eol) {
		pas->bSinkDone = true;
		pas->wakeup();
		return;
	}

	const QString name = QLatin1String(i->name);

	pas->qhSpecMap.insert(name, i->sample_spec);
	pas->qhChanMap.insert(name, i->channel_map);
	pas->qhOutput.insert(name, QLatin1String(i->description));
	pas->qhEchoMap.insert(name, QLatin1String(i->monitor_source_name));
}

void PulseAudioSystem::source_callback(pa_context *, const pa_source_info *i, int eol, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	if (!i || eol) {
		pas->bSourceDone = true;
		pas->wakeup();
		return;
	}

	const QString name = QLatin1String(i->name);

	pas->qhSpecMap.insert(name, i->sample_spec);
	pas->qhChanMap.insert(name, i->channel_map);

	if (i->monitor_of_sink == PA_INVALID_INDEX)
		pas->qhInput.insert(QLatin1String(i->name), QLatin1String(i->description));
}

void PulseAudioSystem::server_callback(pa_context *, const pa_server_info *i, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);

	pas->qsDefaultInput = QLatin1String(i->default_source_name);
	pas->qsDefaultOutput = QLatin1String(i->default_sink_name);

	pas->bServerDone = true;
	pas->wakeup();
}

void PulseAudioSystem::stream_callback(pa_stream *s, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	switch (pa_stream_get_state(s)) {
		case PA_STREAM_FAILED:
			qWarning("PulseAudio: Stream error: %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
			break;
		default:
			break;
	}
	pas->wakeup();
}

void PulseAudioSystem::read_callback(pa_stream *s, size_t bytes, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);

	size_t length = bytes;
	const void *data = NULL;
	pa_stream_peek(s, &data, &length);
	if (data == NULL && length > 0) {
		qWarning("PulseAudio: pa_stream_peek reports no data at current read index.");
	} else if (data == NULL && length == 0) {
		qWarning("PulseAudio: pa_stream_peek reports empty memblockq.");
	} else if (data == NULL || length == 0) {
		qWarning("PulseAudio: invalid pa_stream_peek state encountered.");
		return;
	}

	AudioInputPtr ai = g.ai;
	PulseAudioInput *pai = dynamic_cast<PulseAudioInput *>(ai.get());
	if (! pai) {
		if (length > 0) {
			pa_stream_drop(s);
		}
		pas->wakeup();
		return;
	}

	const pa_sample_spec *pss = pa_stream_get_sample_spec(s);

	if (s == pas->pasInput) {
		if (!pa_sample_spec_equal(pss, &pai->pssMic)) {
			pai->pssMic = *pss;
			pai->iMicFreq = pss->rate;
			pai->iMicChannels = pss->channels;
			if (pss->format == PA_SAMPLE_FLOAT32NE)
				pai->eMicFormat = PulseAudioInput::SampleFloat;
			else
				pai->eMicFormat = PulseAudioInput::SampleShort;
			pai->initializeMixer();
		}
		if (data != NULL) {
			pai->addMic(data, length / pai->iMicSampleSize);
		}
	} else if (s == pas->pasSpeaker) {
		if (!pa_sample_spec_equal(pss, &pai->pssEcho)) {
			pai->pssEcho = *pss;
			pai->iEchoFreq = pss->rate;
			pai->iEchoChannels = pss->channels;
			if (pss->format == PA_SAMPLE_FLOAT32NE)
				pai->eEchoFormat = PulseAudioInput::SampleFloat;
			else
				pai->eEchoFormat = PulseAudioInput::SampleShort;
			pai->initializeMixer();
		}
		if (data != NULL) {
			pai->addEcho(data, length / pai->iEchoSampleSize);
		}
	}

	if (length > 0) {
		pa_stream_drop(s);
	}
}

void PulseAudioSystem::write_callback(pa_stream *s, size_t bytes, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);
	Q_ASSERT(s == pas->pasOutput);

	AudioOutputPtr ao = g.ao;
	PulseAudioOutput *pao = dynamic_cast<PulseAudioOutput *>(ao.get());

	unsigned char buffer[bytes];

	if (! pao) {
		// Transitioning, but most likely transitions back, so just zero.
		memset(buffer, 0, bytes);
		pa_stream_write(s, buffer, bytes, NULL, 0, PA_SEEK_RELATIVE);
		pas->wakeup();
		return;
	}

	const pa_sample_spec *pss = pa_stream_get_sample_spec(s);
	const pa_channel_map *pcm = pa_stream_get_channel_map(pas->pasOutput);
	if (!pa_sample_spec_equal(pss, &pao->pss) || !pa_channel_map_equal(pcm, &pao->pcm)) {
		pao->pss = *pss;
		pao->pcm = *pcm;
		if (pss->format == PA_SAMPLE_FLOAT32NE)
			pao->eSampleFormat = PulseAudioOutput::SampleFloat;
		else
			pao->eSampleFormat = PulseAudioOutput::SampleShort;
		pao->iMixerFreq = pss->rate;
		pao->iChannels = pss->channels;
		unsigned int chanmasks[pss->channels];
		for (int i=0;i<pss->channels;++i) {
			unsigned int cm = 0;
			switch (pcm->map[i]) {
				case PA_CHANNEL_POSITION_LEFT:
					cm = SPEAKER_FRONT_LEFT;
					break;
				case PA_CHANNEL_POSITION_RIGHT:
					cm = SPEAKER_FRONT_RIGHT;
					break;
				case PA_CHANNEL_POSITION_CENTER:
					cm = SPEAKER_FRONT_CENTER;
					break;
				case PA_CHANNEL_POSITION_REAR_LEFT:
					cm = SPEAKER_BACK_LEFT;
					break;
				case PA_CHANNEL_POSITION_REAR_RIGHT:
					cm = SPEAKER_BACK_RIGHT;
					break;
				case PA_CHANNEL_POSITION_REAR_CENTER:
					cm = SPEAKER_BACK_CENTER;
					break;
				case PA_CHANNEL_POSITION_LFE:
					cm = SPEAKER_LOW_FREQUENCY;
					break;
				case PA_CHANNEL_POSITION_SIDE_LEFT:
					cm = SPEAKER_SIDE_LEFT;
					break;
				case PA_CHANNEL_POSITION_SIDE_RIGHT:
					cm = SPEAKER_SIDE_RIGHT;
					break;
				case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:
					cm = SPEAKER_FRONT_LEFT_OF_CENTER;
					break;
				case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER:
					cm = SPEAKER_FRONT_RIGHT_OF_CENTER;
					break;
				default:
					cm = 0;
					break;
			}
			chanmasks[i] = cm;
		}
		pao->initializeMixer(chanmasks);
	}

	const unsigned int iSampleSize = pao->iSampleSize;
	const unsigned int samples = bytes / iSampleSize;
	bool oldAttenuation = pas->bAttenuating;

	// do we have some mixed output?
	if (pao->mix(buffer, samples)) {
		// attenuate if instructed to or it's in settings
		pas->bAttenuating = (g.bAttenuateOthers || g.s.bAttenuateOthers);

	} else {
		memset(buffer, 0, bytes);

		// attenuate if intructed to (self-activated)
		pas->bAttenuating = g.bAttenuateOthers;
	}

	// if the attenuation state has changed
	if (oldAttenuation != pas->bAttenuating) {
		pas->setVolumes();
	}

	pa_stream_write(s, buffer, iSampleSize * samples, NULL, 0, PA_SEEK_RELATIVE);
}

void PulseAudioSystem::volume_sink_input_list_callback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);

	if (eol == 0) {
		// ensure we're not attenuating ourselves!
		if (strcmp(i->name, mumble_sink_input) != 0) {
			// create a new entry
			PulseAttenuation patt;
			patt.index = i->index;
			patt.name = QLatin1String(i->name);
			patt.stream_restore_id = QLatin1String(pa_proplist_gets(i->proplist, "module-stream-restore.id"));
			patt.normal_volume = i->volume;

			// calculate the attenuated volume
			pa_volume_t adj = static_cast<pa_volume_t>(PA_VOLUME_NORM * g.s.fOtherVolume);
			pa_sw_cvolume_multiply_scalar(&patt.attenuated_volume, &i->volume, adj);

			// set it on the sink input
			pa_operation_unref(pa_context_set_sink_input_volume(c, i->index, &patt.attenuated_volume, NULL, NULL));

			// store it
			pas->qhVolumes[i->index] = patt;
		}

	} else if (eol < 0) {
		qWarning("PulseAudio: Sink input introspection error.");
	}
}

void PulseAudioSystem::restore_sink_input_list_callback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);

	if (eol == 0) {
		// if we were tracking this specific sink previously
		if (pas->qhVolumes.contains(i->index)) {
			// and if it has the attenuated volume we applied to it
			if (pa_cvolume_equal(&i->volume, &pas->qhVolumes[i->index].attenuated_volume) != 0) {
				// mark it as matched
				pas->qlMatchedSinks.append(i->index);

				// reset the volume to normal
				pas->iRemainingOperations++;
				pa_operation_unref(pa_context_set_sink_input_volume(c, i->index, &pas->qhVolumes[i->index].normal_volume, restore_volume_success_callback, pas));
			}

		// otherwise, save for matching at the end of iteration
		} else {
			QString restore_id = QLatin1String(pa_proplist_gets(i->proplist, "module-stream-restore.id"));
			PulseAttenuation patt;
			patt.index = i->index;
			patt.normal_volume = i->volume;
			pas->qhUnmatchedSinks[restore_id] = patt;
		}

	} else if (eol < 0) {
		qWarning("PulseAudio: Sink input introspection error.");

	} else {
		// build a list of missing streams by iterating our active list
		QHash<uint32_t, PulseAttenuation>::const_iterator it;
		for (it = pas->qhVolumes.constBegin(); it != pas->qhVolumes.constEnd(); ++it) {
			// skip if previously matched
			if (pas->qlMatchedSinks.contains(it.key())) {
				continue;
			}

			// check if the restore id matches. the only case where this would
			// happen is if the application was reopened during attenuation.
			if (pas->qhUnmatchedSinks.contains(it.value().stream_restore_id)) {
				PulseAttenuation active_sink = pas->qhUnmatchedSinks[it.value().stream_restore_id];
				// if the volume wasn't changed from our attenuation
				if (pa_cvolume_equal(&active_sink.normal_volume, &it.value().attenuated_volume) != 0) {
					// reset the volume to normal
					pas->iRemainingOperations++;
					pa_operation_unref(pa_context_set_sink_input_volume(c, active_sink.index, &it.value().normal_volume, restore_volume_success_callback, pas));
				}
				continue;
			}

			// at this point, we don't know what happened to the sink. add
			// it to a list to check the stream restore database for.
			pas->qhMissingSinks[it.value().stream_restore_id] = it.value();
		}

		// clean up
		pas->qlMatchedSinks.clear();
		pas->qhUnmatchedSinks.clear();
		pas->qhVolumes.clear();

		// if we had missing sinks, check the stream restore database
		// to see if we can find and update them.
		if (pas->qhMissingSinks.count() > 0) {
			pas->iRemainingOperations++;
			pa_operation_unref(pa_ext_stream_restore_read(c, stream_restore_read_callback, pas));
		}

		// trigger the volume completion callback;
		// necessary so that shutdown actions are called
		restore_volume_success_callback(c, 1, pas);
	}
}

void PulseAudioSystem::stream_restore_read_callback(pa_context *c, const pa_ext_stream_restore_info *i, int eol, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);

	if (eol == 0) {
		QString name = QLatin1String(i->name);

		// were we looking for this restoration?
		if (pas->qhMissingSinks.contains(name)) {
			// make sure it still has the volume we gave it
			if (pa_cvolume_equal(&pas->qhMissingSinks[name].attenuated_volume, &i->volume) != 0) {
				// update the stream restore record
				pa_ext_stream_restore_info restore = *i;
				restore.volume = pas->qhMissingSinks[name].normal_volume;
				pas->iRemainingOperations++;
				pa_operation_unref(pa_ext_stream_restore_write(c, PA_UPDATE_REPLACE, &restore, 1, 1, restore_volume_success_callback, pas));
			}

			pas->qhMissingSinks.remove(name);
		}

	} else if (eol < 0) {
		qWarning("PulseAudio: Couldn't read stream restore database.");
		pas->qhMissingSinks.clear();

	} else {
		// verify missing list is empty
		if (pas->qhMissingSinks.count() > 0) {
			qWarning("PulseAudio: Failed to match %d stream(s).", pas->qhMissingSinks.count());
			pas->qhMissingSinks.clear();
		}

		// trigger the volume completion callback;
		// necessary so that shutdown actions are called
		restore_volume_success_callback(c, 1, pas);
	}
}

void PulseAudioSystem::restore_volume_success_callback(pa_context *c, int success, void *userdata) {
	PulseAudioSystem *pas = reinterpret_cast<PulseAudioSystem *>(userdata);

	pas->iRemainingOperations--;

	// if there are no more pending volume adjustments and we're shutting down,
	// let the main thread know
	if (! pas->bRunning && pas->iRemainingOperations == 0) {
		pas->qwcWait.wakeAll();
	}
}

void PulseAudioSystem::query() {
	bSourceDone=bSinkDone=bServerDone = false;
	qhInput.clear();
	qhOutput.clear();
	qhEchoMap.clear();
	qhSpecMap.clear();
	qhChanMap.clear();
	qhInput.insert(QString(), tr("Default Input"));
	qhOutput.insert(QString(), tr("Default Output"));
	pa_operation_unref(pa_context_get_server_info(pacContext, server_callback, this));
	pa_operation_unref(pa_context_get_sink_info_list(pacContext, sink_callback, this));
	pa_operation_unref(pa_context_get_source_info_list(pacContext, source_callback, this));
	wakeup();
}

void PulseAudioSystem::setVolumes() {
	// set attenuation state and volumes
	if (bAttenuating) {
		// ensure the volume map is empty, otherwise it may be dangerous to change
		if (qhVolumes.empty()) {
			// set the new per-application volumes and store the old ones
			pa_operation_unref(pa_context_get_sink_input_info_list(pacContext, volume_sink_input_list_callback, this));
		}
	// clear attenuation state and restore normal volumes
	} else {
		iRemainingOperations++;
		pa_operation_unref(pa_context_get_sink_input_info_list(pacContext, restore_sink_input_list_callback, this));
	}
}

void PulseAudioSystem::contextCallback(pa_context *c) {
	Q_ASSERT(c == pacContext);
	switch (pa_context_get_state(c)) {
		case PA_CONTEXT_READY:
			bPulseIsGood = true;
			pa_operation_unref(pa_context_subscribe(pacContext, PA_SUBSCRIPTION_MASK_SOURCE, NULL, this));
			pa_operation_unref(pa_context_subscribe(pacContext, PA_SUBSCRIPTION_MASK_SINK, NULL, this));
			query();
			break;
		case PA_CONTEXT_TERMINATED:
			qWarning("PulseAudio: Forcibly disconnected from PulseAudio");
			break;
		case PA_CONTEXT_FAILED:
			qWarning("PulseAudio: Connection failure: %s", pa_strerror(pa_context_errno(c)));
			break;
		default:
			return;
	}
	qmWait.lock();
	qwcWait.wakeAll();
	qmWait.unlock();
}

PulseAudioInputRegistrar::PulseAudioInputRegistrar() : AudioInputRegistrar(QLatin1String("PulseAudio"), 10) {
}

AudioInput *PulseAudioInputRegistrar::create() {
	return new PulseAudioInput();
}

const QList<audioDevice> PulseAudioInputRegistrar::getDeviceChoices() {
	QList<audioDevice> qlReturn;

	QStringList qlInputDevs = pasys->qhInput.keys();
	qSort(qlInputDevs);

	if (qlInputDevs.contains(g.s.qsPulseAudioInput)) {
		qlInputDevs.removeAll(g.s.qsPulseAudioInput);
		qlInputDevs.prepend(g.s.qsPulseAudioInput);
	}

	foreach(const QString &dev, qlInputDevs) {
		qlReturn << audioDevice(pasys->qhInput.value(dev), dev);
	}

	return qlReturn;
}

void PulseAudioInputRegistrar::setDeviceChoice(const QVariant &choice, Settings &s) {
	s.qsPulseAudioInput = choice.toString();
}

bool PulseAudioInputRegistrar::canEcho(const QString &osys) const {
	return (osys == name);
}

PulseAudioOutputRegistrar::PulseAudioOutputRegistrar() : AudioOutputRegistrar(QLatin1String("PulseAudio"), 10) {
}

AudioOutput *PulseAudioOutputRegistrar::create() {
	return new PulseAudioOutput();
}

const QList<audioDevice> PulseAudioOutputRegistrar::getDeviceChoices() {
	QList<audioDevice> qlReturn;

	QStringList qlOutputDevs = pasys->qhOutput.keys();
	qSort(qlOutputDevs);

	if (qlOutputDevs.contains(g.s.qsPulseAudioOutput)) {
		qlOutputDevs.removeAll(g.s.qsPulseAudioOutput);
		qlOutputDevs.prepend(g.s.qsPulseAudioOutput);
	}

	foreach(const QString &dev, qlOutputDevs) {
		qlReturn << audioDevice(pasys->qhOutput.value(dev), dev);
	}

	return qlReturn;
}

void PulseAudioOutputRegistrar::setDeviceChoice(const QVariant &choice, Settings &s) {
	s.qsPulseAudioOutput = choice.toString();
}

bool PulseAudioOutputRegistrar::canMuteOthers() const {
	return true;
}

PulseAudioInput::PulseAudioInput() {
	memset(&pssMic, 0, sizeof(pssMic));
	memset(&pssEcho, 0, sizeof(pssEcho));
	bRunning = true;
	if (pasys)
		pasys->wakeup_lock();
}

PulseAudioInput::~PulseAudioInput() {
	bRunning = false;
	qmMutex.lock();
	qwcWait.wakeAll();
	qmMutex.unlock();
	wait();
	if (pasys)
		pasys->wakeup_lock();
}

void PulseAudioInput::run() {
	qmMutex.lock();
	while (bRunning)
		qwcWait.wait(&qmMutex);
	qmMutex.unlock();
}

PulseAudioOutput::PulseAudioOutput() {
	memset(&pss, 0, sizeof(pss));
	memset(&pcm, 0, sizeof(pcm));
	bRunning = true;
	if (pasys)
		pasys->wakeup_lock();
}

PulseAudioOutput::~PulseAudioOutput() {
	bRunning = false;
	qmMutex.lock();
	qwcWait.wakeAll();
	qmMutex.unlock();
	wait();
	if (pasys)
		pasys->wakeup_lock();
}

void PulseAudioOutput::run() {
	qmMutex.lock();
	while (bRunning)
		qwcWait.wait(&qmMutex);
	qmMutex.unlock();
}
