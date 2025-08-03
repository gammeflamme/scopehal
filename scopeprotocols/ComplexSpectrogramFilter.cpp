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
#include "../scopehal/AlignedAllocator.h"
#include "ComplexSpectrogramFilter.h"
#include "FFTFilter.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ComplexSpectrogramFilter::ComplexSpectrogramFilter(const string& color)
	: SpectrogramFilter(color)
{
	//remove base class ports
	m_signalNames.clear();
	m_inputs.clear();

	//Set up channels
	CreateInput("I");
	CreateInput("Q");
	CreateInput("center");

	m_blackmanHarrisComputePipeline.Reinitialize(
		"shaders/ComplexBlackmanHarrisWindow.spv", 3, sizeof(WindowFunctionArgs));
	m_rectangularComputePipeline.Reinitialize(
		"shaders/ComplexRectangularWindow.spv", 3, sizeof(WindowFunctionArgs));
	m_cosineSumComputePipeline.Reinitialize(
		"shaders/ComplexCosineSumWindow.spv", 3, sizeof(WindowFunctionArgs));

	m_postprocessComputePipeline.Reinitialize(
		"shaders/ComplexSpectrogramPostprocess.spv", 2, sizeof(SpectrogramPostprocessArgs));
}

ComplexSpectrogramFilter::~ComplexSpectrogramFilter()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Factory methods

bool ComplexSpectrogramFilter::ValidateChannel(size_t i, StreamDescriptor stream)
{
	if(stream.m_channel == nullptr)
		return false;

	switch(i)
	{
		case 0:
		case 1:
			return (stream.GetType() == Stream::STREAM_TYPE_ANALOG);

		case 2:
			return	(stream.GetType() == Stream::STREAM_TYPE_ANALOG_SCALAR) &&
					(stream.GetYAxisUnits() == Unit(Unit::UNIT_HZ));

		default:
			return false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

string ComplexSpectrogramFilter::GetProtocolName()
{
	return "Complex Spectrogram";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void ComplexSpectrogramFilter::ReallocateBuffers(size_t fftlen, size_t nblocks)
{
	m_cachedFFTLength = fftlen;
	m_cachedFFTNumBlocks = nblocks;

	size_t nouts = fftlen;
	if(m_vkPlan)
	{
		if(m_vkPlan->size() != fftlen)
			m_vkPlan = nullptr;
	}
	if(!m_vkPlan)
	{
		m_vkPlan = make_unique<VulkanFFTPlan>(
			fftlen, nouts, VulkanFFTPlan::DIRECTION_FORWARD, nblocks, VulkanFFTPlan::TYPE_COMPLEX);
	}

	m_rdinbuf.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_NEVER);
	m_rdinbuf.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	m_rdoutbuf.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_NEVER);
	m_rdoutbuf.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
}

void ComplexSpectrogramFilter::Refresh(vk::raii::CommandBuffer& cmdBuf, shared_ptr<QueueHandle> queue)
{
	//Make sure we've got valid inputs
	auto din_i = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(0));
	auto din_q = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(1));
	auto din_freq = GetInput(2);
	if(!din_i || !din_q || !din_freq)
	{
		SetData(nullptr, 0);
		return;
	}
	auto centerFrequency = din_freq.GetScalarValue();

	//Figure out how many FFTs to do
	//For now, consecutive blocks and not a sliding window
	size_t inlen = min(din_i->size(), din_q->size());
	size_t fftlen = m_parameters[m_fftLengthName].GetIntVal();
	size_t nblocks = floor(inlen * 1.0 / fftlen);

	if( (fftlen != m_cachedFFTLength) || (nblocks != m_cachedFFTNumBlocks) )
		ReallocateBuffers(fftlen, nblocks);

	//Figure out range of the FFTs
	double fs_per_sample = din_i->m_timescale;
	float scale = 2.0 / fftlen;
	double sample_ghz = 1e6 / fs_per_sample;
	double bin_hz = round((sample_ghz * 1e9f) / fftlen);
	double fmax = bin_hz * fftlen;

	Unit hz(Unit::UNIT_HZ);
	LogTrace("ComplexSpectrogramFilter: %zu input points, %zu %zu-point FFTs\n", inlen, nblocks, fftlen);
	LogIndenter li;
	LogTrace("FFT range is DC to %s\n", hz.PrettyPrint(fmax).c_str());
	LogTrace("%s per bin\n", hz.PrettyPrint(bin_hz).c_str());

	//Base frequency is center frequency minus half the FFT range
	int64_t baseFrequency = centerFrequency - bin_hz * (fftlen/2);

	//Create the output
	//Reuse existing buffer if available and same size
	size_t nouts = fftlen;
	SpectrogramWaveform* cap = dynamic_cast<SpectrogramWaveform*>(GetData(0));
	if(cap)
	{
		if( (cap->GetBinSize() == bin_hz) &&
			(cap->GetBottomEdgeFrequency() == baseFrequency) &&
			(cap->GetWidth() == nblocks) &&
			(cap->GetHeight() == nouts) )
		{
			//same config, we can reuse it
		}

		//no, ignore it
		else
			cap = nullptr;
	}
	if(!cap)
	{
		cap = new SpectrogramWaveform(
			nblocks,
			nouts,
			bin_hz,
			baseFrequency
			);
	}
	cap->m_startTimestamp = din_i->m_startTimestamp;
	cap->m_startFemtoseconds = din_i->m_startFemtoseconds;
	cap->m_triggerPhase = din_i->m_triggerPhase;
	cap->m_timescale = fs_per_sample * fftlen;
	cap->PrepareForGpuAccess();
	SetData(cap, 0);

	//We also need to adjust the scale by the coherent power gain of the window function
	auto window = static_cast<FFTFilter::WindowFunction>(m_parameters[m_windowName].GetIntVal());
	switch(window)
	{
		case FFTFilter::WINDOW_HAMMING:
			scale *= 1.862;
			break;

		case FFTFilter::WINDOW_HANN:
			scale *= 2.013;
			break;

		case FFTFilter::WINDOW_BLACKMAN_HARRIS:
			scale *= 2.805;
			break;

		//unit
		case FFTFilter::WINDOW_RECTANGULAR:
		default:
			break;
	}

	//Configure the window
	WindowFunctionArgs args;
	args.numActualSamples = fftlen;
	args.npoints = fftlen;
	args.scale = 2 * M_PI / fftlen;
	switch(window)
	{
		case FFTFilter::WINDOW_HANN:
			args.alpha0 = 0.5;
			break;

		case FFTFilter::WINDOW_HAMMING:
			args.alpha0 = 25.0f / 46;
			break;

		default:
			args.alpha0 = 0;
			break;
	}
	args.alpha1 = 1 - args.alpha0;

	//Figure out which window shader to use
	ComputePipeline* wpipe = nullptr;
	switch(window)
	{
		case FFTFilter::WINDOW_BLACKMAN_HARRIS:
			wpipe = &m_blackmanHarrisComputePipeline;
			break;

		case FFTFilter::WINDOW_HANN:
		case FFTFilter::WINDOW_HAMMING:
			wpipe = &m_cosineSumComputePipeline;
			break;

		default:
		case FFTFilter::WINDOW_RECTANGULAR:
			wpipe = &m_rectangularComputePipeline;
			break;
	}

	//Make sure our temporary buffers are big enough
	m_rdinbuf.resize(nblocks * fftlen * 2);
	m_rdoutbuf.resize(nblocks * (nouts * 2) );

	//Cache a bunch of configuration
	float minscale = m_parameters[m_rangeMinName].GetFloatVal();
	float fullscale = m_parameters[m_rangeMaxName].GetFloatVal();
	float range = fullscale - minscale;

	//Prepare to do all of our compute stuff in one dispatch call to reduce overhead
	cmdBuf.begin({});

	//Grab the input and apply the window function
	wpipe->BindBufferNonblocking(0, din_i->m_samples, cmdBuf);
	wpipe->BindBufferNonblocking(1, m_rdinbuf, cmdBuf, true);
	wpipe->BindBufferNonblocking(2, din_q->m_samples, cmdBuf);
	for(size_t block=0; block<nblocks; block++)
	{
		args.offsetIn = block*fftlen;
		args.offsetOut = block*fftlen;

		const uint32_t compute_block_count = GetComputeBlockCount(fftlen, 64);
		if(block == 0)
			wpipe->Dispatch(cmdBuf, args, min(compute_block_count, 32768u), compute_block_count / 32768 + 1);
		else
			wpipe->DispatchNoRebind(cmdBuf, args, min(compute_block_count, 32768u), compute_block_count / 32768 + 1);
	}
	wpipe->AddComputeMemoryBarrier(cmdBuf);

	//Do the actual FFT
	m_vkPlan->AppendForward(
		m_rdinbuf,
		m_rdoutbuf,
		cmdBuf);

	//Postprocess the output
	//TODO: really deep waveforms might generate a lot of blocks here (enough to exceed the max block count in Y)
	//so do multiple dispatches in that case?
	const float impedance = 50;
	SpectrogramPostprocessArgs postargs;
	postargs.nblocks = nblocks;
	postargs.nouts = nouts;
	postargs.logscale = 10.0 / log(10);
	postargs.impscale = scale*scale / impedance;
	postargs.minscale = minscale;
	postargs.irange = 1.0 / range;
	postargs.ygrid = min(g_maxComputeGroupCount[2], nblocks);
	m_postprocessComputePipeline.AddComputeMemoryBarrier(cmdBuf);
	m_postprocessComputePipeline.BindBufferNonblocking(0, m_rdoutbuf, cmdBuf);
	m_postprocessComputePipeline.BindBufferNonblocking(1, cap->GetOutData(), cmdBuf, true);
	size_t xsize = GetComputeBlockCount(nouts, 64);
	size_t ysize = ceil(nblocks * 1.0 / postargs.ygrid);
	size_t zsize = postargs.ygrid;
	//LogDebug("ComplexSpectrogramFilter: grid %zu x %zu x %zu\n", xsize, ysize, zsize);
	m_postprocessComputePipeline.Dispatch(
		cmdBuf,
		postargs,
		xsize,
		ysize,
		zsize
		);

	//Done, block until the compute operations finish
	cmdBuf.end();
	queue->SubmitAndBlock(cmdBuf);

	cap->MarkModifiedFromGpu();
}
