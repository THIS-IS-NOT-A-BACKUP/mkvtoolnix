/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL v2
   see the file COPYING for details
   or visit https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

   TTA output module

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include "common/common_pch.h"

#include <cmath>

#include "common/codec.h"
#include "common/tta.h"
#include "merge/connection_checks.h"
#include "output/p_tta.h"

using namespace libmatroska;

tta_packetizer_c::tta_packetizer_c(generic_reader_c *p_reader,
                                   track_info_c &p_ti,
                                   int channels,
                                   int bits_per_sample,
                                   int sample_rate)
  : generic_packetizer_c(p_reader, p_ti)
  , m_channels(channels)
  , m_bits_per_sample(bits_per_sample)
  , m_sample_rate(sample_rate)
  , m_samples_output(0)
{
  set_track_type(track_audio);
  set_track_default_duration(std::llround(1000000000.0 * mtx::tta::FRAME_TIME));
}

tta_packetizer_c::~tta_packetizer_c() {
}

void
tta_packetizer_c::set_headers() {
  set_codec_id(MKV_A_TTA);
  set_audio_sampling_freq(m_sample_rate);
  set_audio_channels(m_channels);
  set_audio_bit_depth(m_bits_per_sample);

  generic_packetizer_c::set_headers();
}

int
tta_packetizer_c::process(packet_cptr packet) {
  packet->timestamp = std::llround((double)m_samples_output * 1000000000 / m_sample_rate);
  if (-1 == packet->duration) {
    packet->duration  = m_htrack_default_duration;
    m_samples_output += std::llround(m_sample_rate * mtx::tta::FRAME_TIME);

  } else
    m_samples_output += std::llround(packet->duration * m_sample_rate / 1000000000ll);

  add_packet(packet);

  return FILE_STATUS_MOREDATA;
}

connection_result_e
tta_packetizer_c::can_connect_to(generic_packetizer_c *src,
                                 std::string &error_message) {
  tta_packetizer_c *psrc = dynamic_cast<tta_packetizer_c *>(src);
  if (!psrc)
    return CAN_CONNECT_NO_FORMAT;

  connect_check_a_samplerate(m_sample_rate,   psrc->m_sample_rate);
  connect_check_a_channels(m_channels,        psrc->m_channels);
  connect_check_a_bitdepth(m_bits_per_sample, psrc->m_bits_per_sample);

  return CAN_CONNECT_YES;
}
