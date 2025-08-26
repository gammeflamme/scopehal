/***********************************************************************************************************************
*                                                                                                                      *
* libscopehal                                                                                                          *
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

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of VICPSocketTransport
	@ingroup transports
 */

#include "scopehal.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

/**
	@brief Creates a VICP transport

	@param args	Path to the scope, either host:port or hostname with implied port 1861
 */
VICPSocketTransport::VICPSocketTransport(const string& args)
	: m_nextSequence(1)
	, m_lastSequence(1)
	, m_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
{
	char hostname[128];
	unsigned int port = 0;
	if(2 != sscanf(args.c_str(), "%127[^:]:%u", hostname, &port))
	{
		//default if port not specified
		m_hostname = args;
		m_port = 1861;
	}
	else
	{
		m_hostname = hostname;
		m_port = port;
	}

	LogDebug("Connecting to VICP oscilloscope at %s:%d\n", m_hostname.c_str(), m_port);

	if(!m_socket.Connect(m_hostname, m_port))
	{
		m_socket.Close();
		LogError("Couldn't connect to socket\n");
		return;
	}
	if(!m_socket.DisableNagle())
	{
		m_socket.Close();
		LogError("Couldn't disable Nagle\n");
		return;
	}

	//Attempt to set a 32 MB RX buffer.
	if(!m_socket.SetRxBuffer(32 * 1024 * 1024))
		LogWarning("Could not set 32 MB RX buffer. Consider increasing /proc/sys/net/core/rmem_max\n");
}

VICPSocketTransport::~VICPSocketTransport()
{
}

bool VICPSocketTransport::IsConnected()
{
	return m_socket.IsValid();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual transport code

///@brief Return the constant transport name string "vicp"
string VICPSocketTransport::GetTransportName()
{
	return "vicp";
}

string VICPSocketTransport::GetConnectionString()
{
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s:%u", m_hostname.c_str(), m_port);
	return string(tmp);
}

///@brief Gets the next sequence number to be used by a packet
uint8_t VICPSocketTransport::GetNextSequenceNumber()
{
	m_lastSequence = m_nextSequence;

	//EOI increments the sequence number.
	//Wrap mod 256, but skip zero!
	m_nextSequence ++;
	if(m_nextSequence == 0)
		m_nextSequence = 1;

	return m_lastSequence;
}

bool VICPSocketTransport::SendCommand(const string& cmd)
{
	LogTrace("Send (%s): %s\n", m_hostname.c_str(), cmd.c_str());

	//Operation and flags header
	string payload;
	uint8_t op 	= OP_DATA | OP_EOI;

	//TODO: remote, clear, poll flags
	payload += op;
	payload += 0x01;							//protocol version number
	payload += GetNextSequenceNumber();
	payload += '\0';							//reserved

	//Next 4 header bytes are the message length (network byte order)
	uint32_t len = cmd.length();
	payload += (len >> 24) & 0xff;
	payload += (len >> 16) & 0xff;
	payload += (len >> 8)  & 0xff;
	payload += (len >> 0)  & 0xff;

	//Add message data
	payload += cmd;

	//Actually send it
	SendRawData(payload.size(), (const unsigned char*)payload.c_str());
	return true;
}

//ignore endOnSemicolon, VICP uses EOI for framing
string VICPSocketTransport::ReadReply([[maybe_unused]] bool endOnSemicolon, function<void(float)> progress)
{
	string payload;
	size_t nblocks = 0;
	size_t expectedBytes = 0;
	while(true)
	{
		//Read the header
		unsigned char header[8];
		ReadRawData(8, header);

		//Sanity check
		if(header[1] != 1)
		{
			LogError("Bad VICP protocol version\n");
			return "";
		}
		if(header[2] != m_lastSequence)
		{
			//LogError("Bad VICP sequence number %d (expected %d)\n", header[2], m_lastSequence);
			//return "";
		}
		if(header[3] != 0)
		{
			LogError("Bad VICP reserved field\n");
			return "";
		}

		//Read the message data
		uint32_t len = (header[4] << 24) | (header[5] << 16) | (header[6] << 8) | header[7];
		size_t current_size = payload.size();
		payload.resize(current_size + len);
		char* rxbuf = &payload[current_size];
		ReadRawData(len, (unsigned char*)rxbuf);

		//Skip empty blocks, or just newlines
		if( (len == 0) || (rxbuf[0] == '\n' && len == 1))
		{
			//Special handling needed for EOI.
			if(header[0] & OP_EOI)
			{
				//EOI on an empty block is a stop if we have data from previous blocks.
				if(current_size != 0)
					break;

				//But if we have no data, hold off and wait for the next frame
				else
				{
					payload = "";
					continue;
				}
			}
		}

		//Check EOI flag
		if(header[0] & OP_EOI)
			break;

		//Calculate expected block length for large (multi block) data chunks
		if(expectedBytes == 0)
		{
			if( (payload.size() >= 16) && (payload.substr(5, 2) == "#9"))
			{
				string expectedLength = payload.substr(7, 9) + "\0";
				expectedBytes = atoi(expectedLength.c_str());
			}
		}
		if(progress)
			progress(payload.size() * 1.0 / expectedBytes);

		nblocks ++;
	}

	//make sure there's a null terminator
	payload += "\0";
	if(payload.size() > 256)
	{
		LogTrace("Got (%s): large data block of %zu blocks / %zu bytes, not printing\n",
			m_hostname.c_str(), nblocks, payload.size());
	}
	else
	{
		//avoid double newline in trace output
		if(payload[payload.size()-1] == '\n')
			LogTrace("Got (%s): %s", m_hostname.c_str(), payload.c_str());
		else
			LogTrace("Got (%s): %s\n", m_hostname.c_str(), payload.c_str());
	}
	return payload;
}

void VICPSocketTransport::SendRawData(size_t len, const unsigned char* buf)
{
	m_socket.SendLooped(buf, len);
}

size_t VICPSocketTransport::ReadRawData(size_t len, unsigned char* buf, function<void(float)> progress)
{
	size_t chunk_size = len;
	if (progress)
	{
		/* carve up the chunk_size into either 1% or 32kB chunks, whichever is larger; later, we'll want RecvLooped to do this for us */
		chunk_size /= 100;
		if (chunk_size < 32768)
			chunk_size = 32768;
	}

	for (size_t pos = 0; pos < len; )
	{
		size_t n = chunk_size;
		if (n > (len - pos))
			n = len - pos;
		if(!m_socket.RecvLooped(buf + pos, n))
		{
			LogTrace("Failed to get %zu bytes (@ pos %zu)\n", len, pos);
			return 0;
		}
		pos += n;
		if (progress)
		{
			progress((float)pos / (float)len);
		}
	}

	LogTrace("Got %zu bytes\n", len);
	return len;
}

void VICPSocketTransport::FlushRXBuffer(void)
{
	m_socket.FlushRxBuffer();
}

bool VICPSocketTransport::IsCommandBatchingSupported()
{
	return true;
}
