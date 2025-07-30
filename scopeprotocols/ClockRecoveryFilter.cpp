/***********************************************************************************************************************
*                                                                                                                      *
* libscopeprotocols                                                                                                    *
*                                                                                                                      *
* Copyright (c) 2012-2025 Andrew D. Zonenberg and contributors                                                         *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

#include "../scopehal/scopehal.h"
#include "ClockRecoveryFilter.h"

#ifdef __x86_64__
#include <immintrin.h>
#endif

using namespace std;

//#define PLL_DEBUG_OUTPUTS

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ClockRecoveryFilter::ClockRecoveryFilter(const string& color)
	: Filter(color, CAT_CLOCK)
{
	AddDigitalStream("data");
	CreateInput("IN");
	CreateInput("Gate");

	m_baudname = "Symbol rate";
	m_parameters[m_baudname] = FilterParameter(FilterParameter::TYPE_FLOAT, Unit(Unit::UNIT_HZ));
	m_parameters[m_baudname].SetFloatVal(1250000000);	//1.25 Gbps

	m_threshname = "Threshold";
	m_parameters[m_threshname] = FilterParameter(FilterParameter::TYPE_FLOAT, Unit(Unit::UNIT_VOLTS));
	m_parameters[m_threshname].SetFloatVal(0);

	#ifdef PLL_DEBUG_OUTPUTS
	AddStream(Unit::UNIT_FS, "period", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit::UNIT_FS, "dphase", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit::UNIT_FS, "dfreq", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit::UNIT_FS, "drift", Stream::STREAM_TYPE_ANALOG);
	#endif
}

ClockRecoveryFilter::~ClockRecoveryFilter()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Factory methods

bool ClockRecoveryFilter::ValidateChannel(size_t i, StreamDescriptor stream)
{
	switch(i)
	{
		case 0:
			if(stream.m_channel == NULL)
				return false;
			return
				(stream.GetType() == Stream::STREAM_TYPE_ANALOG) ||
				(stream.GetType() == Stream::STREAM_TYPE_DIGITAL);

		case 1:
			if(stream.m_channel == NULL)	//null is legal for gate
				return true;

			return (stream.GetType() == Stream::STREAM_TYPE_DIGITAL);

		default:
			return false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

string ClockRecoveryFilter::GetProtocolName()
{
	return "Clock Recovery (PLL)";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void ClockRecoveryFilter::Refresh(
	[[maybe_unused]] vk::raii::CommandBuffer& cmdBuf,
	[[maybe_unused]] shared_ptr<QueueHandle> queue)
{
	//Require a data signal, but not necessarily a gate
	if(!VerifyInputOK(0))
	{
		SetData(nullptr, 0);
		return;
	}

	auto din = GetInputWaveform(0);
	din->PrepareForCpuAccess();

	auto uadin = dynamic_cast<UniformAnalogWaveform*>(din);
	auto sadin = dynamic_cast<SparseAnalogWaveform*>(din);
	auto uddin = dynamic_cast<UniformDigitalWaveform*>(din);
	auto sddin = dynamic_cast<SparseDigitalWaveform*>(din);
	auto gate = GetInputWaveform(1);
	auto sgate = dynamic_cast<SparseDigitalWaveform*>(gate);
	auto ugate = dynamic_cast<UniformDigitalWaveform*>(gate);
	if(gate)
		gate->PrepareForCpuAccess();

	//Timestamps of the edges
	vector<int64_t> edges;
	if(uadin)
		FindZeroCrossings(uadin, m_parameters[m_threshname].GetFloatVal(), edges);
	else if(sadin)
		FindZeroCrossings(sadin, m_parameters[m_threshname].GetFloatVal(), edges);
	else if(uddin)
		FindZeroCrossings(uddin, edges);
	else if(sddin)
		FindZeroCrossings(sddin, edges);
	if(edges.empty())
	{
		SetData(nullptr, 0);
		return;
	}

	//Get nominal period used for the first cycle of the NCO
	int64_t initialPeriod = round(FS_PER_SECOND / m_parameters[m_baudname].GetFloatVal());
	int64_t halfPeriod = initialPeriod / 2;

	//Disallow frequencies higher than Nyquist of the input
	int64_t fnyquist = 2*din->m_timescale;
	if( initialPeriod < fnyquist)
	{
		SetData(nullptr, 0);
		return;
	}

	//Create the output waveform and copy our timescales
	auto cap = SetupEmptySparseDigitalOutputWaveform(din, 0);
	cap->m_triggerPhase = 0;
	cap->m_timescale = 1;		//recovered clock time scale is single femtoseconds
	cap->PrepareForCpuAccess();

	int64_t tend;
	if(sadin || uadin)
		tend = GetOffsetScaled(sadin, uadin, din->size()-1);
	else
		tend = GetOffsetScaled(sddin, uddin, din->size()-1);

	#ifdef PLL_DEBUG_OUTPUTS
	auto debugPeriod = SetupEmptySparseAnalogOutputWaveform(cap, 1);
	auto debugPhase = SetupEmptySparseAnalogOutputWaveform(cap, 2);
	auto debugFreq = SetupEmptySparseAnalogOutputWaveform(cap, 3);
	auto debugDrift = SetupEmptySparseAnalogOutputWaveform(cap, 4);
	#endif

	//The actual PLL NCO
	//TODO: use the real fibre channel PLL.
	cap->m_offsets.reserve(edges.size());
	if(gate)
		InnerLoopWithGating(*cap, edges, tend, initialPeriod, halfPeriod, fnyquist, gate, sgate, ugate);
	else
		InnerLoopWithNoGating(*cap, edges, tend, initialPeriod, halfPeriod, fnyquist);

	//Generate the squarewave and duration values to match the calculated timestamps
	//TODO: GPU this?
	//Important to FillDurations() after FillSquarewave() since FillDurations() expects to use sample size
	#ifdef __x86_64__
	if(g_hasAvx2)
	{
		FillSquarewaveAVX2(*cap);
		FillDurationsAVX2(*cap);
	}
	else
	#endif
	{
		FillSquarewaveGeneric(*cap);
		FillDurationsGeneric(*cap);
	}

	SetData(cap, 0);

	cap->MarkModifiedFromCpu();
}

/**
	@brief Fills a waveform with a squarewave
 */
void ClockRecoveryFilter::FillSquarewaveGeneric(SparseDigitalWaveform& cap)
{
	size_t len = cap.m_offsets.size();
	cap.m_samples.resize(len);

	bool value = false;
	for(size_t i=0; i<len; i++)
	{
		value = !value;
		cap.m_samples[i] = value;
	}
}

/**
	@brief Main PLL inner loop supporting an external gate/squelch signal
 */
void ClockRecoveryFilter::InnerLoopWithGating(
	SparseDigitalWaveform& cap,
	vector<int64_t>& edges,
	int64_t tend,
	int64_t initialPeriod,
	int64_t halfPeriod,
	int64_t fnyquist,
	WaveformBase* gate,
	SparseDigitalWaveform* sgate,
	UniformDigitalWaveform* ugate)
{
	size_t igate = 0;
	size_t nedge = 1;
	int64_t edgepos = edges[0];
	int64_t period = initialPeriod;

	[[maybe_unused]] int64_t total_error = 0;

	//If gated at T=0, start with output stopped
	bool gating = false;
	if(gate && gate->size())
		gating = !GetValue(sgate, ugate, 0);

	int64_t tlast = 0;
	for(; (edgepos < tend) && (nedge < edges.size()-1); edgepos += period)
	{
		float center = period/2;

		//See if the current edge position is within a gating region
		bool was_gating = gating;
		if(gate != nullptr)
		{
			while(igate < gate->size()-1)
			{
				//See if this edge is within the region
				int64_t a = GetOffsetScaled(sgate, ugate, igate);
				int64_t b = a + GetDurationScaled(sgate, ugate, igate);

				//We went too far, stop
				if(edgepos < a)
					break;

				//Keep looking
				else if(edgepos > b)
					igate ++;

				//Good alignment
				else
				{
					gating = !GetValue(sgate, ugate, igate);

					//If the clock just got ungated, reset the PLL
					if(!gating && was_gating)
					{
						LogTrace("CDR ungated (at %s)\n", Unit(Unit::UNIT_FS).PrettyPrint(edgepos).c_str());
						LogIndenter li;

						//Find the median pulse width in the next few edges
						//(this is likely either our UI width or an integer multiple thereof)
						vector<int64_t> lengths;
						for(size_t i=1; i<=512; i++)
						{
							if(i + nedge >= edges.size())
								break;
							lengths.push_back(edges[nedge+i] - edges[nedge+i-1]);
						}
						std::sort(lengths.begin(), lengths.end());
						auto median = lengths[lengths.size() / 2];
						LogTrace("Median of next %zu edges: %s\n",
							lengths.size(),
							Unit(Unit::UNIT_FS).PrettyPrint(median).c_str());

						//TODO: consider if this might be a multi bit period, rather than the fundamental,
						//depending on the line coding in use? (e.g. TMDS)

						//Look up/down and average everything kinda close to the median (within 25%)
						int64_t sum = 0;
						int64_t navg = 0;
						for(auto w : lengths)
						{
							if( (w >= 0.75*median) && (w <= 1.25*median) )
							{
								sum += w;
								navg ++;
							}
						}
						int64_t avg = sum / navg;
						LogTrace("Average of %lld edges near median: %s\n",
							(long long)navg,
							Unit(Unit::UNIT_FS).PrettyPrint(avg).c_str());

						//For now, assume that this length is our actual pulse width and use it as our period
						period = avg;
						initialPeriod = period;
						halfPeriod = initialPeriod / 2;

						//Align exactly to the next edge
						int64_t tnext = edges[nedge];
						edgepos = tnext + period;
					}

					break;
				}
			}
		}

		//See if the next edge occurred in this UI.
		//If not, just run the NCO open loop.
		//Allow multiple edges in the UI if the frequency is way off.
		int64_t tnext = edges[nedge];
		while( (tnext + center < edgepos) && (nedge+1 < edges.size()) )
		{
			if(!gating)
			{
				//Find phase error
				int64_t dphase = (edgepos - tnext) - period;

				//If we're more than half a UI off, assume this is actually part of the next UI
				if(dphase > halfPeriod)
					dphase -= period;
				if(dphase < -halfPeriod)
					dphase += period;

				total_error += i64abs(dphase);

				//Find frequency error
				int64_t uiLen = (tnext - tlast);
				float numUIs = round(uiLen * 1.0 / initialPeriod);
				if(numUIs < 0.1)		//Sanity check: no correction if we have a glitch
					uiLen = period;
				else
					uiLen /= numUIs;
				int64_t dperiod = period - uiLen;

				if(tlast != 0)
				{
					//Frequency error term
					period -= dperiod * 0.006;

					//Frequency drift term (delta from refclk)
					//period -= (period - initialPeriod) * 0.0001;

					//Phase error term
					period -= dphase * 0.002;

					//HACK: immediate bang-bang phase shift
					if(dphase > 0)
						edgepos -= period / 400;
					else
						edgepos += period / 400;

					#ifdef PLL_DEBUG_OUTPUTS
						debugPeriod->m_offsets.push_back(edgepos + period/2);
						debugPeriod->m_durations.push_back(period);
						debugPeriod->m_samples.push_back(period);

						debugPhase->m_offsets.push_back(edgepos + period/2);
						debugPhase->m_durations.push_back(period);
						debugPhase->m_samples.push_back(dphase);

						debugFreq->m_offsets.push_back(edgepos + period/2);
						debugFreq->m_durations.push_back(period);
						debugFreq->m_samples.push_back(dperiod);

						debugDrift->m_offsets.push_back(edgepos + period/2);
						debugDrift->m_durations.push_back(period);
						debugDrift->m_samples.push_back(period - initialPeriod);
					#endif

					if(period < fnyquist)
					{
						LogWarning("PLL attempted to lock to frequency near or above Nyquist\n");
						nedge = edges.size();
						break;
					}
				}
			}

			tlast = tnext;
			tnext = edges[++nedge];
		}

		//Add the sample (90 deg phase offset from the internal NCO)
		if(!gating)
			cap.m_offsets.push_back(edgepos + period/2);
	}

	total_error /= edges.size();
	//LogTrace("average phase error %zu\n", total_error);
}

void ClockRecoveryFilter::InnerLoopWithNoGating(
	SparseDigitalWaveform& cap,
	vector<int64_t>& edges,
	int64_t tend,
	int64_t initialPeriod,
	int64_t halfPeriod,
	int64_t fnyquist)
{
	size_t nedge = 1;
	int64_t edgepos = edges[0];

	//[[maybe_unused]] int64_t total_error = 0;

	float initialFrequency = 1.0 / initialPeriod;
	int64_t glitchCutoff = initialPeriod / 10;
	size_t edgemax = edges.size() - 1;
	float fHalfPeriod = halfPeriod;

	int64_t tlast = 0;
	int64_t iperiod = initialPeriod;
	float fperiod = iperiod;
	for(; (edgepos < tend) && (nedge < edgemax); edgepos += iperiod)
	{
		int64_t center = iperiod/2;

		//See if the next edge occurred in this UI.
		//If not, just run the NCO open loop.
		//Allow multiple edges in the UI if the frequency is way off.
		int64_t tnext = edges[nedge];
		while( (tnext + center < edgepos) && (nedge < edgemax) )
		{
			//Find phase error
			int64_t dphase = (edgepos - tnext) - iperiod;
			float fdphase = dphase;

			//If we're more than half a UI off, assume this is actually part of the next UI
			if(fdphase > fHalfPeriod)
				fdphase -= fperiod;
			if(fdphase < -fHalfPeriod)
				fdphase += fperiod;

			//total_error += i64abs(dphase);

			//Find frequency error
			float uiLen = (tnext - tlast);
			float fdperiod = 0;
			if(uiLen > glitchCutoff)		//Sanity check: no correction if we have a glitch
			{
				float numUIs = round(uiLen * initialFrequency);
				if(numUIs != 0)	//divide by zero check needed in some cases
				{
					uiLen /= numUIs;
					fdperiod = fperiod - uiLen;
				}
			}

			if(tlast != 0)
			{
				//Frequency and phase error term
				float errorTerm = (fdperiod * 0.006) + (fdphase * 0.002);
				fperiod -= errorTerm;
				iperiod = fperiod;

				//HACK: immediate bang-bang phase shift
				int64_t bangbang = fperiod * 0.0025;
				if(dphase > 0)
					edgepos -= bangbang;
				else
					edgepos += bangbang;

				/*
				#ifdef PLL_DEBUG_OUTPUTS
					debugPeriod->m_offsets.push_back(edgepos + period/2);
					debugPeriod->m_durations.push_back(period);
					debugPeriod->m_samples.push_back(period);

					debugPhase->m_offsets.push_back(edgepos + period/2);
					debugPhase->m_durations.push_back(period);
					debugPhase->m_samples.push_back(dphase);

					debugFreq->m_offsets.push_back(edgepos + period/2);
					debugFreq->m_durations.push_back(period);
					debugFreq->m_samples.push_back(dperiod);

					debugDrift->m_offsets.push_back(edgepos + period/2);
					debugDrift->m_durations.push_back(period);
					debugDrift->m_samples.push_back(period - initialPeriod);
				#endif
				*/

				if(iperiod < fnyquist)
				{
					LogWarning("PLL attempted to lock to frequency near or above Nyquist\n");
					nedge = edges.size();
					break;
				}
			}

			tlast = tnext;
			tnext = edges[++nedge];
		}

		//Add the sample (90 deg phase offset from the internal NCO)
		cap.m_offsets.push_back(edgepos + center);
	}

	//total_error /= edges.size();
	//LogTrace("average phase error %zu\n", total_error);
}

#ifdef __x86_64__
/**
	@brief AVX2 optimized version of FillSquarewaveGeneric()
 */
__attribute__((target("avx2")))
void ClockRecoveryFilter::FillSquarewaveAVX2(SparseDigitalWaveform& cap)
{
	size_t len = cap.m_offsets.size();
	cap.m_samples.resize(len);
	if(!len)
		return;

	//Load the squarewave dummy fill pattern
	bool filler[32] =
	{
		false, true, false, true, false, true, false, true,
		false, true, false, true, false, true, false, true,
		false, true, false, true, false, true, false, true,
		false, true, false, true, false, true, false, true
	};
	auto fill = _mm256_loadu_si256(reinterpret_cast<__m256i*>(filler));

	size_t end = len - (len % 32);
	uint8_t* ptr = reinterpret_cast<uint8_t*>(&cap.m_samples[0]);
	for(size_t i=0; i<end; i+=32)
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(ptr + i ), fill);

	bool value = false;
	for(size_t i=end; i<len; i++)
	{
		value = !value;
		cap.m_samples[i] = value;
	}
}
#endif /* __x86_64__ */

Filter::DataLocation ClockRecoveryFilter::GetInputLocation()
{
	//We explicitly manage our input memory and don't care where it is when Refresh() is called
	return LOC_DONTCARE;
}
